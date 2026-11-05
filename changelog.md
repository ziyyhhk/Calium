# 0.1.0

First build. Everything here is new, so the honest summary is what exists rather than what changed.

## Added

- Frame capture and a custom pass chain. GD draws its frame as normal; Calium takes it over at the
  end of `drawScene`, runs its own full-screen shader passes, and presents the result. The capture
  can be switched off with a hotkey at any time and the game goes back to compositing itself.
- Render scale, from 0.5 to 1.0, with an adaptive controller that holds a target frame time.
- Two-level bloom, colour grade, edge smoothing, vignette, chromatic split, unsharp mask.
- Batch culling (experimental, off by default) measured from the vertex data a batch is about to
  submit, with every measurement failure falling back to drawing the batch.
- On-level stats panel: fps, frame time, CPU time, optional GPU time, shaded resolution, batches
  drawn and skipped.
- Presets: Performance, Balanced, Quality, Cinematic, Custom.
- Builds for Windows, macOS, iOS, Android 32 and Android 64, packaged as one `.geode`.
- Host unit tests for the logic core, run on every CI build.

## Known limits

- Not yet played through on any platform. First release; expect the visual judgement calls to move.
- The edge smoother is a compact luma-gradient filter, not FXAA 3.11.
- No Metal or Vulkan path exists, because Geometry Dash itself has none: desktop GL on Windows and
  macOS, OpenGL ES 2.0 on Android and iOS. The shaders are written in portable ESSL 1.00 for that
  reason.
- Depth-based effects (DOF, SSAO) are not possible with this capture method, since only the finished
  colour image is taken over.
