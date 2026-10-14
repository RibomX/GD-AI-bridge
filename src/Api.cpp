// Editor API endpoints. Every handler runs on the main thread (see Server.cpp).
#include "Api.hpp"

#include <Geode/Geode.hpp>
#include <unordered_set>

using namespace geode::prelude;

namespace {
    // GET params arrive as strings, POST bodies as numbers; accept both.
    double num(matjson::Value const& input, std::string_view key, double fallback) {
        if (!input.contains(key)) return fallback;
        auto const& v = input[key];
        if (v.isNumber()) return v.asDouble().unwrapOr(fallback);
        if (v.isString()) return numFromString<double>(v.asString().unwrap()).unwrapOr(fallback);
        return fallback;
    }

    bool flag(matjson::Value const& input, std::string_view key) {
        if (!input.contains(key)) return false;
        auto const& v = input[key];
        if (v.isString()) return v.asString().unwrap() == "1" || v.asString().unwrap() == "true";
        return v.asBool().unwrapOr(false);
    }

    LevelEditorLayer* editor() {
        auto lel = LevelEditorLayer::get();
        return lel && lel->m_editorUI ? lel : nullptr;
    }

    ApiResult notInEditor() {
        return ApiResult::error(409, "Not in the level editor. Open a level in the editor first.");
    }

    // The editor layer puts the ground at y = 90, level data at y = 0.
    // The API always uses level data coordinates, both for reading and writing.
    constexpr float GROUND_OFFSET = 90.f;

    matjson::Value objectToJson(GameObject* obj, bool withRaw, LevelEditorLayer* lel) {
        auto pos = obj->getPosition();
        auto json = matjson::makeObject({
            {"uid", obj->m_uniqueID},
            {"id", obj->m_objectID},
            {"x", pos.x},
            {"y", pos.y - GROUND_OFFSET},
            {"rotation", obj->getRotation()},
            {"scale_x", obj->getScaleX()},
            {"scale_y", obj->getScaleY()},
        });
        if (withRaw) json["raw"] = std::string(obj->getSaveString(lel));
        return json;
    }

    std::vector<GameObject*> findByUids(LevelEditorLayer* lel, matjson::Value const& uids) {
        std::unordered_set<int> wanted;
        for (auto const& v : uids) {
            if (auto id = v.asInt()) wanted.insert(static_cast<int>(*id));
        }
        std::vector<GameObject*> found;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            if (wanted.contains(obj->m_uniqueID)) found.push_back(obj);
        }
        return found;
    }

    // Turn {"id":1,"x":15,"y":15,...} into GD's object save string "1,1,2,15,3,15".
    Result<std::string> objectString(matjson::Value const& o) {
        if (!o.isObject() || !o.contains("id")) return Err("Each object needs at least an \"id\"");
        std::string s = fmt::format("1,{},2,{},3,{}", num(o, "id", 1), num(o, "x", 0), num(o, "y", 0));
        if (o.contains("rotation")) s += fmt::format(",6,{}", num(o, "rotation", 0));
        if (o.contains("scale")) s += fmt::format(",32,{}", num(o, "scale", 1));
        if (o.contains("scale_x")) s += fmt::format(",128,{}", num(o, "scale_x", 1));
        if (o.contains("scale_y")) s += fmt::format(",129,{}", num(o, "scale_y", 1));
        if (flag(o, "flip_x")) s += ",4,1";
        if (flag(o, "flip_y")) s += ",5,1";
        if (o.contains("groups") && o["groups"].isArray()) {
            std::vector<std::string> groups;
            for (auto const& g : o["groups"]) groups.push_back(fmt::format("{}", g.asInt().unwrapOr(0)));
            s += ",57," + fmt::format("{}", fmt::join(groups, "."));
        }
        // Any other raw GD property, e.g. {"props": {"21": 3}} sets color channel 1.
        if (o.contains("props") && o["props"].isObject()) {
            for (auto const& p : o["props"]) {
                auto value = p.isString() ? p.asString().unwrap() : p.dump(matjson::NO_INDENTATION);
                s += fmt::format(",{},{}", *p.getKey(), value);
            }
        }
        return Ok(s);
    }

    void refreshEditorUI(LevelEditorLayer* lel) {
        lel->m_editorUI->updateButtons();
        lel->m_editorUI->updateObjectInfoLabel();
    }

    // Upper bound for objects created by one request, so a typo can't freeze the game.
    constexpr size_t MAX_BATCH = 10000;

    // Create objects from a GD object string as a single undo step, and report their uids.
    ApiResult createBatch(LevelEditorLayer* lel, std::string const& str) {
        auto created = lel->createObjectsFromString(str, true, true);
        if (!created || created->count() == 0) return ApiResult::error(400, "No objects were created");
        lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        refreshEditorUI(lel);

        auto uids = matjson::Value::array();
        for (auto obj : CCArrayExt<GameObject*>(created)) uids.push(obj->m_uniqueID);
        return ApiResult::ok(matjson::makeObject({{"created", created->count()}, {"uids", uids}}));
    }

    ApiResult checkEditable(LevelEditorLayer* lel) {
        if (!lel) return notInEditor();
        if (lel->m_playbackMode == PlaybackMode::Playing) return ApiResult::error(409, "Stop the playtest first");
        return ApiResult::ok(nullptr);
    }

    // A GD save string is "key,value,key,value,..."; edit it as ordered pairs.
    using SaveFields = std::vector<std::pair<std::string, std::string>>;

    SaveFields parseSaveString(std::string const& str) {
        SaveFields fields;
        auto parts = string::split(str, ",");
        for (size_t i = 0; i + 1 < parts.size(); i += 2) fields.emplace_back(parts[i], parts[i + 1]);
        return fields;
    }

    void setField(SaveFields& fields, std::string const& key, std::string const& value) {
        for (auto& [k, v] : fields) {
            if (k == key) { v = value; return; }
        }
        fields.emplace_back(key, value);
    }

    double fieldNum(SaveFields const& fields, std::string const& key) {
        for (auto const& [k, v] : fields) {
            if (k == key) return numFromString<double>(v).unwrapOr(0);
        }
        return 0;
    }

    std::string joinSaveString(SaveFields const& fields) {
        std::string s;
        for (auto const& [k, v] : fields) {
            if (!s.empty()) s += ",";
            s += k + "," + v;
        }
        return s;
    }

    std::string levelSaveString(GameObject* obj, LevelEditorLayer* lel) {
        return std::string(obj->getSaveString(lel));
    }

    // Objects a request targets: {"uids": [...]} or {"region": {x_min, x_max, y_min, y_max}}.
    Result<std::vector<GameObject*>> findTargets(LevelEditorLayer* lel, matjson::Value const& input) {
        if (input.contains("uids") && input["uids"].isArray()) return Ok(findByUids(lel, input["uids"]));
        if (input.contains("region") && input["region"].isObject()) {
            auto const& r = input["region"];
            double xMin = num(r, "x_min", -1e9), xMax = num(r, "x_max", 1e9);
            double yMin = num(r, "y_min", -1e9), yMax = num(r, "y_max", 1e9);
            std::vector<GameObject*> found;
            for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
                auto p = obj->getPosition();
                auto y = p.y - GROUND_OFFSET;
                if (p.x >= xMin && p.x <= xMax && y >= yMin && y <= yMax) found.push_back(obj);
            }
            return Ok(found);
        }
        return Err("Send {\"uids\": [...]} or {\"region\": {\"x_min\", \"x_max\", \"y_min\", \"y_max\"}}");
    }

    // A template object ({"id", "rotation", "groups", "props", ...}) placed at x, y.
    Result<std::string> placedString(matjson::Value const& tmpl, double x, double y) {
        auto o = tmpl;
        o["x"] = x;
        o["y"] = y;
        return objectString(o);
    }

    // --- endpoints ---

    ApiResult status(matjson::Value const&) {
        auto lel = editor();
        if (!lel) return ApiResult::ok(matjson::makeObject({{"in_editor", false}}));
        return ApiResult::ok(matjson::makeObject({
            {"in_editor", true},
            {"level_name", std::string(lel->m_level->m_levelName)},
            {"object_count", lel->m_objects->count()},
            {"playtesting", lel->m_playbackMode != PlaybackMode::Not},
        }));
    }

    ApiResult getObjects(matjson::Value const& input) {
        auto lel = editor();
        if (!lel) return notInEditor();
        double xMin = num(input, "x_min", -1e9), xMax = num(input, "x_max", 1e9);
        size_t offset = num(input, "offset", 0), limit = num(input, "limit", 1000);
        bool raw = flag(input, "raw");
        // "compact": one "uid,id,x,y[,rotation,scale_x,scale_y]" entry per object, ';'-separated.
        // Several times fewer tokens than JSON, which matters for AIs reading big levels.
        bool compact = input.contains("format") && input["format"].asString().unwrapOr("") == "compact";

        auto list = matjson::Value::array();
        std::string compactList;
        size_t matched = 0, listed = 0;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            auto x = obj->getPositionX();
            if (x < xMin || x > xMax) continue;
            if (matched++ < offset || listed >= limit) continue;
            listed++;
            if (!compact) {
                list.push(objectToJson(obj, raw, lel));
                continue;
            }
            auto y = obj->getPositionY() - GROUND_OFFSET;
            compactList += fmt::format("{},{},{},{}", obj->m_uniqueID, obj->m_objectID, x, y);
            auto rot = obj->getRotation(), sx = obj->getScaleX(), sy = obj->getScaleY();
            if (rot != 0 || sx != 1 || sy != 1) compactList += fmt::format(",{},{},{}", rot, sx, sy);
            compactList += ";";
        }
        if (compact) {
            return ApiResult::ok(matjson::makeObject({
                {"total", matched},
                {"format", "uid,id,x,y[,rotation,scale_x,scale_y];..."},
                {"objects", compactList},
            }));
        }
        return ApiResult::ok(matjson::makeObject({{"total", matched}, {"objects", list}}));
    }

    char mapChar(GameObject* obj) {
        if (obj->m_isTrigger || obj->m_isDecoration || obj->m_isNoTouch) return 0;
        switch (obj->m_objectType) {
            case GameObjectType::Solid: return '#';
            case GameObjectType::Slope: return '/';
            case GameObjectType::Breakable: return '%';
            case GameObjectType::Hazard:
            case GameObjectType::AnimatedHazard: return '^';
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::RedJumpPad:
            case GameObjectType::SpiderPad: return '_';
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SpiderOrb:
            case GameObjectType::TeleportOrb: return 'o';
            case GameObjectType::Modifier: return '>';
            case GameObjectType::SecretCoin:
            case GameObjectType::UserCoin:
            case GameObjectType::Collectible: return '$';
            case GameObjectType::Decoration: return 0;
            default: return 'P'; // every remaining type is a portal
        }
    }

    // When several objects share a cell, show the one that matters most for gameplay.
    int mapPriority(char c) {
        switch (c) {
            case '^': return 6;
            case 'P': return 5;
            case 'o': return 4;
            case '_': return 3;
            case '>': return 2;
            case '$': return 1;
            default: return 0;
        }
    }

    // ASCII picture of a level section, one character per 30x30 block.
    ApiResult getMap(matjson::Value const& input) {
        auto lel = editor();
        if (!lel) return notInEditor();
        constexpr double CELL = 30;
        double xMin = num(input, "x_min", 0), xMax = num(input, "x_max", xMin + 60 * CELL);
        double yMin = num(input, "y_min", 0), yMax = num(input, "y_max", yMin + 12 * CELL);
        int cols = static_cast<int>(std::ceil((xMax - xMin) / CELL));
        int rows = static_cast<int>(std::ceil((yMax - yMin) / CELL));
        if (cols <= 0 || rows <= 0) return ApiResult::error(400, "Empty area");
        if (cols > 300 || rows > 80) return ApiResult::error(400, "Area too big (max 300 x 80 blocks)");

        std::vector<std::string> grid(rows, std::string(cols, '.'));
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            auto c = mapChar(obj);
            if (!c) continue;
            auto p = obj->getPosition();
            int col = static_cast<int>(std::floor((p.x - xMin) / CELL));
            int row = static_cast<int>(std::floor((p.y - GROUND_OFFSET - yMin) / CELL));
            if (col < 0 || col >= cols || row < 0 || row >= rows) continue;
            auto& cell = grid[row][col];
            if (cell == '.' || mapPriority(c) > mapPriority(cell)) cell = c;
        }

        // Top row first, each prefixed with the y of its cell center.
        std::string map;
        for (int r = rows - 1; r >= 0; r--) {
            map += fmt::format("{:>5}|{}\n", yMin + r * CELL + CELL / 2, grid[r]);
        }
        if (yMin <= 0) map += fmt::format("{:>5}|{}\n", "", std::string(cols, '='));
        return ApiResult::ok(matjson::makeObject({
            {"x_min", xMin},
            {"cell", CELL},
            {"legend", "# block, / slope, % breakable, ^ hazard, _ pad, o orb, P portal, > speed, $ coin, = ground, . empty. "
                       "Column i covers x from x_min + 30*i to x_min + 30*(i+1); row labels are cell-center y."},
            {"map", map},
        }));
    }

    ApiResult getSelection(matjson::Value const& input) {
        auto lel = editor();
        if (!lel) return notInEditor();
        auto list = matjson::Value::array();
        for (auto obj : CCArrayExt<GameObject*>(lel->m_editorUI->getSelectedObjects())) {
            list.push(objectToJson(obj, flag(input, "raw"), lel));
        }
        return ApiResult::ok(matjson::makeObject({{"objects", list}}));
    }

    ApiResult getLevelString(matjson::Value const&) {
        auto lel = editor();
        if (!lel) return notInEditor();
        return ApiResult::ok(matjson::makeObject({{"level_string", std::string(lel->getLevelString())}}));
    }

    ApiResult addObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;

        // Either structured objects, or a raw GD object string ("1,1,2,15,3,15;1,8,2,45,3,15").
        std::string str;
        if (input.contains("objects") && input["objects"].isArray()) {
            for (auto const& o : input["objects"]) {
                auto s = objectString(o);
                if (!s) return ApiResult::error(400, s.unwrapErr());
                str += *s + ";";
            }
        } else if (input.contains("raw") && input["raw"].isString()) {
            str = input["raw"].asString().unwrap();
        } else {
            return ApiResult::error(400, "Send {\"objects\": [...]} or {\"raw\": \"<GD object string>\"}");
        }
        return createBatch(lel, str);
    }

    // Fill a rectangle with copies of one object, on a grid (default: one per block).
    ApiResult fillObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;
        if (!input.contains("id")) return ApiResult::error(400, "Send {\"id\", \"x1\", \"y1\", \"x2\", \"y2\", \"step\"?}");

        double x1 = num(input, "x1", 15), y1 = num(input, "y1", 15);
        double x2 = num(input, "x2", x1), y2 = num(input, "y2", y1);
        double stepX = num(input, "step_x", num(input, "step", 30));
        double stepY = num(input, "step_y", num(input, "step", 30));
        if (stepX <= 0 || stepY <= 0) return ApiResult::error(400, "step must be > 0");
        if (x2 < x1) std::swap(x1, x2);
        if (y2 < y1) std::swap(y1, y2);

        auto cols = static_cast<size_t>((x2 - x1) / stepX) + 1;
        auto rows = static_cast<size_t>((y2 - y1) / stepY) + 1;
        if (cols * rows > MAX_BATCH) {
            return ApiResult::error(400, fmt::format("That would create {} objects (max {})", cols * rows, MAX_BATCH));
        }

        std::string str;
        for (size_t r = 0; r < rows; r++) {
            for (size_t c = 0; c < cols; c++) {
                auto s = placedString(input, x1 + c * stepX, y1 + r * stepY);
                if (!s) return ApiResult::error(400, s.unwrapErr());
                str += *s + ";";
            }
        }
        return createBatch(lel, str);
    }

    // Place copies of one object along a straight line from (x1, y1) to (x2, y2).
    ApiResult lineObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;
        if (!input.contains("id")) return ApiResult::error(400, "Send {\"id\", \"x1\", \"y1\", \"x2\", \"y2\"?, \"step\"? or \"count\"?}");

        double x1 = num(input, "x1", 15), y1 = num(input, "y1", 15);
        double x2 = num(input, "x2", x1), y2 = num(input, "y2", y1);
        double length = std::hypot(x2 - x1, y2 - y1);

        size_t count;
        if (input.contains("count")) {
            count = static_cast<size_t>(std::max(1.0, num(input, "count", 1)));
        } else {
            double step = num(input, "step", 30);
            if (step <= 0) return ApiResult::error(400, "step must be > 0");
            count = static_cast<size_t>(length / step) + 1;
        }
        if (count > MAX_BATCH) return ApiResult::error(400, fmt::format("That would create {} objects (max {})", count, MAX_BATCH));

        std::string str;
        for (size_t i = 0; i < count; i++) {
            double t = count == 1 ? 0 : static_cast<double>(i) / (count - 1);
            auto s = placedString(input, x1 + (x2 - x1) * t, y1 + (y2 - y1) * t);
            if (!s) return ApiResult::error(400, s.unwrapErr());
            str += *s + ";";
        }
        return createBatch(lel, str);
    }

    // Duplicate objects with an offset, optionally several times (dx, dy applied per copy).
    ApiResult copyObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;
        auto targets = findTargets(lel, input);
        if (!targets) return ApiResult::error(400, targets.unwrapErr());
        auto objects = std::move(targets).unwrap();
        if (objects.empty()) return ApiResult::error(400, "No objects matched");

        double dx = num(input, "dx", 0), dy = num(input, "dy", 0);
        auto times = static_cast<size_t>(std::max(1.0, num(input, "times", 1)));
        if (objects.size() * times > MAX_BATCH) {
            return ApiResult::error(400, fmt::format("That would create {} objects (max {})", objects.size() * times, MAX_BATCH));
        }

        std::vector<SaveFields> sources;
        for (auto obj : objects) sources.push_back(parseSaveString(levelSaveString(obj, lel)));

        std::string str;
        for (size_t t = 1; t <= times; t++) {
            for (auto fields : sources) {
                setField(fields, "2", fmt::format("{}", fieldNum(fields, "2") + dx * t));
                setField(fields, "3", fmt::format("{}", fieldNum(fields, "3") + dy * t));
                str += joinSaveString(fields) + ";";
            }
        }
        return createBatch(lel, str);
    }

    // Move objects in place by (dx, dy). Keeps their uids; one undo step.
    ApiResult moveObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;
        auto targets = findTargets(lel, input);
        if (!targets) return ApiResult::error(400, targets.unwrapErr());
        auto objects = std::move(targets).unwrap();
        if (objects.empty()) return ApiResult::ok(matjson::makeObject({{"moved", 0}}));

        auto arr = CCArray::create();
        for (auto obj : objects) arr->addObject(obj);
        // The undo object snapshots the objects' state, so it must exist before the move.
        lel->addToUndoList(UndoObject::createWithTransformObjects(arr, UndoCommand::Transform), false);

        CCPoint offset(num(input, "dx", 0), num(input, "dy", 0));
        for (auto obj : objects) lel->m_editorUI->moveObject(obj, offset);
        refreshEditorUI(lel);
        return ApiResult::ok(matjson::makeObject({{"moved", objects.size()}}));
    }

    // Change any properties of existing objects. GD can't edit most properties in place,
    // so objects are replaced: they get new uids, and undoing takes two steps.
    ApiResult updateObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;
        if (!input.contains("set") || !input["set"].isObject()) {
            return ApiResult::error(400, "Send {\"uids\": [...], \"set\": {\"x\", \"y\", \"rotation\", \"scale\", \"groups\", \"props\", ...}}");
        }
        auto targets = findTargets(lel, input);
        if (!targets) return ApiResult::error(400, targets.unwrapErr());
        auto objects = std::move(targets).unwrap();
        if (objects.empty()) return ApiResult::error(400, "No objects matched");

        // Turn "set" into save-string fields once, reusing the normal object parser.
        auto set = input["set"];
        if (!set.contains("id")) set["id"] = 0;
        auto setString = objectString(set);
        if (!setString) return ApiResult::error(400, setString.unwrapErr());
        auto overrides = parseSaveString(*setString);

        std::string str;
        for (auto obj : objects) {
            auto fields = parseSaveString(levelSaveString(obj, lel));
            for (auto const& [k, v] : overrides) {
                // Only override what the caller actually set.
                bool given = (k == "1" && input["set"].contains("id")) || (k == "2" && set.contains("x")) ||
                             (k == "3" && set.contains("y")) || (k != "1" && k != "2" && k != "3");
                if (given) setField(fields, k, v);
            }
            str += joinSaveString(fields) + ";";
        }

        auto ui = lel->m_editorUI;
        auto arr = CCArray::create();
        for (auto obj : objects) arr->addObject(obj);
        ui->deselectAll();
        ui->selectObjects(arr, true);
        ui->onDeleteSelected(nullptr);
        return createBatch(lel, str);
    }

    ApiResult deleteObjects(matjson::Value const& input) {
        auto lel = editor();
        if (auto check = checkEditable(lel); check.status != 200) return check;
        auto targets = findTargets(lel, input);
        if (!targets) return ApiResult::error(400, targets.unwrapErr());
        auto objects = std::move(targets).unwrap();
        if (objects.empty()) return ApiResult::ok(matjson::makeObject({{"deleted", 0}}));

        // Go through the editor's own delete so it creates a normal undo step.
        auto ui = lel->m_editorUI;
        auto arr = CCArray::create();
        for (auto obj : objects) arr->addObject(obj);
        ui->deselectAll();
        ui->selectObjects(arr, true);
        ui->onDeleteSelected(nullptr);
        refreshEditorUI(lel);
        return ApiResult::ok(matjson::makeObject({{"deleted", objects.size()}}));
    }

    ApiResult select(matjson::Value const& input) {
        auto lel = editor();
        if (!lel) return notInEditor();
        auto ui = lel->m_editorUI;
        ui->deselectAll();
        if (input.contains("uids") && input["uids"].isArray()) {
            auto arr = CCArray::create();
            for (auto obj : findByUids(lel, input["uids"])) arr->addObject(obj);
            ui->selectObjects(arr, true);
        }
        refreshEditorUI(lel);
        return ApiResult::ok(matjson::makeObject({{"selected", ui->getSelectedObjects()->count()}}));
    }

    ApiResult undo(matjson::Value const&) {
        auto lel = editor();
        if (!lel) return notInEditor();
        lel->m_editorUI->undoLastAction(nullptr);
        return ApiResult::ok(matjson::makeObject({{"ok", true}}));
    }

    ApiResult playtest(matjson::Value const& input) {
        auto lel = editor();
        if (!lel) return notInEditor();
        auto action = input.contains("action") ? input["action"].asString().unwrapOr("") : "";
        if (action == "start") lel->m_editorUI->onPlaytest(nullptr);
        else if (action == "stop") lel->m_editorUI->onStopPlaytest(nullptr);
        else return ApiResult::error(400, "Send {\"action\": \"start\"} or {\"action\": \"stop\"}");
        return ApiResult::ok(matjson::makeObject({{"ok", true}}));
    }
}

std::vector<ApiRoute> const& apiRoutes() {
    static std::vector<ApiRoute> routes = {
        {"GET", "/status", "Whether the editor is open, level name, object count, playtest state", status},
        {"GET", "/objects", "List objects. Query: x_min, x_max, offset, limit (default 1000), raw=1, format=compact", getObjects},
        {"GET", "/map", "ASCII map of an area, one char per block. Query: x_min, x_max, y_min, y_max", getMap},
        {"GET", "/selection", "Currently selected objects. Query: raw=1", getSelection},
        {"GET", "/level-string", "Full level string of the open level", getLevelString},
        {"POST", "/objects", "Add objects: {\"objects\": [{\"id\",\"x\",\"y\",\"rotation\",\"scale\",\"scale_x\",\"scale_y\",\"flip_x\",\"flip_y\",\"groups\",\"props\"}]} or {\"raw\": \"...\"}. One undo step.", addObjects},
        {"POST", "/objects/fill", "Fill a rectangle: {\"id\", \"x1\", \"y1\", \"x2\", \"y2\", \"step\"? (30), + any object field}. One undo step.", fillObjects},
        {"POST", "/objects/line", "Objects along a line: {\"id\", \"x1\", \"y1\", \"x2\", \"y2\"?, \"step\"? (30) or \"count\"?, + any object field}. One undo step.", lineObjects},
        {"POST", "/objects/copy", "Duplicate: {\"uids\" or \"region\", \"dx\", \"dy\", \"times\"? (1)}. Offset applies per copy. One undo step.", copyObjects},
        {"POST", "/objects/move", "Move in place: {\"uids\" or \"region\", \"dx\", \"dy\"}. Keeps uids. One undo step.", moveObjects},
        {"POST", "/objects/update", "Change properties: {\"uids\" or \"region\", \"set\": {any object field}}. Objects get new uids; undo takes 2 steps.", updateObjects},
        {"POST", "/objects/delete", "Delete objects: {\"uids\": [...]} or {\"region\": {...}}. One undo step.", deleteObjects},
        {"POST", "/select", "Select objects in the editor: {\"uids\": [...]} (empty = deselect all)", select},
        {"POST", "/undo", "Undo the last editor action", undo},
        {"POST", "/playtest", "{\"action\": \"start\" | \"stop\"}", playtest},
    };
    return routes;
}

matjson::Value apiInfo() {
    auto endpoints = matjson::Value::array();
    for (auto const& r : apiRoutes()) {
        endpoints.push(matjson::makeObject({
            {"method", r.method}, {"path", r.path}, {"description", r.description},
        }));
    }
    endpoints.push(matjson::makeObject({
        {"method", "POST"},
        {"path", "/test"},
        {"description", "Play the level with planned inputs and wait for the result: {\"inputs\": [{\"x\": press_x, \"release\"?: x}] "
                        "(no release = tap), \"until_x\"? (default: past the last object), \"speed\"? (0.25-4), \"timeout\"? (s)}. "
                        "Returns result passed/died/stopped/timeout, position, progress, objects near death and the player path."},
    }));
    // Most used object IDs, so an AI can build without guessing.
    static auto const commonIds = matjson::makeObject({
        {"blocks", "1 default block"},
        {"hazards", "8 spike, 39 small spike"},
        {"pads", "35 yellow, 140 pink, 67 blue (gravity)"},
        {"orbs", "36 yellow, 141 pink, 84 blue (gravity)"},
        {"gamemode_portals", "12 cube, 13 ship, 47 ball, 111 ufo, 660 wave, 745 robot, 1331 spider, 1933 swing"},
        {"other_portals", "10 gravity normal, 11 gravity flip, 45 mirror on, 46 mirror off, 99 normal size, 101 mini"},
        {"speed", "200 slow, 201 normal, 202 fast, 203 faster, 1334 fastest"},
        {"triggers", "899 color, 901 move, 1006 pulse, 1007 alpha, 1049 toggle, 1268 spawn, 1346 rotate"},
        {"misc", "31 start position"},
    });
    return matjson::makeObject({
        {"name", "GD AI Bridge"},
        {"version", Mod::get()->getVersion().toVString()},
        {"units", "1 block = 30 units. Grid cell centers sit at 15, 45, 75... y = 15 is the first row above the ground."},
        {"common_object_ids", commonIds},
        {"endpoints", endpoints},
    });
}
