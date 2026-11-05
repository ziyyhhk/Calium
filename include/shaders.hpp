#pragma once

// Calium shader sources, in one header of inline strings so there is exactly one copy of each and no
// linkage surprises between translation units.
//
// Written once, in the portable GLSL ES 1.00 dialect that cocos2d-x 2.x uses everywhere:
//  - attribute names a_position / a_texCoord, which CCGLProgram binds to indices 0 and 2 before
//    linking, on every platform;
//  - the sampler uniform is named CC_Texture0, which is what cocos' own updateUniforms() looks up;
//  - the `#ifdef GL_ES` precision preamble, so the same source compiles as desktop GLSL 120 on
//    Windows/macOS and as ESSL 1.00 on Android/iOS;
//  - texture2D() only: no texelFetch, no image load/store, no layout() qualifiers, no #version.
//
// Variants come from prepending #define lines to the same source (see glcore.cpp) instead of from
// several near-identical shaders, so a fix is made in one place.

namespace calium::shaders {

    // Fragment preamble. highp where the device allows it: doing UV arithmetic in mediump is what
    // makes cheap mobile post-processing shimmer on big screens.
    inline constexpr char const* kPrec =
        "#ifdef GL_ES\n"
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "precision highp float;\n"
        "#else\n"
        "precision mediump float;\n"
        "#endif\n"
        "#endif\n";

    // ---------------------------------------------------------------- vertex

    // a_position arrives as a 2-component array; GL pads the vec4 to (x, y, 0, 1), which is exactly
    // the full-screen quad in clip space. No matrices, so no dependence on the engine's camera.
    inline constexpr char const* kVSFull =
        "attribute vec4 a_position;\n"
        "attribute vec2 a_texCoord;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "    v_uv = a_texCoord;\n"
        "    gl_Position = a_position;\n"
        "}\n";

    // ---------------------------------------------------------------- copy / present

    // The last blit to the window, and any plain down/up-scale.
    inline constexpr char const* kFSCopy =
        "varying vec2 v_uv;\n"
        "uniform sampler2D CC_Texture0;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(CC_Texture0, v_uv);\n"
        "}\n";

    // ---------------------------------------------------------------- bright extract

    // Soft knee rather than a hard cutoff: a hard threshold makes neon flicker in and out as the
    // camera pans, which is far more noticeable than the glow itself.
    inline constexpr char const* kFSBright =
        "varying vec2 v_uv;\n"
        "uniform sampler2D CC_Texture0;\n"
        "uniform float uThreshold;\n"
        "uniform float uKnee;\n"
        "uniform vec2  uTexel;\n"
        "void main() {\n"
        "    vec3 c = texture2D(CC_Texture0, v_uv).rgb;\n"
        "    vec3 c1 = texture2D(CC_Texture0, v_uv + vec2( uTexel.x, 0.0)).rgb;\n"
        "    vec3 c2 = texture2D(CC_Texture0, v_uv - vec2( uTexel.x, 0.0)).rgb;\n"
        "    vec3 c3 = texture2D(CC_Texture0, v_uv + vec2(0.0,  uTexel.y)).rgb;\n"
        "    vec3 c4 = texture2D(CC_Texture0, v_uv - vec2(0.0,  uTexel.y)).rgb;\n"
        "    c = max(c, max(max(c1, c2), max(c3, c4)));\n"
        "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
        "    float knee = max(uKnee, 1e-4);\n"
        "    float w = clamp((l - uThreshold + knee) / (2.0 * knee), 0.0, 1.0);\n"
        "    float k = max(l - uThreshold, 0.0) * w;\n"
        "    gl_FragColor = vec4(c * k, 1.0);\n"
        "}\n";

    // ---------------------------------------------------------------- separable blur

    // Nine taps along uDir with Gaussian weights (0.2270, 0.1946, 0.1216, 0.0541, 0.0162 - the 1,3,6,7,
    // 6,3,1 kernel normalised). uTexel carries the step and uDir the axis, so one program serves both
    // directions and every level of the chain.
    inline constexpr char const* kFSBlur =
        "varying vec2 v_uv;\n"
        "uniform sampler2D CC_Texture0;\n"
        "uniform vec2 uTexel;\n"
        "uniform vec2 uDir;\n"
        "void main() {\n"
        "    vec2 o = uTexel * uDir;\n"
        "    vec3 s = texture2D(CC_Texture0, v_uv).rgb * 0.2270270270;\n"
        "    s += (texture2D(CC_Texture0, v_uv + o).rgb + texture2D(CC_Texture0, v_uv - o).rgb) * 0.1945945946;\n"
        "    s += (texture2D(CC_Texture0, v_uv + o * 2.0).rgb + texture2D(CC_Texture0, v_uv - o * 2.0).rgb) * 0.1216216216;\n"
        "    s += (texture2D(CC_Texture0, v_uv + o * 3.0).rgb + texture2D(CC_Texture0, v_uv - o * 3.0).rgb) * 0.0540540541;\n"
        "    s += (texture2D(CC_Texture0, v_uv + o * 4.0).rgb + texture2D(CC_Texture0, v_uv - o * 4.0).rgb) * 0.0162162162;\n"
        "    gl_FragColor = vec4(s, 1.0);\n"
        "}\n";

    // ---------------------------------------------------------------- composite

    // Bloom add, then the colour grade, then an optional unsharp. Folding these into one pass saves
    // two full-screen trips and the inputs are already in registers.
    //   USE_GRADE   exposure / lift / gamma / contrast / saturation
    //   USE_SHARP   unsharp mask against a four-tap blur of the same texture
    inline constexpr char const* kFSComposite =
        "varying vec2 v_uv;\n"
        "uniform sampler2D CC_Texture0;\n"
        "uniform sampler2D uBloom;\n"
        "uniform vec2  uTexel;\n"
        "uniform float uBloomAmount;\n"
        "#if USE_GRADE\n"
        "uniform float uExposure;\n"
        "uniform float uGamma;\n"
        "uniform float uContrast;\n"
        "uniform float uSaturation;\n"
        "uniform float uLift;\n"
        "#endif\n"
        "#if USE_SHARP\n"
        "uniform float uSharpen;\n"
        "#endif\n"
        "void main() {\n"
        "    vec3 c = texture2D(CC_Texture0, v_uv).rgb;\n"
        "    c += texture2D(uBloom, v_uv).rgb * uBloomAmount;\n"
        "#if USE_SHARP\n"
        "    vec3 bl = (texture2D(CC_Texture0, v_uv + vec2( uTexel.x, 0.0)).rgb\n"
        "             + texture2D(CC_Texture0, v_uv - vec2( uTexel.x, 0.0)).rgb\n"
        "             + texture2D(CC_Texture0, v_uv + vec2(0.0,  uTexel.y)).rgb\n"
        "             + texture2D(CC_Texture0, v_uv - vec2(0.0,  uTexel.y)).rgb) * 0.25;\n"
        "    c += (c - bl) * uSharpen * 2.0;\n"
        "#endif\n"
        "#if USE_GRADE\n"
        "    c = clamp(c * uExposure + uLift, 0.0, 1.0);\n"
        // pow() of a negative base is undefined, hence the clamp immediately before it.
        "    c = pow(c, vec3(1.0 / max(uGamma, 0.05)));\n"
        "    c = (c - 0.5) * uContrast + 0.5;\n"
        "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
        "    c = mix(vec3(l), c, uSaturation);\n"
        "#endif\n"
        "    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
        "}\n";

    // ---------------------------------------------------------------- final

    // Runs at display resolution, because AA and edge effects are worthless when blurred up from a
    // smaller buffer.
    //   USE_FXAA     luma based edge smoothing
    //   USE_VIGNETTE radial darkening
    //   USE_CA       radial chromatic split
    //
    // This is not NVIDIA FXAA 3.11 - that is a much larger shader with a quality table and a spill
    // path. It is a compact two-tap luma-gradient smoother: cheaper on a phone, and named for what
    // it actually is.
    inline constexpr char const* kFSFinal =
        "varying vec2 v_uv;\n"
        "uniform sampler2D CC_Texture0;\n"
        "uniform vec2  uTexel;\n"
        "#if USE_FXAA\n"
        "float caliumLum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "#endif\n"
        "#if USE_VIGNETTE\n"
        "uniform float uVignette;\n"
        "#endif\n"
        "#if USE_CA\n"
        "uniform float uChroma;\n"
        "#endif\n"
        "void main() {\n"
        "    vec2 uv = v_uv;\n"
        "#if USE_CA\n"
        // Offset grows with distance from the centre, which is where lens fringing appears.
        "    vec2 coff = (uv - 0.5) * uChroma * uTexel * 4.0;\n"
        "    vec3 c = vec3(\n"
        "        texture2D(CC_Texture0, uv + coff).r,\n"
        "        texture2D(CC_Texture0, uv).g,\n"
        "        texture2D(CC_Texture0, uv - coff).b);\n"
        "#else\n"
        "    vec3 c = texture2D(CC_Texture0, uv).rgb;\n"
        "#endif\n"
        "#if USE_FXAA\n"
        "    vec3 nh = texture2D(CC_Texture0, uv - vec2(uTexel.x, 0.0)).rgb;\n"
        "    vec3 nv = texture2D(CC_Texture0, uv - vec2(0.0, uTexel.y)).rgb;\n"
        "    float lC = caliumLum(c), lH = caliumLum(nh), lV = caliumLum(nv);\n"
        // Skip nearly-black pixels: in a dark level an edge there is object outline, and blurring it
        // makes shadows look milky rather than fixing anything.
        "    if (max(max(lC, lH), lV) < 0.0625) { lC = 0.0; lH = 0.0; lV = 0.0; }\n"
        "    vec2 dir = vec2(lC - lH, lC - lV);\n"
        "    float d = max(abs(dir.x), abs(dir.y));\n"
        "    if (d > 1e-5) {\n"
        "        dir = (dir / d) * uTexel * 0.5;\n"
        "        vec3 a = texture2D(CC_Texture0, uv - dir).rgb;\n"
        "        vec3 b = texture2D(CC_Texture0, uv + dir).rgb;\n"
        "        c = c * 0.5 + (a + b) * 0.25;\n"
        "    }\n"
        "#endif\n"
        "#if USE_VIGNETTE\n"
        "    float r = length(uv - 0.5) * 1.4142;\n"
        "    c *= 1.0 - uVignette * smoothstep(0.45, 1.05, r);\n"
        "#endif\n"
        "    gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
        "}\n";

}
