// Host tests for the Calium logic core. No Geode, no cocos, no GL - just the decisions, checked.
//
// The point of these is the two things that must never happen: pixels disappearing that should not
// have, and the resolution controller thrashing. Everything else in the mod is plumbing.

#include "calium_core.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace calium::core;

namespace {

    int g_checks = 0;
    int g_failed = 0;

    void check(bool ok, std::string const& what) {
        ++g_checks;
        if (!ok) {
            ++g_failed;
            std::printf("  FAIL: %s\n", what.c_str());
        }
    }

    void checkEq(double got, double want, std::string const& what, double tol = 1e-4) {
        ++g_checks;
        if (!std::isfinite(got) || std::abs(got - want) > tol) {
            ++g_failed;
            std::printf("  FAIL: %s (got %.6f, want %.6f)\n", what.c_str(), got, want);
        }
    }

    Bounds box(float x0, float y0, float x1, float y1) { return Bounds{x0, y0, x1, y1}; }

    // The window the tests use: 800x450 world units wide, centred-ish.
    Bounds const kView = { 0.f, 0.f, 800.f, 450.f };

    // ---------------------------------------------------------------- bounds scanning

    void testBoundsFromQuads() {
        std::printf("boundsFromQuads\n");

        // A batch of two quads, spanning (10,10) to (110,60).
        std::vector<QuadPos> quads = {
            { 10.f, 10.f, 60.f, 35.f },
            { 50.f, 30.f, 110.f, 60.f },
        };
        ScanLimit lim;
        BoundsResult r = boundsFromQuads(quads.data(), static_cast<unsigned>(quads.size()), lim);
        check(r.complete, "two contiguous quads are a complete scan");
        checkEq(r.bounds.minX, 10.f, "minX");
        checkEq(r.bounds.minY, 10.f, "minY");
        checkEq(r.bounds.maxX, 110.f, "maxX");
        checkEq(r.bounds.maxY, 60.f, "maxY");
        checkEq(static_cast<double>(r.skipped), 0.0, "nothing skipped");

        // Empty / null input must be reported as unknown, never as "empty so cull it".
        BoundsResult nullRes = boundsFromQuads(nullptr, 0, lim);
        check(!nullRes.complete, "null quad pointer is incomplete");
        BoundsResult zeroRes = boundsFromQuads(quads.data(), 0, lim);
        check(!zeroRes.complete, "zero quad count is incomplete");

        // One corrupt vertex invalidates the whole measurement.
        std::vector<QuadPos> broken = {
            { 10.f, 10.f, 20.f, 20.f },
            { std::nanf(""), 5.f, 6.f, 6.f },
            { 30.f, 30.f, 40.f, 40.f },
        };
        BoundsResult br = boundsFromQuads(broken.data(), 3, lim);
        check(!br.complete, "NaN in the atlas aborts the scan as incomplete");

        // Cost guard: only the head of the atlas was looked at, so the bounds may be too small.
        std::vector<QuadPos> many(500, QuadPos{0.f, 0.f, 1.f, 1.f});
        ScanLimit small;
        small.maxQuads = 100;
        BoundsResult mr = boundsFromQuads(many.data(), 500, small);
        check(!mr.complete, "truncated scan is incomplete");
        checkEq(static_cast<double>(mr.skipped), 400.0, "truncated scan reports the tail");

        // Strided sampling is by definition not a complete measurement.
        ScanLimit strided;
        strided.stride = 4;
        BoundsResult sr = boundsFromQuads(many.data(), 500, strided);
        check(!sr.complete, "strided scan is incomplete");
        checkEq(static_cast<double>(sr.scanned), 125.0, "strided scan visits a quarter");

        // Regression: stride past the end must not underflow the skipped count.
        std::vector<QuadPos> five(5, QuadPos{0.f, 0.f, 1.f, 1.f});
        ScanLimit s2;
        s2.stride = 2;
        BoundsResult r2 = boundsFromQuads(five.data(), 5, s2);
        check(r2.skipped < 1000, "skipped count does not wrap around for a short batch");
        check(!r2.complete, "stride 2 is not complete");
    }

    // ---------------------------------------------------------------- culling policy

    void testDecideBatch() {
        std::printf("decideBatch\n");

        CullPolicy p;
        p.enabled = true;

        auto query = [&](Bounds b, bool complete = true, bool particle = false) {
            CullQuery q;
            q.batchBounds.bounds   = b;
            q.batchBounds.complete = complete;
            q.isParticleBatch      = particle;
            q.view                 = kView;
            return q;
        };

        check(decideBatch(query(box(1000.f, 1000.f, 1100.f, 1100.f)), p) == Verdict::Skip,
              "a small batch well outside the view is skipped");

        check(decideBatch(query(box(700.f, 380.f, 790.f, 440.f)), p) == Verdict::Draw,
              "a batch inside the view draws");

        // The margin is what keeps half-visible objects on screen.
        CullPolicy wide = p;
        wide.marginPx = 100.f;
        check(decideBatch(query(box(850.f, 0.f, 900.f, 50.f)), wide) == Verdict::Draw,
              "a batch 50 units off screen is kept by a 100 unit margin");
        check(decideBatch(query(box(950.f, 0.f, 1000.f, 50.f)), wide) == Verdict::Skip,
              "a batch 150 units off screen is past the 100 unit margin");

        // Every guard must fail open (draw).
        CullPolicy off = p;
        off.enabled = false;
        check(decideBatch(query(box(10000.f, 10000.f, 10100.f, 10100.f)), off) == Verdict::Draw,
              "culling disabled draws everything");

        CullPolicy persp = p;
        persp.perspective = true;
        check(decideBatch(query(box(10000.f, 10000.f, 10100.f, 10100.f)), persp) == Verdict::Draw,
              "perspective (3D level) refuses to cull");

        CullPolicy rot = p;
        rot.rotation = true;
        check(decideBatch(query(box(10000.f, 10000.f, 10100.f, 10100.f)), rot) == Verdict::Draw,
              "rotated camera refuses to cull");

        check(decideBatch(query(box(1000.f, 1000.f, 1100.f, 1100.f), false), p) == Verdict::Draw,
              "incomplete bounds are never culled");

        check(decideBatch(query(box(1000.f, 1000.f, 900.f, 1100.f)), p) == Verdict::Draw,
              "flipped (insane) bounds are never culled");

        std::vector<QuadPos> nanq = { { std::nanf(""), 0.f, 1.f, 1.f } };
        CullQuery nanQuery;
        nanQuery.batchBounds = boundsFromQuads(nanq.data(), 1, ScanLimit{});
        nanQuery.view = kView;
        check(decideBatch(nanQuery, p) == Verdict::Draw, "NaN bounds are never culled");

        // The whole reason level geometry cannot be blanked by this: a level atlas spans far more
        // than maxCandidateExtent, so it is not even considered, off view or not.
        CullPolicy extent = p;
        extent.maxCandidateExtent = 1200.f;
        check(decideBatch(query(box(-50000.f, -2000.f, 60000.f, 4000.f)), extent) == Verdict::Draw,
              "a level-wide atlas is never a cull candidate");

        CullPolicy noSprites = p;
        noSprites.allowSpriteBatches = false;
        check(decideBatch(query(box(1000.f, 1000.f, 1100.f, 1100.f)), noSprites) == Verdict::Draw,
              "sprite batches respected when disabled");
        check(decideBatch(query(box(1000.f, 1000.f, 1100.f, 1100.f), true, true), noSprites) == Verdict::Skip,
              "particle batches still cull when sprites are off");

        // A degenerate point inside the view must draw, not vanish.
        check(decideBatch(query(box(400.f, 200.f, 400.f, 200.f)), p) == Verdict::Draw,
              "zero-size batch inside the view draws");
    }

    // ---------------------------------------------------------------- frame grab method

    void testGrabMethod() {
        std::printf("decideGrabMethod\n");

        GLFacts desktop21;                 // GD on Windows/older drivers
        desktop21.gles = false;
        desktop21.glMajor = 2;
        desktop21.glMinor = 1;
        check(decideGrabMethod(desktop21) == GrabMethod::CopyTex, "desktop GL 2.1 uses copy-tex");

        GLFacts gles2 = desktop21;         // Android / iOS
        gles2.gles = true;
        gles2.glMajor = 2;
        check(decideGrabMethod(gles2) == GrabMethod::CopyTex, "GLES2 without MSAA uses copy-tex");

        GLFacts gles2msaa = gles2;
        gles2msaa.backbufferSamples = 4;
        check(decideGrabMethod(gles2msaa) == GrabMethod::Disabled,
              "GLES2 with an MSAA backbuffer is refused rather than guessed at");

        GLFacts gl3 = gles2msaa;
        gl3.glMajor = 3;
        gl3.hasBlit = true;
        check(decideGrabMethod(gl3) == GrabMethod::Blit, "MSAA with blit available resolves via blit");

        GLFacts gl3noBlit = gl3;
        gl3noBlit.hasBlit = false;
        check(decideGrabMethod(gl3noBlit) == GrabMethod::Disabled,
              "MSAA with no blit entry point is disabled, not silently wrong");

        GLFacts gl4 = desktop21;
        gl4.glMajor = 4;
        gl4.glMinor = 5;
        gl4.hasBlit = true;
        check(decideGrabMethod(gl4) == GrabMethod::Blit, "GL 4.5 prefers blit");

        check(std::string(grabMethodName(GrabMethod::Blit)) == "blit", "grab method names for the log");
    }

    // ---------------------------------------------------------------- pass chain

    void testPlanChain() {
        std::printf("planChain\n");

        EffectFlags none;
        Chain plain = planChain(none, false);
        check(plain.passes.size() == 2, "no effects: grab + composite only");
        check(plain.passes.front() == Pass::Grab, "chain always starts with the grab");
        check(!plain.needsFullscreenTarget, "plain scaled chain needs no full size target");
        check(!anyEffect(none), "empty flags report no effects");

        EffectFlags bloom;
        bloom.bloom = true;
        Chain b = planChain(bloom, false);
        check(b.needsHalfRes, "bloom needs the half size target");
        check(b.passes.size() == 4, "bloom chain is grab, bright, blur h, blur v, composite");
        check(b.passes[1] == Pass::Bright, "bright extract runs before the blurs");
        check(b.passes[2] == Pass::BlurH && b.passes[3] == Pass::BlurV,
              "the two blur directions stay in h then v order");
        check(b.passes.back() == Pass::Composite, "bloom chain composites last when nothing follows");

        EffectFlags late;
        late.fxaa = true;
        Chain l = planChain(late, true);
        check(l.needsFullscreenTarget, "FXAA after a scaled composite needs a full size target");
        check(l.passes.back() == Pass::Fullscreen, "FXAA runs as the final pass");
        check(l.passes.size() == 3, "grab, composite, fullscreen");

        EffectFlags all;
        all.bloom = all.grade = all.fxaa = all.vignette = all.chromatic = true;
        Chain a = planChain(all, true);
        check(a.passes.front() == Pass::Grab && a.passes.back() == Pass::Fullscreen,
              "full chain is well ordered at both ends");
        check(a.passes.size() == 5, "full chain pass count");
    }

    // ---------------------------------------------------------------- adaptive resolution

    void testScaleStep() {
        std::printf("scaleStep\n");

        ScaleConfig cfg;
        cfg.min = 0.5f;
        cfg.max = 1.0f;
        cfg.targetMs = 16.67;
        cfg.framesToAct = 24;
        cfg.step = 0.125f;

        ScaleState st;
        st.scale = 1.0f;

        // Inside the dead band: the controller must not move at all.
        for (int i = 0; i < 200; ++i) check(!scaleStep(st, 16.0, cfg), "dead band holds the scale");
        checkEq(st.scale, 1.0, "scale unchanged inside the dead band");

        // Too slow: it takes framesToAct consecutive bad frames, then drops exactly one step.
        for (unsigned i = 0; i < cfg.framesToAct - 1; ++i)
            check(!scaleStep(st, 30.0, cfg), "no move before the streak completes");
        check(scaleStep(st, 30.0, cfg), "moves on the frame that completes the streak");
        checkEq(st.scale, 0.875f, "one step down");

        // A single good frame in the middle resets the streak, so a hitch cannot stack up.
        for (unsigned i = 0; i < cfg.framesToAct - 1; ++i) scaleStep(st, 30.0, cfg);
        scaleStep(st, 16.0, cfg);   // dead band resets
        float before = st.scale;
        check(!scaleStep(st, 30.0, cfg), "streak restarts after a reset frame");
        checkEq(st.scale, before, "scale unchanged by a restarted streak");

        // Junk measurements must be ignored entirely (pause, alt-tab, timer resolution).
        before = st.scale;
        for (int i = 0; i < 400; ++i) {
            scaleStep(st, 0.0, cfg);
            scaleStep(st, -5.0, cfg);
            scaleStep(st, std::nanf(""), cfg);
        }
        checkEq(st.scale, before, "invalid frame times never move the scale");

        // Fast frames raise quality again, and it stops at max.
        ScaleState low;
        low.scale = 0.5f;
        int moves = 0;
        for (int i = 0; i < 4000; ++i) if (scaleStep(low, 4.0, cfg)) ++moves;
        checkEq(low.scale, 1.0, "climbs back to max");
        check(moves == 4, "climbs in four 1/8 steps from 0.5 to 1.0");

        // Never escapes its configured range, even with absurd inputs.
        ScaleState edge;
        edge.scale = 0.5f;
        for (int i = 0; i < 100000; ++i) scaleStep(edge, 9000.0, cfg);
        check(edge.scale >= cfg.min - 1e-6f, "cannot fall below min");
        checkEq(edge.scale, 0.5f, "sits at min and stays there");

        checkEq(quantizeScale(0.9f, 0.5f, 1.0f), 0.875f, "0.9 snaps down to the 1/8 grid");
        checkEq(quantizeScale(2.5f, 0.5f, 1.0f), 1.0f, "out of range clamps to max");
        checkEq(quantizeScale(std::nanf(""), 0.5f, 1.0f), 0.5f, "NaN collapses to min");
    }

    // ---------------------------------------------------------------- stats

    void testRollingStats() {
        std::printf("RollingStats\n");

        RollingStats rs(4);
        checkEq(rs.avg(), 0.0, "empty average is zero, not NaN");
        checkEq(rs.count(), 0.0, "empty count");
        checkEq(rs.percentile(0.5), 0.0, "empty percentile is zero");

        rs.push(10.0);
        rs.push(20.0);
        rs.push(30.0);
        rs.push(40.0);
        checkEq(rs.avg(), 25.0, "average over a full ring");
        checkEq(rs.minElement(), 10.0, "min over a full ring");
        checkEq(rs.latest(), 40.0, "latest is the newest value");
        checkEq(rs.percentile(0.0), 10.0, "p0 is the min");
        checkEq(rs.percentile(1.0), 40.0, "p100 is the max");
        checkEq(rs.percentile(0.5), 20.0, "median of 10/20/30/40 is the lower middle");

        // Wrap once: the evicted 10 must leave the sum, or avg drifts forever.
        rs.push(50.0);
        checkEq(rs.count(), 4.0, "count stays at the capacity");
        checkEq(rs.avg(), 35.0, "average correct after eviction");
        checkEq(rs.minElement(), 20.0, "min follows the eviction");
        checkEq(rs.latest(), 50.0, "latest after wrap");

        for (int i = 0; i < 1000; ++i) rs.push(50.0);
        checkEq(rs.avg(), 50.0, "average does not drift after 1000 pushes", 1e-3);

        rs.push(std::nanf(""));
        rs.push(std::numeric_limits<double>::infinity());
        checkEq(rs.avg(), 50.0, "NaN and infinity are refused");
        checkEq(rs.count(), 4.0, "refused values do not change the count");

        rs.clear();
        checkEq(rs.count(), 0.0, "cleared");
        checkEq(rs.avg(), 0.0, "cleared average");

        checkEq(fpsFromMs(0.0), 0.0, "zero ms does not report infinite fps");
        checkEq(fpsFromMs(20.0), 50.0, "20 ms is 50 fps");

        RollingStats one(0);
        one.push(5.0);
        checkEq(one.avg(), 5.0, "a zero capacity ring still holds one sample");
    }

    // ---------------------------------------------------------------- presets

    void testPresets() {
        std::printf("presets\n");

        check(presetFromName("Balanced") == Preset::Balanced, "name parses");
        check(presetFromName("nonsense") == Preset::Custom, "unknown name falls back to Custom");
        check(std::string(presetName(Preset::Cinematic)) == "Cinematic", "name round trips");

        PresetValues perf = presetValues(Preset::Performance);
        check(perf.captureEnabled, "performance preset captures");
        check(perf.renderScale < 1.0f, "performance preset renders below native");
        check(!anyEffect(perf.effects), "performance preset adds no effects");

        PresetValues custom = presetValues(Preset::Custom);
        check(!custom.captureEnabled, "Custom leaves the capture off, so settings win");

        PresetValues cine = presetValues(Preset::Cinematic);
        check(cine.effects.bloom && cine.effects.fxaa && cine.effects.vignette,
              "cinematic turns the full chain on");
        check(cine.adaptive, "cinematic uses the adaptive controller");

        // No preset may ask for a scale the clamp would reject.
        for (int i = 0; i <= 4; ++i) {
            PresetValues v = presetValues(static_cast<Preset>(i));
            check(v.renderScale >= 0.5f && v.renderScale <= 1.0f, "preset scale is in range");
            check(std::abs(v.renderScale / kScaleQuantum - std::round(v.renderScale / kScaleQuantum)) < 1e-3,
                  "preset scale is already on the quantum grid");
        }
    }

}

int main() {
    std::printf("calium core tests\n");
    testBoundsFromQuads();
    testDecideBatch();
    testGrabMethod();
    testPlanChain();
    testScaleStep();
    testRollingStats();
    testPresets();
    std::printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
