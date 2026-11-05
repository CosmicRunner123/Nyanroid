#include "diesel/renderer.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <cstddef>

#include "nyan/common/error.hpp"
#include "nyan/diesel/image.hpp"

namespace nyan::render::dieselr {

namespace {

struct Vertex {
    float x, y, w;  // game-screen pixels (y down) and the projective w
    float u, v;
    float r, g, b, a;
};

// Positions are back-buffer pixels with y down; the back buffer keeps row 0 at the top of the
// picture (present() turns it the right way up). w goes into the clip position so the texture is
// interpolated as the projective quad asks.
const char* kVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform vec2 uSize;
out vec2 vUv;
out vec4 vColor;
void main() {
    vec2 ndc = vec2(aPos.x / uSize.x * 2.0 - 1.0, aPos.y / uSize.y * 2.0 - 1.0);
    gl_Position = vec4(ndc * aPos.z, 0.0, aPos.z);
    vUv = aUv;
    vColor = aColor;
}
)";

// Particles (Mware's vsParticle / psParticle_<blend>_<colour>): the vertices come in D3D11's clip
// space (y up, 0 <= z <= w); y turns over for the back buffer's rows and z goes to GL's -w .. w, so
// GL clips them as D3D11 would. The texture and the vertex colour combine by the file's colour mode
// (multiply; add or subtract the colour, the alphas multiplied); the multiply blend fades the colour
// towards white by its alpha, the screen blend premultiplies (the shaders' last steps; their soft
// particle part is off). Dither as the sprites' (below).
const char* kParticleVs = R"(#version 300 es
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
out vec2 vUv;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPos.x, -aPos.y, aPos.z * 2.0 - aPos.w, aPos.w);
    vUv = aUv;
    vColor = aColor;
}
)";
const char* kParticleFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform int uCalc;
uniform int uGroup;
uniform bool uDither;
uniform sampler2D uDitherTex;
uniform vec2 uDitherSize;
uniform vec2 uDitherOffset;
out vec4 oColor;
void main() {
    vec4 t = texture(uTex, vUv);
    vec4 c = uCalc == 1 ? vec4(t.rgb + vColor.rgb, t.a * vColor.a)
           : uCalc == 2 ? vec4(t.rgb - vColor.rgb, t.a * vColor.a)
                        : t * vColor;
    // Dither (g_DitherEnable): dropped where the alpha is below the pattern, else opaque; the
    // multiply and screen steps keep the alpha from before.
    float a = c.a;
    if (uDither) {
        if (a < texture(uDitherTex, (gl_FragCoord.xy + uDitherOffset) / uDitherSize).r) discard;
        c.a = 1.0;
    }
    if (uGroup == 1) c.rgb = c.rgb * a + (1.0 - a);
    else if (uGroup == 2) c.rgb = c.rgb * a;
    oColor = clamp(c, 0.0, 1.0);
}
)";

// The texture (createColor's primitive: white) times the corner colour. Multiply draws its colour faded
// towards white by its alpha for a dst * src blend; screen premultiplies for src + dst * (1 - src)
// (psSprite_Mul_Normal / psSprite_Screen_Normal and their psPrimitive twins: s.rgb * s.a + (1 - s.a)
// and s.rgb * s.a, the dither's discard first in Muramasa's and Phantom's; You and Me and Her's older
// shaders have no dither branch, and none of its scenes dithers).
const char* kFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform bool uTextured;
uniform bool uGlyph;
uniform int uMode;
uniform vec2 uSize;
uniform sampler2D uRule;
uniform bool uTransition;
uniform float uTransNow;
uniform float uTransRate;
uniform sampler2D uMask;
uniform bool uMasked;
uniform bool uMaskInverse;
uniform sampler2D uDst;
uniform bool uDither;
uniform sampler2D uDitherTex;
uniform vec2 uDitherSize;
uniform vec2 uDitherOffset;
uniform bool uDisplaced;
uniform sampler2D uDisplacement;
uniform vec2 uDisplaceScale;
out vec4 oColor;
// The modes that read the picture so far (psSprite_<Mode>_Normal, Mware's pipeline mode + 1): s the
// sprite's colour, d the target's under the pixel; the result goes to the target clamped (UNORM),
// a NaN as 0, and is mixed in by the source alpha.
float either(bool low, float a, float b) { return low ? a : b; }
vec3 mixMode(int mode, vec3 s, vec3 d) {
    vec3 f = s;
    if (mode == 6 || mode == 12) {  // overlay (by d) / hard light (by s)
        vec3 by = mode == 6 ? d : s;
        vec3 hi = 1.0 - 2.0 * (1.0 - d) * (1.0 - s);
        for (int i = 0; i < 3; ++i) f[i] = either(by[i] < 0.5, 2.0 * d[i] * s[i], hi[i]);
    } else if (mode == 7) {  // colour dodge
        f = d / (1.0 - s);
    } else if (mode == 8) {  // linear dodge
        f = s + d;
    } else if (mode == 9) {  // colour burn
        f = 1.0 - (1.0 - d) / s;
    } else if (mode == 10) {  // linear burn
        f = s + d - 1.0;
    } else if (mode == 11) {  // soft light: d^(2(1 - s)) below a half, d^(0.5 / s) above
        for (int i = 0; i < 3; ++i) {
            float l = log2(abs(d[i]));
            f[i] = either(s[i] < 0.5, exp2(2.0 * (1.0 - s[i]) * l), exp2(l * (0.5 / s[i])));
        }
    } else if (mode == 13) {  // vivid light
        for (int i = 0; i < 3; ++i) {
            float lo = 1.0 - 2.0 * s[i], hi = 2.0 - 2.0 * s[i];
            f[i] = either(s[i] < 0.5, lo >= d[i] ? 0.0 : (d[i] - lo) / (2.0 * s[i]), d[i] < hi ? d[i] / hi : 1.0);
        }
    } else if (mode == 14) {  // linear light
        for (int i = 0; i < 3; ++i) {
            float lo = 1.0 - 2.0 * s[i], hi = 2.0 - 2.0 * s[i], l = 2.0 * s[i] + d[i] - 1.0;
            f[i] = either(s[i] < 0.5, lo >= d[i] ? 0.0 : l, d[i] < hi ? l : 1.0);
        }
    } else if (mode == 15) {  // pin light
        for (int i = 0; i < 3; ++i) f[i] = either(s[i] < 0.5, min(d[i], s[i] + s[i]), max(2.0 * s[i] - 1.0, d[i]));
    } else if (mode == 16) {  // hard mix
        vec3 x = s + d;
        for (int i = 0; i < 3; ++i) f[i] = x[i] < 1.0 ? 0.0 : x[i];
    } else if (mode == 17) {
        f = min(s, d);
    } else if (mode == 18) {
        f = max(s, d);
    } else if (mode == 19) {
        f = abs(d - s);
    } else if (mode == 20) {
        f = -(s * d) * 2.0 + (s + d);
    }
    for (int i = 0; i < 3; ++i) f[i] = isnan(f[i]) ? 0.0 : clamp(f[i], 0.0, 1.0);
    return f;
}
// Mware's rule ramp (psUpdateTransitionRule, written for each of the table's 256 texels u; with no
// softness psUpdateTransitionRule_Rate0: now >= u), the steps as the shader takes them.
float ramp(float u) {
    if (uTransRate == 0.0) return uTransNow >= u ? 1.0 : 0.0;
    float k = 1.0 / uTransRate;
    k = k + k;
    float a = 1.0 / k + 1.0;
    a = -uTransNow * a + 1.0;
    float b = k * uTransNow;
    return clamp(u * (-k * a - b) + b, 0.0, 1.0);
}
// The table sampled at the rule's value with CSprite2D's point / clamp sampler (+0xfc, slot 4): the
// texel the value falls in.
float rule(float r) {
    return ramp((clamp(floor(r * 256.0), 0.0, 255.0) + 0.5) / 256.0);
}
void main() {
    // A displacement map (psSprite_*_DisplacementMap*): its red and blue where the pixel is on the
    // target, x 2 - 1, times the map's scale, move the texture coordinate.
    vec2 uv = vUv;
    if (uDisplaced) {
        vec4 m = texture(uDisplacement, gl_FragCoord.xy / uSize);
        uv += (vec2(m.r, m.b) * 2.0 - 1.0) * uDisplaceScale;
    }
    vec4 c = uGlyph ? vec4(vColor.rgb, vColor.a * texture(uTex, uv).r)
                    : (uTextured ? texture(uTex, uv) : vec4(1.0)) * vColor;
    // A transition: the rule picture where the pixel is on the screen (the back buffer is the screen,
    // row 0 at the top as the picture's), its red through the ramp, scales the alpha.
    if (uTransition) c.a *= rule(texture(uRule, gl_FragCoord.xy / uSize).r);
    // A mask: its texture where the pixel is on its target times the colour, its alpha (or 1 - it)
    // times the alpha (psSprite_*_Mask).
    if (uMasked) {
        vec4 m = texture(uMask, gl_FragCoord.xy / uSize);
        c = vec4(c.rgb * m.rgb, c.a * (uMaskInverse ? 1.0 - m.a : m.a));
    }
    // The texture shaders of modes 0 .. 3 (psSprite_Blend_*: Mware's technique 2 for them, 0x10016190;
    // the other modes have shaders of their own, primitives none of this) drop a pixel whose alpha is
    // below 1/255.
    if (uMode >= 0 && uMode <= 3 && (uTextured || uGlyph) && c.a < 0.00392157) discard;
    // Dither (setDither: g_DitherEnable, g_DitherPatternScale): the pattern's red where the pixel is
    // on its target (the pattern repeating from the top-left, moved by the offset; point sampled);
    // the pixel is dropped where its alpha is below it, else drawn opaque. Multiply and screen mix
    // the colour by the alpha from before.
    float a = c.a;
    if (uDither) {
        if (a < texture(uDitherTex, (gl_FragCoord.xy + uDitherOffset) / uDitherSize).r) discard;
        c.a = 1.0;
    }
    if (uMode == 4) c.rgb = mix(vec3(1.0), c.rgb, a);
    else if (uMode == 5) c.rgb = c.rgb * a;
    else if (uMode >= 6 && uMode <= 20) c.rgb = mixMode(uMode, c.rgb, texelFetch(uDst, ivec2(gl_FragCoord.xy), 0).rgb);
    oColor = c;
}
)";

const char* kPresentVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    vUv = aUv;
}
)";

const char* kPresentFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() {
    oColor = vec4(texture(uTex, vUv).rgb, 1.0);
}
)";

// The texture filters (Mware's CTextureFilter and CPostEffectDX11; section 5o): each pass draws a
// triangle over a buffer the texture's size and reads the previous picture; uv as Direct3D's (row 0
// of the texture at v 0). The maths are those of shader/PostEffect/ps*.cso, read from their DXBC.
const char* kFilterVs = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) << 1), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// psRipple: pixels pushed along the line from the centre by a sine of the distance, damped inside
// the first quarter wave.
const char* kRippleFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uAspect;
uniform float uPhase, uFrequency, uPower;
uniform vec2 uCenter;
uniform int uBorder;
out vec4 oColor;
void main() {
    vec2 d = (uCenter - vUv) * uAspect;
    float r2 = dot(d, d);
    float r = sqrt(r2);
    vec2 n = r2 > 0.0 ? d * inversesqrt(r2) : vec2(0.0);
    float k = r * uFrequency;
    vec2 off = n * sin(k - uPhase) * uPower;
    float s = sin(k);
    off *= max(abs(k) >= 1.5708 ? 1.0 : 0.0, s * s);
    vec2 uv = vUv + off / uAspect;
    bool outside = uBorder == 1 && (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0);
    oColor = outside ? vec4(0.0) : texture(uTex, uv);
}
)";

// psRaster: pixels pushed along uDir.xy by a sine travelling along uDir.zw. Ripple and raster read
// through a border sampler (0 outside the texture: uBorder) or a wrapping one.
const char* kRasterFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uAspect;
uniform float uPhase, uFrequency, uPower;
uniform vec4 uDir;
uniform int uBorder;
out vec4 oColor;
void main() {
    float p = dot(uDir.zw, vUv) * uFrequency + uPhase;
    vec2 uv = vUv + sin(p) * uPower * uDir.xy / uAspect;
    bool outside = uBorder == 1 && (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0);
    oColor = outside ? vec4(0.0) : texture(uTex, uv);
}
)";

// psLens: inside the radius, pixels pulled towards the centre by the sphere's height there times
// the distance.
const char* kLensFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uAspect;
uniform float uDistance, uRadius;
uniform vec2 uCenter;
out vec4 oColor;
void main() {
    vec2 d = (vUv - uCenter) * uAspect;
    float h = sqrt(max(uRadius * uRadius - dot(d, d), 0.0));
    float l2 = dot(d, d) + h * h;
    vec2 off = l2 > 0.0 ? -d * inversesqrt(l2) * max(h * uDistance, 0.0) : vec2(0.0);
    oColor = texture(uTex, vUv + off / uAspect);
}
)";

// psRadialBlur / psDirectionalBlur: ten taps along a step (towards the centre, as long as the
// distance times the power, in texels; or the direction), weights 0.19, 0.17 .. 0.01.
const char* kBlurFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform int uRadial;
uniform vec2 uCenter;
uniform float uPower;
uniform vec2 uInvTexSize;
uniform vec2 uStep;
out vec4 oColor;
void main() {
    vec2 st = uStep;
    if (uRadial == 1) {
        vec2 dd = uCenter - vUv;
        float l2 = dot(dd, dd);
        st = l2 > 0.0 ? dd * inversesqrt(l2) * uInvTexSize * (sqrt(l2) * uPower) : vec2(0.0);
    }
    vec4 c = texture(uTex, vUv) * 0.19;
    c += texture(uTex, vUv + st) * 0.17;
    c += texture(uTex, vUv + st * 2.0) * 0.15;
    c += texture(uTex, vUv + st * 3.0) * 0.13;
    c += texture(uTex, vUv + st * 4.0) * 0.11;
    c += texture(uTex, vUv + st * 5.0) * 0.09;
    c += texture(uTex, vUv + st * 6.0) * 0.07;
    c += texture(uTex, vUv + st * 7.0) * 0.05;
    c += texture(uTex, vUv + st * 8.0) * 0.03;
    c += texture(uTex, vUv + st * 9.0) * 0.01;
    oColor = c;
}
)";

// The colour filters (psNegaposi, psMonochrome, psSepia, psColorLevel, psHSV). Their luma takes
// (0.114, 0.587, 0.299) of (r, g, b), as the shaders do.
const char* kColorFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform int uKind;    // 0 negaposi, 1 monochrome, 2 sepia, 3 colour level, 4 HSV
uniform vec4 uParam;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv);
    vec3 o = c.rgb;
    float y = dot(c.rgb, vec3(0.114, 0.587, 0.299));
    if (uKind == 0) {
        o = uParam.x * (c.rgb * -2.0 + 1.0) + c.rgb;
    } else if (uKind == 1) {
        vec3 m = vec3(y + 1.402 * uParam.z, y - 0.34414 * uParam.y - 0.71414 * uParam.z, y + 1.772 * uParam.y);
        o = uParam.x * (m - c.rgb) + c.rgb;
    } else if (uKind == 2) {
        o = uParam.x * (vec3(y + 0.117647, y + 0.117647, y) - c.rgb) + c.rgb;
    } else if (uKind == 3) {
        vec3 v = max((c.rgb - uParam.x) / (uParam.y - uParam.x + 0.001), vec3(0.0));
        o = exp2(log2(v) * uParam.z);
    } else {
        float mx = max(c.r, max(c.g, c.b));
        float d = mx - min(c.r, min(c.g, c.b));
        float sat = mx != 0.0 ? d / mx : 0.0;
        vec3 t = vec3(c.g - c.b, c.b - c.r, c.r - c.g) / d + vec3(0.0, 2.0, 4.0);
        float h6 = c.r == mx ? t.x : (c.g == mx ? t.y : t.z);
        float h = h6 < 0.0 ? h6 * 0.166667 + 1.0 : h6 * 0.166667;
        vec3 hsv = clamp(vec3(h, sat, mx), 0.0, 1.0);
        float s2 = hsv.y + uParam.y;
        s2 = s2 >= 1.0 ? 1.0 : max(s2, 0.0);
        float v2 = c.a * uParam.w + hsv.z;
        if (s2 == 0.0) {
            o = vec3(v2);
        } else {
            float h2 = hsv.x + uParam.x;
            h2 = h2 >= 1.0 ? h2 - 1.0 : (0.0 >= h2 ? h2 + 1.0 : h2);
            h2 = h2 >= 1.0 ? h2 - 1.0 : h2;
            float i = floor(h2 * 6.0);
            float f = h2 * 6.0 - i;
            float p = (1.0 - s2) * v2;
            float q = (1.0 - s2 * f) * v2;
            float u = (1.0 - s2 * (1.0 - f)) * v2;
            if (i < 1.0) o = vec3(v2, u, p);
            else if (i < 2.0) o = vec3(q, v2, p);
            else if (i < 3.0) o = vec3(p, v2, u);
            else if (i < 4.0) o = vec3(p, q, v2);
            else if (i < 5.0) o = vec3(u, p, v2);
            else o = vec3(v2, p, q);
        }
        o += uParam.z;
    }
    oColor = vec4(o, c.a);
}
)";

// Shading's passes: psGaussianBlur_X / _Y (sixteen taps: weight i at −i and at +(i + 1) steps),
// psDownSample_2x2 (the 2 x 2 texels' mean, dropped where the alpha is 0) and
// psMultipleGaussianBlurCommit (the full-size blur moved towards each smaller copy by its weight).
const char* kGaussFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform float uWeight[8];
uniform vec2 uStep;
out vec4 oColor;
void main() {
    vec4 c = vec4(0.0);
    for (int i = 0; i < 8; ++i) {
        c += texture(uTex, vUv - uStep * float(i)) * uWeight[i];
        c += texture(uTex, vUv + uStep * float(i + 1)) * uWeight[i];
    }
    oColor = c;
}
)";
const char* kDownFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform vec2 uTexel;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv - uTexel) + texture(uTex, vUv + vec2(0.0, -uTexel.y)) +
             texture(uTex, vUv + vec2(-uTexel.x, 0.0)) + texture(uTex, vUv);
    if (0.0 >= c.a) discard;
    oColor = c * 0.25;
}
)";
const char* kCommitFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uTex;
uniform sampler2D uLevel0, uLevel1, uLevel2, uLevel3;
uniform vec4 uWeights;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv);
    c = uWeights.x * (texture(uLevel0, vUv) - c) + c;
    c = uWeights.y * (texture(uLevel1, vUv) - c) + c;
    c = uWeights.z * (texture(uLevel2, vUv) - c) + c;
    c = uWeights.w * (texture(uLevel3, vUv) - c) + c;
    if (0.0 >= c.a) discard;
    oColor = c;
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        fail(std::string("Diesel shader compile failed: ") + buf);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetProgramInfoLog(p, sizeof buf, nullptr, buf);
        fail(std::string("Diesel shader link failed: ") + buf);
    }
    return p;
}

GLuint makeSampler(GLint filter) {
    GLuint s = 0;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return s;
}

}  // namespace

Renderer::Renderer(int screenW, int screenH, ReadFile read, Log log, OpenFile open)
    : screenW_(screenW), screenH_(screenH), read_(std::move(read)), log_(std::move(log)) {
    if (open) movies_ = std::make_unique<diesel::MovieFrames>(std::move(open), log_);
    prog_ = link(kVs, kFs);
    progPresent_ = link(kPresentVs, kPresentFs);
    uSize_ = glGetUniformLocation(prog_, "uSize");
    uMode_ = glGetUniformLocation(prog_, "uMode");
    uTextured_ = glGetUniformLocation(prog_, "uTextured");
    uGlyph_ = glGetUniformLocation(prog_, "uGlyph");
    uTransition_ = glGetUniformLocation(prog_, "uTransition");
    uTransNow_ = glGetUniformLocation(prog_, "uTransNow");
    uTransRate_ = glGetUniformLocation(prog_, "uTransRate");
    uMasked_ = glGetUniformLocation(prog_, "uMasked");
    uMaskInverse_ = glGetUniformLocation(prog_, "uMaskInverse");
    spriteDither_ = {glGetUniformLocation(prog_, "uDither"), glGetUniformLocation(prog_, "uDitherSize"),
                     glGetUniformLocation(prog_, "uDitherOffset")};
    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "uTex"), 0);
    glUniform1i(glGetUniformLocation(prog_, "uRule"), 1);
    glUniform1i(glGetUniformLocation(prog_, "uMask"), 2);
    glUniform1i(glGetUniformLocation(prog_, "uDst"), 3);
    glUniform1i(glGetUniformLocation(prog_, "uDitherTex"), kDitherUnit);
    glUniform1i(glGetUniformLocation(prog_, "uDisplacement"), kDisplacementUnit);
    uDisplaced_ = glGetUniformLocation(prog_, "uDisplaced");
    uDisplaceScale_ = glGetUniformLocation(prog_, "uDisplaceScale");
    glUseProgram(progPresent_);
    glUniform1i(glGetUniformLocation(progPresent_, "uTex"), 0);
    progRipple_ = link(kFilterVs, kRippleFs);
    progRaster_ = link(kFilterVs, kRasterFs);
    progLens_ = link(kFilterVs, kLensFs);
    progBlur_ = link(kFilterVs, kBlurFs);
    progColor_ = link(kFilterVs, kColorFs);
    progGauss_ = link(kFilterVs, kGaussFs);
    progDown_ = link(kFilterVs, kDownFs);
    progCommit_ = link(kFilterVs, kCommitFs);
    glUseProgram(progCommit_);
    for (int i = 0; i < 4; ++i) {
        glUniform1i(glGetUniformLocation(progCommit_, ("uLevel" + std::to_string(i)).c_str()), 2 + i);
    }
    for (GLuint p : {progRipple_, progRaster_, progLens_, progBlur_, progColor_, progGauss_, progDown_, progCommit_}) {
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "uTex"), 0);
    }
    glGenVertexArrays(1, &filterVao_);
    progParticle_ = link(kParticleVs, kParticleFs);
    glUseProgram(progParticle_);
    glUniform1i(glGetUniformLocation(progParticle_, "uTex"), 0);
    uParticleCalc_ = glGetUniformLocation(progParticle_, "uCalc");
    uParticleGroup_ = glGetUniformLocation(progParticle_, "uGroup");
    particleDither_ = {glGetUniformLocation(progParticle_, "uDither"),
                       glGetUniformLocation(progParticle_, "uDitherSize"),
                       glGetUniformLocation(progParticle_, "uDitherOffset")};
    glUniform1i(glGetUniformLocation(progParticle_, "uDitherTex"), kDitherUnit);
    glGenVertexArrays(1, &particleVao_);
    glGenBuffers(1, &particleVbo_);
    glBindVertexArray(particleVao_);
    glBindBuffer(GL_ARRAY_BUFFER, particleVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(diesel::ParticleVertex),
                          reinterpret_cast<void*>(offsetof(diesel::ParticleVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(diesel::ParticleVertex),
                          reinterpret_cast<void*>(offsetof(diesel::ParticleVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(diesel::ParticleVertex),
                          reinterpret_cast<void*>(offsetof(diesel::ParticleVertex, color)));
    glBindVertexArray(0);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, r)));
    glBindVertexArray(0);

    samplerNearest_ = makeSampler(GL_NEAREST);
    samplerLinear_ = makeSampler(GL_LINEAR);
    // CSprite2D's wrapping samplers (Mware 0x100214c0: +0x108 linear / wrap, +0x100 point / wrap).
    samplerRepeat_ = makeSampler(GL_LINEAR);
    glSamplerParameteri(samplerRepeat_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(samplerRepeat_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    samplerRepeatNearest_ = makeSampler(GL_NEAREST);
    glSamplerParameteri(samplerRepeatNearest_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(samplerRepeatNearest_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // The dither pattern's sampler (Mware 0x100570b0(1, 0): point, wrap).
    samplerDither_ = makeSampler(GL_NEAREST);
    glSamplerParameteri(samplerDither_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(samplerDither_, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // The default pattern (the graphics' vtable +0x80: a 4 x 4 ordered dither, its red 15 .. 240).
    static const uint8_t kBayer[16] = {0x0f, 0x87, 0x2d, 0xa5, 0xc3, 0x4b, 0xe1, 0x69,
                                       0x3c, 0xb4, 0x1e, 0x96, 0xf0, 0x78, 0xd2, 0x5a};
    glGenTextures(1, &ditherDefault_);
    glBindTexture(GL_TEXTURE_2D, ditherDefault_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 4, 4, 0, GL_RED, GL_UNSIGNED_BYTE, kBayer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glGenTextures(1, &backTex_);
    glBindTexture(GL_TEXTURE_2D, backTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screenW_, screenH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenTextures(1, &atlas_);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    {
        std::vector<uint8_t> zero(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasSize, kAtlasSize, 0, GL_RED, GL_UNSIGNED_BYTE, zero.data());
    }
    glGenFramebuffers(1, &backFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, backFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, backTex_, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Renderer::~Renderer() {
    if (dstTex_) glDeleteTextures(1, &dstTex_);
    for (auto& [id, t] : screens_) {
        if (t.tex) glDeleteTextures(1, &t.tex);
    }
    for (auto& [id, t] : targets_) {
        if (t.tex) glDeleteTextures(1, &t.tex);
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    }
    for (auto& [id, t] : movieTextures_) {
        if (t.tex) glDeleteTextures(1, &t.tex);
    }
    for (auto& [path, t] : textures_) {
        if (t.tex) glDeleteTextures(1, &t.tex);
    }
    for (auto& [key, p] : pictures_) {
        if (p.t.tex) glDeleteTextures(1, &p.t.tex);
    }
    glDeleteFramebuffers(1, &backFbo_);
    glDeleteTextures(1, &backTex_);
    glDeleteTextures(1, &atlas_);
    if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
    if (shadowTex_) glDeleteTextures(1, &shadowTex_);
    shadowW_ = shadowH_ = 0;
    glDeleteSamplers(1, &samplerNearest_);
    glDeleteSamplers(1, &samplerLinear_);
    glDeleteSamplers(1, &samplerRepeat_);
    glDeleteSamplers(1, &samplerRepeatNearest_);
    glDeleteSamplers(1, &samplerDither_);
    glDeleteTextures(1, &ditherDefault_);
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &particleVbo_);
    glDeleteVertexArrays(1, &particleVao_);
    glDeleteProgram(progParticle_);
    glDeleteProgram(prog_);
    glDeleteProgram(progPresent_);
    for (GLuint p : {progRipple_, progRaster_, progLens_, progBlur_, progColor_, progGauss_, progDown_, progCommit_}) {
        glDeleteProgram(p);
    }
    releaseShading();
    glDeleteVertexArrays(1, &filterVao_);
    for (int i = 0; i < 2; ++i) {
        if (filterFbo_[i]) glDeleteFramebuffers(1, &filterFbo_[i]);
        if (filterTex_[i]) glDeleteTextures(1, &filterTex_[i]);
    }
}

Renderer::Texture* Renderer::texture(const std::string& path, const std::array<int, 4>* rect) {
    std::string key = path;
    if (rect) key += "\x1f" + std::to_string((*rect)[0]) + "," + std::to_string((*rect)[1]) + "," + std::to_string((*rect)[2]) + "," + std::to_string((*rect)[3]);
    auto [it, fresh] = textures_.try_emplace(key);
    Texture& t = it->second;
    t.lastUsed = draws_;
    if (!fresh) return t.failed ? nullptr : &t;
    try {
        auto data = read_(path);
        if (!data) fail("not found");
        diesel::Image img = diesel::decodeImage(*data);
        if (rect) {
            // createSubresource's texture: that part of the picture (Fitted: transparent outside it).
            const int rx = (*rect)[0], ry = (*rect)[1], rw = (*rect)[2], rh = (*rect)[3];
            if (rw <= 0 || rh <= 0) fail("empty part");
            diesel::Image part;
            part.width = rw;
            part.height = rh;
            part.rgba.assign(static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4, 0);
            for (int y = 0; y < rh; ++y) {
                int sy = ry + y;
                if (sy < 0 || sy >= img.height) continue;
                for (int x = 0; x < rw; ++x) {
                    int sx = rx + x;
                    if (sx < 0 || sx >= img.width) continue;
                    std::memcpy(&part.rgba[(static_cast<size_t>(y) * static_cast<size_t>(rw) + static_cast<size_t>(x)) * 4],
                                &img.rgba[(static_cast<size_t>(sy) * static_cast<size_t>(img.width) + static_cast<size_t>(sx)) * 4], 4);
                }
            }
            img = std::move(part);
        }
        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        t.w = img.width;
        t.h = img.height;
    } catch (const std::exception& e) {
        t.failed = true;
        if (log_) log_("texture " + path + ": " + e.what());
        return nullptr;
    }
    return &t;
}

Renderer::Texture* Renderer::pictureTexture(const std::shared_ptr<const diesel::Image>& picture) {
    auto [it, fresh] = pictures_.try_emplace(picture.get());
    PictureTexture& p = it->second;
    p.t.lastUsed = draws_;
    if (!fresh) return p.t.failed ? nullptr : &p.t;
    p.picture = picture;
    if (picture->width <= 0 || picture->height <= 0) {
        p.t.failed = true;
        return nullptr;
    }
    glGenTextures(1, &p.t.tex);
    glBindTexture(GL_TEXTURE_2D, p.t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, picture->width, picture->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 picture->rgba.data());
    p.t.w = picture->width;
    p.t.h = picture->height;
    return &p.t;
}

Renderer::MovieTexture* Renderer::movieTexture(const diesel::DrawItem& d) {
    if (!movies_) return nullptr;
    const diesel::MovieFrames::Picture* p = movies_->picture(d);
    if (!p || p->width <= 0 || p->height <= 0) return nullptr;
    MovieTexture& t = movieTextures_[d.movie];
    t.lastUsed = draws_;
    if (t.serial != p->serial) {
        if (!t.tex) glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (t.w != p->width || t.h != p->height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p->width, p->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, p->rgba.data());
            t.w = p->width;
            t.h = p->height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p->width, p->height, GL_RGBA, GL_UNSIGNED_BYTE, p->rgba.data());
        }
        t.serial = p->serial;
    }
    return &t;
}

const Renderer::GlyphSlot* Renderer::glyphSlot(const std::shared_ptr<const diesel::GlyphImage>& g) {
    if (auto it = glyphSlots_.find(g.get()); it != glyphSlots_.end()) return &it->second;
    int w = g->pitch, h = g->rows;
    if (w <= 0 || h <= 0 || w + 2 > kAtlasSize || h + 2 > kAtlasSize) return nullptr;
    if (atlasX_ + w + 1 > kAtlasSize) {
        atlasX_ = 1;
        atlasY_ += atlasRow_ + 1;
        atlasRow_ = 0;
    }
    if (atlasY_ + h + 1 > kAtlasSize) {
        // Full: start again (the glyphs still in use come back on demand).
        glyphSlots_.clear();
        atlasX_ = atlasY_ = 1;
        atlasRow_ = 0;
    }
    GlyphSlot slot{g, atlasX_, atlasY_};
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, slot.x, slot.y, w, h, GL_RED, GL_UNSIGNED_BYTE, g->alpha.data());
    atlasX_ += w + 1;
    atlasRow_ = std::max(atlasRow_, h);
    return &glyphSlots_.emplace(g.get(), std::move(slot)).first->second;
}

void Renderer::releaseIdle() {
    for (auto it = targets_.begin(); it != targets_.end();) {
        if (draws_ - it->second.lastUsed > static_cast<uint64_t>(kTextureIdleDraws)) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            if (it->second.fbo) glDeleteFramebuffers(1, &it->second.fbo);
            it = targets_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = screens_.begin(); it != screens_.end();) {
        if (draws_ - it->second.lastUsed > static_cast<uint64_t>(kTextureIdleDraws)) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            it = screens_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = movieTextures_.begin(); it != movieTextures_.end();) {
        if (draws_ - it->second.lastUsed > static_cast<uint64_t>(diesel::MovieFrames::kIdleFrames)) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            it = movieTextures_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (draws_ - it->second.lastUsed > static_cast<uint64_t>(kTextureIdleDraws)) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pictures_.begin(); it != pictures_.end();) {
        if (draws_ - it->second.t.lastUsed > static_cast<uint64_t>(kTextureIdleDraws)) {
            if (it->second.t.tex) glDeleteTextures(1, &it->second.t.tex);
            it = pictures_.erase(it);
        } else {
            ++it;
        }
    }
}

// The blend modes (BLEND_MODE_*, CSprite +0x110): 0 none, 1 normal, 2 add, 3 subtract, 4 multiply
// (psSprite_Mul: colour faded to white by alpha, times the target), 5 screen (psSprite_Screen:
// premultiplied, plus the target times one minus it). 6 .. 20 (overlay, colour dodge, linear dodge,
// colour burn, linear burn, soft / hard / vivid / linear / pin light, hard mix, darken, lighten,
// difference, exclusion) read the picture so far (psSprite_<Mode>_Normal's Texture_Destination) and
// are mixed in by their alpha.
// Mware's blend states (CSprite2D 0x10022180 -> CBlend 0x100573c0 / 0x100574c0): the colour factors
// below (modes 1 and 6 .. 20 SRC_ALPHA / INV_SRC_ALPHA; 2 SRC_ALPHA + ONE; 3 the same reversed; 4
// DEST_COLOR x 0; 5 INV_DEST_COLOR + ONE over the shader's premultiplied colour, which is the same as
// ONE + INV_SRC_COLOR here); the alpha is written only into a render target (the draw 0x10021930
// takes the second state when the target asks for it), where it adds, ONE + ONE, in every mode, and
// the screen's alpha is left as it is. Mode 0 turns blending off: red, green and blue written on the
// screen, all four into a render target, from the normal shader (the technique of modes 1 .. 3). A flag
// at CSprite2D +0xe8 (vtable +0x148) would draw every mode with blending off through the WriteAlpha
// technique (the vertex colour, the texture's alpha plus the target's); the exes only ever clear it.
void Renderer::setBlend(int mode) {
    const bool target = currentFbo_ != backFbo_;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, target ? GL_TRUE : GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case 0: glDisable(GL_BLEND); break;
    case 2: glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE); break;
    case 3:
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        break;
    case 4: glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE); break;
    case 5: glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ONE, GL_ONE); break;
    default:
        if (mode != 1 && !readsTarget(mode) && unsupportedModes_.insert(mode).second && log_) {
            log_("blend mode " + std::to_string(mode) + " drawn as normal");
        }
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
        break;
    }
}

// An item's rule picture on unit 1 and its threshold and softness, if it has a transition.
bool Renderer::bindTransition(const diesel::DrawItem& d) {
    if (d.transitionRule.empty()) return false;
    Texture* rule = texture(d.transitionRule);
    if (!rule || rule->w <= 0 || rule->h <= 0) return false;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, rule->tex);
    glBindSampler(1, samplerLinear_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(uTransNow_, d.transitionNow);
    glUniform1f(uTransRate_, d.transitionRate);
    return true;
}

bool Renderer::bindDither(const diesel::DrawItem& d, const DitherUniforms& u) {
    GLuint tex = 0;
    int w = 0, h = 0;
    if (d.dither) {
        Texture* t = d.ditherTexture.empty() ? nullptr : texture(d.ditherTexture);
        if (t && t->w > 0 && t->h > 0) {
            tex = t->tex, w = t->w, h = t->h;
        } else {
            tex = ditherDefault_, w = h = 4;
        }
    }
    glUniform1i(u.on, tex ? 1 : 0);
    if (!tex) return false;
    glActiveTexture(GL_TEXTURE0 + kDitherUnit);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(kDitherUnit, samplerDither_);
    glActiveTexture(GL_TEXTURE0);
    glUniform2f(u.size, static_cast<float>(w), static_cast<float>(h));
    glUniform2f(u.offset, d.ditherX, d.ditherY);
    return true;
}

bool Renderer::bindMask(const diesel::DrawItem& d) {
    if (!d.mask) return false;
    auto it = targets_.find(d.mask);
    if (it == targets_.end() || it->second.w <= 0) return false;
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, it->second.tex);
    glBindSampler(2, samplerLinear_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uMaskInverse_, d.maskInverse ? 1 : 0);
    return true;
}

bool Renderer::bindDisplacement(const diesel::DrawItem& d) {
    bool on = false;
    if (d.displacementMap) {
        auto it = targets_.find(d.displacementMap);
        if (it != targets_.end() && it->second.w > 0) {
            // The map with CSprite2D's point / clamp sampler (+0xfc, slot 5); the texture then with
            // the linear / clamp one (+0x104), whatever its filtering.
            glActiveTexture(GL_TEXTURE0 + kDisplacementUnit);
            glBindTexture(GL_TEXTURE_2D, it->second.tex);
            glBindSampler(kDisplacementUnit, samplerNearest_);
            glActiveTexture(GL_TEXTURE0);
            glBindSampler(0, samplerLinear_);
            glUniform2f(uDisplaceScale_, d.displacementScaleX, d.displacementScaleY);
            on = true;
        }
    }
    glUniform1i(uDisplaced_, on ? 1 : 0);
    return on;
}

bool Renderer::drawGlyph(const diesel::DrawItem& d, float dx, float dy, const float (*color)[4]) {
    if (!d.glyph) return false;
    const GlyphSlot* s = glyphSlot(d.glyph);
    if (!s) return false;
    const float n = static_cast<float>(kAtlasSize);
    float u0 = static_cast<float>(s->x) / n, v0 = static_cast<float>(s->y) / n;
    float u1 = static_cast<float>(s->x + d.glyph->pitch) / n, v1 = static_cast<float>(s->y + d.glyph->rows) / n;
    const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
    Vertex v[4];
    for (size_t i = 0; i < 4; ++i) {
        const auto& q = d.quad.corner[i];
        const float* c = color ? color[i] : d.color[i].data();
        v[i] = {q[0] + dx, q[1] + dy, q[2], us[i], vs[i], c[0], c[1], c[2], c[3]};
    }
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return true;
}

// A text's shadow (CText 0x4d92a0 / 0x4d9300 / 0x4d94a0): its target cleared to white with alpha 0,
// the glyphs drawn white at their offsets with the render target's blend (their alpha adds up,
// clamped), then the target over the frame as a sprite in the shadow's colour and alpha. The shadow's
// target is the size of the one the text is drawn on: `setSurface` makes it again at its surface's
// size (0x4d6190: the size the surface's +0xbc gives, the graphics' +0x74 making the target, +0x98
// its sprite), and the sprite covers the whole target it is drawn on (0x4d94a0: the quad from -1 to 1),
// so the shadow lies on the text's target pixel for pixel. Nyanroid keeps one buffer as large as the
// largest such target and uses its corner. The glyphs
// go through the glyph batches, whose material has the text's mask (CText::setMask 0x4d8100 hands
// it to both): the mask clips the shadow's glyphs on the shadow's target, and the sprite laid over
// the frame (+0x788, never given the mask) shows only what they left. Phantom's backlog hides the
// lines scrolled out of its window this way.
void Renderer::drawShadow(const diesel::DrawItem& d) {
    const diesel::ShadowBatch& batch = *d.shadow;
    if (!shadowTex_ || shadowW_ < currentW_ || shadowH_ < currentH_) {
        if (!shadowTex_) glGenTextures(1, &shadowTex_);
        if (!shadowFbo_) glGenFramebuffers(1, &shadowFbo_);
        shadowW_ = std::max(shadowW_, currentW_);
        shadowH_ = std::max(shadowH_, currentH_);
        glBindTexture(GL_TEXTURE_2D, shadowTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, shadowW_, shadowH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadowTex_, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, currentW_, currentH_);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(1, 1, 1, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glUniform1i(uMasked_, bindMask(d) ? 1 : 0);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    glUniform1i(uGlyph_, 1);
    glUniform1i(uTextured_, 0);
    glUniform1i(uMode_, 1);
    glUniform1i(uTransition_, 0);
    glBindSampler(0, samplerNearest_);
    for (size_t i = 0; i < batch.glyphs.size(); ++i) {
        const auto& g = batch.glyphs[i];
        float white[4][4];
        for (size_t k = 0; k < 4; ++k) white[k][0] = white[k][1] = white[k][2] = 1.0f, white[k][3] = g.color[k][3];
        // The glyph batches draw the shadow with their material, a transition's rule and dither
        // included.
        glUniform1i(uTransition_, bindTransition(g) ? 1 : 0);
        bindDither(g, spriteDither_);
        if (!bindDisplacement(g)) glBindSampler(0, samplerNearest_);
        if (i < batch.offsets.size()) {
            for (const auto& o : batch.offsets[i]) drawGlyph(g, o[0], o[1], white);
        }
    }
    glUniform1i(uTransition_, 0);
    glUniform1i(uDisplaced_, 0);
    glUniform1i(uMasked_, 0);
    // The shadow's sprite (CText +0x788) has the text's dither too.
    bindDither(d, spriteDither_);
    glBindFramebuffer(GL_FRAMEBUFFER, currentFbo_);
    glViewport(0, 0, currentW_, currentH_);
    setBlend(1);
    glUniform1i(uGlyph_, 0);
    glUniform1i(uTextured_, 1);
    glBindTexture(GL_TEXTURE_2D, shadowTex_);
    // The corner of the buffer that is the target's size, laid over the whole target.
    const float w = static_cast<float>(currentW_), h = static_cast<float>(currentH_);
    const float su = w / static_cast<float>(shadowW_), sv = h / static_cast<float>(shadowH_);
    const auto& c = batch.color;
    Vertex v[4] = {{0, 0, 1, 0, 0, c[0], c[1], c[2], c[3]},
                   {w, 0, 1, su, 0, c[0], c[1], c[2], c[3]},
                   {0, h, 1, 0, sv, c[0], c[1], c[2], c[3]},
                   {w, h, 1, su, sv, c[0], c[1], c[2], c[3]}};
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUniform1i(spriteDither_.on, 0);
}

// The picture under an item, copied from the target being drawn into the destination texture at
// the same place (its quad's bounds), for the modes that read it.
void Renderer::copyDestination(const diesel::DrawItem& d) {
    if (!dstTex_ || dstW_ < currentW_ || dstH_ < currentH_) {
        if (!dstTex_) glGenTextures(1, &dstTex_);
        dstW_ = std::max(dstW_, currentW_);
        dstH_ = std::max(dstH_, currentH_);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, dstTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dstW_, dstH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& c : d.quad.corner) {
        x0 = std::min(x0, c[0]), y0 = std::min(y0, c[1]);
        x1 = std::max(x1, c[0]), y1 = std::max(y1, c[1]);
    }
    int ix0 = std::clamp(static_cast<int>(std::floor(x0)) - 1, 0, currentW_);
    int iy0 = std::clamp(static_cast<int>(std::floor(y0)) - 1, 0, currentH_);
    int ix1 = std::clamp(static_cast<int>(std::ceil(x1)) + 1, 0, currentW_);
    int iy1 = std::clamp(static_cast<int>(std::ceil(y1)) + 1, 0, currentH_);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, dstTex_);
    if (ix1 > ix0 && iy1 > iy0) glCopyTexSubImage2D(GL_TEXTURE_2D, 0, ix0, iy0, ix0, iy0, ix1 - ix0, iy1 - iy0);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::bindTarget(GLuint fbo, int w, int h) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // setBlend masks the screen's alpha per item
    currentFbo_ = fbo;
    currentW_ = w;
    currentH_ = h;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glUniform2f(uSize_, static_cast<float>(w), static_cast<float>(h));
}

Renderer::TargetTexture* Renderer::targetTexture(const diesel::RenderTarget& t) {
    TargetTexture& tt = targets_[t.id];
    tt.lastUsed = draws_;
    if (tt.w != t.width || tt.h != t.height) {
        if (!tt.tex) glGenTextures(1, &tt.tex);
        if (!tt.fbo) glGenFramebuffers(1, &tt.fbo);
        glBindTexture(GL_TEXTURE_2D, tt.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, tt.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tt.tex, 0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        tt.w = t.width;
        tt.h = t.height;
    }
    return &tt;
}

void Renderer::draw(const std::vector<diesel::DrawItem>& items) { draw(items, {}); }

// The frame: each render target and mask drawn into its texture first, a target before whatever
// shows it or is masked by it (the task's draw orders them so, 0x4861b0), then the screen; a delayed
// target (RenderTarget::delayed) is drawn when the screen's pass reaches the item showing it. A
// capture on the screen takes the screen as drawn so far when its turn comes (captureNow); one drawn
// into a target, or turned off, keeps what it had; a picture loaded from a save, and the app's screen
// fade's copy of the last frame, are put in before the frame is drawn (captureScreens).
Renderer::ScreenTexture& Renderer::screenTexture(const diesel::DrawItem& d) {
    ScreenTexture& st = screens_[d.screen];
    st.lastUsed = draws_;
    if (!st.tex) {
        glGenTextures(1, &st.tex);
        glBindTexture(GL_TEXTURE_2D, st.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screenW_, screenH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    return st;
}

void Renderer::captureScreens(const std::vector<diesel::DrawItem>& items) {
    for (const auto& d : items) {
        if (d.kind != diesel::DrawItem::Kind::Screen || !d.screen) continue;
        ScreenTexture& st = screenTexture(d);
        if (d.takeLastFrame) {
            // The last frame as shown (the back buffer before this frame is drawn).
            glBindFramebuffer(GL_FRAMEBUFFER, backFbo_);
            glBindTexture(GL_TEXTURE_2D, st.tex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, screenW_, screenH_);
            st.taken = true;
            st.picture = nullptr;
            continue;
        }
        if (!d.capture && d.screenPicture && d.screenPicture.get() != st.picture && d.screenPicture->width == screenW_ &&
            d.screenPicture->height == screenH_) {
            // The picture's rows are from the top; the captures' textures hold the bottom row first.
            glBindTexture(GL_TEXTURE_2D, st.tex);
            const size_t row = static_cast<size_t>(screenW_) * 4;
            std::vector<uint8_t> flipped(d.screenPicture->rgba.size());
            for (int y = 0; y < screenH_; ++y) {
                std::memcpy(flipped.data() + static_cast<size_t>(y) * row,
                            d.screenPicture->rgba.data() + static_cast<size_t>(screenH_ - 1 - y) * row, row);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, screenW_, screenH_, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
            st.taken = true;
            st.picture = d.screenPicture.get();
        }
    }
}

// SQSprite vtable +0x7c (0x50d4b0): the device copies the screen as it stands into the capture's
// texture (`current` the framebuffer being drawn into, bound again after).
void Renderer::captureNow(const diesel::DrawItem& d, GLuint current) {
    ScreenTexture& st = screenTexture(d);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, backFbo_);
    glBindTexture(GL_TEXTURE_2D, st.tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, screenW_, screenH_);
    st.taken = true;
    st.picture = nullptr;
    glBindFramebuffer(GL_FRAMEBUFFER, current);
}

bool Renderer::readScreen(uint32_t screen, int width, int height, std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0) return false;
    GLuint fbo = backFbo_, tmp = 0;
    auto it = screens_.find(screen);
    if (screen && it != screens_.end() && it->second.taken) {
        glGenFramebuffers(1, &tmp);
        glBindFramebuffer(GL_FRAMEBUFFER, tmp);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, it->second.tex, 0);
        fbo = tmp;
    }
    bool ok = readFramebuffer(fbo, screenW_, screenH_, width, height, rgba);
    if (tmp) glDeleteFramebuffers(1, &tmp);
    return ok;
}

bool Renderer::readTarget(uint32_t target, int width, int height, std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0) return false;
    auto it = targets_.find(target);
    if (it == targets_.end() || !it->second.fbo || it->second.w <= 0 || it->second.h <= 0) return false;
    return readFramebuffer(it->second.fbo, it->second.w, it->second.h, width, height, rgba);
}

bool Renderer::readFramebuffer(GLuint fbo, int screenW, int screenH, int width, int height, std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> full(static_cast<size_t>(screenW) * static_cast<size_t>(screenH) * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, screenW, screenH, GL_RGBA, GL_UNSIGNED_BYTE, full.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The device's stretch copy (linear: Fitted), at pixel centres. The read rows begin at the
    // bottom of the picture.
    rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    for (int y = 0; y < height; ++y) {
        float fy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(screenH) / static_cast<float>(height) - 0.5f,
                              0.0f, static_cast<float>(screenH - 1));
        int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, screenH - 1);
        float ty = fy - static_cast<float>(y0);
        y0 = screenH - 1 - y0, y1 = screenH - 1 - y1;
        for (int x = 0; x < width; ++x) {
            float fx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(screenW) / static_cast<float>(width) - 0.5f,
                                  0.0f, static_cast<float>(screenW - 1));
            int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, screenW - 1);
            float tx = fx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int xx, int yy) {
                    return static_cast<float>(full[(static_cast<size_t>(yy) * static_cast<size_t>(screenW) + static_cast<size_t>(xx)) * 4 +
                                                   static_cast<size_t>(c)]);
                };
                float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                rgba[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::clamp(top + (bottom - top) * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return true;
}

void Renderer::draw(const std::vector<diesel::DrawItem>& items, const std::vector<diesel::RenderTarget>& targets) {
    ++draws_;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    captureScreens(items);
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glActiveTexture(GL_TEXTURE0);
    std::map<uint32_t, const diesel::RenderTarget*> byId;
    for (const auto& t : targets) {
        if (t.width <= 0 || t.height <= 0) continue;
        byId[t.id] = &t;
        targetTexture(t);
    }
    std::map<uint32_t, std::vector<const diesel::DrawItem*>> lists;
    for (const auto& d : items) lists[d.target].push_back(&d);
    std::vector<uint32_t> order;
    std::map<uint32_t, int> state;  // 1 visiting (a loop is cut there), 2 done
    std::function<void(uint32_t)> visit = [&](uint32_t id) {
        if (state[id]) return;
        state[id] = 1;
        auto need = [&](uint32_t dep) {
            if (dep && dep != id && byId.count(dep)) visit(dep);
        };
        if (auto it = byId.find(id); it != byId.end()) need(it->second->copyFrom);
        if (auto it = lists.find(id); it != lists.end()) {
            for (const auto* d : it->second) {
                if (d->kind == diesel::DrawItem::Kind::Target) need(d->renderTarget);
                need(d->mask);
                need(d->displacementMap);
            }
        }
        state[id] = 2;
        if (id) order.push_back(id);
    };
    visit(0);
    for (const auto& [id, t] : byId) visit(id);
    static const std::vector<const diesel::DrawItem*> kNone;
    auto drawTarget = [&](uint32_t id) {
        const diesel::RenderTarget& t = *byId[id];
        TargetTexture& tt = targets_[id];
        bindTarget(tt.fbo, tt.w, tt.h);
        if (t.clear || (t.draw && t.reset)) {
            glClearColor(t.clearColor[0], t.clearColor[1], t.clearColor[2], t.clearColor[3]);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        if (t.copyFrom) {
            auto src = targets_.find(t.copyFrom);
            if (src != targets_.end() && src->second.fbo) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, src->second.fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tt.fbo);
                glBlitFramebuffer(0, 0, src->second.w, src->second.h, 0, 0, tt.w, tt.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                bindTarget(tt.fbo, tt.w, tt.h);
            }
        }
        if (t.draw) {
            auto it = lists.find(id);
            drawList(it != lists.end() ? it->second : kNone);
        }
    };
    for (uint32_t id : order) {
        if (!byId[id]->delayed) drawTarget(id);
    }
    bindTarget(backFbo_, screenW_, screenH_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // The screen's pass (0x47ed90, the screen's level): before a capture is drawn it takes the screen
    // as drawn so far; before the item showing a delayed target the target is drawn.
    auto screen = lists.find(0);
    std::vector<const diesel::DrawItem*> chunk;
    std::set<uint32_t> drawnLate;
    auto flush = [&]() {
        if (!chunk.empty()) drawList(chunk);
        chunk.clear();
    };
    for (const auto* d : screen != lists.end() ? screen->second : kNone) {
        if (d->kind == diesel::DrawItem::Kind::Target && byId.count(d->renderTarget) && byId[d->renderTarget]->delayed &&
            drawnLate.insert(d->renderTarget).second) {
            flush();
            drawTarget(d->renderTarget);
            bindTarget(backFbo_, screenW_, screenH_);
        }
        if (d->kind == diesel::DrawItem::Kind::Screen && d->capture && d->screen) {
            flush();
            captureNow(*d, backFbo_);
        }
        chunk.push_back(d);
    }
    flush();
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindSampler(2, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (movies_) movies_->endFrame();
    if (draws_ % 60 == 0) releaseIdle();
}

// Shading (CTextureFilter 0x1002b110): the picture and four copies of it, each half the size of the
// one before (2 x 2 means), are blurred across and then down (CPostEffectDX11 0x10027150: the eight
// weights exp(−0.02 (2i + 1)²) over both sides, the step the power over that copy's size); the
// full-size blur then moves towards each copy by its weight. A copy counts when its weight, from
// the level's spread and factor (SHADE_LEVEL 0, 2, 4: 3 and 0.05625, 5 and 0.15625, 10 and 0.625),
// the density x 256 and the power, is at least 1 / 255 of its range. The result goes on as the
// filtered picture (`next` the buffer the next pass draws into).
GLuint Renderer::shade(GLuint src, int w, int h, const diesel::TextureFilter& f, int& next) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (shadeW_ != w || shadeH_ != h) {
        releaseShading();
        auto make = [](ShadeBuffer& b, int bw, int bh) {
            b.w = std::max(1, bw), b.h = std::max(1, bh);
            glGenTextures(1, &b.tex);
            glBindTexture(GL_TEXTURE_2D, b.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, b.w, b.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glGenFramebuffers(1, &b.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b.tex, 0);
        };
        make(shadeFull_, w, h);
        for (int k = 0; k < 4; ++k) {
            make(shadeA_[k], w >> (k + 1), h >> (k + 1));
            make(shadeB_[k], w >> (k + 1), h >> (k + 1));
        }
        shadeW_ = w, shadeH_ = h;
    }
    // The copies' weights.
    float spread = 0.0f, factor = 1.0f;
    if (f.shadingLevel == 0) spread = 3.0f, factor = 0.0562499985f;
    else if (f.shadingLevel == 2) spread = 5.0f, factor = 0.15625f;
    else if (f.shadingLevel == 4) spread = 10.0f, factor = 0.625f;
    const float s2 = spread * spread;
    const float t = factor * f.shadingDensity * f.shadingPower * s2;
    const float range[4] = {s2 * 4.0f, s2 * 16.0f, s2 * 64.0f, s2 * 256.0f};
    const float scale[4] = {3.0f * 0.25f, 15.0f * 0.0625f, 63.0f * 0.015625f, 255.0f * 0.00390625f};
    float weight[4];
    bool on[4];
    for (int k = 0; k < 4; ++k) {
        float r = std::min(t, range[k]) / range[k];
        on[k] = 0.00392156886f <= r * scale[k];  // false for a 0 range (NaN)
        weight[k] = on[k] ? r : 0.0f;
    }
    // The kernel (the parameters' constructor, 0x1002abf0).
    float kernel[8], sum = 0.0f;
    for (int i = 0; i < 8; ++i) {
        float x = static_cast<float>(i) + static_cast<float>(i) + 1.0f;
        kernel[i] = std::exp(x * x * -0.0199999996f);
        sum += kernel[i];
    }
    for (float& k : kernel) k *= 1.0f / (sum + sum);

    auto bindTo = [](GLuint fbo, int bw, int bh) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, bw, bh);
    };
    auto clear = [](GLuint fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    clear(shadeFull_.fbo);
    for (int k = 0; k < 4; ++k) {
        if (on[k]) clear(shadeA_[k].fbo), clear(shadeB_[k].fbo);
    }
    glBindVertexArray(filterVao_);
    glActiveTexture(GL_TEXTURE0);
    auto draw = [] { glDrawArrays(GL_TRIANGLES, 0, 3); };
    // The copies: the first from the picture, each next from the one before.
    glUseProgram(progDown_);
    GLint texel = glGetUniformLocation(progDown_, "uTexel");
    for (int k = 0; k < 4; ++k) {
        if (!on[k]) continue;
        GLuint from = k == 0 ? src : shadeA_[k - 1].tex;
        int fw = k == 0 ? w : shadeA_[k - 1].w, fh = k == 0 ? h : shadeA_[k - 1].h;
        bindTo(shadeA_[k].fbo, shadeA_[k].w, shadeA_[k].h);
        glBindTexture(GL_TEXTURE_2D, from);
        glBindSampler(0, samplerNearest_);
        glUniform2f(texel, 1.0f / static_cast<float>(fw), 1.0f / static_cast<float>(fh));
        draw();
    }
    // The blurs: across, then down.
    glUseProgram(progGauss_);
    glUniform1fv(glGetUniformLocation(progGauss_, "uWeight"), 8, kernel);
    GLint step = glGetUniformLocation(progGauss_, "uStep");
    auto blur = [&](GLuint from, int fw, int fh, GLuint fbo, bool down) {
        bindTo(fbo, fw, fh);
        glBindTexture(GL_TEXTURE_2D, from);
        glBindSampler(0, samplerLinear_);
        if (down) glUniform2f(step, 0.0f, f.shadingPower / static_cast<float>(fh));
        else glUniform2f(step, f.shadingPower / static_cast<float>(fw), 0.0f);
        draw();
    };
    GLuint across = filterTex_[next];
    blur(src, w, h, filterFbo_[next], false);
    next ^= 1;
    for (int k = 3; k >= 0; --k) {
        if (on[k]) blur(shadeA_[k].tex, shadeA_[k].w, shadeA_[k].h, shadeB_[k].fbo, false);
    }
    GLuint full = filterTex_[next];
    blur(across, w, h, filterFbo_[next], true);
    next ^= 1;
    for (int k = 3; k >= 0; --k) {
        if (on[k]) blur(shadeB_[k].tex, shadeB_[k].w, shadeB_[k].h, shadeA_[k].fbo, true);
    }
    // The commit into the cleared full-size buffer.
    glUseProgram(progCommit_);
    bindTo(shadeFull_.fbo, w, h);
    for (int k = 0; k < 4; ++k) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE2 + k));
        glBindTexture(GL_TEXTURE_2D, shadeA_[k].tex);
        glBindSampler(static_cast<GLuint>(2 + k), samplerLinear_);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, full);
    glBindSampler(0, samplerNearest_);
    glUniform4f(glGetUniformLocation(progCommit_, "uWeights"), weight[0], weight[1], weight[2], weight[3]);
    draw();
    for (int k = 0; k < 4; ++k) glBindSampler(static_cast<GLuint>(2 + k), 0);
    return shadeFull_.tex;
}

void Renderer::releaseShading() {
    auto drop = [](ShadeBuffer& b) {
        if (b.tex) glDeleteTextures(1, &b.tex);
        if (b.fbo) glDeleteFramebuffers(1, &b.fbo);
        b = {};
    };
    drop(shadeFull_);
    for (int k = 0; k < 4; ++k) drop(shadeA_[k]), drop(shadeB_[k]);
    shadeW_ = shadeH_ = 0;
}

// The material's filter chain (0x1002c440) on a texture: each active filter in its order draws the
// picture so far into the next of two buffers the texture's size, with the constants
// CPostEffectDX11 works out (ripple 0x10028240, raster 0x100283d0, radial blur 0x10028d00,
// directional blur 0x10029050, lens 0x100285d0); the last buffer is what the sprite draws. The
// exe filters again only when a parameter changes; Nyanroid every frame it is drawn.
GLuint Renderer::applyFilter(GLuint src, int w, int h, const diesel::TextureFilter& f) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (w <= 0 || h <= 0) return src;
    if (filterW_ != w || filterH_ != h) {
        for (int i = 0; i < 2; ++i) {
            if (!filterTex_[i]) glGenTextures(1, &filterTex_[i]);
            if (!filterFbo_[i]) glGenFramebuffers(1, &filterFbo_[i]);
            glBindTexture(GL_TEXTURE_2D, filterTex_[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, filterFbo_[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, filterTex_[i], 0);
        }
        filterW_ = w, filterH_ = h;
    }
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float invH = 1.0f / fh;
    constexpr float kTwoPi = 6.28318548f;            // 0x1019f674
    constexpr float kFrequency = 0.0490873866f;      // 0x1019f560 (pi / 64)
    constexpr float kPower = 128.0f;                 // 0x1019f6cc
    glDisable(GL_BLEND);
    glBindVertexArray(filterVao_);
    glViewport(0, 0, w, h);
    GLuint picture = src;
    int next = 0;
    auto pass = [&](GLuint prog, bool displaces = false) {
        glBindFramebuffer(GL_FRAMEBUFFER, filterFbo_[next]);
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, picture);
        glBindSampler(0, displaces && f.wrap ? samplerRepeat_ : samplerLinear_);
        if (displaces) glUniform1i(glGetUniformLocation(prog, "uBorder"), f.wrap ? 0 : 1);
    };
    auto done = [&] {
        glDrawArrays(GL_TRIANGLES, 0, 3);
        picture = filterTex_[next];
        next ^= 1;
    };
    auto u = [](GLuint p, const char* n) { return glGetUniformLocation(p, n); };
    if (f.ripple()) {
        pass(progRipple_, true);
        glUniform2f(u(progRipple_, "uAspect"), fw * invH, 1.0f);
        glUniform1f(u(progRipple_, "uPhase"), f.ripplePhase * kTwoPi);
        glUniform1f(u(progRipple_, "uFrequency"), f.rippleFrequency * kFrequency * fh);
        glUniform1f(u(progRipple_, "uPower"), f.ripplePower * 0.1f * invH * kPower);  // x 0.1: 0x1002c050
        glUniform2f(u(progRipple_, "uCenter"), f.rippleCenter[0] / fw, f.rippleCenter[1] / fh);
        done();
    }
    if (f.raster()) {
        pass(progRaster_, true);
        // (1, 0, 0) and (0, 1, 0) turned by the rotation (Mware's Vector3D::RotationZ, degrees).
        float a = f.rasterRotate * 0.0174532924f;
        float c = std::cos(a), sn = std::sin(a);
        glUniform2f(u(progRaster_, "uAspect"), fw * invH, 1.0f);
        glUniform1f(u(progRaster_, "uPhase"), f.rasterPhase * kTwoPi);
        glUniform1f(u(progRaster_, "uFrequency"), f.rasterFrequency * kFrequency * fh);
        glUniform1f(u(progRaster_, "uPower"), invH * f.rasterPower * kPower);
        glUniform4f(u(progRaster_, "uDir"), c, sn, -sn, c);
        done();
    }
    if (f.radial()) {
        pass(progBlur_);
        glUniform1i(u(progBlur_, "uRadial"), 1);
        glUniform2f(u(progBlur_, "uCenter"), f.radialCenter[0] / fw, f.radialCenter[1] / fh);
        glUniform1f(u(progBlur_, "uPower"), f.radialPower);
        glUniform2f(u(progBlur_, "uInvTexSize"), 1.0f / fw, invH);
        done();
    }
    if (f.directionalBlur()) {
        pass(progBlur_);
        glUniform1i(u(progBlur_, "uRadial"), 0);
        glUniform2f(u(progBlur_, "uStep"), f.directional[0] / fw * f.directionalPower,
                    f.directional[1] * invH * f.directionalPower);
        done();
    }
    if (f.shading()) {
        picture = shade(picture, w, h, f, next);
        glBindVertexArray(filterVao_);
        glViewport(0, 0, w, h);
    }
    auto color = [&](int kind, float a, float b, float c, float d) {
        pass(progColor_);
        glBindSampler(0, samplerNearest_);  // the post effect's point sampler (CPostEffectDX11 +0x588)
        glUniform1i(u(progColor_, "uKind"), kind);
        glUniform4f(u(progColor_, "uParam"), a, b, c, d);
        done();
    };
    if (f.negaposiOn()) color(0, f.negaposi, 0.0f, 0.0f, 0.0f);
    if (f.monochromeOn()) color(1, f.monochrome[0], f.monochrome[1], f.monochrome[2], 0.0f);
    if (f.sepiaOn()) color(2, f.sepia, 0.0f, 0.0f, 0.0f);
    // The colour level's power within 0.1 .. 10 (0x10028e40).
    if (f.colorLevelOn()) color(3, f.colorLevel[0], f.colorLevel[1], std::clamp(f.colorLevel[2], 0.1f, 10.0f), 0.0f);
    if (f.hsvOn()) color(4, f.hsv[0], f.hsv[1], f.hsv[2], f.hsv[3]);
    if (f.lens()) {
        pass(progLens_);
        glUniform2f(u(progLens_, "uAspect"), fw * invH, 1.0f);
        glUniform1f(u(progLens_, "uDistance"), f.lensDistance);
        glUniform1f(u(progLens_, "uRadius"), f.lensRadius * invH);
        glUniform2f(u(progLens_, "uCenter"), f.lensCenter[0] / fw, f.lensCenter[1] / fh);
        done();
    }
    // Back to the frame's state.
    glBindVertexArray(vao_);
    glUseProgram(prog_);
    glBindFramebuffer(GL_FRAMEBUFFER, currentFbo_);
    glViewport(0, 0, currentW_, currentH_);
    glEnable(GL_BLEND);
    return picture;
}

// A particle system's frame (CParticle3D's draw, 0x1000d1a0): two triangles a particle, in the
// pipeline state its batch was given (0x47ab20): blend mode 0 opaque; 1 and 6 .. 20 alpha; 2 add;
// 3 subtract (reverse); 4 multiply (dst x colour); 5 screen. A particle of alpha 0 is skipped (its
// indices are all 0 there). The frame's program and buffers are put back afterwards.
void Renderer::drawParticles(const diesel::DrawItem& d) {
    const diesel::ParticleBatch& b = *d.particles;
    Texture* t = texture(b.texture);
    if (!t || t->w <= 0 || t->h <= 0) return;
    std::vector<diesel::ParticleVertex> tri;
    tri.reserve(b.vertices.size() / 4 * 6);
    for (size_t i = 0; i + 3 < b.vertices.size(); i += 4) {
        if (!(b.vertices[i].color[3] > 0.0f)) continue;
        for (size_t k : {0u, 1u, 2u, 2u, 1u, 3u}) tri.push_back(b.vertices[i + k]);
    }
    if (tri.empty()) return;
    glUseProgram(progParticle_);
    glBindVertexArray(particleVao_);
    glBindBuffer(GL_ARRAY_BUFFER, particleVbo_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glBindSampler(0, samplerLinear_);
    int mode = b.blendMode;
    glUniform1i(uParticleCalc_, b.colorMode == 1 ? 1 : (b.colorMode == 2 ? 2 : 0));
    glUniform1i(uParticleGroup_, mode == 4 ? 1 : (mode == 5 ? 2 : 0));
    // Dither: the system's (each CParticle3D's +0xac, its constants 0x1000d1a0 sets as the sprites').
    bindDither(d, particleDither_);
    // The particles' modes as the sprites' blend states (alpha only into a render target, added).
    const bool intoTarget = currentFbo_ != backFbo_;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, intoTarget ? GL_TRUE : GL_FALSE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case 0: glDisable(GL_BLEND); break;
    case 2: glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE); break;
    case 3:
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
        break;
    case 4: glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE); break;
    case 5: glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ONE, GL_ONE); break;
    default: glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE); break;
    }
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tri.size() * sizeof(diesel::ParticleVertex)), tri.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tri.size()));
    glBlendEquation(GL_FUNC_ADD);
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
}

void Renderer::drawList(const std::vector<const diesel::DrawItem*>& list) {
    for (const diesel::DrawItem* item : list) {
        const diesel::DrawItem& d = *item;
        bool textured = false, glyph = false;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        GLuint srcTex = 0;
        int srcW = 0, srcH = 0;
        if (d.kind == diesel::DrawItem::Kind::Shadow) {
            if (d.shadow) drawShadow(d);
            continue;
        }
        if (d.kind == diesel::DrawItem::Kind::Particles) {
            if (d.particles) drawParticles(d);
            continue;
        }
        if (d.kind == diesel::DrawItem::Kind::Glyph) {
            if (!d.glyph) continue;
            const GlyphSlot* s = glyphSlot(d.glyph);
            if (!s) continue;
            glBindTexture(GL_TEXTURE_2D, atlas_);
            glBindSampler(0, d.filtering ? samplerLinear_ : samplerNearest_);
            const float n = static_cast<float>(kAtlasSize);
            u0 = static_cast<float>(s->x) / n;
            v0 = static_cast<float>(s->y) / n;
            u1 = static_cast<float>(s->x + d.glyph->pitch) / n;
            v1 = static_cast<float>(s->y + d.glyph->rows) / n;
            glyph = true;
        } else if (d.kind == diesel::DrawItem::Kind::Texture) {
            Texture* t = d.picture ? pictureTexture(d.picture) : texture(d.texture, d.subresource ? &d.subRect : nullptr);
            if (!t || t->w <= 0 || t->h <= 0) continue;
            glBindTexture(GL_TEXTURE_2D, t->tex);
            srcTex = t->tex, srcW = t->w, srcH = t->h;
            glBindSampler(0, d.filtering ? samplerLinear_ : samplerNearest_);
            u0 = static_cast<float>(d.srcX) / static_cast<float>(t->w);
            v0 = static_cast<float>(d.srcY) / static_cast<float>(t->h);
            u1 = static_cast<float>(d.srcX + d.srcW) / static_cast<float>(t->w);
            v1 = static_cast<float>(d.srcY + d.srcH) / static_cast<float>(t->h);
            textured = true;
        } else if (d.kind == diesel::DrawItem::Kind::Movie) {
            MovieTexture* t = movieTexture(d);
            if (!t) continue;
            glBindTexture(GL_TEXTURE_2D, t->tex);
            srcTex = t->tex, srcW = t->w, srcH = t->h;
            glBindSampler(0, d.filtering ? samplerLinear_ : samplerNearest_);
            u0 = static_cast<float>(d.srcX) / static_cast<float>(t->w);
            v0 = static_cast<float>(d.srcY) / static_cast<float>(t->h);
            u1 = static_cast<float>(d.srcX + d.srcW) / static_cast<float>(t->w);
            v1 = static_cast<float>(d.srcY + d.srcH) / static_cast<float>(t->h);
            textured = true;
        } else if (d.kind == diesel::DrawItem::Kind::Target) {
            auto it = targets_.find(d.renderTarget);
            if (it == targets_.end() || it->second.w <= 0) continue;
            glBindTexture(GL_TEXTURE_2D, it->second.tex);
            srcTex = it->second.tex, srcW = it->second.w, srcH = it->second.h;
            glBindSampler(0, d.filtering ? samplerLinear_ : samplerNearest_);
            u0 = static_cast<float>(d.srcX) / static_cast<float>(it->second.w);
            v0 = static_cast<float>(d.srcY) / static_cast<float>(it->second.h);
            u1 = static_cast<float>(d.srcX + d.srcW) / static_cast<float>(it->second.w);
            v1 = static_cast<float>(d.srcY + d.srcH) / static_cast<float>(it->second.h);
            textured = true;
        } else if (d.kind == diesel::DrawItem::Kind::Screen) {
            auto it = screens_.find(d.screen);
            if (it == screens_.end() || !it->second.taken) continue;
            glBindTexture(GL_TEXTURE_2D, it->second.tex);
            srcTex = it->second.tex, srcW = screenW_, srcH = screenH_;
            glBindSampler(0, d.filtering ? samplerLinear_ : samplerNearest_);
            u0 = static_cast<float>(d.srcX) / static_cast<float>(screenW_);
            v0 = static_cast<float>(d.srcY) / static_cast<float>(screenH_);
            u1 = static_cast<float>(d.srcX + d.srcW) / static_cast<float>(screenW_);
            v1 = static_cast<float>(d.srcY + d.srcH) / static_cast<float>(screenH_);
            textured = true;
        } else if (d.kind != diesel::DrawItem::Kind::Color) {
            continue;
        }
        bool visible = false;
        for (const auto& c : d.color) visible = visible || c[3] > 0.0f;
        // Nothing at alpha 0, whatever the blend (Fitted: Phantom's lip sync keeps a BLEND_MODE_DISABLE
        // colour sprite, stimg_lip_N_trans, at alpha 0 over each mouth; drawn, it would cut a hole in
        // every face that is not speaking).
        if (!visible) continue;
        if (textured && srcTex && d.filter.any()) {
            GLuint filtered = applyFilter(srcTex, srcW, srcH, d.filter);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, filtered);
            glBindSampler(0, d.filtering ? samplerLinear_ : samplerNearest_);
        }
        // The texture's sampler (CSprite2D's texture bind, Mware 0x10023100, also over a filtered
        // copy): wrapping while the UV offset (+0xa0 / +0xa4) or the pair at +0x154 is non-zero
        // (+0x108 linear / +0x100 point), else clamped (+0x104 / +0xfc); linear by the filtering
        // (+0x14c). A displacement map binds the linear / clamp one after it (0x10021fad).
        if (textured && (d.uvOffsetX != 0.0f || d.uvOffsetY != 0.0f)) {
            glBindSampler(0, d.filtering ? samplerRepeat_ : samplerRepeatNearest_);
        }
        Vertex v[4];
        float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
        if (d.partUv) {
            for (size_t i = 0; i < 4; ++i) {
                us[i] = (u1 - u0) * d.uv[i][0] + u0;
                vs[i] = (v1 - v0) * d.uv[i][1] + v0;
            }
        }
        // vsSprite adds g_Offset.zw, the sprite's UV offset, to every texture coordinate: CSprite +0x5f4,
        // handed over by each vertex build (0x456400 -> the Mware sprite's slot +0xcc -> CSprite2D
        // 0x10022940: +0xa0, read into the constants by 0x10021930). The texture then wraps (above).
        if (textured) {
            for (size_t i = 0; i < 4; ++i) {
                us[i] += d.uvOffsetX;
                vs[i] += d.uvOffsetY;
            }
        }
        for (size_t i = 0; i < 4; ++i) {
            const auto& q = d.quad.corner[i];
            const auto& c = d.color[i];
            v[i] = {q[0], q[1], q[2], us[i], vs[i], c[0], c[1], c[2], c[3]};
        }
        glUniform1i(uTransition_, bindTransition(d) ? 1 : 0);
        const bool dithered = bindDither(d, spriteDither_);
        const bool displaced = (textured || glyph) && bindDisplacement(d);
        glUniform1i(uMasked_, bindMask(d) ? 1 : 0);
        glUniform1i(uTextured_, textured ? 1 : 0);
        glUniform1i(uGlyph_, glyph ? 1 : 0);
        // Mware's CSprite2D (0x10022180) draws linear dodge (8) as add (2).
        const int mode = d.blendMode == 8 ? 2 : d.blendMode;
        if (readsTarget(mode)) copyDestination(d);
        glUniform1i(uMode_, mode);
        setBlend(mode);
        glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (dithered) glUniform1i(spriteDither_.on, 0);
        if (displaced) glUniform1i(uDisplaced_, 0);
    }
}

std::array<float, 4> Renderer::viewport(int width, int height) const {
    float fw = static_cast<float>(width), fh = static_cast<float>(height);
    float aspect = static_cast<float>(screenW_) / static_cast<float>(screenH_);
    float w = fw, h = fh;
    if (fh <= 0.0f || fw <= 0.0f) return {0, 0, 0, 0};
    if (fw / fh > aspect) w = fh * aspect;
    else h = fw / aspect;
    return {(fw - w) * 0.5f, (fh - h) * 0.5f, w, h};
}

void Renderer::present(int width, int height, GLuint target) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (width <= 0 || height <= 0) return;
    auto vp = viewport(width, height);
    float w = vp[2] / static_cast<float>(width), h = vp[3] / static_cast<float>(height);
    // Back buffer row 0 is the top of the picture: the top of the screen.
    Vertex v[4] = {};
    const float xs[4] = {-w, w, -w, w}, ys[4] = {h, h, -h, -h}, us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; ++i) v[i].x = xs[i], v[i].y = ys[i], v[i].w = 1, v[i].u = us[i], v[i].v = vs[i];
    glUseProgram(progPresent_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, backTex_);
    glBindSampler(0, samplerLinear_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindSampler(0, 0);
    glBindVertexArray(0);
}

}  // namespace nyan::render::dieselr
