#pragma once

#include <Geode/Geode.hpp>
#include <functional>

class HotKey {
public:
    static void registerKey(cocos2d::enumKeyCodes key, std::function<void()> callback);
};
