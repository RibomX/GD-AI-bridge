#include "Api.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/AppDelegate.hpp>

using namespace geode::prelude;

$on_mod(Loaded) {
    startServer(static_cast<int>(Mod::get()->getSettingValue<int64_t>("port")));
}

// GD stops its main loop when minimized, which would make every AI request
// time out. In the editor, keep running so the AI can work in the background.
class $modify(AppDelegate) {
    void applicationDidEnterBackground() {
        if (LevelEditorLayer::get() && Mod::get()->getSettingValue<bool>("run-minimized")) return;
        AppDelegate::applicationDidEnterBackground();
    }

    void applicationWillEnterForeground() {
        if (LevelEditorLayer::get() && Mod::get()->getSettingValue<bool>("run-minimized")) return;
        AppDelegate::applicationWillEnterForeground();
    }
};
