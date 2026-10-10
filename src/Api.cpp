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

        auto list = matjson::Value::array();
        size_t matched = 0;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            auto x = obj->getPositionX();
            if (x < xMin || x > xMax) continue;
            if (matched++ < offset || list.size() >= limit) continue;
            list.push(objectToJson(obj, raw, lel));
        }
        return ApiResult::ok(matjson::makeObject({{"total", matched}, {"objects", list}}));
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
        if (!lel) return notInEditor();
        if (lel->m_playbackMode == PlaybackMode::Playing) return ApiResult::error(409, "Stop the playtest first");

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

        auto created = lel->createObjectsFromString(str, true, true);
        if (!created || created->count() == 0) return ApiResult::error(400, "No objects were created");
        // One undo step for the whole batch, like pasting.
        lel->addToUndoList(UndoObject::createWithArray(created, UndoCommand::Paste), false);
        refreshEditorUI(lel);

        auto uids = matjson::Value::array();
        for (auto obj : CCArrayExt<GameObject*>(created)) uids.push(obj->m_uniqueID);
        return ApiResult::ok(matjson::makeObject({{"created", created->count()}, {"uids", uids}}));
    }

    ApiResult deleteObjects(matjson::Value const& input) {
        auto lel = editor();
        if (!lel) return notInEditor();
        if (lel->m_playbackMode == PlaybackMode::Playing) return ApiResult::error(409, "Stop the playtest first");
        if (!input.contains("uids") || !input["uids"].isArray()) return ApiResult::error(400, "Send {\"uids\": [...]}");

        auto objects = findByUids(lel, input["uids"]);
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
        {"GET", "/objects", "List objects. Query: x_min, x_max, offset, limit (default 1000), raw=1", getObjects},
        {"GET", "/selection", "Currently selected objects. Query: raw=1", getSelection},
        {"GET", "/level-string", "Full level string of the open level", getLevelString},
        {"POST", "/objects", "Add objects: {\"objects\": [{\"id\",\"x\",\"y\",\"rotation\",\"scale\",\"scale_x\",\"scale_y\",\"flip_x\",\"flip_y\",\"groups\",\"props\"}]} or {\"raw\": \"...\"}. One undo step.", addObjects},
        {"POST", "/objects/delete", "Delete objects: {\"uids\": [...]}. One undo step.", deleteObjects},
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
