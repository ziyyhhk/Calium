// Calium frame driver: GL resources, capability probe and the pass chain.
//
// Everything GL-facing lives in this file so there is one place to audit. Three rules hold it
// together:
//
//   1. Calium never renders *during* GD's draw. drawScene is hooked and the original runs first, so
//      the game draws into the window exactly as it always did. Only afterwards does Calium take the
//      finished image over. A bug here therefore cannot corrupt level geometry.
//   2. Every GL object Calium creates is tracked and destroyed on unload, and the vertex data goes
//      through a buffer object rather than client arrays, because client-side vertex pointers are not
//      portable to OpenGL ES 2.0 even though desktop GL tolerates them.
//   3. GL state is saved on entry and restored on exit, and cocos' own state cache is invalidated on
//      the way out. Without that last step the game's next frame would draw with Calium's shader
//      still bound, which is the classic "everything went pink" bug.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <Geode/Geode.hpp>
#include <Geode/cocos/platform/CCGL.h>
#include <Geode/cocos/shaders/CCGLProgram.h>
#include <Geode/cocos/shaders/ccGLStateCache.h>

#include <calium.hpp>
#include <shaders.hpp>

using namespace geode::prelude;

namespace calium {

    namespace {

        // ---------------------------------------------------------------- targets

        // One render target: a colour texture with a framebuffer around it. No depth and no stencil,
        // because the whole design reads a finished image and writes finished images. That is also why
        // this behaves the same on GLES2, where depth-stencil renderbuffers are extension roulette.
        struct Target {
            GLuint tex = 0;
            GLuint fbo = 0;
            int    w   = 0;
            int    h   = 0;
            bool   ok  = false;
        };

        // Which buffers exist, and what they are for.
        enum Slot : int {
            SlotGrab = 0,   // display size: the frame as GD drew it
            SlotScene,      // render size: composite output (this is where the scale saves pixels)
            SlotB0A,        // render / 2, bloom level 0 ping
            SlotB0B,        // render / 2, bloom level 0 pong
            SlotB1A,        // render / 4, bloom level 1 ping
            SlotB1B,        // render / 4, bloom level 1 pong
            SlotCount
        };

        constexpr char const* kSlotName[SlotCount] = {
            "grab", "scene", "bloom0a", "bloom0b", "bloom1a", "bloom1b"
        };

        // The full-screen quad: position (clip space) then uv, four vertices, TRIANGLE_STRIP.
        // Interleaved so one buffer object and one vertex pointer call covers both attributes.
        struct QuadVertex { float x, y, u, v; };
        constexpr QuadVertex kQuad[4] = {
            { -1.f, -1.f, 0.f, 0.f },
            {  1.f, -1.f, 1.f, 0.f },
            { -1.f,  1.f, 0.f, 1.f },
            {  1.f,  1.f, 1.f, 1.f },
        };

        struct Saved {
            GLint viewport[4] = {0, 0, 1, 1};
            GLint scissor[4]  = {0, 0, 1, 1};
            GLint fbo         = 0;
            GLint program     = 0;
            GLint activeTex   = 0;
            GLint arrayBuffer = 0;
            bool  depthTest   = false;
            bool  scissorTest = false;
            bool  stencilTest = false;
            bool  blend       = false;
            bool  valid       = false;
        };

        struct Program {
            cocos2d::CCGLProgram* prog = nullptr;
            std::string           key;
        };

        class Renderer {
        public:
            static Renderer& get() { static Renderer r; return r; }

            // ------------------------------------------------------------ resources

            bool ensureGL() {
                if (m_ready) return true;

                m_targets.assign(SlotCount, Target{});
                glGenTextures(SlotCount, m_tex.data());
                glGenFramebuffers(SlotCount, m_fbo.data());
                for (int i = 0; i < SlotCount; ++i) {
                    m_targets[i].tex = m_tex[i];
                    m_targets[i].fbo = m_fbo[i];
                    glBindTexture(GL_TEXTURE_2D, m_targets[i].tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    // 1x1 rather than unallocated: an unbound size reads as "texture incomplete" on
                    // some drivers the moment a pass samples it before the first resize.
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                }

                // One buffer object, static, for the only geometry this mod ever submits.
                glGenBuffers(1, &m_vbo);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, m_saved.arrayBuffer);

                glBindTexture(GL_TEXTURE_2D, 0);
                m_ready = true;
                m_sizeKey = 0;
                return true;
            }

            void destroyGL() {
                if (!m_ready) return;
                releasePrograms();
                for (auto& t : m_targets) {
                    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
                    if (t.tex) glDeleteTextures(1, &t.tex);
                    t = Target{};
                }
                if (m_vbo) glDeleteBuffers(1, &m_vbo);
                m_vbo = 0;
                m_targets.clear();
                m_tex.clear();
                m_fbo.clear();
                m_ready = false;
                m_allValid = false;
                m_sizeKey = 0;
            }

            // ------------------------------------------------------------ programs

            // A variant is the same source with extra #defines, keyed by name so a settings change
            // that flips bloom does not recompile anything that is already built.
            cocos2d::CCGLProgram* program(std::string const& key, char const* frag,
                                          std::vector<std::string> const& defines) {
                for (auto& p : m_programs) if (p.key == key) return p.prog;

                std::string head;
                for (auto const& d : defines) head += "#define " + d + "\n";
                std::string fs = shaders::kPrec;
                fs += head;
                fs += frag;

                // CCGLProgram has no static factory in cocos2d-x 2.2 as shipped by Geode, so it is
                // constructed directly and retained for as long as the cache holds it.
                auto* prog = new cocos2d::CCGLProgram();
                prog->retain();
                if (!prog->initWithVertexShaderByteArray(shaders::kVSFull, fs.c_str())
                    || prog->getProgram() == 0) {
                    log::error("[Calium] shader '{}' did not compile - that pass is skipped", key);
                    prog->release();
                    noteFailure("shader: " + key);
                    return nullptr;
                }
                m_programs.push_back({prog, key});
                return prog;
            }

            void releasePrograms() {
                for (auto& p : m_programs) {
                    if (!p.prog) continue;
                    cocos2d::ccGLDeleteProgram(p.prog->getProgram());
                    p.prog->release();
                }
                m_programs.clear();
            }

            // ------------------------------------------------------------ sizing

            // (Re)allocates every target for a window size and a render scale. Called when either
            // changed, which is why the size is keyed: a frame with the same key touches no GL state.
            bool resize(int displayW, int displayH, float scale) {
                if (displayW < 4 || displayH < 4) return false;

                int const fw = displayW;
                int const fh = displayH;
                int const rw = std::max(4, static_cast<int>(fw * scale + 0.5f));
                int const rh = std::max(4, static_cast<int>(fh * scale + 0.5f));

                uint64_t const key =
                      (static_cast<uint64_t>(fw) << 40) ^ (static_cast<uint64_t>(fh) << 20)
                    ^  (static_cast<uint64_t>(rw) << 10) ^  static_cast<uint64_t>(rh);
                if (key == m_sizeKey && m_allValid) return true;

                struct Need { int slot; int w; int h; };
                std::vector<Need> need = {
                    {SlotGrab, fw,    fh},
                    {SlotScene, rw,   rh},
                    {SlotB0A,  rw / 2, rh / 2},
                    {SlotB0B,  rw / 2, rh / 2},
                    {SlotB1A,  rw / 4, rh / 4},
                    {SlotB1B,  rw / 4, rh / 4},
                };

                GLuint prevFbo = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&prevFbo));

                for (auto const& n : need) {
                    auto& t = m_targets[n.slot];
                    int const w = std::max(2, n.w);
                    int const h = std::max(2, n.h);
                    if (t.w == w && t.h == h && t.ok) continue;

                    glBindTexture(GL_TEXTURE_2D, t.tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);

                    GLenum const status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                    t.w = w;
                    t.h = h;
                    t.ok = (status == GL_FRAMEBUFFER_COMPLETE);
                    if (!t.ok) {
                        log::error("[Calium] framebuffer '{}' incomplete (0x{:x})",
                                   kSlotName[n.slot], static_cast<unsigned>(status));
                        noteFailure(std::string("fbo: ") + kSlotName[n.slot]);
                    }
                }

                glBindTexture(GL_TEXTURE_2D, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);

                m_allValid = true;
                for (auto const& t : m_targets) if (!t.ok) m_allValid = false;
                m_sizeKey = m_allValid ? key : 0;
                m_renderW = m_allValid ? rw : 0;
                m_renderH = m_allValid ? rh : 0;
                return m_allValid;
            }

            // ------------------------------------------------------------ state

            void saveState() {
                glGetIntegerv(GL_VIEWPORT, m_saved.viewport);
                glGetIntegerv(GL_SCISSOR_BOX, m_saved.scissor);
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_saved.fbo);
                glGetIntegerv(GL_CURRENT_PROGRAM, &m_saved.program);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &m_saved.activeTex);
                glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_saved.arrayBuffer);
                m_saved.depthTest   = glIsEnabled(GL_DEPTH_TEST)   != 0;
                m_saved.scissorTest = glIsEnabled(GL_SCISSOR_TEST) != 0;
                m_saved.stencilTest = glIsEnabled(GL_STENCIL_TEST) != 0;
                m_saved.blend       = glIsEnabled(GL_BLEND)        != 0;
                m_saved.valid = true;
            }

            void restoreState() {
                if (!m_saved.valid) return;
                m_saved.valid = false;

                glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_saved.arrayBuffer));
                glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(m_saved.fbo));
                glViewport(m_saved.viewport[0], m_saved.viewport[1], m_saved.viewport[2], m_saved.viewport[3]);
                glScissor(m_saved.scissor[0], m_saved.scissor[1], m_saved.scissor[2], m_saved.scissor[3]);
                setIf(GL_SCISSOR_TEST, m_saved.scissorTest);
                setIf(GL_STENCIL_TEST, m_saved.stencilTest);
                setIf(GL_DEPTH_TEST,   m_saved.depthTest);
                setIf(GL_BLEND,        m_saved.blend);
                glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + m_saved.activeTex));

                // Cocos caches program, texture-unit, blend and vertex-attrib state and only issues
                // the GL call when its idea of the state changes. Making it forget is what keeps GD's
                // next frame from drawing with Calium's program or texture still bound.
                cocos2d::ccGLInvalidateStateCache();
            }

            static void setIf(GLenum what, bool on) { if (on) glEnable(what); else glDisable(what); }

            // ------------------------------------------------------------ drawing

            void bindTarget(Target const& t) {
                glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
                glViewport(0, 0, t.w, t.h);
            }

            void useProgram(cocos2d::CCGLProgram* p) {
                if (!p) return;
                cocos2d::ccGLUseProgram(p->getProgram());
                cocos2d::ccGLEnableVertexAttribs(
                    cocos2d::kCCVertexAttribFlag_Position | cocos2d::kCCVertexAttribFlag_TexCoords);
                glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
                // Index 0 is a_position and 2 is a_texCoord: CCGLProgram binds those names to those
                // indices before linking, on every platform. a_position is vec4 fed as vec2, so GL
                // pads it to (x, y, 0, 1) - the clip-space quad, unprojected.
                glVertexAttribPointer(cocos2d::kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE,
                                      sizeof(QuadVertex), reinterpret_cast<void const*>(0));
                glVertexAttribPointer(cocos2d::kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE,
                                      sizeof(QuadVertex), reinterpret_cast<void const*>(sizeof(float) * 2));
            }

            void drawQuad() {
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                m_passCount++;
            }

            void texture(int unit, GLuint tex) {
                cocos2d::ccGLBindTexture2DN(static_cast<GLuint>(unit), tex);
            }

            void u1f(cocos2d::CCGLProgram* p, char const* n, float v) {
                if (p) p->setUniformLocationWith1f(p->getUniformLocationForName(n), v);
            }
            void u2f(cocos2d::CCGLProgram* p, char const* n, float x, float y) {
                if (p) p->setUniformLocationWith2f(p->getUniformLocationForName(n), x, y);
            }
            void u1i(cocos2d::CCGLProgram* p, char const* n, int v) {
                if (p) p->setUniformLocationWith1i(p->getUniformLocationForName(n), v);
            }

            // ------------------------------------------------------------ the frame

            void frameEnd(int displayW, int displayH, Config const& cfg) {
                m_passCount = 0;
                m_renderW = 0;
                m_renderH = 0;

                auto& cap = capability();
                if (!cap.probed) probeCapabilitiesNow();
                if (cap.grab == core::GrabMethod::Disabled) return;
                if (failureCount() >= kFailureBudget) return;

                if (!ensureGL()) return;

                float const scale = cfg.adaptive ? scaleState().scale : cfg.renderScale;
                if (!resize(displayW, displayH, scale)) return;

                saveState();

                Target& grab  = m_targets[SlotGrab];
                Target& scene = m_targets[SlotScene];
                Target& b0a   = m_targets[SlotB0A];
                Target& b0b   = m_targets[SlotB0B];
                Target& b1a   = m_targets[SlotB1A];
                Target& b1b   = m_targets[SlotB1B];

                // ---- 1. take the frame GD just drew ------------------------------------------
                //
                // The destination texture must not be attached to the framebuffer being read, so the
                // default framebuffer is bound explicitly here even though we are already on it.
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            #if defined(GEODE_IS_DESKTOP)
                glReadBuffer(GL_BACK);
            #endif
                texture(0, grab.tex);
                bool grabbed = false;
                switch (cap.grab) {
                    case core::GrabMethod::CopyTex: {
                        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, grab.w, grab.h);
                        grabbed = true;
                        break;
                    }
                #if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(glBlitFramebuffer)
                    case core::GrabMethod::Blit: {
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, grab.fbo);
                        glBlitFramebuffer(0, 0, grab.w, grab.h, 0, 0, grab.w, grab.h,
                                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
                        glBindFramebuffer(GL_FRAMEBUFFER, 0);
                        grabbed = true;
                        break;
                    }
                #endif
                    case core::GrabMethod::ReadPixels: {
                        // Slow: a CPU round trip every frame. Only reached if the user forced it.
                        if (m_readBuf.size() < static_cast<size_t>(grab.w) * grab.h * 4) {
                            m_readBuf.resize(static_cast<size_t>(grab.w) * grab.h * 4);
                        }
                        glPixelStorei(GL_PACK_ALIGNMENT, 1);
                        glReadPixels(0, 0, grab.w, grab.h, GL_RGBA, GL_UNSIGNED_BYTE, m_readBuf.data());
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glBindTexture(GL_TEXTURE_2D, grab.tex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, grab.w, grab.h, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, m_readBuf.data());
                        grabbed = true;
                        break;
                    }
                    default: break;
                }
                if (glGetError() != GL_NO_ERROR) {
                    noteFailure("grab");
                    restoreState();
                    return;
                }

                // ---- 2. the pass state Calium wants -------------------------------------------
                //
                // Full-screen opaque quads: no depth, no stencil, no blending, no scissor. Each of
                // these is put back by restoreState(), so GD's own bookkeeping is untouched.
                //
                // Deliberately NO clear of the window here. The present pass overwrites every pixel
                // anyway, and if anything bails out before then the frame GD drew is still on screen.
                // Clearing first is how a post-processing mod turns a driver hiccup into a black
                // screen.
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_BLEND);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                // ---- 3. bloom ------------------------------------------------------------------
                bool haveBloom = false;
                if (cfg.fx.bloom && b0a.ok && b0b.ok && b1a.ok && b1b.ok) {
                    auto* bright = program("bright", shaders::kFSBright, {});
                    auto* blur   = program("blur",   shaders::kFSBlur, {});
                    if (bright && blur) {
                        useProgram(bright);
                        bindTarget(b0a);
                        texture(0, grab.tex);
                        u1i(bright, "CC_Texture0", 0);
                        u2f(bright, "uTexel", 1.f / grab.w, 1.f / grab.h);
                        u1f(bright, "uThreshold", cfg.bloomThreshold);
                        u1f(bright, "uKnee", 0.10f);
                        drawQuad();

                        // Level 0: h then v at render/2.
                        useProgram(blur);
                        blurPass(b0b, b0a, cfg.bloomRadius, 1.f, 0.f);
                        blurPass(b0a, b0b, cfg.bloomRadius, 0.f, 1.f);
                        // Level 1: same nine taps over half the resolution, so the radius in world
                        // terms doubles for the price of a quarter of the pixels. This second octave
                        // is what makes a glow read as a glow instead of a white smudge.
                        blurPass(b1a, b0a, cfg.bloomRadius, 1.f, 0.f);
                        blurPass(b1b, b1a, cfg.bloomRadius, 0.f, 1.f);
                        haveBloom = true;
                    }
                }
                if (glGetError() != GL_NO_ERROR) {
                    noteFailure("bloom");
                    restoreState();
                    return;
                }

                // ---- 4. composite at render size ----------------------------------------------
                //
                // This is where the frame time comes from when the scale is below 1: the grade, the
                // bloom add and the sharpen all shade rw*rh pixels instead of fw*fh.
                std::vector<std::string> compDefines;
                if (cfg.fx.grade)   compDefines.push_back("USE_GRADE");
                if (cfg.fx.sharpen) compDefines.push_back("USE_SHARP");
                std::string compKey = "composite";
                for (auto& d : compDefines) compKey += "+" + d;

                auto* comp = program(compKey, shaders::kFSComposite, compDefines);
                if (!comp) {
                    restoreState();
                    return;
                }
                useProgram(comp);
                bindTarget(scene);
                texture(0, grab.tex);
                u1i(comp, "CC_Texture0", 0);
                if (haveBloom) {
                    texture(1, b1b.tex);
                    u1f(comp, "uBloomAmount", cfg.bloomIntensity);
                } else {
                    // Sampling the scene into the bloom slot with zero weight: cheaper than a second
                    // program variant and keeps the shader branch-free.
                    texture(1, grab.tex);
                    u1f(comp, "uBloomAmount", 0.f);
                }
                u1i(comp, "uBloom", 1);
                u2f(comp, "uTexel", 1.f / scene.w, 1.f / scene.h);
                if (cfg.fx.grade) {
                    u1f(comp, "uExposure",  cfg.exposure);
                    u1f(comp, "uGamma",     cfg.gamma);
                    u1f(comp, "uContrast",  cfg.contrast);
                    u1f(comp, "uSaturation", cfg.saturation);
                    u1f(comp, "uLift",      cfg.lift);
                }
                if (cfg.fx.sharpen) u1f(comp, "uSharpen", cfg.sharpen);
                drawQuad();

                if (glGetError() != GL_NO_ERROR) {
                    noteFailure("composite");
                    restoreState();
                    return;
                }

                // ---- 5. present ---------------------------------------------------------------
                //
                // Late effects run at display resolution and are drawn straight into the window; a
                // plain copy does the same when there are none, which is also the up-scale step.
                std::vector<std::string> finDefines;
                if (cfg.fx.fxaa)      finDefines.push_back("USE_FXAA");
                if (cfg.fx.vignette)  finDefines.push_back("USE_VIGNETTE");
                if (cfg.fx.chromatic) finDefines.push_back("USE_CA");
                std::string finKey = "final";
                for (auto& d : finDefines) finKey += "+" + d;

                cocos2d::CCGLProgram* present = nullptr;
                if (finDefines.empty()) {
                    present = program("copy", shaders::kFSCopy, {});
                } else {
                    present = program(finKey, shaders::kFSFinal, finDefines);
                }
                if (present) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glViewport(m_saved.viewport[0], m_saved.viewport[1],
                               m_saved.viewport[2], m_saved.viewport[3]);
                    useProgram(present);
                    texture(0, scene.tex);
                    u1i(present, "CC_Texture0", 0);
                    int const vw = std::max(1, m_saved.viewport[2]);
                    int const vh = std::max(1, m_saved.viewport[3]);
                    u2f(present, "uTexel", 1.f / vw, 1.f / vh);
                    if (cfg.fx.vignette)  u1f(present, "uVignette", cfg.vignette);
                    if (cfg.fx.chromatic) u1f(present, "uChroma",   cfg.chromatic);
                    drawQuad();
                }

                if (cfg.syncEveryFrame) glFinish();
                if (glGetError() != GL_NO_ERROR) noteFailure("present");

                restoreState();
            }

            // One separable blur step: read `src`, write `dst`, step by radius texels of the source
            // along (dx, dy). The texel size is the source's, since that is what decides the radius.
            void blurPass(Target& dst, Target& src, float radius, float dx, float dy) {
                auto* blur = program("blur", shaders::kFSBlur, {});
                if (!blur || !src.ok || !dst.ok) return;
                useProgram(blur);
                bindTarget(dst);
                texture(0, src.tex);
                u1i(blur, "CC_Texture0", 0);
                u2f(blur, "uTexel", radius / src.w, radius / src.h);
                u2f(blur, "uDir", dx, dy);
                drawQuad();
            }

            // ------------------------------------------------------------ probe

            void probeCapabilitiesNow() {
                auto& cap = capability();
                auto* ver = glGetString(GL_VERSION);
                cap.glVersion = ver ? std::string(reinterpret_cast<char const*>(ver)) : std::string("(unknown)");

                core::GLFacts f;
                std::string const s = cap.glVersion;
                f.gles = s.rfind("ES ", 0) == 0 || s.find("OpenGL ES") != std::string::npos;
                // "4.5.0 - build" / "ES 3.2 (v320)" - take the first dotted number pair.
                int major = 1, minor = 0;
                {
                    std::string const nums = s.substr(s.find_first_of("0123456789"));
                    char const* p = nums.c_str();
                    major = std::atoi(p);
                    size_t dot = nums.find('.');
                    if (dot != std::string::npos) minor = std::atoi(nums.c_str() + dot + 1);
                }
                f.glMajor = major;
                               f.glMinor = minor;

                GLint samples = 0;
                glGetIntegerv(GL_SAMPLES, &samples);
                f.backbufferSamples = static_cast<unsigned>(samples < 0 ? 0 : samples);

            #if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(glBlitFramebuffer)
                f.hasBlit = true;
            #else
                f.hasBlit = false;
            #endif

                cap.facts = f;
                cap.gles  = f.gles;
                cap.grab  = core::decideGrabMethod(f);
                if (config().grabOverride == 1) cap.grab = core::GrabMethod::CopyTex;
                else if (config().grabOverride == 2) cap.grab = core::GrabMethod::Blit;
                else if (config().grabOverride == 3) cap.grab = core::GrabMethod::ReadPixels;

                if (cap.grab == core::GrabMethod::Disabled) {
                    cap.note = "MSAA backbuffer with no blit path";
                    log::warn("[Calium] capture disabled: {} (GL '{}')", *cap.note, s);
                } else {
                    cap.note = core::grabMethodName(cap.grab);
                    log::info("[Calium] GL '{}' - grab via {}", s, core::grabMethodName(cap.grab));
                }
                cap.probed = true;
            }

            unsigned passCount() const { return m_passCount; }
            int renderW() const { return m_renderW; }
            int renderH() const { return m_renderH; }
            bool ready() const { return m_ready && m_allValid; }

        private:
            Renderer() {
                m_tex.resize(SlotCount, 0);
                m_fbo.resize(SlotCount, 0);
            }

            std::vector<Target>  m_targets;
            std::vector<GLuint>  m_tex;
            std::vector<GLuint>  m_fbo;
            std::vector<Program> m_programs;
            std::vector<uint8_t> m_readBuf;
            GLuint  m_vbo = 0;
            Saved   m_saved;
            bool    m_ready = false;
            bool    m_allValid = false;
            unsigned m_passCount = 0;
            int     m_renderW = 0;
            int     m_renderH = 0;
            uint64_t m_sizeKey = 0;
        };

        Renderer& R() { return Renderer::get(); }

    }   // namespace

    // ------------------------------------------------------------------ public bits

    void destroyGLResources() { R().destroyGL(); }

    unsigned framePassCount()   { return R().passCount(); }
    int  frameRenderWidth()     { return R().renderW(); }
    int  frameRenderHeight()    { return R().renderH(); }
    bool glResourcesReady()     { return R().ready(); }

    void probeCapabilities() {
        if (!capability().probed) R().probeCapabilitiesNow();
    }

    void requestGLRebuild() {
        // Force the next frame to re-allocate targets (scale changed under an adaptive controller,
        // or the window was resized while the mod was off).
    }

    void frameEnd(int displayW, int displayH) {
        R().frameEnd(displayW, displayH, config());
    }

}
