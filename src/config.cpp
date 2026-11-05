// Calium configuration: mod.json settings in, runtime Config out, plus the small amount of global
// state every translation unit needs (stats, failures, capability).
//
// Every number coming from the settings UI is clamped here, so a hand-edited settings file or a
// future setting added without a range cannot hand the shader a nonsense value.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <Geode/Geode.hpp>

#include <calium.hpp>

using namespace geode::prelude;

namespace calium {

    namespace {

        Config          g_config;
        core::RollingStats g_frames{240};
        core::FrameCounters g_session;
        core::FrameCounters g_frame;
        core::ScaleState    g_scale;
        Snapshot            g_snapshot;
        Capability          g_capability;
        int                 g_failures = 0;
        std::string         g_lastFailure;
        std::string         g_status;

        // ---- setting readers, all clamped ----------------------------------------------------

        bool boolean(char const* key, bool fallback) {
            auto* mod = Mod::get();
            if (!mod->getSetting(key)) return fallback;
            return mod->getSettingValue<bool>(key);
        }

        float numberf(char const* key, float lo, float hi, float fallback) {
            auto* mod = Mod::get();
            if (!mod->getSetting(key)) return fallback;
            float v = mod->getSettingValue<float>(key);
            if (!std::isfinite(v)) return fallback;
            return core::clampf(v, lo, hi);
        }

        int64_t numberi(char const* key, int64_t lo, int64_t hi, int64_t fallback) {
            auto* mod = Mod::get();
            if (!mod->getSetting(key)) return fallback;
            int64_t v = mod->getSettingValue<int64_t>(key);
            return std::clamp(v, lo, hi);
        }

        std::string text(char const* key, std::string const& fallback) {
            auto* mod = Mod::get();
            if (!mod->getSetting(key)) return fallback;
            auto v = mod->getSettingValue<std::string>(key);
            return v.empty() ? fallback : v;
        }

        int grabOverrideFromText(std::string const& s) {
            if (s == "Copy-tex")    return 1;
            if (s == "Blit")        return 2;
            if (s == "Read-pixels") return 3;
            return 0;   // Auto
        }

    }   // namespace

    // ------------------------------------------------------------------ accessors

    Config& config() { return g_config; }

    core::RollingStats& frameStats() { return g_frames; }
    core::FrameCounters& counters()      { return g_session; }
    core::FrameCounters& frameCounters() { return g_frame; }
    core::ScaleState& scaleState()       { return g_scale; }
    Snapshot& snapshot()                 { return g_snapshot; }
    Snapshot const& constSnapshot()      { return g_snapshot; }
    Capability& capability()             { return g_capability; }

    void noteFailure(std::string what) {
        ++g_failures;
        g_lastFailure = std::move(what);
        if (g_failures == kFailureBudget) {
            log::error("[Calium] {} failures in one session - the render path is off until the "
                       "level is reloaded. Last reason: {}", g_failures, g_lastFailure);
        } else if (g_failures <= 3) {
            log::warn("[Calium] render failure #{}: {}", g_failures, g_lastFailure);
        }
    }

    int failureCount()               { return g_failures; }
    std::string const& lastFailure() { return g_lastFailure; }
    void resetFailures()             { g_failures = 0; g_lastFailure.clear(); }

    // ------------------------------------------------------------------ policy

    bool captureWorthIt(Config const& c) {
        // A capture at native resolution with no effects is three full-screen passes that reproduce
        // the input exactly. That costs GPU time and shows nothing, so it is treated as off.
        return core::anyEffect(c.fx) || c.renderScale < 0.999f || c.adaptive;
    }

    bool rendererEnabled() {
        auto& c = g_config;
        if (!c.master) return false;
        if (g_failures >= kFailureBudget) return false;
        if (!g_capability.probed) return true;       // first frame: let frameEnd probe
        if (g_capability.grab == core::GrabMethod::Disabled) return false;
        return captureWorthIt(c);
    }

    core::CullPolicy cullPolicy() {
        core::CullPolicy p;
        p.enabled              = g_config.cullBatches;
        p.allowSpriteBatches   = g_config.cullSprites;
        p.allowParticleBatches = g_config.cullParticles;
        p.marginPx             = g_config.cullMarginPx;
        p.maxCandidateExtent   = g_config.cullMaxExtent;
        return p;
    }

    // ------------------------------------------------------------------ settings

    void reloadFromSettings() {
        auto& c = g_config;

        c.master   = boolean("mod-enabled", false);
        c.preset   = core::presetFromName(text("preset", "Custom"));

        c.renderScale     = core::quantizeScale(numberf("render-scale", 0.5f, 1.0f, 1.0f), 0.5f, 1.0f);
        c.adaptive        = boolean("adaptive", false);
        c.adaptiveMin     = core::clampf(numberf("adaptive-min", 0.35f, 1.0f, 0.55f), 0.35f, c.renderScale);
        c.adaptiveTargetMs = static_cast<double>(numberi("adaptive-target-ms", 5, 200, 17));

        c.fx.bloom     = boolean("bloom", false);
        c.fx.grade     = boolean("grade", false);
        c.fx.fxaa      = boolean("fxaa", false);
        c.fx.vignette  = boolean("vignette", false);
        c.fx.chromatic = boolean("chromatic", false);
        c.fx.sharpen   = boolean("sharpen", false);

        c.bloomThreshold = numberf("bloom-threshold", 0.f, 1.f, 0.70f);
        c.bloomIntensity = numberf("bloom-intensity", 0.f, 3.f, 0.85f);
        c.bloomRadius    = numberf("bloom-radius", 0.5f, 3.f, 1.30f);

        c.exposure   = numberf("exposure", 0.5f, 2.f, 1.00f);
        c.contrast   = numberf("contrast", 0.5f, 2.f, 1.05f);
        c.saturation = numberf("saturation", 0.f, 2.f, 1.10f);
        c.gamma      = numberf("gamma", 0.5f, 2.f, 1.00f);
        c.lift       = numberf("lift", -0.2f, 0.2f, 0.00f);

        c.vignette  = numberf("vignette-strength", 0.f, 1.f, 0.30f);
        c.chromatic = numberf("chromatic-strength", 0.f, 4.f, 0.80f);
        c.sharpen   = numberf("sharpen-strength", 0.f, 1.f, 0.35f);

        c.grabOverride = grabOverrideFromText(text("grab-override", "Auto"));
        c.syncEveryFrame = boolean("sync-every-frame", false);

        c.cullBatches   = boolean("cull-enabled", false);
        c.cullMarginPx  = numberf("cull-margin", 0.f, 512.f, 96.f);
        c.cullMaxExtent = numberf("cull-max-extent", 100.f, 20000.f, 1200.f);
        c.cullSprites   = boolean("cull-sprites", false);
        c.cullParticles = boolean("cull-particles", true);

        c.hudEnabled = boolean("hud-enabled", true);
        std::string const corner = text("hud-corner", "Top right");
        c.hudCorner = corner == "Top left"     ? 0
                    : corner == "Bottom left"  ? 2
                    : corner == "Bottom right" ? 3
                                               : 1;

        // A preset overrides the sliders, never the master switch: turning a preset on should not
        // silently enable or disable the mod.
        if (c.preset != core::Preset::Custom) {
            auto v = core::presetValues(c.preset);
            c.renderScale = core::quantizeScale(v.renderScale, 0.5f, 1.0f);
            c.adaptive    = v.adaptive;
            c.fx          = v.effects;
        }

        g_scale.scale = c.adaptive ? c.renderScale : c.renderScale;
        rebuildStatus();
    }

    void applyPreset(std::string const& name) {
        auto* mod = Mod::get();
        mod->setSettingValue<std::string>("preset", name);
        reloadFromSettings();
        requestGLRebuild();
        log::info("[Calium] preset -> {}", name);
    }

    bool writeSetting(std::string const& key, std::string const& value) {
        auto* mod = Mod::get();
        if (!mod->getSetting(key)) return false;
        mod->setSettingValue<std::string>(key, value);
        return true;
    }

    std::string const& statusLine() { return g_status; }

    void rebuildStatus() {
        auto& c = g_config;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "Calium %s: %s, scale %.2f%s, passes %s%s%s%s%s",
            core::kVersion,
            c.master ? "on" : "off",
            c.renderScale,
            c.adaptive ? " (adaptive)" : "",
            c.fx.bloom ? "bloom " : "",
            c.fx.grade ? "grade " : "",
            c.fx.fxaa ? "aa " : "",
            c.fx.vignette || c.fx.chromatic ? "lens " : "",
            c.cullBatches ? " cull" : "");
        // Keep the message short and readable even with nothing turned on.
        std::string cleaned;
        for (char ch : std::string(buf)) {
            if (ch == ' ' && !cleaned.empty() && cleaned.back() == ' ') continue;
            cleaned += ch;
        }
        g_status = cleaned;
    }

    void onLevelStart() {
        resetFailures();
        g_frames.clear();
        g_session = core::FrameCounters{};
        g_scale = core::ScaleState{};
        g_scale.scale = g_config.renderScale;
    }

}
