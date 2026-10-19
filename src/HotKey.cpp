#include "HotKey.hpp"
#include <Geode/modify/CCKeyboardDispatcher.hpp>

using namespace geode::prelude;

static std::unordered_map<enumKeyCodes, std::function<void()>> s_callbacks;

void HotKey::registerKey(enumKeyCodes key, std::function<void()> callback) {
    s_callbacks[key] = callback;
}

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            auto it = s_callbacks.find(key);
            if (it != s_callbacks.end()) {
                it->second();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
