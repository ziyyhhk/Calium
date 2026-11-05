#pragma once

// Calium - mod-side declarations shared by every translation unit.
//
// The shape of the whole thing, in one paragraph: GD renders its frame normally into the window's
// framebuffer, and Calium then takes over the *end* of that frame. It copies the finished image out
// of the default framebuffer, runs a chain of its own full-screen shader passes over it (optionally
// at a reduced resolution, which is where the frame time comes back), and blits the result over the
// window before the engine swaps buffers. Nothing about how GD submits its own geometry is changed,
// which is why switching the capture off returns the game to exactly vanilla behaviour. The batch
// hooks only ever observe draws, plus an opt-in culler that is written to fail open.

#include <cstdint>
#include <string>

#include <calium_core.hpp>

namespace calium {

    // ---------------------------------------------------------------- settings

    struct Config {
        // Master switch. Off means Calium does not touch GL at all.
        bool  master = false;

        core::Preset preset = core::Preset::Custom;

        // Internal resolution multiplier. 1.0 = native. Everything between the grab and the final
        // blit runs at this fraction of the window size, so 0.75 means 56% of the pixels.
        float renderScale = 1.0f;
        bool  adaptive    = false;
        float adaptiveMin = 0.55f;
        double adaptiveTargetMs = 16.67;

        // Post chain.
        core::EffectFlags fx;
        float bloomThreshold = 0.70f;   // 0..1
        float bloomIntensity = 0.85f;   // 0..3
        float bloomRadius    = 1.30f;   // 0.5..3 texel multiplier
        float exposure   = 1.00f;
        float contrast   = 1.05f;
        float saturation = 1.10f;
        float gamma      = 1.00f;
        float lift       = 0.00f;       // -0.2..0.2 added before the gamma curve
        float vignette   = 0.30f;       // 0..1 strength
        float chromatic  = 0.8f;        // 0..4 pixels of RGB offset at the edge
        float sharpen    = 0.0f;        // 0..1 unsharp amount, folded into the composite

        // Grab method override. Auto probes and decides; the others force a path so a device that
        // was refused can be pushed (at the user's own risk of a black frame).
        // 0 = auto, 1 = copy-tex, 2 = blit, 3 = read-pixels
        int   grabOverride = 0;

        // Opt-in batch culling. Deliberately conservative; see core::decideBatch.
        bool  cullBatches   = false;
        float cullMarginPx  = 96.f;
        float cullMaxExtent = 1200.f;
        bool  cullSprites   = false;    // level atlases: leave off unless you know why
        bool  cullParticles = true;

        // Diagnostics.
        bool  hudEnabled    = true;
        int   hudCorner     = 1;        // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        bool  syncEveryFrame = false;   // glFinish per frame: accurate GPU timing, costs frames
    };

    // One frame of measurements, as shown in the HUD. Everything here is counted or clocked, never
    // estimated. The two timings are different things and are labelled as such.
    struct Snapshot {
        double frameMs = 0.0;      // wall time between the start of two frames: what you feel
        double cpuMs   = 0.0;      // time spent inside drawScene, including our passes
        double avgMs   = 0.0;
        double p99Ms   = 0.0;
        double fps     = 0.0;
        double gpuMs   = -1.0;     // only valid with syncEveryFrame, else -1
        uint64_t batchesDrawn   = 0;
        uint64_t batchesSkipped = 0;
        uint64_t quadsSubmitted = 0;
        uint64_t quadsScanned   = 0;
        uint64_t passesRun      = 0;
        float  scale     = 1.0f;
        int    width     = 0;
        int    height    = 0;
        int    renderW   = 0;
        int    renderH   = 0;
        bool   active    = false;
        bool   degraded  = false;   // the renderer switched itself off
        std::string why;            // short reason when it is not running
    };

    // ---------------------------------------------------------------- accessors

    Config&  config();
    void     reloadFromSettings();          // mod.json -> Config, clamped through core:: helpers
    void     applyPreset(std::string const& name);
    bool     writeSetting(std::string const& key, std::string const& value);

    core::RollingStats&  frameStats();
    core::FrameCounters& counters();        // accumulated over the session
    core::FrameCounters& frameCounters();   // reset every frame
    core::ScaleState&    scaleState();
    core::CullPolicy     cullPolicy();      // Config + live game facts
    Snapshot&            snapshot();
    Snapshot const&      constSnapshot();

    // ---------------------------------------------------------------- capability / safety

    struct Capability {
        core::GLFacts    facts;
        core::GrabMethod grab  = core::GrabMethod::Disabled;
        bool             probed = false;
        std::string      glVersion;
        std::string      note;
        bool             gles = false;
    };
    Capability& capability();
    void probeCapabilities();               // call on the GL thread, once

    // Calium stops hurting itself: after this many GL failures in a session the capture path turns
    // off for good until the level is reloaded, and the reason is shown instead of a black screen.
    inline constexpr int kFailureBudget = 12;
    void noteFailure(std::string what);
    int  failureCount();
    std::string const& lastFailure();
    void resetFailures();

    // Is the capture worth running at all? False when off, when unsafe, or when it would buy nothing
    // (native scale with no effects: three full-screen passes for a pixel-for-pixel copy).
    bool rendererEnabled();
    bool captureWorthIt(Config const& c);

    // Frame timing, filled in by the hook, read by the controller and the HUD.
    void    recordFrameTimes(double frameMs, double cpuMs, double gpuMs);
    double  lastFrameMs();
    double  lastCpuMs();

    // ---------------------------------------------------------------- lifecycle (main.cpp)

    bool initMod();
    void shutdownMod();                      // frees every GL object; must be idempotent
    void onLevelStart();                     // clears per-level state and the failure budget

    std::string const& statusLine();         // one-line summary for the log / popup
    void rebuildStatus();                    // recompose statusLine() from the current Config

    // ---------------------------------------------------------------- frame driver (glcore.cpp)

    void frameEnd(int displayW, int displayH);   // the whole chain, called after GD's draw
    unsigned framePassCount();                   // passes actually run last frame
    int  frameRenderWidth();
    int  frameRenderHeight();
    bool glResourcesReady();
    void destroyGLResources();                   // must be called on the GL thread
    void requestGLRebuild();                     // settings changed size/format
}
