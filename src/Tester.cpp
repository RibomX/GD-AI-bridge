// Automated playtests: plays the open level with a planned list of inputs and
// reports where (and on what) the player died, or that the run passed.
#include "Api.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

namespace {
    constexpr float GROUND_OFFSET = 90.f;

    struct Input {
        double press;
        double release; // < 0: release on the next physics step (a tap)
    };

    struct TestRun {
        bool active = false;
        bool finished = false;
        std::vector<Input> inputs;
        size_t next = 0;
        bool holding = false;
        double untilX = 0;
        double startX = 0;
        double lastX = 0;
        double nextPathX = 0;
        double pathStep = 30;
        std::string path;
        std::string outcome;
        CCPoint deathPos;
        std::chrono::steady_clock::time_point started;
        float oldTimeScale = 1;
    };

    TestRun g_run;

    LevelEditorLayer* editor() {
        auto lel = LevelEditorLayer::get();
        return lel && lel->m_editorUI ? lel : nullptr;
    }

    void finish(std::string outcome, CCPoint pos) {
        if (!g_run.active) return;
        g_run.active = false;
        g_run.finished = true;
        g_run.outcome = std::move(outcome);
        g_run.deathPos = pos;
        CCDirector::get()->getScheduler()->setTimeScale(g_run.oldTimeScale);
        // Leave the playtest outside of the game's own update.
        queueInMainThread([] {
            if (auto lel = editor(); lel && lel->m_playbackMode != PlaybackMode::Not) {
                lel->m_editorUI->onStopPlaytest(nullptr);
            }
        });
    }

    // Objects close to where the player died, most likely the cause.
    matjson::Value objectsNear(LevelEditorLayer* lel, CCPoint pos) {
        std::vector<std::pair<float, GameObject*>> nearby;
        for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
            if (obj->m_isDecoration || obj->m_isTrigger || obj->m_isNoTouch) continue;
            auto d = ccpDistance(obj->getPosition(), pos);
            if (d <= 45) nearby.emplace_back(d, obj);
        }
        std::sort(nearby.begin(), nearby.end(), [](auto& a, auto& b) { return a.first < b.first; });
        auto list = matjson::Value::array();
        for (size_t i = 0; i < nearby.size() && i < 4; i++) {
            auto obj = nearby[i].second;
            list.push(fmt::format("{},{},{},{}", obj->m_uniqueID, obj->m_objectID, obj->getPositionX(),
                                  obj->getPositionY() - GROUND_OFFSET));
        }
        return list;
    }
}

class $modify(GJBaseGameLayer) {
    // Runs every physics step: press/release planned inputs and track progress.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_run.active && static_cast<GJBaseGameLayer*>(this) == LevelEditorLayer::get() && m_player1) {
            double x = m_player1->getPositionX();
            if (g_run.holding) {
                auto const& in = g_run.inputs[g_run.next];
                if (in.release < 0 || x >= in.release) {
                    this->handleButton(false, 1, true);
                    g_run.holding = false;
                    g_run.next++;
                }
            }
            if (!g_run.holding && g_run.next < g_run.inputs.size() && x >= g_run.inputs[g_run.next].press) {
                this->handleButton(true, 1, true);
                g_run.holding = true;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (g_run.active && static_cast<GJBaseGameLayer*>(this) == LevelEditorLayer::get() && m_player1) {
            auto pos = m_player1->getPosition();
            g_run.lastX = pos.x;
            while (pos.x >= g_run.nextPathX) {
                g_run.path += fmt::format("{:.0f},{:.0f};", pos.x, pos.y - GROUND_OFFSET);
                g_run.nextPathX += g_run.pathStep;
            }
            if (pos.x >= g_run.untilX) finish("passed", pos);
        }
    }
};

class $modify(PlayerObject) {
    void playerDestroyed(bool noEffects) {
        if (g_run.active) {
            auto lel = LevelEditorLayer::get();
            if (lel && static_cast<PlayerObject*>(this) == lel->m_player1) finish("died", getPosition());
        }
        PlayerObject::playerDestroyed(noEffects);
    }
};

ApiResult testStart(matjson::Value const& input) {
    auto lel = editor();
    if (!lel) return ApiResult::error(409, "Not in the level editor. Open a level in the editor first.");
    if (g_run.active) return ApiResult::error(409, "A test is already running");
    if (lel->m_playbackMode != PlaybackMode::Not) lel->m_editorUI->onStopPlaytest(nullptr);

    std::vector<Input> inputs;
    if (input.contains("inputs") && input["inputs"].isArray()) {
        for (auto const& i : input["inputs"]) {
            auto press = i.contains("x") ? i["x"].asDouble().unwrapOr(0) : i.asDouble().unwrapOr(0);
            auto release = i.isObject() && i.contains("release") ? i["release"].asDouble().unwrapOr(-1) : -1;
            inputs.push_back({press, release});
        }
    }
    std::sort(inputs.begin(), inputs.end(), [](auto& a, auto& b) { return a.press < b.press; });

    // Default goal: a bit past the last gameplay object.
    double lastObjectX = 0;
    for (auto obj : CCArrayExt<GameObject*>(lel->m_objects)) {
        if (!obj->m_isDecoration && !obj->m_isTrigger) lastObjectX = std::max<double>(lastObjectX, obj->getPositionX());
    }
    double untilX = input.contains("until_x") ? input["until_x"].asDouble().unwrapOr(0) : lastObjectX + 90;

    // Faster than real time, within the range GD's physics stays stable in.
    double speed = input.contains("speed") ? input["speed"].asDouble().unwrapOr(1) : 1;
    speed = std::clamp(speed, 0.25, 4.0);

    g_run = TestRun{};
    g_run.inputs = std::move(inputs);
    g_run.untilX = untilX;
    g_run.started = std::chrono::steady_clock::now();
    auto scheduler = CCDirector::get()->getScheduler();
    g_run.oldTimeScale = scheduler->getTimeScale();
    scheduler->setTimeScale(static_cast<float>(speed));

    lel->m_editorUI->onPlaytest(nullptr);
    g_run.startX = lel->m_player1 ? lel->m_player1->getPositionX() : 0;
    g_run.nextPathX = g_run.startX;
    // Finer path sampling is useful for measuring jumps; coarser saves tokens.
    g_run.pathStep = std::clamp(input.contains("path_step") ? input["path_step"].asDouble().unwrapOr(30) : 30.0, 5.0, 300.0);
    g_run.active = true;
    return ApiResult::ok(matjson::makeObject({{"started", true}, {"until_x", untilX}}));
}

std::optional<ApiResult> testPoll(double timeoutSeconds) {
    auto lel = editor();
    if (g_run.active) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_run.started).count();
        // The playtest ended without us noticing (user pressed stop, left the editor...).
        if (!lel || lel->m_playbackMode == PlaybackMode::Not) {
            bool dead = lel && lel->m_player1 && lel->m_player1->m_isDead;
            finish(dead ? "died" : "stopped", lel && lel->m_player1 ? lel->m_player1->getPosition()
                                                                     : CCPoint(static_cast<float>(g_run.lastX), 0));
        }
        else if (elapsed > timeoutSeconds) finish("timeout", lel->m_player1->getPosition());
        else return std::nullopt;
    }
    if (!g_run.finished) return ApiResult::error(409, "No test has been run");

    double length = g_run.untilX - g_run.startX;
    double progress = length > 0 ? std::clamp((g_run.deathPos.x - g_run.startX) / length * 100, 0.0, 100.0) : 0;
    auto result = matjson::makeObject({
        {"result", g_run.outcome},
        {"x", std::round(g_run.deathPos.x)},
        {"y", std::round(g_run.deathPos.y - GROUND_OFFSET)},
        {"progress_percent", std::round(progress)},
        {"inputs_used", g_run.next},
        {"path", g_run.path},
    });
    if (g_run.outcome == "died" && lel) result["objects_near_death"] = objectsNear(lel, g_run.deathPos);
    return ApiResult::ok(result);
}
