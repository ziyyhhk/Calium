// Calium hook layer.
//
// Two jobs, both deliberately small:
//
//   - CCDirector::drawScene: the frame boundary. The original runs first and unchanged, so GD's own
//     rendering path is never intercepted; afterwards the post chain gets the finished image. This is
//     also where the two honest timings are taken.
//
//   - CCSpriteBatchNode::draw / CCParticleBatchNode::draw: observe what the engine is about to submit,
//     and (only if the user turns the culler on) skip batches that are provably off screen. The
//     decision itself is core::decideBatch, unit tested; this file only gathers the facts.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCParticleBatchNode.hpp>
#include <Geode/modify/CCSpriteBatchNode.hpp>

#include <calium.hpp>

using namespace geode::prelude;

namespace calium {

    namespace {

        using clock = std::chrono::steady_clock;

        clock::time_point g_lastFrameStart{};
        bool              g_haveLastFrame = false;
        uint64_t          g_frameIndex = 0;

        // ---------------------------------------------------------------- view rect

        struct ViewState {
            core::Bounds view;
            bool   ok         = false;
            bool   perspective = false;
            uint64_t frame = 0;
        };

        ViewState g_view;

        // The visible rectangle in world space, found by pushing the four corners of the visible
        // window through the running scene's node-to-world transform. Valid only while the projection
        // is the flat 2D one; under a perspective camera a rectangle is not a rectangle any more, so
        // the state says "not ok" and the culler stands down.
        ViewState computeView() {
            ViewState out;
            auto* director = cocos2d::CCDirector::sharedDirector();
            auto* scene = director ? director->getRunningScene() : nullptr;
            if (!scene) return out;

            out.perspective = director->getProjection() != cocos2d::kCCDirectorProjection2D;
            if (out.perspective) return out;

            cocos2d::CCPoint const origin = director->getVisibleOrigin();
            cocos2d::CCSize const  size   = director->getVisibleSize();
            if (size.width <= 0.f || size.height <= 0.f) return out;

            cocos2d::CCPoint const corners[4] = {
                { origin.x,             origin.y },
                { origin.x + size.width, origin.y },
                { origin.x,             origin.y + size.height },
                { origin.x + size.width, origin.y + size.height },
            };

            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (auto const& c : corners) {
                cocos2d::CCPoint w = scene->convertToWorldSpace(c);
                if (!std::isfinite(w.x) || !std::isfinite(w.y)) return out;
                minX = std::min(minX, w.x);
                minY = std::min(minY, w.y);
                maxX = std::max(maxX, w.x);
                maxY = std::max(maxY, w.y);
            }
            out.view = { minX, minY, maxX, maxY };
            out.ok = true;
            return out;
        }

        ViewState const& viewState() {
            if (g_view.frame != g_frameIndex) {
                g_view = computeView();
                g_view.frame = g_frameIndex;
            }
            return g_view;
        }

        // ---------------------------------------------------------------- batch facts

        // A batch is measured from the vertex data it is about to hand the GPU, not from node
        // metadata. That is the only version of the truth that cannot drift from what would have been
        // drawn, and it is what makes skipping safe.
        struct BatchFacts {
            core::BoundsResult local;
            core::Bounds       world;
            bool               rotated = false;
            bool               usable  = false;
            unsigned           quads   = 0;
        };

        BatchFacts measure(cocos2d::CCNode* node, cocos2d::CCTextureAtlas* atlas) {
            BatchFacts f;
            if (!node || !atlas) return f;

            unsigned const total = atlas->getTotalQuads();
            f.quads = total;
            if (total == 0) return f;              // nothing to draw either way; not our business

            cocos2d::ccV3F_C4B_T2F_Quad* quads = atlas->getQuads();
            if (!quads) return f;

            // Copy the positions into the SDK-free shape the tested code works on. Each quad gives
            // one box: the true extent over all four corners, because the four corner records are
            // named by texture slot (tl/tr/bl/br), not by geometry, so a flipped sprite would slip
            // past a two-corner reading.
            std::vector<core::QuadPos> pos(total);
            for (unsigned i = 0; i < total; ++i) {
                cocos2d::ccV3F_C4B_T2F_Quad const& q = quads[i];
                float xs[4] = { q.tl.vertices.x, q.tr.vertices.x, q.bl.vertices.x, q.br.vertices.x };
                float ys[4] = { q.tl.vertices.y, q.tr.vertices.y, q.bl.vertices.y, q.br.vertices.y };
                pos[i].tlX = *std::min_element(xs, xs + 4);
                pos[i].brX = *std::max_element(xs, xs + 4);
                pos[i].tlY = *std::min_element(ys, ys + 4);
                pos[i].brY = *std::max_element(ys, ys + 4);
            }

            core::ScanLimit limit;
            limit.maxQuads = 8192;   // cost ceiling: a huge atlas stops being worth measuring
            limit.stride   = 1;
            f.local = core::boundsFromQuads(pos.data(), total, limit);

            // Local box to world box. Four corner maps; batch nodes are few, so this is cheap.
            cocos2d::CCPoint const lc[4] = {
                { f.local.bounds.minX, f.local.bounds.minY },
                { f.local.bounds.maxX, f.local.bounds.minY },
                { f.local.bounds.minX, f.local.bounds.maxY },
                { f.local.bounds.maxX, f.local.bounds.maxY },
            };
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (auto const& p : lc) {
                cocos2d::CCPoint w = node->convertToWorldSpace(p);
                if (!std::isfinite(w.x) || !std::isfinite(w.y)) return f;
                minX = std::min(minX, w.x);
                minY = std::min(minY, w.y);
                maxX = std::max(maxX, w.x);
                maxY = std::max(maxY, w.y);
            }
            f.world = { minX, minY, maxX, maxY };

            // Note there is no rotation test here on purpose. The world box above is built from the
            // four images of the local corners, so it is a superset of the geometry however the node
            // is rotated or skewed - a rotation makes the culler *more* likely to draw, never less.
            // What it does not survive is perspective, and that is refused via the view state.
            f.usable = f.local.complete;
            return f;
        }

        // The one decision path shared by both batch classes.
        void evaluateAndDraw(cocos2d::CCNode* node, cocos2d::CCTextureAtlas* atlas,
                             bool particle, std::function<void()> original) {
            auto& cnt = frameCounters();
            auto& ses = counters();

            // Which kinds of batch may be culled is the user's setting, read through cullPolicy();
            // nothing here is allowed to override it. The live facts (projection kind) are then added
            // to the copy, and they can only ever make the policy refuse.
            core::CullPolicy policy = cullPolicy();

            // Counting is free-ish and always on, because "did it actually do something" is the only
            // honest way to answer that question.
            if (!policy.enabled) {
                ++cnt.batchesDrawn;
                ++ses.batchesDrawn;
                original();
                return;
            }

            BatchFacts const f = measure(node, atlas);
            cnt.quadsScanned += f.quads;
            ses.quadsScanned += f.quads;

            ViewState const& vs = viewState();
            core::CullQuery q;
            q.batchBounds = f.local;
            q.batchBounds.bounds = f.world;
            q.isParticleBatch = particle;
            q.view = vs.view;
            policy.perspective = vs.perspective || !vs.ok;
            policy.rotation = f.rotated;

            // Only trust the world box if the local scan was complete; measure() leaves f.usable false
            // otherwise, and decideBatch fails open on an incomplete box anyway.
            if (!f.usable) q.batchBounds.complete = false;

            if (core::decideBatch(q, policy) == core::Verdict::Skip) {
                ++cnt.batchesSkipped;
                ++ses.batchesSkipped;
                return;                      // the batch is not submitted at all
            }
            ++cnt.batchesDrawn;
            ++ses.batchesDrawn;
            cnt.quadsSubmitted += f.quads;
            ses.quadsSubmitted += f.quads;
            original();
        }

    }   // namespace

    // ------------------------------------------------------------------ frame plumbing

    void onGameFrameStart() {
        frameCounters() = core::FrameCounters{};
        ++g_frameIndex;
    }

    uint64_t frameIndex() { return g_frameIndex; }

    void recordFrameTimes(double frameMs, double cpuMs, double gpuMs) {
        auto& s = snapshot();
        s.frameMs = frameMs;
        s.cpuMs   = cpuMs;
        s.gpuMs   = gpuMs;
        frameStats().push(frameMs);
    }

    double lastFrameMs() { return snapshot().frameMs; }
    double lastCpuMs()   { return snapshot().cpuMs; }

}

// ---------------------------------------------------------------------- the hooks

using namespace calium;

class $modify(CCDirector) {
    void drawScene() {
        auto const start = clock::now();

        double frameMs = 0.0;
        if (g_haveLastFrame) {
            frameMs = std::chrono::duration<double, std::milli>(start - g_lastFrameStart).count();
            // A frame gap over half a second is a pause, a tab switch or a level load, not a frame.
            if (frameMs > 500.0) frameMs = 0.0;
        }
        g_lastFrameStart = start;
        g_haveLastFrame = true;

        onGameFrameStart();

        // GD draws exactly the way it always does. Nothing here intercepts it.
        CCDirector::drawScene();

        auto const afterGame = clock::now();
        double cpuMs = std::chrono::duration<double, std::milli>(afterGame - start).count();

        double gpuMs = -1.0;
        if (rendererEnabled()) {
            auto* self = cocos2d::CCDirector::sharedDirector();
            cocos2d::CCSize const px = self->getWinSizeInPixels();
            calium::frameEnd(static_cast<int>(px.width), static_cast<int>(px.height));
            if (config().syncEveryFrame) {
                // With a glFinish inside the chain (done by the driver when the setting is on), the
                // wall time across it is the frame's GPU cost rather than just the CPU submission.
                gpuMs = std::chrono::duration<double, std::milli>(clock::now() - start).count();
            }
        }

        auto const end = clock::now();
        cpuMs = std::chrono::duration<double, std::milli>(end - start).count();
        recordFrameTimes(frameMs, cpuMs, gpuMs);
    }
};

class $modify(CCSpriteBatchNode) {
    void draw() {
        evaluateAndDraw(this, getTextureAtlas(), false, [this] { CCSpriteBatchNode::draw(); });
    }
};

class $modify(CCParticleBatchNode) {
    void draw() {
        evaluateAndDraw(this, getTextureAtlas(), true, [this] { CCParticleBatchNode::draw(); });
    }
};
