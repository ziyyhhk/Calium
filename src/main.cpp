// Calium entry point: load-time setup, live setting reloads, and the hotkeys.
//
// There is no Mod subclass here. Geode 5 gives a plain C++ mod everything it needs through the
// $on_mod hook plus Mod::get(), and keeping the state in free functions means it all lives in one
// translation unit (config.cpp) instead of being reachable through an object whose lifetime during
// unload is anybody's guess.

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>

#include <calium.hpp>

using namespace geode::prelude;

namespace calium {

    namespace {

        // The order the cycle hotkey walks. Custom is in the list because it means "whatever the
        // sliders say", which is the state people usually want to get back to.
        constexpr std::array<char const*, 5> kPresetOrder = {
            "Custom", "Performance", "Balanced", "Quality", "Cinematic"
        };

    }   // namespace

    // ------------------------------------------------------------------ hotkeys

    void toggleMaster() {
        auto* mod = Mod::get();
        bool const on = mod->getSettingValue<bool>("mod-enabled");
        mod->setSettingValue<bool>("mod-enabled", !on);
        reloadFromSettings();

        // Report what actually happened, not what was asked for: on a device whose backbuffer the
        // grab refused, "on" would be a lie.
        char const* reason = nullptr;
        if (!rendererEnabled()) {
            reason = capability().grab == core::GrabMethod::Disabled ? "grab refused on this device"
                   : (failureCount() >= kFailureBudget             ? "stopped after repeated failures"
                                                                   : "nothing to do at this scale");
        }
        Notification::create(rendererEnabled()
            ? fmt::format("Calium on - {}", statusLine())
            : fmt::format("Calium off: {}", reason))->show();
    }

    void cyclePreset() {
        std::string const current = Mod::get()->getSettingValue<std::string>("preset");
        size_t next = 0;
        for (size_t i = 0; i < kPresetOrder.size(); ++i) {
            if (current == kPresetOrder[i]) { next = i + 1; break; }
        }
        if (next >= kPresetOrder.size()) next = 0;
        std::string const name{kPresetOrder[next]};
        applyPreset(name);
        Notification::create(fmt::format("Calium preset: {}", name))->show();
    }

    // ------------------------------------------------------------------ lifecycle

    bool initMod() { return true; }

    void shutdownMod() {
        // Deliberately does not free the GL resources. They can only be released on the render
        // thread with a live context, and Geode does not promise that during an unload; getting it
        // wrong is a crash on closing the game, which is far worse than the handful of megabytes of
        // render targets that stay allocated until the process exits. Anything that has to happen on
        // the GL thread happens inside the drawScene path (see glcore.cpp).
    }

}

$on_mod(Loaded) {
    calium::initMod();
    calium::reloadFromSettings();
    calium::onLevelStart();

    log::info("[Calium] {}", calium::statusLine());

    // Re-read the Config whenever anything in the settings changes. Going through the mod-wide
    // settings event, rather than one listener per key, means adding a setting never requires
    // touching this file. (Mod::settingReact is protected, so the public listener helper is the
    // supported route.)
    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        calium::reloadFromSettings();
        calium::requestGLRebuild();
    });

    // Registered as keybind *settings*, so they are rebindable from the loader's own keybind page and
    // work with a keyboard or a controller.
    listenForKeybindSettingPresses("toggle-mod-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) calium::toggleMaster();
    });
    listenForKeybindSettingPresses("cycle-preset-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) calium::cyclePreset();
    });
    listenForKeybindSettingPresses("toggle-hud-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            calium::config().hudEnabled = !calium::config().hudEnabled;
            log::info("[Calium] HUD {}", calium::config().hudEnabled ? "shown" : "hidden");
        }
    });
}
