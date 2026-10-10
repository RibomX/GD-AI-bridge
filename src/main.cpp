#include "Api.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

$on_mod(Loaded) {
    startServer(static_cast<int>(Mod::get()->getSettingValue<int64_t>("port")));
}
