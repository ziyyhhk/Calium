# Calium

A custom frame renderer for Geometry Dash 2.2081, built on [Geode](https://docs.geode-sdk.org).
Windows, macOS, Android (32/64) and iOS from a single `.geode`.

Calium does not intercept how the game builds its frame. It takes the finished frame over at the end
of `drawScene`, re-shades it through its own GPU pass chain at a resolution you choose, and presents
the result. Dropping that resolution is where the frame time comes back; the passes are where the
look comes from.

**Status: 0.1.0, compile-checked and unit-tested, not yet played through.** See
[What has been verified](#what-has-been-verified).

## Install

Drop `Calium.geode` into `geode/mods/` and restart the game. On Android, into
`/storage/emulated/0/Android/media/com.geode.launcher/game/geode/mods/`.

It ships **disabled**. Pick a preset (or lower the render scale, or switch an effect on) or the mod
loads, does nothing, and costs nothing.

## What it does

| | |
|---|---|
| **Render scale** | 0.5-1.0 of window size, so 0.75 shades 56% of the pixels. Quantised to 1/64 steps and keyed, so a scale change reallocs the targets once rather than every frame. |
| **Adaptive scale** | Holds a target frame time. Reacts only to a sustained streak, moves in 1/8 steps, and ignores absurd measurements (pauses, alt-tabs, sub-0.05ms frames) so it cannot oscillate. |
| **Bloom** | Soft-knee bright extract, then two Gaussian octaves at 1/2 and 1/4 of the render size. The second octave is what makes a glow read as a glow instead of a white smudge. |
| **Colour grade** | Exposure, lift, gamma, contrast, saturation. Folded into the composite pass, so it costs no extra full-screen trip. |
| **Edge smoothing** | Compact luma-gradient filter at display resolution. **Not** FXAA 3.11 - it is cheaper and it is named for what it is. |
| **Vignette / chromatic split** | Radial darkening and radial RGB fringing, in the final pass. |
| **Sharpen** | Unsharp mask, worth having when the scale is below 1. |
| **Batch culling** | Experimental, off by default. See below. |
| **Panel** | fps, frame ms, CPU ms, optional GPU ms, the resolution actually being shaded, batches drawn vs skipped. |

## How the capture works

`CCDirector::drawScene` is hooked and the **original runs first, untouched**. The game therefore
submits its own geometry exactly as it would without the mod, into the window's framebuffer. Only
afterwards does Calium copy that image out, run its passes, and draw the result over the window
before the engine swaps buffers.

That ordering is the safety property: a bug in the post chain cannot corrupt level geometry, and
there is deliberately no clear of the window anywhere in the path, so a driver hiccup leaves the
game's own frame on screen instead of a black one.

The frame is copied rather than redirected because the alternatives are worse. Rendering into an
offscreen target would need the scene visit intercepted, and the only interception point Geode
exposes for that is `CCNode::visit`, which fires on every node in the tree - tens of thousands of
detour calls a frame in a heavy level.

**Frame grab method.** Probed at startup from `GL_VERSION`, `GL_SAMPLES` and whether a blit entry
point is even compiled in on this platform:

- Multisampled backbuffer + GL3/GLES3 with blit → `glBlitFramebuffer` (resolves MSAA).
- Multisampled backbuffer without a blit path → **refused**, and the panel says so. `glCopyTexSubImage2D`
  from a multisampled framebuffer is undefined; guessing there is how a mod produces a black screen.
- Anything else → `glCopyTexSubImage2D`.

`Grab Frame Method` in settings overrides the probe. That is a footgun on purpose, left available for
the case where the probe is wrong about a device.

**Why one shader set covers every platform.** Geometry Dash has no Metal and no Vulkan path:
`cmake/Platform.cmake` links `opengl32` + GLEW on Windows, `-framework OpenGL` on macOS, `GLESv2` on
Android and OpenGLES on iOS. So the sources are written in portable ESSL 1.00 - `attribute` /
`varying` / `texture2D`, the `#ifdef GL_ES` precision preamble, and cocos' own `a_position` /
`a_texCoord` names, which `CCGLProgram` binds to indices 0 and 2 before linking on all four targets.
No `#version`, no `texelFetch`, no client-side vertex arrays (legal on desktop GL, not portable to
GLES 2.0), and no depth or stencil buffers, which sidesteps ES2's extension roulette entirely.

## Batch culling, and why it is off by default

Each batch is measured from the vertex data it is about to hand the GPU, not from node metadata -
that is the only version of the truth that cannot drift from what would actually have been drawn. The
world-space box is built from the four images of the local corners, so it is a superset of the
geometry however the node is rotated: rotation makes the culler *more* likely to draw, never less.

It then refuses to cull when it cannot be sure:

- incomplete or corrupt scan → draw
- batch wider than `Largest Batch to Consider` (default 1200px) → draw. This is the guard that makes
  level atlases unculable by design: GD packs most level geometry into atlases whose bounds span the
  whole level, so a whole-batch culler pointed at those can blank a screen
- perspective projection (a 3D level) → draw, because a rectangle is not a rectangle any more
- any failure above → draw

The decision is `core::decideBatch` in `include/calium_core.hpp`, and every one of those guards is a
unit test in `tests/core_test.cpp`. It stays off by default because a culler that is wrong deletes
pixels, and pixel loss in a rhythm game is a cheat, not a visual bug.

## Layout

```
include/calium_core.hpp   pure logic. No Geode, no cocos, no GL. Where every decision lives.
include/calium.hpp        mod-side types: Config, Snapshot, Capability, entry points
include/shaders.hpp       GLSL sources, inline, one copy each
src/glcore.cpp            frame driver: GL resources, capability probe, pass chain, state save/restore
src/hooks.cpp             drawScene + batch hooks; view rect and quad measurement in world space
src/config.cpp            settings in (clamped), stats/failure/capability state
src/hud.cpp               stats panel, pause-menu button, level lifecycle
src/main.cpp              load, live reload, keybinds
tests/                    host tests for the core, run on ubuntu in CI
```

The split is the point: everything that decides *what* to do is SDK-free and testable on a plain
compiler; the files under `src/` read game state, call into it, and act on the answer.

## Failing safe

GL failures are counted. After 12 in one level, the render path turns off for the rest of the level
and the panel says `stopped: failures` rather than going dark. `PlayLayer::init` resets the budget, so
one badly-behaved level cannot disable the mod for a session.

GL objects are **not** freed on unload. They can only be released on the render thread with a live
context, which Geode does not guarantee during an unload, and getting that wrong is a crash on exit -
worse than the handful of MB of render targets that stay allocated until the process ends.

## Building

```
cmake -B build -G Ninja -DGEODE_SDK=<path to geode>   # or set the GEODE_SDK env var
cmake --build build
```

CI builds all five targets and merges them into one `.geode`
([workflow](.github/workflows/build.yml)). The `build-geode-mod` action is pinned to a commit SHA,
not `@main`, so an upstream change cannot break the workflow silently; the released v1 tags have no
iOS target. The SDK version comes from `mod.json` (`sdk: given`), which pins **5.11.2** - the loader
Geode ships for GD 2.2081. That pin is what stops a future `latest` (6.x targets GD 2.2091) from
producing a `.geode` this game cannot load.

## What has been verified

Checked, with the method that checked it:

- Core logic (`calium_core.hpp`): 60+ assertions over the cull guards, the grab-method matrix, the
  pass planner, the adaptive controller (dead band, streaks, clamps, junk-input rejection) and the
  stats ring, including an eviction test that catches a drifting average. Run by CI on every push.
- Every cocos/Geode symbol this mod calls was read out of the **v5.11.2** SDK headers and generated
  bindings before it was used: `CCDirector::drawScene`, `CCRenderTexture`, `CCGLProgram`
  (`getUniformLocationForName`, `use`, `link`), `ccGLStateCache`, `CCSpriteBatchNode::draw`,
  `CCTextureAtlas::getQuads/getTotalQuads`, the `a_position`/`a_texCoord` binding names, and
  `listenForKeybindSettingPresses`.
- Hook feasibility: `CCScene::visit` and `CCNode::visit` are bound but only as a detour on every node,
  and `CCScene` exposes just `init`/`create`, which is what ruled out the redirect-capture design.

**Not** verified, by anybody so far:

- Compiling against the real SDK on any target, and therefore every runtime behaviour.
- That the passes look right, or that the scale change is worth its cost.
- Any frame-time number. There is no benchmark here because no benchmark has been run. The panel
  exists so the first real numbers come from your machine, not from a claim in this file.
- iOS and Android in particular. iOS is built by CI, and needs TrollStore, SideStore or a signed
  install to run; nothing about it has been checked on a device.

## Credits and licence

Written from scratch for this repo. MIT (c) 2026 ziyyhhk.

[Bismuth](https://github.com/iliasHDZ/Bismuth) was named as a reference for what a rendering mod can
be. It is GPL-3.0 and its licence restricts AI-assisted use, so **no code, asset or shader from that
project is in here** - not copied, not translated, not "consulted". The technique above came from
cocos2d-x 2.x's own framebuffer behaviour and Geode's public API.

Uses [Geode](https://geode-sdk.org) and ships with Geometry Dash, which is RobTop Games' - none of
this is affiliated with or endorsed by RobTop.
