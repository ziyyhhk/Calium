#pragma once

// Calium - pure logic core.
//
// This file deliberately includes NO Geode, NO cocos2d and NO OpenGL. Everything that decides
// *what* the renderer does lives here so it can be compiled and unit tested on a plain host
// compiler in CI, without the Geode SDK. The .cpp files under src/ are thin adapters: they read
// real game/GL state, call into this namespace, and act on the answer.
//
// Rule for anyone editing: if it needs a cocos or GL type, it does not belong in this header.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace calium::core {

    // ---------------------------------------------------------------- constants

    inline constexpr char const* kModId   = "calium.renderer";
    inline constexpr char const* kName    = "Calium";
    inline constexpr char const* kVersion = "0.1.0";

    // Render scale is quantized so the framebuffer size does not change every frame, which would
    // mean a glTexImage2D reallocation per frame. 1/64 keeps the steps visually smooth.
    inline constexpr float kScaleQuantum = 1.0f / 64.0f;

    // Frame times below this are treated as broken measurements (timer resolution, or a frame that
    // was skipped while the game paused) and must not drive the adaptive controller.
    inline constexpr double kMinValidFrameMs = 0.05;

    // ---------------------------------------------------------------- small math

    struct Vec2 {
        float x = 0.f;
        float y = 0.f;
    };

    // Axis aligned box stored as two corners. min <= max when the box is not flipped.
    struct Bounds {
        float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;

        float width()  const { return maxX - minX; }
        float height() const { return maxY - minY; }

        bool isFinite() const {
            return std::isfinite(minX) && std::isfinite(minY)
                && std::isfinite(maxX) && std::isfinite(maxY);
        }

        // A point-sized box is still a legitimate thing to draw, so this only rejects nonsense
        // (inverted / NaN). Callers must not use it to mean "empty".
        bool isSane() const {
            return isFinite() && maxX >= minX && maxY >= minY;
        }

        float maxExtent() const { return std::max(width(), height()); }

        Bounds inflated(float m) const {
            return { minX - m, minY - m, maxX + m, maxY + m };
        }
    };

    inline float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    inline double clampd(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    inline bool rectsOverlap(Bounds const& a, Bounds const& b) {
        return a.maxX >= b.minX && b.maxX >= a.minX
            && a.maxY >= b.minY && b.maxY >= a.minY;
    }

    // Snap a scale to the quantum and into [lo, hi]. NaN/infinity collapse to lo.
    inline float quantizeScale(float s, float lo, float hi) {
        if (!std::isfinite(s)) return lo;
        float q = std::round(s / kScaleQuantum) * kScaleQuantum;
        return clampf(q, lo, hi);
    }

    // ---------------------------------------------------------------- quad bounds

    // The four values a batch really submits per quad, reduced to positions only. The hook layer
    // fills these straight out of the texture atlas, so what we test is exactly what the GPU would
    // have been handed - not an approximation of it from node metadata.
    struct QuadPos {
        float tlX = 0.f, tlY = 0.f;
        float brX = 0.f, brY = 0.f;
    };

    struct BoundsResult {
        Bounds bounds;
        bool   complete = false;   // false = the scan was cut short, so the bounds may be too small
        unsigned scanned = 0;
        unsigned skipped = 0;      // quads not visited because of the cost guard
    };

    struct ScanLimit {
        unsigned maxQuads = 4096;  // hard cost ceiling per batch per frame
        unsigned stride   = 1;     // >1 samples the atlas; forces complete=false
    };

    // Bounding box of a batch's quads. Note this is in the batch node's own space; the caller
    // transforms it before comparing with the view.
    inline BoundsResult boundsFromQuads(QuadPos const* quads, unsigned count, ScanLimit const& lim) {
        BoundsResult out;
        if (quads == nullptr || count == 0) {
            out.complete = false;      // nothing to trust: treat as "unknown", never cull
            return out;
        }

        float first = quads[0].tlX;
        out.bounds = { first, quads[0].tlY, first, quads[0].tlY };

        unsigned const step = lim.stride == 0 ? 1u : lim.stride;
        unsigned const ceiling = lim.maxQuads == 0 ? count : std::min(count, lim.maxQuads);

        bool any = false;
        unsigned i = 0;
        for (; i < ceiling; i += step) {
            QuadPos const& q = quads[i];
            if (!std::isfinite(q.tlX) || !std::isfinite(q.tlY)
             || !std::isfinite(q.brX) || !std::isfinite(q.brY)) {
                // One corrupt quad invalidates the whole batch's bounds. Bail out honestly.
                out.complete = false;
                out.scanned  = i;
                out.skipped  = count - i;
                return out;
            }
            out.bounds.minX = std::min({ out.bounds.minX, q.tlX, q.brX });
            out.bounds.minY = std::min({ out.bounds.minY, q.tlY, q.brY });
            out.bounds.maxX = std::max({ out.bounds.maxX, q.tlX, q.brX });
            out.bounds.maxY = std::max({ out.bounds.maxY, q.tlY, q.brY });
            any = true;
        }

        unsigned const visitedEnd = std::min(ceiling, count);
        out.scanned = any ? std::min(i, visitedEnd) : 0u;
        out.skipped = count > out.scanned ? count - out.scanned : 0u;
        out.complete = (visitedEnd >= count) && (step == 1u);
        return out;
    }

    // ---------------------------------------------------------------- culling policy

    enum class Verdict {
        Draw,   // submit the batch (the safe answer)
        Skip,   // provably nothing visible would be lost
    };

    struct CullPolicy {
        bool  enabled            = false;
        bool  allowSpriteBatches = true;
        bool  allowParticleBatches = true;

        // Extra world units kept around the visible rect. Objects half off screen must still draw.
        float marginPx     = 64.f;

        // GD packs whole level atlases into one batch whose bounds span the entire level, so those
        // batches are enormous and can never be provably off screen. Anything wider than this is
        // not even considered - it stops a mis-measured level batch from blanking the screen.
        float maxCandidateExtent = 1200.f;

        // The test below is a 2D orthographic one. With a perspective camera (GD's 3D levels) the
        // view rect no longer describes what is visible, so the policy refuses to cull at all.
        bool  perspective  = false;
        bool  rotation     = false;   // rotated camera: same reason, refuse

        ScanLimit scan;
    };

    struct CullQuery {
        BoundsResult batchBounds;         // already in world space by the caller
        Bounds       view;                // visible rect in the same space
        bool         isParticleBatch = false;
    };

    // The whole decision, separate from the GL call, so it can be tested. Every guard returns Draw:
    // being wrong costs frames, never pixels.
    inline Verdict decideBatch(CullQuery const& q, CullPolicy const& p) {
        if (!p.enabled)                       return Verdict::Draw;
        if (p.perspective || p.rotation)      return Verdict::Draw;
        if (q.isParticleBatch && !p.allowParticleBatches) return Verdict::Draw;
        if (!q.isParticleBatch && !p.allowSpriteBatches)  return Verdict::Draw;

        // Unknown or corrupt bounds: we cannot prove anything, so draw it.
        if (!q.batchBounds.complete)  return Verdict::Draw;
        if (!q.batchBounds.bounds.isSane()) return Verdict::Draw;
        if (!q.view.isSane())               return Verdict::Draw;

        // A batch that spans more than a screen-ish area is level geometry, not decoration.
        if (q.batchBounds.bounds.maxExtent() > p.maxCandidateExtent) return Verdict::Draw;

        Bounds const inflated = q.batchBounds.bounds.inflated(p.marginPx);
        if (!rectsOverlap(inflated, q.view)) return Verdict::Skip;
        return Verdict::Draw;
    }

    // ---------------------------------------------------------------- frame grab method

    // How to get the finished frame into a texture. This is the single most device-dependent
    // choice in the mod, so it is decided by pure logic over probed GL facts.
    enum class GrabMethod {
        Disabled,   // post-processing off / no safe method found
        CopyTex,    // glCopyTexSubImage2D - works on desktop GL and on non-multisampled ES2
        Blit,       // glBlitFramebuffer - needs GL3 / GLES3, resolves MSAA properly
        ReadPixels, // last resort, slow (CPU round trip)
    };

    struct GLFacts {
        bool  gles          = false;  // OpenGL ES context at all
        int   glMajor       = 2;      // parsed from GL_VERSION
        int   glMinor       = 0;
        unsigned backbufferSamples = 0;   // GL_SAMPLES on the default framebuffer
        bool  hasBlit       = false;  // glBlitFramebuffer resolved and usable
        bool  allowSlowRead = false;  // user opted into the ReadPixels path
    };

    inline bool versionAtLeast(GLFacts const& f, int major, int minor) {
        return f.glMajor > major || (f.glMajor == major && f.glMinor >= minor);
    }

    // Rationale for each branch, in short:
    //  - Multisampled default framebuffer + CopyTex is undefined (GL_INVALID_OPERATION on ES2), so
    //    MSAA must go through Blit, and only exists on GL3/GLES3.
    //  - On GLES2 with no MSAA, CopyTex is the normal path.
    //  - ReadPixels is offered only if the user asks, because it stalls the pipeline.
    inline GrabMethod decideGrabMethod(GLFacts const& f) {
        bool const msaa = f.backbufferSamples > 1;
        if (msaa) {
            if (f.hasBlit && versionAtLeast(f, 3, 0)) return GrabMethod::Blit;
            return GrabMethod::Disabled;
        }
        if (f.gles) {
            if (versionAtLeast(f, 3, 0) && f.hasBlit) return GrabMethod::Blit;
            return GrabMethod::CopyTex;
        }
        if (f.hasBlit && versionAtLeast(f, 3, 0)) return GrabMethod::Blit;
        return GrabMethod::CopyTex;
    }

    inline char const* grabMethodName(GrabMethod m) {
        switch (m) {
            case GrabMethod::Disabled:   return "disabled";
            case GrabMethod::CopyTex:    return "copy-tex";
            case GrabMethod::Blit:       return "blit";
            case GrabMethod::ReadPixels: return "read-pixels";
        }
        return "unknown";
    }

    // ---------------------------------------------------------------- pass chain

    enum class Pass : unsigned {
        Grab = 0,       // frame into the scene texture
        Bright,         // threshold -> small texture
        BlurH,          // horizontal separable blur pass 1
        BlurV,          // vertical separable blur pass 1
        BlurH2,         // second octave, wider radius, quarter resolution
        BlurV2,
        Composite,      // scene + bloom + grade, at render scale
        Fullscreen,     // final ops (FXAA / vignette / CA) at display scale
        COUNT
    };

    inline char const* passName(Pass p) {
        switch (p) {
            case Pass::Grab:       return "grab";
            case Pass::Bright:     return "bright";
            case Pass::BlurH:      return "blur-h";
            case Pass::BlurV:      return "blur-v";
            case Pass::BlurH2:     return "blur-h2";
            case Pass::BlurV2:     return "blur-v2";
            case Pass::Composite:  return "composite";
            case Pass::Fullscreen: return "present";
            default:               return "?";
        }
    }

    struct EffectFlags {
        bool bloom      = false;
        bool grade      = false;   // brightness / contrast / saturation / gamma
        bool fxaa       = false;
        bool vignette   = false;
        bool chromatic  = false;
        bool sharpen    = false;
    };

    struct Chain {
        std::vector<Pass> passes;
        bool  needsHalfRes  = false;
        bool  needsQuarter  = false;
        bool  needsFullscreenTarget = false;
    };

    // Bloom at quarter/ eighth resolution is the cheap, good-looking option; the wide octave is what
    // makes neon glow read as glow instead of as a white smear.
    inline Chain planChain(EffectFlags const& e, bool scaled) {
        Chain c;
        c.passes.push_back(Pass::Grab);

        if (e.bloom) {
            c.passes.push_back(Pass::Bright);
            c.passes.push_back(Pass::BlurH);
            c.passes.push_back(Pass::BlurV);
            c.needsHalfRes = true;
        }
        // Composite folds grade and sharpen in, since it already samples the scene.
        c.passes.push_back(Pass::Composite);

        // FXAA must run on the final pixels at display resolution, so anything that happens after
        // the scaled composite needs a full size target.
        bool const lateOps = e.fxaa || e.vignette || e.chromatic;
        if (lateOps) {
            c.passes.push_back(Pass::Fullscreen);
            c.needsFullscreenTarget = scaled || e.bloom;
        }
        return c;
    }

    // The renderer uses this to decide whether the capture path is worth switching on at all: a
    // grab plus a plain copy costs GPU time and buys nothing visible.
    inline bool anyEffect(EffectFlags const& e) {
        return e.bloom || e.grade || e.fxaa || e.vignette || e.chromatic || e.sharpen;
    }

    // ---------------------------------------------------------------- adaptive resolution

    struct ScaleConfig {
        float min        = 0.50f;
        float max        = 1.00f;
        double targetMs  = 16.67;   // ~60 fps of GPU budget for the frame
        double upBandMs  = 2.5;     // must beat the target by this to raise quality
        double downBandMs = 2.0;    // must miss the target by this to drop quality
        unsigned framesToAct = 24;  // sustained, so one hitch cannot move the scale
        float step       = 0.125f;
    };

    struct ScaleState {
        float  scale = 1.0f;
        unsigned streak = 0;
        int      streakDir = 0;   // +1 wanting to go up, -1 wanting to go down
    };

    // Returns true when the scale changed. Deliberately asymmetric: it is allowed to react fast to a
    // dropped frame and slow to a good one, so the scale does not oscillate while a level loads.
    inline bool scaleStep(ScaleState& st, double frameMs, ScaleConfig const& cfg) {
        if (frameMs < kMinValidFrameMs || !std::isfinite(frameMs)) {
            st.streak = 0;
            st.streakDir = 0;
            return false;
        }

        int dir = 0;
        if (frameMs > cfg.targetMs + cfg.downBandMs)      dir = -1;  // too slow: fewer pixels
        else if (frameMs < cfg.targetMs - cfg.upBandMs)   dir = +1;  // plenty of headroom
        else                                              dir = 0;   // in the dead band, leave it

        if (dir == 0) { st.streak = 0; st.streakDir = 0; return false; }
        if (dir != st.streakDir) { st.streakDir = dir; st.streak = 1; }
        else { st.streak += 1; }

        if (st.streak < cfg.framesToAct) return false;
        st.streak = 0;

        float const target = st.scale + dir * cfg.step;
        float const next = quantizeScale(target, cfg.min, cfg.max);
        if (std::abs(next - st.scale) < 1e-4f) return false;
        st.scale = next;
        return true;
    }

    // ---------------------------------------------------------------- stats

    struct FrameCounters {
        uint64_t batchesDrawn   = 0;
        uint64_t batchesSkipped = 0;
        uint64_t quadsSubmitted = 0;
        uint64_t quadsScanned   = 0;
        uint64_t passesRun      = 0;
    };

    // Fixed size ring of frame times. Percentiles are computed on demand from a scratch copy; with
    // a couple of hundred entries that is nothing next to a frame.
    class RollingStats {
    public:
        explicit RollingStats(std::size_t cap = 240)
            : m_cap(cap == 0 ? 1 : cap), m_data(cap == 0 ? 1 : cap) {}

        void push(double ms) {
            if (!std::isfinite(ms)) return;
            // The ring is full, so the value in this slot is being evicted: take it back out of the
            // running sum or avg() would drift upward forever.
            if (m_count == m_cap) m_sum -= m_data[m_next];
            m_data[m_next] = ms;
            m_next = (m_next + 1) % m_cap;
            if (m_count < m_cap) m_count += 1;
            m_sum += ms;
        }

        std::size_t count() const { return m_count; }

        double avg() const {
            if (m_count == 0) return 0.0;
            return m_sum / static_cast<double>(m_count);
        }

        double latest() const {
            if (m_count == 0) return 0.0;
            std::size_t idx = (m_next + m_cap - 1) % m_cap;
            return m_data[idx];
        }

        double minElement() const {
            double best = std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < m_count; ++i) best = std::min(best, m_data[i]);
            return std::isfinite(best) ? best : 0.0;
        }

        // p in [0,1]; 0.5 is the median, 0.99 the worst of a hundred.
        double percentile(double p) const {
            if (m_count == 0) return 0.0;
            std::vector<double> tmp(m_data.begin(), m_data.begin() + m_count);
            std::sort(tmp.begin(), tmp.end());
            double clamped = clampd(p, 0.0, 1.0);
            std::size_t idx = static_cast<std::size_t>(clamped * static_cast<double>(tmp.size() - 1));
            return tmp[idx];
        }

        void clear() {
            m_count = 0;
            m_next = 0;
            m_sum = 0.0;
        }

    private:
        std::size_t m_cap;
        std::size_t m_next = 0;
        std::size_t m_count = 0;
        double m_sum = 0.0;
        std::vector<double> m_data;
    };

    // Frames per second from a frame time, with a floor so a zero does not print as "inf".
    inline double fpsFromMs(double ms) {
        if (!std::isfinite(ms) || ms < kMinValidFrameMs) return 0.0;
        return 1000.0 / ms;
    }

    // ---------------------------------------------------------------- presets

    enum class Preset { Custom = 0, Performance, Balanced, Quality, Cinematic };

    struct PresetValues {
        bool  captureEnabled;
        float renderScale;
        bool  adaptive;
        EffectFlags effects;
    };

    inline Preset presetFromName(std::string const& s) {
        if (s == "Performance") return Preset::Performance;
        if (s == "Quality")     return Preset::Quality;
        if (s == "Cinematic")   return Preset::Cinematic;
        if (s == "Balanced")    return Preset::Balanced;
        return Preset::Custom;
    }

    inline char const* presetName(Preset p) {
        switch (p) {
            case Preset::Performance: return "Performance";
            case Preset::Balanced:    return "Balanced";
            case Preset::Quality:     return "Quality";
            case Preset::Cinematic:   return "Cinematic";
            default:                  return "Custom";
        }
    }

    // Performance: fewer pixels, nothing extra. The point is frames on a weak GPU or a phone.
    // Balanced: same resolution, cheap AA. Quality: adds grade + bloom. Cinematic: full chain at
    // reduced scale so the cost lands in the same budget as Quality.
    inline PresetValues presetValues(Preset p) {
        PresetValues v;
        switch (p) {
            case Preset::Performance:
                v.captureEnabled = true;
                v.renderScale = 0.75f;
                v.adaptive = false;
                v.effects = {};
                break;
            case Preset::Balanced:
                v.captureEnabled = true;
                v.renderScale = 1.0f;
                v.adaptive = false;
                v.effects.fxaa = true;
                break;
            case Preset::Quality:
                v.captureEnabled = true;
                v.renderScale = 1.0f;
                v.adaptive = false;
                v.effects.fxaa = true;
                v.effects.grade = true;
                v.effects.bloom = true;
                break;
            case Preset::Cinematic:
                v.captureEnabled = true;
                v.renderScale = 0.875f;
                v.adaptive = true;
                v.effects.fxaa = true;
                v.effects.grade = true;
                v.effects.bloom = true;
                v.effects.vignette = true;
                v.effects.chromatic = true;
                break;
            default:
                v.captureEnabled = false;
                v.renderScale = 1.0f;
                v.adaptive = false;
                v.effects = {};
                break;
        }
        return v;
    }

}
