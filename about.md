# Calium is a custom renderer for Geometry Dash. It does not change how the game builds its frame -
# it takes the finished frame over at the end, shades it again through a chain of its own GPU passes,
# and presents it. Because the passes run at a resolution you choose, dropping that resolution buys
# frames on a weak GPU or a phone: 0.75 means 56% of the pixels get shaded.
#
# What is in the chain: two-level bloom, a colour grade (exposure, lift, gamma, contrast,
# saturation), an edge smoother, a vignette, a chromatic split and an unsharp mask, plus a batch
# culler that measures the vertex data a batch is about to submit and skips it when that box is
# provably outside the view.
#
# The panel in the corner of a level shows fps, frame time, CPU time, the resolution actually being
# shaded and how many batches were drawn versus skipped, so you can see whether a setting did
# anything instead of guessing. Ctrl+Shift+C toggles the whole thing mid-level.
#
# It is off when you install it. Pick a preset, or lower the render scale, or switch an effect on.
#
# This is an early build. It has been compile-checked and its decision logic is unit tested, but it
# has not been played through on Windows, Android or iOS yet, so expect to report what looks wrong.
