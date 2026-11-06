#include "nyan/render/stage_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "font.hpp"
#include "nyan/common/sjis.hpp"
#include "nyan/cs2/emote_player.hpp"
#include "nyan/cs2/font_select.hpp"
#include "nyan/cs2/particle.hpp"
#include "nyan/cs2/hg_image.hpp"
#include "nyan/cs2/mersenne_twister.hpp"

namespace nyan::render {
namespace {

const char* kQuadVs = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform vec2 uTarget;
out vec2 vUv;
out vec4 vColor;
void main() {
    vUv = aUv;
    vColor = aColor;
    vec2 p = aPos / uTarget * 2.0 - 1.0;
    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);
}
)";

const char* kQuadFs = R"(#version 300 es
// highp: mobile GPUs run mediump as 16-bit floats, too coarse for coordinates in a 4096-wide atlas
// (E-mote parts read their neighbours' texels: thin seams).
precision highp float;
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform sampler2D uTex2;
uniform float uMix;
uniform int uMode;
uniform int uOutPremul;
uniform int uAlphaTest;  // colour out times alpha (the E-mote multiply blend)
uniform int uPremul;  // the texture holds premultiplied colour (merged image parts)
// An image whose alpha limits what is drawn, laid over a screen rect (x, y, w, h; top-down): the face
// window's mask (`fmask ... MASK`).
uniform sampler2D uScreenMask;
uniform int uHasScreenMask;
uniform highp vec4 uMaskRect;
uniform highp vec2 uTarget;
out vec4 outColor;
float screenMask() {
    if (uHasScreenMask == 0) return 1.0;
    highp vec2 m = (vec2(gl_FragCoord.x, uTarget.y - gl_FragCoord.y) - uMaskRect.xy) / uMaskRect.zw;
    if (m.x < 0.0 || m.y < 0.0 || m.x > 1.0 || m.y > 1.0) return 0.0;
    return texture(uScreenMask, m).a;
}
void shade() {
    if (uMode == 1) {
        outColor = vColor;
    } else if (uMode == 2) {
        outColor = vec4(vColor.rgb, vColor.a * texture(uTex, vUv).r);
    } else if (uMode == 3) {
        // An E-mote crossfade (cs2.exe 0x4f81d0 / 0x4f9430): both poses as straight colour, a pixel
        // clear in one taking the other's colour, then colour and alpha mixed.
        vec4 a = texture(uTex, vUv), b = texture(uTex2, vUv);
        vec3 ca = a.a > 0.0 ? a.rgb / a.a : b.a > 0.0 ? b.rgb / b.a : vec3(0.0);
        vec3 cb = b.a > 0.0 ? b.rgb / b.a : ca;
        outColor = vec4(mix(ca, cb, uMix), mix(a.a, b.a, uMix)) * vColor;
    } else {
        vec4 c = texture(uTex, vUv);
        if (uPremul == 1) c.rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
        outColor = c * vColor;
    }
}
void main() {
    shade();
    outColor.a *= screenMask();
    if (uOutPremul == 1) outColor.rgb *= outColor.a;
    if (uAlphaTest == 1 && outColor.a <= 64.0 / 255.0) discard;  // E-mote stencil masks (ALPHAREF 0x40)
}
)";

const char* kTransitionVs = R"(#version 300 es
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kTransitionFs = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uPrev;
uniform sampler2D uNext;
uniform sampler2D uMask;
uniform float uT;
uniform int uKind;     // 0 cut, 1 fade, 2 wipe, 3 mosaic, 4 fading mosaic
uniform vec2 uMosaic;  // mosaic: block size in texture coordinates (x, y)
uniform int uHasMask;
uniform int uHard;
out vec4 outColor;
void main() {
    if (uKind == 3) {
        // Old screen pixelating, then the new one resolving (block size from the CPU side).
        vec2 cell = (floor(vUv / uMosaic) + 0.5) * uMosaic;
        outColor = uT < 0.5 ? texture(uPrev, cell) : texture(uNext, cell);
        return;
    }
    if (uKind == 4) {
        // The old screen in blocks, at weight uT, over the new one.
        vec2 cell = (floor(vUv / uMosaic) + 0.5) * uMosaic;
        outColor = mix(texture(uNext, vUv), texture(uPrev, cell), uT);
        return;
    }
    vec4 a = texture(uPrev, vUv);
    vec4 b = texture(uNext, vUv);
    float k = uT;
    if (uKind == 2 && uHasMask == 1) {
        // Mask images are stored top-down; framebuffer UVs are bottom-up.
        // The engine's wipes (Task_Eff2D_Wipe / kcTaskRenderWipe) build a 256-entry table each
        // frame from p = 512 t and give a pixel of mask value m the new image's weight
        // clamp(p - 256 + m, 0, 256) / 256: bright mask areas change first, each pixel over half
        // the wipe (one of Fruit's masks, 12 radial steps, fades sector after sector; one of
        // Hatsumira's goes top to bottom). `wipe` and `rwipe` run it alike.
        float m = texture(uMask, vec2(vUv.x, 1.0 - vUv.y)).r * 255.0;
        k = clamp((floor(512.0 * uT) - 256.0 + m) / 256.0, 0.0, 1.0);
        // `wipe2` (Task_Eff2D_Wipe2) switches each pixel outright once m >= 256 - 256 t.
        if (uHard == 1) k = m >= 256.0 - floor(256.0 * uT) ? 1.0 : 0.0;
    }
    outColor = mix(a, b, k);
}
)";

// An FE script's screen wipe: the old frame over the new one, premultiplied, its weight going
// as in kTransitionFs (the mask's bright parts clear first).
const char* kScreenWipeFs = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uPrev;
uniform sampler2D uMask;
uniform float uT;
uniform int uHasMask;
out vec4 outColor;
void main() {
    float k = uT;
    if (uHasMask == 1) {
        float m = texture(uMask, vec2(vUv.x, 1.0 - vUv.y)).r * 255.0;
        k = clamp((floor(512.0 * uT) - 256.0 + m) / 256.0, 0.0, 1.0);
    }
    vec4 a = texture(uPrev, vUv);
    outColor = vec4(a.rgb * (1.0 - k), 1.0 - k);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        fail(std::string("shader compile failed: ") + buf);
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
        char buf[2048];
        glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        fail(std::string("shader link failed: ") + buf);
    }
    return p;
}

void setTexParams(GLint filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool isWide(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

// Whether Shift-JIS has the character as two bytes (all but ASCII and the half-width kana): the
// engine's text is Shift-JIS, and a UTF-8 game's ’ takes a whole cell on PC too (Amakano 2). A
// character given by its Unicode code (`\$n;`) is measured by Windows' code page 932 conversion
// (amakanoPE.exe 0x581980 -> WideCharToMultiByte), which also maps a few by best fit.
bool sjisDoubleByte(uint32_t cp) {
    if (cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F) || cp == 0xFFFD) return false;
    switch (cp) {
    case 0x2014: case 0x2016: case 0x2212: case 0x301C: case 0x00A2: case 0x00A3: case 0x00AC:
        return true;
    default:
        break;
    }
    if (isWide(cp)) return true;
    thread_local std::unordered_map<uint32_t, bool> known;
    if (auto it = known.find(cp); it != known.end()) return it->second;
    std::string u;
    if (cp < 0x800) {
        u += static_cast<char>(0xC0 | (cp >> 6));
    } else if (cp < 0x10000) {
        u += static_cast<char>(0xE0 | (cp >> 12));
        u += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    } else {
        u += static_cast<char>(0xF0 | (cp >> 18));
        u += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        u += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    }
    u += static_cast<char>(0x80 | (cp & 0x3F));
    const std::string s = utf8ToSjis(u);
    return known[cp] = s.size() == 2;
}


// The engine's image blur (kcImage, applied once when the image loads; grisaia.exe 0x544f60).
// The frame first grows by the radii into its canvas where there is room (left/right x, top/bottom
// y), the new area transparent. Then rows, then columns, go through a tent kernel of radius r:
// weight(d) = ((r + 1 - |d|) * 256 + r / 2) / (r + 1), the window padded with its end pixels.
// Within a window, transparent and see-through taps take the average colour of the window's
// non-transparent pixels in proportion to their transparency, so edges don't darken; alpha is
// blurred with the same weights. Sums scale by 65536 / (r + 1) and round at bit 23.
void engineBlurPass(std::vector<uint8_t>& px, int w, int h, int r, bool vertical) {
    if (r <= 0) return;
    r = std::min(r, 255);
    std::vector<uint32_t> weight(2 * static_cast<size_t>(r) + 1);
    for (int d = -r; d <= r; ++d) weight[static_cast<size_t>(d + r)] = static_cast<uint32_t>(((r + 1 - std::abs(d)) * 256 + r / 2) / (r + 1));
    const uint32_t scale = 0x10000u / static_cast<uint32_t>(r + 1);
    const int len = vertical ? h : w, lines = vertical ? w : h;
    std::vector<uint8_t> line(static_cast<size_t>(len + 2 * r) * 4), out(static_cast<size_t>(len) * 4);
    auto at = [&](int l, int i) -> uint8_t* {
        return &px[(static_cast<size_t>(vertical ? i : l) * w + (vertical ? l : i)) * 4];
    };
    auto round24 = [](uint32_t v) { return static_cast<uint8_t>((v >> 24) + ((v >> 23) & 1)); };
    for (int l = 0; l < lines; ++l) {
        for (int i = 0; i < len + 2 * r; ++i) std::memcpy(&line[static_cast<size_t>(i) * 4], at(l, std::clamp(i - r, 0, len - 1)), 4);
        // Running totals of the window's non-transparent pixels (for its average colour).
        uint32_t n = 0, tr = 0, tg = 0, tb = 0;
        auto add = [&](const uint8_t* p, int sign) {
            if (!p[3]) return;
            n += sign, tr += sign * p[0], tg += sign * p[1], tb += sign * p[2];
        };
        for (int k = 0; k < 2 * r; ++k) add(&line[static_cast<size_t>(k) * 4], 1);
        for (int i = 0; i < len; ++i) {
            const uint8_t* win = &line[static_cast<size_t>(i) * 4];
            add(win + 2 * r * 4, 1);
            if (i > 0) add(win - 4, -1);
            uint8_t* o = &out[static_cast<size_t>(i) * 4];
            if (!n) {
                std::memset(o, 0, 4);
                continue;
            }
            uint32_t ar = tr / n, ag = tg / n, ab = tb / n;
            uint32_t sr = 0, sg = 0, sb = 0, sa = 0;
            for (int k = 0; k <= 2 * r; ++k) {
                const uint8_t* p = win + k * 4;
                uint32_t a = p[3], c0 = ar, c1 = ag, c2 = ab;
                if (a == 255) c0 = p[0], c1 = p[1], c2 = p[2];
                else if (a) c0 = (p[0] * a + ar * (255 - a)) >> 8, c1 = (p[1] * a + ag * (255 - a)) >> 8, c2 = (p[2] * a + ab * (255 - a)) >> 8;
                uint32_t wk = weight[static_cast<size_t>(k)];
                sr += c0 * wk, sg += c1 * wk, sb += c2 * wk, sa += a * wk;
            }
            o[0] = round24(sr * scale), o[1] = round24(sg * scale), o[2] = round24(sb * scale), o[3] = round24(sa * scale);
        }
        for (int i = 0; i < len; ++i) std::memcpy(at(l, i), &out[static_cast<size_t>(i) * 4], 4);
    }
}

void engineBlur(cs2::RgbaImage& img, int& offX, int& offY, int canvasW, int canvasH, int rx, int ry) {
    rx = std::clamp(rx, 0, 255), ry = std::clamp(ry, 0, 255);
    const int w = static_cast<int>(img.width), h = static_cast<int>(img.height);
    int left = std::min(rx, offX), top = std::min(ry, offY);
    int right = std::max(0, std::min(rx, canvasW - w - offX)), bottom = std::max(0, std::min(ry, canvasH - h - offY));
    const int nw = w + left + right, nh = h + top + bottom;
    std::vector<uint8_t> px(static_cast<size_t>(nw) * nh * 4, 0);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&px[(static_cast<size_t>(y + top) * nw + left) * 4], &img.pixels[static_cast<size_t>(y) * w * 4], static_cast<size_t>(w) * 4);
    }
    engineBlurPass(px, nw, nh, rx, false);
    engineBlurPass(px, nw, nh, ry, true);
    img.pixels.assign(px.begin(), px.end());
    img.width = static_cast<uint32_t>(nw), img.height = static_cast<uint32_t>(nh);
    offX -= left, offY -= top;
}

// The engine's image `mode` (kcImage at 0x543040 / 0x543220 / 0x5431c0), rewriting the pixels
// in place, alpha untouched. 1 mono: Y = trunc(.298912 R) + trunc(.586611 G) + trunc(.114478 B)
// (per-channel 256-entry tables of float products); 2 sepia: that Y as R + 30, G + 5, B - 35,
// clamped; 3 np: 255 - each channel.
void engineColorMode(cs2::RgbaImage& img, int mode) {
    if (mode < 1 || mode > 3) return;
    uint8_t tr[256], tg[256], tb[256];
    for (int i = 0; i < 256; ++i) {
        tr[i] = static_cast<uint8_t>(static_cast<int>(static_cast<float>(i) * 0.298911988735199f));
        tg[i] = static_cast<uint8_t>(static_cast<int>(static_cast<float>(i) * 0.5866109728813171f));
        tb[i] = static_cast<uint8_t>(static_cast<int>(static_cast<float>(i) * 0.11447799950838089f));
    }
    for (size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        uint8_t* p = &img.pixels[i];
        if (mode == 3) {
            p[0] = 255 - p[0], p[1] = 255 - p[1], p[2] = 255 - p[2];
            continue;
        }
        const int y = static_cast<uint8_t>(tr[p[0]] + tg[p[1]] + tb[p[2]]);
        if (mode == 1) {
            p[0] = p[1] = p[2] = static_cast<uint8_t>(y);
        } else {
            p[0] = static_cast<uint8_t>(std::min(y + 30, 255));
            p[1] = static_cast<uint8_t>(std::min(y + 5, 255));
            p[2] = static_cast<uint8_t>(std::max(y - 35, 0));
        }
    }
}

}  // namespace

StageRenderer::StageRenderer(const cs2::AssetIndex& assets, Bytes font, Options options)
    : assets_(assets), opt_(options), font_(std::make_unique<FontAtlas>(std::move(font), opt_.fontFace)) {
    cur_ = font_.get();
    textAround_ = opt_.textAround;
    textRightDown_ = opt_.textRightDown;
    quadProgram_ = link(kQuadVs, kQuadFs);
    uTarget_ = glGetUniformLocation(quadProgram_, "uTarget");
    uMode_ = glGetUniformLocation(quadProgram_, "uMode");
    uTex_ = glGetUniformLocation(quadProgram_, "uTex");
    uPremul_ = glGetUniformLocation(quadProgram_, "uPremul");
    uTex2_ = glGetUniformLocation(quadProgram_, "uTex2");
    uOutPremul_ = glGetUniformLocation(quadProgram_, "uOutPremul");
    uAlphaTest_ = glGetUniformLocation(quadProgram_, "uAlphaTest");
    uMix_ = glGetUniformLocation(quadProgram_, "uMix");
    uScreenMask_ = glGetUniformLocation(quadProgram_, "uScreenMask");
    uHasScreenMask_ = glGetUniformLocation(quadProgram_, "uHasScreenMask");
    uMaskRect_ = glGetUniformLocation(quadProgram_, "uMaskRect");
    transitionProgram_ = link(kTransitionVs, kTransitionFs);
    uPrev_ = glGetUniformLocation(transitionProgram_, "uPrev");
    uNext_ = glGetUniformLocation(transitionProgram_, "uNext");
    uMask_ = glGetUniformLocation(transitionProgram_, "uMask");
    uT_ = glGetUniformLocation(transitionProgram_, "uT");
    uKind_ = glGetUniformLocation(transitionProgram_, "uKind");
    uMosaic_ = glGetUniformLocation(transitionProgram_, "uMosaic");
    uHasMask_ = glGetUniformLocation(transitionProgram_, "uHasMask");
    uHard_ = glGetUniformLocation(transitionProgram_, "uHard");
    wipeProgram_ = link(kTransitionVs, kScreenWipeFs);
    uWipePrev_ = glGetUniformLocation(wipeProgram_, "uPrev");
    uWipeMask_ = glGetUniformLocation(wipeProgram_, "uMask");
    uWipeT_ = glGetUniformLocation(wipeProgram_, "uT");
    uWipeHasMask_ = glGetUniformLocation(wipeProgram_, "uHasMask");

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, r)));
    glBindVertexArray(0);
    glGenVertexArrays(1, &emptyVao_);

    for (Target* t : {&scene_, &prev_, &display_, &fadeOld_, &fadeNew_, &fadeMix_, &lastFrame_}) {
        glGenTextures(1, &t->tex);
        glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, opt_.virtualWidth, opt_.virtualHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        setTexParams(GL_LINEAR);
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        if (t == &scene_ || t == &fadeOld_ || t == &fadeNew_) {
            glGenRenderbuffers(1, &t->stencil);
            glBindRenderbuffer(GL_RENDERBUFFER, t->stencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, opt_.virtualWidth, opt_.virtualHeight);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->stencil);
        }
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

StageRenderer::~StageRenderer() {
    trimCache();
    for (auto& e : emotes_) {
        if (e.tex) glDeleteTextures(1, &e.tex);
    }
    for (auto& f : emoteFades_) {
        glDeleteTextures(1, &f.a);
        glDeleteTextures(1, &f.b);
    }
    if (surface_.fbo) {
        glDeleteFramebuffers(1, &surface_.fbo);
        glDeleteTextures(1, &surface_.tex);
    }
    for (Target* t : {&scene_, &prev_, &display_, &fadeOld_, &fadeNew_, &fadeMix_, &lastFrame_}) {
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->tex);
        if (t->stencil) glDeleteRenderbuffers(1, &t->stencil);
    }
    glDeleteBuffers(1, &vbo_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteVertexArrays(1, &emptyVao_);
    glDeleteProgram(quadProgram_);
    glDeleteProgram(transitionProgram_);
    glDeleteProgram(wipeProgram_);
    if (movieTex_) glDeleteTextures(1, &movieTex_);
    for (auto& [n, v] : planeVideos_) {
        if (v.tex) glDeleteTextures(1, &v.tex);
    }
}

void StageRenderer::trimCache() {
    for (auto& [name, t] : textures_) glDeleteTextures(1, &t.id);
    textures_.clear();
    lru_.clear();
    textureBytes_ = 0;
}

void StageRenderer::evict() {
    while (textureBytes_ > opt_.textureBudgetBytes && lru_.size() > 1) {
        const std::string& victim = lru_.back();
        auto it = textures_.find(victim);
        if (it != textures_.end()) {
            textureBytes_ -= it->second.bytes;
            glDeleteTextures(1, &it->second.id);
            textures_.erase(it);
        }
        lru_.pop_back();
    }
}

const StageRenderer::Texture* StageRenderer::texture(const std::string& name) {
    auto it = textures_.find(name);
    if (it != textures_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return &it->second;
    }
    if (missingTextures_.count(name)) return nullptr;

    if (!name.empty() && name[0] == '@') {
        // Engine-provided image (save thumbnails).
        int w = 0, h = 0;
        Bytes rgba;
        if (!opt_.externalTexture || !opt_.externalTexture(name, w, h, rgba) || w <= 0 || h <= 0 ||
            rgba.size() != static_cast<size_t>(w) * h * 4) {
            missingTextures_[name] = true;
            return nullptr;
        }
        Texture t;
        t.width = w;
        t.height = h;
        t.bytes = rgba.size();
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        setTexParams(GL_LINEAR);
        lru_.push_front(name);
        t.lru = lru_.begin();
        textureBytes_ += t.bytes;
        auto& stored = textures_.emplace(name, t).first->second;
        evict();
        return &stored;
    }

    // "name|blur=x,y|mode=nn": the image as the engine's `blur` and then each `mode` conversion
    // in turn leave it (see engineBlur, engineColorMode).
    std::string file = name;
    int blurX = 0, blurY = 0;
    std::string colorModes;
    bool canvas = false;  // "|canvas": the frame placed on its whole canvas (texture coordinates span it)
    if (auto bar = file.find('|'); bar != std::string::npos) {
        if (auto b = file.find("|blur=", bar); b != std::string::npos) std::sscanf(file.c_str() + b + 6, "%d,%d", &blurX, &blurY);
        if (auto m = file.find("|mode=", bar); m != std::string::npos) {
            colorModes = file.substr(m + 6, file.find('|', m + 6) - (m + 6));
        }
        canvas = file.find("|canvas", bar) != std::string::npos;
        file = file.substr(0, bar);
    }
    // "name#id" selects the frame with that id from a multi-frame image (UI sheets).
    int64_t frameId = -1;
    if (auto hash = file.find('#'); hash != std::string::npos) {
        frameId = std::atoll(file.c_str() + hash + 1);
        file = file.substr(0, hash);
    }
    auto ref = assets_.find(file + ".hg3");
    if (!ref) {
        missingTextures_[name] = true;
        return nullptr;
    }
    Texture t;
    try {
        // A frame of a multi-frame image keeps the parsed file for the next one (an animated event
        // CG's frames, 14 MB each file, are asked for one per tick until they are all cached).
        if (frameId < 0 || file != parsedFile_ || !parsed_) {
            parsed_ = std::make_shared<cs2::HgImage>(cs2::HgImage::parse(ref->archive->read(*ref->entry)));
            parsedFile_ = frameId < 0 ? std::string() : file;
        }
        std::shared_ptr<const cs2::HgImage> keep = parsed_;
        if (frameId < 0) parsed_.reset();
        const cs2::HgImage& img = *keep;
        size_t index = 0;
        bool found = frameId < 0;
        for (size_t i = 0; frameId >= 0 && i < img.frames().size(); ++i) {
            if (img.frames()[i].id == frameId) index = i, found = true;
        }
        if (!found) {
            // A frame id the image doesn't have (an .anm step past its last frame) is no image,
            // not the first frame.
            missingTextures_[name] = true;
            return nullptr;
        }
        cs2::RgbaImage px = img.decode(index);
        const cs2::HgFrame& f = img.frames()[index];
        t.offsetX = f.offsetX;
        t.offsetY = f.offsetY;
        if (blurX > 0 || blurY > 0) {
            engineBlur(px, t.offsetX, t.offsetY, static_cast<int>(f.totalWidth), static_cast<int>(f.totalHeight), blurX, blurY);
        }
        for (char c : colorModes) engineColorMode(px, c - '0');
        if (canvas && f.totalWidth > 0 && f.totalHeight > 0) {
            cs2::RgbaImage full;
            full.width = f.totalWidth;
            full.height = f.totalHeight;
            full.pixels.assign(static_cast<size_t>(full.width) * full.height * 4, 0);
            for (uint32_t y = 0; y < px.height; ++y) {
                int64_t ty = static_cast<int64_t>(y) + t.offsetY;
                if (ty < 0 || ty >= static_cast<int64_t>(full.height)) continue;
                for (uint32_t x = 0; x < px.width; ++x) {
                    int64_t tx = static_cast<int64_t>(x) + t.offsetX;
                    if (tx < 0 || tx >= static_cast<int64_t>(full.width)) continue;
                    std::memcpy(&full.pixels[(static_cast<size_t>(ty) * full.width + static_cast<size_t>(tx)) * 4],
                                &px.pixels[(static_cast<size_t>(y) * px.width + x) * 4], 4);
                }
            }
            px = std::move(full);
            t.offsetX = t.offsetY = 0;
        }
        t.width = static_cast<int>(px.width);
        t.height = static_cast<int>(px.height);
        t.baseX = f.baseX;
        t.baseY = f.baseY;
        t.canvasW = static_cast<int>(f.totalWidth);
        t.canvasH = static_cast<int>(f.totalHeight);
        for (const auto& a : f.attributes) t.attributes[a.id] = {a.x, a.y};
        t.bytes = px.pixels.size();
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.pixels.data());
        setTexParams(GL_LINEAR);
    } catch (const Error& e) {
        std::fprintf(stderr, "texture %s: %s\n", name.c_str(), e.what());
        missingTextures_[name] = true;
        return nullptr;
    }
    lru_.push_front(name);
    t.lru = lru_.begin();
    textureBytes_ += t.bytes;
    auto& stored = textures_.emplace(name, t).first->second;
    evict();
    return &stored;
}

const StageRenderer::Texture* StageRenderer::userGlyph(uint32_t cp) {
    auto it = opt_.userFontGlyphs.find(cp);
    if (it == opt_.userFontGlyphs.end()) return nullptr;
    return texture(opt_.userFontImage + "#" + std::to_string(it->second));
}

void StageRenderer::drawUserGlyph(const Texture& t, float x, float y, float cell, float alpha) {
    // The frame's canvas is scaled to the character cell, like a full-width glyph of the font size.
    float s = cell / static_cast<float>(std::max(1, std::max(t.canvasW, t.canvasH)));
    quad(Mode::Texture, t.id, {x + t.offsetX * s, y + t.offsetY * s, t.width * s, t.height * s}, 0, 0, 1, 1, 0xFFFFFFFF,
         alpha);
}

void StageRenderer::begin(int targetW, int targetH) {
    targetW_ = static_cast<float>(targetW);
    targetH_ = static_cast<float>(targetH);
    glUseProgram(quadProgram_);
    glUniform2f(uTarget_, targetW_, targetH_);
    glUniform1i(uTex_, 0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void StageRenderer::flush() {
    if (batch_.empty()) return;
    if (batchMode_ == Mode::Glyph) {  // upload glyphs rasterised for this batch (whichever font)
        font_->flush();
        for (auto& [k, a] : fonts_) {
            if (a) a->flush();
        }
    }
    glUniform1i(uMode_, static_cast<int>(batchMode_));
    glUniform1i(uPremul_, premul_ ? 1 : 0);
    glUniform1i(uOutPremul_, multiply_ ? 1 : 0);
    glUniform1i(uAlphaTest_, alphaTest_ ? 1 : 0);
    if (multiply_) glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glUniform1i(uHasScreenMask_, screenMask_ ? 1 : 0);
    if (screenMask_) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, screenMask_);
        glUniform1i(uScreenMask_, 1);
        glUniform4f(uMaskRect_, screenMaskRect_.x, screenMaskRect_.y, screenMaskRect_.w, screenMaskRect_.h);
    }
    if (batchMode_ == Mode::Mix) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, batchTex2_);
        glUniform1i(uTex2_, 2);
        glUniform1f(uMix_, batchMix_);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batchTex_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.size() * sizeof(Vertex)), batch_.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.size()));
    if (multiply_) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    batch_.clear();
}

void StageRenderer::quad(Mode mode, GLuint tex, const Rect& r, float u0, float v0, float u1, float v1, uint32_t argb,
                         float alpha) {
    if (alpha <= 0.0f) return;
    if (!batch_.empty() && (mode != batchMode_ || (mode != Mode::Solid && tex != batchTex_))) flush();
    batchMode_ = mode;
    batchTex_ = tex;
    float a = ((argb >> 24) & 0xFF) / 255.0f * alpha;
    float cr = ((argb >> 16) & 0xFF) / 255.0f * tone_[0], cg = ((argb >> 8) & 0xFF) / 255.0f * tone_[1],
          cb = (argb & 0xFF) / 255.0f * tone_[2];
    const float st = shear_ * (shearY_ - r.y), sb = shear_ * (shearY_ - r.y - r.h);
    Vertex tl{r.x + st, r.y, u0, v0, cr, cg, cb, a}, tr{r.x + r.w + st, r.y, u1, v0, cr, cg, cb, a};
    Vertex bl{r.x + sb, r.y + r.h, u0, v1, cr, cg, cb, a}, br{r.x + r.w + sb, r.y + r.h, u1, v1, cr, cg, cb, a};
    batch_.insert(batch_.end(), {tl, tr, bl, tr, br, bl});
}

void StageRenderer::quadLR(Mode mode, GLuint tex, const Rect& r, float u0, float v0, float u1, float v1, uint32_t argb,
                           float left, float right) {
    if (left <= 0.0f && right <= 0.0f) return;
    if (!batch_.empty() && (mode != batchMode_ || (mode != Mode::Solid && tex != batchTex_))) flush();
    batchMode_ = mode;
    batchTex_ = tex;
    float a = ((argb >> 24) & 0xFF) / 255.0f;
    float cr = ((argb >> 16) & 0xFF) / 255.0f * tone_[0], cg = ((argb >> 8) & 0xFF) / 255.0f * tone_[1],
          cb = (argb & 0xFF) / 255.0f * tone_[2];
    const float st = shear_ * (shearY_ - r.y), sb = shear_ * (shearY_ - r.y - r.h);
    Vertex tl{r.x + st, r.y, u0, v0, cr, cg, cb, a * left}, tr{r.x + r.w + st, r.y, u1, v0, cr, cg, cb, a * right};
    Vertex bl{r.x + sb, r.y + r.h, u0, v1, cr, cg, cb, a * left}, br{r.x + r.w + sb, r.y + r.h, u1, v1, cr, cg, cb, a * right};
    batch_.insert(batch_.end(), {tl, tr, bl, tr, br, bl});
}

float StageRenderer::placeWipe(std::vector<LaidGlyph>& glyphs, size_t from) {
    // Where each glyph of the new text lies along the wipe (grisaia.exe 0x5488a0): its x in its line
    // from the line's leftmost, plus the lengths of the lines before. Returns the whole length.
    float done = 0, lineMin = 0, lineMax = 0, lineY = 0;
    bool open = false;
    for (size_t i = from; i < glyphs.size(); ++i) {
        LaidGlyph& g = glyphs[i];
        if (g.cp == 0) continue;
        int px = std::max(1, static_cast<int>(std::lround(g.size * viewScale_)));
        float right = g.x + cur_->glyph(g.cp, px).advance;
        if (!open || g.y != lineY) {
            if (open) done += lineMax - lineMin;
            lineMin = g.x, lineMax = right, lineY = g.y, open = true;
        }
        lineMin = std::min(lineMin, g.x);
        lineMax = std::max(lineMax, right);
        g.wipe = g.x - lineMin + done;
    }
    return open ? done + lineMax - lineMin : 0;
}

float StageRenderer::wipeAlpha(float offset) const {
    // grisaia.exe 0x54cc70: a pixel k past the wipe's trailing edge keeps (ramp[k] + 1) / 256 of its
    // opacity, ramp[k] = 255 - k * 255 / width (0x54c8c0); behind the edge it is whole, past the
    // soft part nothing.
    float k = offset - wipeHead_;
    if (k < 0) return 1.0f;
    if (k >= wipeWidth_) return 0.0f;
    return (255.0f - std::floor(k * 255.0f / wipeWidth_) + 1.0f) / 256.0f;
}

void StageRenderer::quadCorners(Mode mode, GLuint tex, const Vec2 c[4], float u0, float v0, float u1, float v1,
                                uint32_t argb, float alpha) {
    if (alpha <= 0.0f) return;
    if (!batch_.empty() && (mode != batchMode_ || (mode != Mode::Solid && tex != batchTex_))) flush();
    batchMode_ = mode;
    batchTex_ = tex;
    float a = ((argb >> 24) & 0xFF) / 255.0f * alpha;
    float cr = ((argb >> 16) & 0xFF) / 255.0f * tone_[0], cg = ((argb >> 8) & 0xFF) / 255.0f * tone_[1],
          cb = (argb & 0xFF) / 255.0f * tone_[2];
    Vertex tl{c[0].x, c[0].y, u0, v0, cr, cg, cb, a}, tr{c[1].x, c[1].y, u1, v0, cr, cg, cb, a};
    Vertex br{c[2].x, c[2].y, u1, v1, cr, cg, cb, a}, bl{c[3].x, c[3].y, u0, v1, cr, cg, cb, a};
    batch_.insert(batch_.end(), {tl, tr, bl, tr, br, bl});
}

void StageRenderer::quadCorners(Mode mode, GLuint tex, const Vec2 c[4], float u0, float v0, float u1, float v1,
                                const uint32_t argb[4]) {
    if (!batch_.empty() && (mode != batchMode_ || (mode != Mode::Solid && tex != batchTex_))) flush();
    batchMode_ = mode;
    batchTex_ = tex;
    auto vertex = [&](int k, float u, float v) {
        const uint32_t x = argb[k];
        return Vertex{c[k].x, c[k].y, u, v, ((x >> 16) & 0xFF) / 255.0f * tone_[0], ((x >> 8) & 0xFF) / 255.0f * tone_[1],
                      (x & 0xFF) / 255.0f * tone_[2], ((x >> 24) & 0xFF) / 255.0f};
    };
    Vertex tl = vertex(0, u0, v0), tr = vertex(1, u1, v0), br = vertex(2, u1, v1), bl = vertex(3, u0, v1);
    batch_.insert(batch_.end(), {tl, tr, bl, tr, br, bl});
}

StageRenderer::Vec2 StageRenderer::PlaneXform::apply(float x, float y) const {
    float dx = (x - baseX) * scaleX, dy = (y - baseY) * scaleY;
    Vec2 v{posX + dx * m00 + dy * m10, posY + dx * m01 + dy * m11};
    if (!camera) return v;
    // cs2.exe 0x5825c0: (v - C) scaled, times the camera's matrix, then C - P added.
    const float ex = (v.x - camCX) * camSX, ey = (v.y - camCY) * camSY;
    return {camCX - camX + ex * c00 + ey * c10, camCY - camY + ex * c01 + ey * c11};
}

void StageRenderer::cameraXform(const cs2::Stage& stage, uint64_t frame, PlaneXform& xf) const {
    auto it = stage.planes.find(cs2::kCameraPlane);
    if (it == stage.planes.end()) return;
    const cs2::Plane& c = it->second;
    cs2::MotionState ms = cs2::evaluate(c.motions, frame);
    xf.camX = (ms.hasPos ? ms.x : static_cast<float>(c.x.value(frame))) + ms.dx;
    xf.camY = (ms.hasPos ? ms.y : static_cast<float>(c.y.value(frame))) + ms.dy;
    xf.camSX = (ms.setScaleX >= 0 ? ms.setScaleX : c.scaleX + ms.addScaleX) * ms.scaleX;
    xf.camSY = (ms.setScaleY >= 0 ? ms.setScaleY : c.scaleY + ms.addScaleY) * ms.scaleY;
    auto r = cs2::rotation2x2(c.rotX + ms.rotX, c.rotY + ms.rotY, c.rotation + ms.rotation);
    xf.c00 = r[0], xf.c01 = r[1], xf.c10 = r[2], xf.c11 = r[3];
    // The centre: half the virtual screen (0x57d220: the screen size x 0.5).
    xf.camCX = static_cast<float>(opt_.virtualWidth) * 0.5f;
    xf.camCY = static_cast<float>(opt_.virtualHeight) * 0.5f;
    xf.camera = xf.camX != 0 || xf.camY != 0 || xf.camSX != 1 || xf.camSY != 1 || xf.c00 != 1 || xf.c01 != 0 ||
                xf.c10 != 0 || xf.c11 != 1;
}

StageRenderer::PlaneXform StageRenderer::planeXform(const cs2::Stage& stage, int32_t plane, uint64_t frame) const {
    PlaneXform xf;
    auto it = stage.planes.find(plane);
    if (it == stage.planes.end()) {
        // Untouched plane: its surface sits where vram.xml puts it.
        auto off = opt_.planeOffsets.find(plane);
        auto o = off == opt_.planeOffsets.end() ? opt_.planeOffset : off->second;
        xf.posX = static_cast<float>(o.first);
        xf.posY = static_cast<float>(o.second);
        cameraXform(stage, frame, xf);
        return xf;
    }
    cameraXform(stage, frame, xf);
    const cs2::Plane& p = it->second;
    cs2::MotionState ms = cs2::evaluate(p.motions, frame);
    xf.baseX = static_cast<float>(p.baseX);
    xf.baseY = static_cast<float>(p.baseY);
    xf.posX = (ms.hasPos ? ms.x : static_cast<float>(p.x.value(frame))) + ms.dx;
    xf.posY = (ms.hasPos ? ms.y : static_cast<float>(p.y.value(frame))) + ms.dy;
    auto r = cs2::rotation2x2(p.rotX + ms.rotX, p.rotY + ms.rotY, p.rotation + ms.rotation);
    xf.m00 = r[0], xf.m01 = r[1], xf.m10 = r[2], xf.m11 = r[3];
    xf.scaleX = (ms.setScaleX >= 0 ? ms.setScaleX : p.scaleX + ms.addScaleX) * ms.scaleX;
    xf.scaleY = (ms.setScaleY >= 0 ? ms.setScaleY : p.scaleY + ms.addScaleY) * ms.scaleY;
    xf.alpha = (ms.setAlpha >= 0 ? ms.setAlpha : std::clamp(p.alpha.value(frame), 0, 255)) / 255.0f;
    // An mccfade writes the colour outright, its alpha the opacity (setAlpha above).
    xf.color = ms.hasColor ? (ms.color | 0xFF000000u) : p.color;
    xf.visible = ms.setVisible >= 0 ? ms.setVisible != 0 : p.visible;  // an mcdisp sets it
    return xf;
}

void StageRenderer::drawImage(const cs2::StageImage& img, uint64_t frame, const PlaneXform& plane, float scale,
                              float ox, float oy, float shiftX, float shiftY, ImageMask* mask) {
    if (!plane.visible) return;
    cs2::MotionState ms = cs2::evaluate(img.motions, frame);
    if (!(ms.setVisible >= 0 ? ms.setVisible != 0 : img.visible)) return;  // an mcdisp sets it
    float alpha = (ms.setAlpha >= 0 ? ms.setAlpha : std::clamp(img.alpha.value(frame), 0, 255)) / 255.0f * plane.alpha *
                  ((plane.color >> 24) & 0xFF) / 255.0f;
    if (alpha <= 0 && !mask) return;
    // `base`: the given canvas point lands on the position instead of the image's own base point.
    if (img.baseX != INT32_MIN && !img.parts.empty()) {
        if (const Texture* t0 = texture(img.parts.front())) {
            shiftX += static_cast<float>(t0->baseX - img.baseX);
            shiftY += static_cast<float>(t0->baseY - img.baseY);
        }
    }
    float ax = (ms.hasPos ? ms.x : static_cast<float>(img.x.value(frame))) + ms.dx + shiftX;
    float ay = (ms.hasPos ? ms.y : static_cast<float>(img.y.value(frame))) + ms.dy + shiftY;
    float sx = (ms.setScaleX >= 0 ? ms.setScaleX : img.scaleX.value(frame) / 100.0f + ms.addScaleX) * ms.scaleX;
    float sy = (ms.setScaleY >= 0 ? ms.setScaleY : img.scaleY.value(frame) / 100.0f + ms.addScaleY) * ms.scaleY;
    // `tone`: each colour channel scaled as the image was loaded; values over 100% saturate (the
    // target clamps before blending, like the engine's min(255, c * pct / 100) table).
    struct ToneScope {
        std::array<float, 3>& t;
        ~ToneScope() { t = {1, 1, 1}; }
    } toneScope{tone_};
    for (int k = 0; k < 3; ++k) {
        tone_[k] = static_cast<float>(img.toneLoaded[k]) / 100.0f * ((plane.color >> (16 - 8 * k)) & 0xFF) / 255.0f;
    }

    if (mask) {
        // The mask's first part, placed as it would be drawn; its bounding box on screen.
        const Texture* t = img.parts.empty() ? nullptr : texture(img.parts.front());
        if (!t) return;
        const float rx = static_cast<float>(t->offsetX - t->baseX), ry = static_cast<float>(t->offsetY - t->baseY);
        const float xs[4] = {rx, rx + t->width, rx + t->width, rx};
        const float ys[4] = {ry, ry, ry + t->height, ry + t->height};
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int k = 0; k < 4; ++k) {
            Vec2 pl = plane.apply(ax + xs[k] * sx, ay + ys[k] * sy);
            float cx = ox + pl.x * scale, cy = oy + pl.y * scale;
            x0 = std::min(x0, cx), y0 = std::min(y0, cy), x1 = std::max(x1, cx), y1 = std::max(y1, cy);
        }
        mask->tex = t->id;
        mask->rect = {x0, y0, x1 - x0, y1 - y0};
        return;
    }

    if (!img.emote.empty()) {
        sweepEmoteFades(frame);
        if (img.emotePlayer && img.emoteFading(frame)) {
            drawEmoteFade(img, frame, plane, scale, ox, oy, ax, ay, sx, sy, alpha);
            return;
        }
        if (img.emotePlayer) {
            if (EmoteEntry* e = emote(img.emotePlayer->modelPtr())) drawEmote(img, *e, frame, plane, scale, ox, oy, ax, ay, sx, sy, alpha);
        }
        return;
    }

    const std::array<float, 4> turn = ms.rotation != 0 || ms.rotX != 0 || ms.rotY != 0
                                          ? cs2::rotation2x2(ms.rotX, ms.rotY, ms.rotation)
                                          : std::array<float, 4>{1, 0, 0, 1};
    // Image-local rect (relative to its anchor) -> plane-local -> screen.
    auto emit = [&](Mode mode, GLuint tex, float rx, float ry, float rw, float rh, uint32_t color) {
        Vec2 c[4];
        float xs[4] = {rx, rx + rw, rx + rw, rx};
        float ys[4] = {ry, ry, ry + rh, ry + rh};
        // `\lr` `\ud` `\r90..270` (StageImage::turn): the image mirrored, then turned, about its
        // base point (cs2.exe 0x61fa50 / 0x6201f0: a point (x, y) from the base goes to (-x, y), (x,
        // -y), then (-y, x) for 90, (-x, -y) for 180, (y, -x) for 270); the texture goes with it.
        if (img.turn) {
            for (int k = 0; k < 4; ++k) {
                float x = xs[k], y = ys[k];
                if (img.turn & 1) x = -x;
                if (img.turn & 2) y = -y;
                switch (img.turn & 0x30) {
                    case 0x10: std::tie(x, y) = std::make_pair(-y, x); break;
                    case 0x20: x = -x, y = -y; break;
                    case 0x30: std::tie(x, y) = std::make_pair(y, -x); break;
                    default: break;
                }
                xs[k] = x, ys[k] = y;
            }
        }
        // A turn (`mcrot`): scale, then the engine's RotX * RotY * RotZ flattened, about the base point.
        for (int k = 0; k < 4; ++k) {
            const float lx = xs[k] * sx, ly = ys[k] * sy;
            Vec2 pl = plane.apply(ax + lx * turn[0] + ly * turn[2], ay + lx * turn[1] + ly * turn[3]);
            c[k] = {ox + pl.x * scale, oy + pl.y * scale};
        }
        quadCorners(mode, tex, c, 0, 0, 1, 1, color, alpha);
    };
    if (img.solid) {
        emit(Mode::Solid, 0, 0, 0, static_cast<float>(img.solidWidth), static_cast<float>(img.solidHeight),
             img.solidColor);
        return;
    }
    // An image of several parts drawn see-through is one image at that opacity: its parts are merged
    // first (Hatsumira's opening sky: a 38% sepia copy of a sky + cloud-layer image would otherwise
    // show the cloud layer's outline wherever both parts stack).
    const bool blurred = img.blurX > 0 || img.blurY > 0;
    const std::string blurSuffix = (blurred ? "|blur=" + std::to_string(img.blurX) + "," + std::to_string(img.blurY) : "") +
                                   (img.filterApplied.empty() ? "" : "|mode=" + img.filterApplied);
    if (img.parts.size() > 1 && alpha < 0.999f && !blurred) {
        bool animated = false;
        for (const auto& part : img.parts) animated = animated || animation(part);
        std::vector<std::string> parts = img.parts;
        for (auto& part : parts) part += blurSuffix;
        if (const Texture* t = animated ? nullptr : partsTexture(parts)) {
            flush();
            premul_ = true;
            emit(Mode::Texture, t->id, static_cast<float>(t->offsetX), static_cast<float>(t->offsetY),
                 static_cast<float>(t->width), static_cast<float>(t->height), 0xFFFFFFFF);
            flush();
            premul_ = false;
            return;
        }
    }
    for (size_t k = 0; k < img.parts.size(); ++k) {
        const std::string& part = img.parts[k];
        // Parts with an ANM script (`<name>.anm`) show the frame it last named that the image has
        // (cs2.exe 0x6500e0 changes frames only for an id it finds: Hatsumira's mask_noizu.anm
        // cycles frames 1-4 of a 0-3 image, and the static stays on). Before the script names one,
        // and while it is stopped, the part's own frame.
        const cs2::AnmPlayer* player = img.anm && img.anm->parts == img.parts ? img.anm->player(k) : nullptr;
        if (!player && !(img.anm && img.anm->parts == img.parts) && animation(part)) {
            auto& loose = looseAnm_[part];
            if (!loose) loose = std::make_unique<cs2::AnmPlayer>(animations_[part], 1);
            loose->advance(frame);
            player = loose.get();
        }
        const Texture* t = nullptr;
        if (player && !player->shown()) continue;  // DISP 0
        if (player && !player->history().empty()) {
            for (int32_t id : player->history()) {
                if ((t = texture(part + "#" + std::to_string(id) + blurSuffix))) break;
            }
        } else {
            t = texture(part + blurSuffix);
        }
        if (!t) continue;
        emit(Mode::Texture, t->id, static_cast<float>(t->offsetX - t->baseX), static_cast<float>(t->offsetY - t->baseY),
             static_cast<float>(t->width), static_cast<float>(t->height), 0xFFFFFFFF);
    }
}

StageRenderer::EmoteEntry* StageRenderer::emote(const std::shared_ptr<const cs2::EmoteModel>& model) {
    if (!model) return nullptr;
    for (auto it = emotes_.begin(); it != emotes_.end(); ++it) {
        if (it->model != model) continue;
        emotes_.splice(emotes_.begin(), emotes_, it);
        return &emotes_.front();
    }
    EmoteEntry e;
    e.model = model;
    glGenTextures(1, &e.tex);
    glBindTexture(GL_TEXTURE_2D, e.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, model->textureWidth(), model->textureHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 model->rgba().data());
    setTexParams(GL_LINEAR);
    emotes_.push_front(std::move(e));
    // A scene shows at most a handful of characters; older textures are dropped.
    while (emotes_.size() > 8) {
        if (emotes_.back().tex) glDeleteTextures(1, &emotes_.back().tex);
        emotes_.pop_back();
    }
    return &emotes_.front();
}

void StageRenderer::drawEmote(const cs2::StageImage& img, EmoteEntry& e, uint64_t frame, const PlaneXform& plane,
                              float scale, float ox, float oy, float ax, float ay, float sx, float sy, float alpha) {
    // The pose, as triangles over each part's grid: model space -> screen (cs2.exe 0x574070: the
    // model at the anchor plus the profile's offset plus #scaleoffset x (1 - zoom), scaled by #scale
    // x zoom; the bank's x zoom serves both axes) -> plane.
    (void)sy;
    const float zoom = sx;
    const float ex = static_cast<float>(img.emoteX) + img.emoteScaleOffX * (1.0f - zoom);
    const float ey = static_cast<float>(img.emoteY) + img.emoteScaleOffY * (1.0f - zoom);
    const float k = img.emoteScale * zoom;
    // The bank's turn: the model's root layer turned about its origin (emotedriver 0x1001bb90:
    // degrees as node 0's angle, the layers' convention), before scale and placement.
    const float deg = img.rotation + cs2::evaluate(img.motions, frame).rotation;
    const float rc = std::cos(deg * 3.14159265f / 180.0f), rs = std::sin(deg * 3.14159265f / 180.0f);
    auto draws = e.model->evaluate(img.emotePlayer->values(), 0);
    emitEmote(draws, e, [&](float x, float y) {
        const float tx = rc * x - rs * y, ty = rs * x + rc * y;
        Vec2 pl = plane.apply(ax + ex + tx * k, ay + ey + ty * k);
        return Vec2{ox + pl.x * scale, oy + pl.y * scale};
    }, tone_, alpha, true);
}

void StageRenderer::emitEmote(const std::vector<cs2::EmoteModel::Draw>& draws, const EmoteEntry& e,
                              const std::function<Vec2(float, float)>& toScreen, std::array<float, 3> tone,
                              float alpha, bool stencils) {
    const float tw = static_cast<float>(e.model->textureWidth()), th = static_cast<float>(e.model->textureHeight());
    if (!batch_.empty() && (batchMode_ != Mode::Texture || batchTex_ != e.tex)) flush();
    batchMode_ = Mode::Texture;
    batchTex_ = e.tex;
    auto emit = [&](const cs2::EmoteModel::Draw& d, float a) {
        const int n = d.grid;
        std::vector<Vertex> pts(static_cast<size_t>((n + 1) * (n + 1)));
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                size_t k = static_cast<size_t>(j * (n + 1) + i);
                Vec2 sp = toScreen(d.xy[k * 2], d.xy[k * 2 + 1]);
                float u = (d.icon->left + d.icon->width * static_cast<float>(i) / n) / tw;
                float v = (d.icon->top + d.icon->height * static_cast<float>(j) / n) / th;
                pts[k] = {sp.x, sp.y, u, v, tone[0], tone[1], tone[2], a};
            }
        }
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                size_t k = static_cast<size_t>(j * (n + 1) + i);
                const Vertex &p00 = pts[k], &p10 = pts[k + 1], &p01 = pts[k + n + 1], &p11 = pts[k + n + 2];
                batch_.insert(batch_.end(), {p00, p10, p01, p10, p11, p01});
            }
        }
    };
    for (const auto& d : draws) {
        const float a = alpha * d.opacity;
        if (a <= 0 || !d.icon) continue;
        if ((d.blend == 3) != multiply_) {
            flush();
            multiply_ = d.blend == 3;
        }
        if (stencils && !d.stencil.empty()) {
            // emotedriver.dll's stencil back end (0x10062cb0, 0x10062f70, 0x100633b0): the stencil
            // cleared, each mask's images drawn unseen (depth always failing, z-fail raising the
            // stencil where it equals the masks so far, alpha test over 64/255), then the image
            // where it equals their count.
            flush();
            const bool mul = multiply_;
            multiply_ = false;
            glEnable(GL_STENCIL_TEST);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            alphaTest_ = true;
            int ref = 0;
            for (size_t k = 0; k < d.stencil.size() && k < d.maskDraws.size(); ++k) {
                if (d.stencil[k].mode != 1) continue;
                glStencilFunc(GL_EQUAL, ref, 0xFF);
                glStencilOp(GL_KEEP, GL_INCR, GL_INCR);
                for (const auto& m : d.maskDraws[k]) {
                    if (m.icon) emit(m, alpha * m.opacity);
                }
                flush();
                ++ref;
            }
            alphaTest_ = false;
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_EQUAL, ref, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            multiply_ = mul;
            emit(d, a);
            flush();
            glDisable(GL_STENCIL_TEST);
            continue;
        }
        emit(d, a);
    }
    if (multiply_) {
        flush();
        multiply_ = false;
    }
}

void StageRenderer::sweepEmoteFades(uint64_t frame) {
    for (auto it = emoteFades_.begin(); it != emoteFades_.end();) {
        if (it->used + 2 < frame || it->used > frame) {
            glDeleteTextures(1, &it->a);
            glDeleteTextures(1, &it->b);
            it = emoteFades_.erase(it);
        } else {
            ++it;
        }
    }
}

GLuint StageRenderer::poseTexture(EmoteEntry& e, const cs2::EmotePlayer& player, float offX, float offY, float modelScale,
                                  float x0, float y0, float k, int w, int h) {
    // The pose over clear black, straight-alpha parts leaving premultiplied colour; rows go in
    // mirrored so the texture reads top row first.
    flush();
    GLint prevFbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    bool scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    float prevW = targetW_, prevH = targetH_;
    const auto tone = tone_;
    tone_ = {1, 1, 1};
    const bool premul = premul_;
    premul_ = false;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    setTexParams(GL_LINEAR);
    GLuint fbo = 0, depthStencil = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffers(1, &depthStencil);  // for the pose's stencils
    glBindRenderbuffer(GL_RENDERBUFFER, depthStencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthStencil);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    begin(w, h);
    auto draws = e.model->evaluate(player.values(), 0);
    emitEmote(draws, e, [&](float x, float y) {
        return Vec2{(offX + x * modelScale - x0) * k, static_cast<float>(h) - (offY + y * modelScale - y0) * k};
    }, {1, 1, 1}, 1.0f, true);
    flush();
    multiply_ = false;
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depthStencil);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    begin(static_cast<int>(prevW), static_cast<int>(prevH));
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);
    tone_ = tone;
    premul_ = premul;
    return tex;
}

void StageRenderer::drawEmoteFade(const cs2::StageImage& img, uint64_t frame, const PlaneXform& plane, float scale,
                                  float ox, float oy, float ax, float ay, float sx, float sy, float alpha) {
    // cs2.exe's bank crossfade (0x4dae30): the old pose and the new, both still, mixed by
    // min(t - start, frames) / frames (0x4f9430 lerps each channel).
    EmoteEntry* oldE = emote(img.emoteFadeFrom->modelPtr());
    EmoteEntry* newE = emote(img.emotePlayer->modelPtr());
    if (!oldE || !newE) return;
    EmoteFade* f = nullptr;
    for (auto& c : emoteFades_) {
        if (c.from == img.emoteFadeFrom.get() && c.start == img.emoteFadeStart) f = &c;
    }
    // Bank-local units: screen offset from the anchor over the zoom, so each pose sits at its
    // placement offset / zoom and is drawn at its #scale (see drawEmote).
    (void)sy;
    const float zoom = sx;
    if (std::fabs(zoom) < 1e-4f) return;
    const float oldX = (static_cast<float>(img.emoteFadeX) + img.emoteFadeScaleOffX * (1.0f - zoom)) / zoom;
    const float oldY = (static_cast<float>(img.emoteFadeY) + img.emoteFadeScaleOffY * (1.0f - zoom)) / zoom;
    const float newX = (static_cast<float>(img.emoteX) + img.emoteScaleOffX * (1.0f - zoom)) / zoom;
    const float newY = (static_cast<float>(img.emoteY) + img.emoteScaleOffY * (1.0f - zoom)) / zoom;
    if (!f) {
        // Joint bounds of both poses, bank-local.
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        auto bounds = [&](const EmoteEntry& e, const cs2::EmotePlayer& p, float offX, float offY, float ms) {
            for (const auto& d : e.model->evaluate(p.values(), 0)) {
                if (d.opacity <= 0 || !d.icon) continue;
                for (size_t q = 0; q + 1 < d.xy.size(); q += 2) {
                    x0 = std::min(x0, offX + d.xy[q] * ms), x1 = std::max(x1, offX + d.xy[q] * ms);
                    y0 = std::min(y0, offY + d.xy[q + 1] * ms), y1 = std::max(y1, offY + d.xy[q + 1] * ms);
                }
            }
        };
        bounds(*oldE, *img.emoteFadeFrom, oldX, oldY, img.emoteFadeScale);
        bounds(*newE, *img.emotePlayer, newX, newY, img.emoteScale);
        if (x1 <= x0 || y1 <= y0) return;
        x0 = std::floor(x0) - 1, y0 = std::floor(y0) - 1, x1 = std::ceil(x1) + 1, y1 = std::ceil(y1) + 1;
        // Texels per unit: about the screen's, at most 4096 a side.
        float k = std::clamp(scale * std::max(std::fabs(sx), std::fabs(sy)), 0.25f, 4.0f);
        k = std::min({k, 4096.0f / (x1 - x0), 4096.0f / (y1 - y0)});
        int w = std::max(1, static_cast<int>(std::ceil((x1 - x0) * k))), h = std::max(1, static_cast<int>(std::ceil((y1 - y0) * k)));
        EmoteFade nf;
        nf.from = img.emoteFadeFrom.get();
        nf.start = img.emoteFadeStart;
        nf.x0 = x0, nf.y0 = y0, nf.w = static_cast<float>(w) / k, nf.h = static_cast<float>(h) / k;
        nf.a = poseTexture(*oldE, *img.emoteFadeFrom, oldX, oldY, img.emoteFadeScale, x0, y0, k, w, h);
        nf.b = poseTexture(*newE, *img.emotePlayer, newX, newY, img.emoteScale, x0, y0, k, w, h);
        emoteFades_.push_back(nf);
        f = &emoteFades_.back();
    }
    f->used = frame;
    const float t = static_cast<float>(std::min<uint64_t>(frame - img.emoteFadeStart, static_cast<uint64_t>(img.emoteFadeFrames)));
    const float mixT = t / static_cast<float>(std::max(1, img.emoteFadeFrames));
    flush();
    batchMode_ = Mode::Mix;
    batchTex_ = f->a;
    batchTex2_ = f->b;
    batchMix_ = mixT;
    const float xs[4] = {f->x0, f->x0 + f->w, f->x0 + f->w, f->x0};
    const float ys[4] = {f->y0, f->y0, f->y0 + f->h, f->y0 + f->h};
    const float us[4] = {0, 1, 1, 0}, vs[4] = {0, 0, 1, 1};
    // The bank's turn (see drawEmote), about the new pose's origin.
    const float deg = img.rotation + cs2::evaluate(img.motions, frame).rotation;
    const float rc = std::cos(deg * 3.14159265f / 180.0f), rs = std::sin(deg * 3.14159265f / 180.0f);
    Vertex c[4];
    for (int q = 0; q < 4; ++q) {
        const float dx = xs[q] - newX, dy = ys[q] - newY;
        const float bx = newX + rc * dx - rs * dy, by = newY + rs * dx + rc * dy;
        Vec2 pl = plane.apply(ax + bx * zoom, ay + by * zoom);
        c[q] = {ox + pl.x * scale, oy + pl.y * scale, us[q], vs[q], tone_[0], tone_[1], tone_[2], alpha};
    }
    batch_.insert(batch_.end(), {c[0], c[1], c[3], c[1], c[2], c[3]});
    flush();
}

const StageRenderer::Texture* StageRenderer::partsTexture(const std::vector<std::string>& parts) {
    std::string key = "@@parts:";
    for (const auto& p : parts) key += p + "|";
    if (auto it = textures_.find(key); it != textures_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        return &it->second;
    }
    // Each part sits at (offset - base) from the image's anchor; the merged canvas spans them all.
    struct Piece {
        GLuint id;
        float x, y, w, h;
    };
    std::vector<Piece> pieces;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& p : parts) {
        const Texture* t = texture(p);
        if (!t) continue;
        Piece pc{t->id, static_cast<float>(t->offsetX - t->baseX), static_cast<float>(t->offsetY - t->baseY),
                 static_cast<float>(t->width), static_cast<float>(t->height)};
        x0 = std::min(x0, pc.x), y0 = std::min(y0, pc.y), x1 = std::max(x1, pc.x + pc.w), y1 = std::max(y1, pc.y + pc.h);
        pieces.push_back(pc);
    }
    if (pieces.empty()) return nullptr;
    int w = static_cast<int>(x1 - x0), h = static_cast<int>(y1 - y0);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return nullptr;

    flush();
    GLint prevFbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    bool scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    float prevW = targetW_, prevH = targetH_;

    Texture t;
    t.width = w;
    t.height = h;
    t.offsetX = static_cast<int>(x0);
    t.offsetY = static_cast<int>(y0);
    t.bytes = static_cast<size_t>(w) * h * 4;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    setTexParams(GL_LINEAR);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.id, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    begin(w, h);
    // Straight-alpha parts over clear black leave premultiplied colour (drawn with uPremul). Rows go
    // in mirrored so the texture reads top row first, like the decoded images.
    for (const auto& pc : pieces) {
        quad(Mode::Texture, pc.id, {pc.x - x0, static_cast<float>(h) - (pc.y - y0) - pc.h, pc.w, pc.h}, 0, 1, 1, 0,
             0xFFFFFFFF, 1.0f);
    }
    flush();
    glDeleteFramebuffers(1, &fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    begin(static_cast<int>(prevW), static_cast<int>(prevH));
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (stencil) glEnable(GL_STENCIL_TEST);

    lru_.push_front(key);
    t.lru = lru_.begin();
    textureBytes_ += t.bytes;
    auto& stored = textures_.emplace(key, t).first->second;
    evict();
    return textures_.count(key) ? &stored : nullptr;
}

const cs2::Kx2Mesh* StageRenderer::mesh(const std::string& name) {
    auto it = meshes_.find(name);
    if (it == meshes_.end()) {
        std::optional<cs2::Kx2Mesh> m;
        if (auto ref = assets_.find(name + ".kx2")) {
            try {
                m = cs2::Kx2Mesh::parse(ref->archive->read(*ref->entry));
            } catch (const Error&) {
            }
        }
        it = meshes_.emplace(name, std::move(m)).first;
    }
    return it->second ? &*it->second : nullptr;
}

const cs2::AnmScript* StageRenderer::animation(const std::string& part) {
    auto it = animations_.find(part);
    if (it == animations_.end()) {
        std::shared_ptr<const cs2::AnmScript> anm;
        if (auto ref = assets_.find(part + ".anm")) {
            try {
                if (auto s = cs2::AnmScript::parse(ref->archive->read(*ref->entry))) anm = std::make_shared<const cs2::AnmScript>(std::move(*s));
            } catch (const Error&) {
            }
        }
        it = animations_.emplace(part, std::move(anm)).first;
    }
    return it->second.get();
}

std::map<int32_t, std::vector<StageRenderer::PlaneItem>> StageRenderer::planeItems(const cs2::Stage& stage) const {
    std::map<int32_t, std::vector<PlaneItem>> byPlane;
    // E-mote banks are not in these planes: each is a layer of its own (see drawScene).
    for (cs2::Layer layer : {cs2::Layer::Bg, cs2::Layer::Cg, cs2::Layer::Eg, cs2::Layer::Fg}) {
        for (const auto& [index, img] : stage.layer(layer)) byPlane[img.plane].push_back({layer, index, &img, 0, 0});
    }
    // A plane moved in VRAM (`pl N vram x y`) shows whatever is drawn into its window there,
    // including other planes' images.
    std::map<int32_t, std::vector<PlaneItem>> shared;
    for (const auto& [n, p] : stage.planes) {
        if (!p.hasVram) continue;
        auto win = vramWindow(stage, n);
        if (!win) continue;
        std::vector<PlaneItem> items;
        for (const auto& [src, own] : byPlane) {
            auto from = src == n ? win : vramWindow(stage, src);
            if (!from) continue;
            const auto& w = *win;
            const auto& f = *from;
            bool overlap = f[0] < w[0] + w[2] && w[0] < f[0] + f[2] && f[1] < w[1] + w[3] && w[1] < f[1] + f[3];
            if (!overlap) continue;
            for (PlaneItem it : own) {
                it.offX = static_cast<float>(f[0] - w[0]);
                it.offY = static_cast<float>(f[1] - w[1]);
                items.push_back(it);
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const PlaneItem& x, const PlaneItem& y) {
            return x.layer != y.layer ? x.layer < y.layer : x.index < y.index;
        });
        shared[n] = std::move(items);
    }
    for (auto& [n, items] : shared) byPlane[n] = std::move(items);
    auto imageDepth = [this](const PlaneItem& it) {
        return std::isnan(it.img->depth) ? cs2::defaultDepth(it.layer, it.index, mplFaceFrom_) : it.img->depth;
    };
    for (auto& [n, items] : byPlane) {
        std::stable_sort(items.begin(), items.end(),
                         [&](const PlaneItem& x, const PlaneItem& y) { return imageDepth(x) < imageDepth(y); });
    }
    // A plane showing a video is drawn even with no images on it.
    for (const auto& [n, p] : stage.planes) {
        if (!p.video.empty()) byPlane[n];
    }
    return byPlane;
}

StageRenderer::PlaneVideo* StageRenderer::planeVideo(int32_t n, const cs2::Plane& p, uint64_t frame) {
    PlaneVideo& v = planeVideos_[n];
    if (v.name != p.video) {
        v.name = p.video;
        v.video.reset();
        v.uploaded = false;
        v.start = UINT64_MAX;
        // Scripts name the clip with or without its extension (`michiru_ed_b.mpg`).
        std::string file = p.video;
        if (!(file.size() > 4 && file.compare(file.size() - 4, 4, ".mpg") == 0)) file += ".mpg";
        if (auto ref = assets_.find(file)) v.video = media::MpegVideo::open(ref->archive->openEntry(*ref->entry));
        if (!v.video) {
            missingTextures_[p.video + ".mpg"] = true;
            return nullptr;
        }
        v.video->seek(0);  // loaded: its first picture shows until it plays
    }
    if (!v.video) return nullptr;
    if (p.videoPlays != 0 && v.start != p.videoStart) {
        // A new `vcom play`: from the top.
        v.start = p.videoStart;
        v.lapStart = p.videoStart;
        v.lapsDone = 0;
        v.video->rewind();
    }
    bool changed = !v.uploaded;
    if (p.videoPlays != 0 && frame >= v.lapStart) {
        if (v.video->seek(static_cast<double>(frame - v.lapStart) / 60.0)) changed = true;
        if (v.video->ended() && (p.videoPlays < 0 || v.lapsDone + 1 < p.videoPlays)) {
            ++v.lapsDone;
            v.lapStart = frame;
            v.video->rewind();
            if (v.video->seek(0)) changed = true;
        }
    }
    if (!v.tex) glGenTextures(1, &v.tex);
    glBindTexture(GL_TEXTURE_2D, v.tex);
    if (!v.uploaded) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, v.video->width(), v.video->height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     v.video->rgba().data());
        setTexParams(GL_LINEAR);
        v.uploaded = true;
    } else if (changed) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, v.video->width(), v.video->height(), GL_RGBA, GL_UNSIGNED_BYTE,
                        v.video->rgba().data());
    }
    return &v;
}

void StageRenderer::drawScene(const cs2::Stage& stage, uint64_t frame) {
    glBindFramebuffer(GL_FRAMEBUFFER, scene_.fbo);
    glViewport(0, 0, opt_.virtualWidth, opt_.virtualHeight);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    begin(opt_.virtualWidth, opt_.virtualHeight);
    // Each plane is its own surface, composited in plane order; inside a plane the layers go
    // bg, cg, eg, fg (a plane-0 white flash stays under the backdrop on plane 1).
    std::map<int32_t, std::vector<PlaneItem>> byPlane = planeItems(stage);
    // A colour plane (`pl N $AARRGGBB`) is drawn in its plane's place even with nothing on it.
    for (const auto& [n, p] : stage.planes) {
        if (p.fill) byPlane[n];
    }
    // A plane cross-fading on its own (`draw ... <plane>`) is drawn from its before and after,
    // in its place even when the after has nothing on it.
    const auto& tr = stage.transition;
    bool planeFade = tr.plane >= 0 && tr.from && tr.active(frame);
    if (planeFade) byPlane[tr.plane];
    // Draw order is the engine's depth (base z), back to front: images by their slot's default or
    // `pri`, planes by 5000 + 1000 * plane or `pri`.
    auto imageDepth = [this](const PlaneItem& it) {
        return std::isnan(it.img->depth) ? cs2::defaultDepth(it.layer, it.index, mplFaceFrom_) : it.img->depth;
    };
    std::vector<std::pair<float, int32_t>> order;
    for (auto& [n, items] : byPlane) {
        std::stable_sort(items.begin(), items.end(),
                         [&](const PlaneItem& x, const PlaneItem& y) { return imageDepth(x) < imageDepth(y); });
        auto pit = stage.planes.find(n);
        float d = pit != stage.planes.end() && !std::isnan(pit->second.depth) ? pit->second.depth : cs2::defaultPlaneDepth(n);
        order.push_back({d, n});
    }
    // E-mote banks (sscript 0x0c32f5): bank k is an "m2plane" layer of its own, object 67 + k, placed
    // by vram.xml mpl/mplNN/pos and drawn among the planes at depth 4500 + 1000 k (60100 from bank 10
    // on, 0x0c559b), just under plane k. Hiding or moving plane k leaves it alone. Its own coordinates
    // are screen coordinates. Keyed below the plane numbers.
    constexpr int32_t kBankKey = -1000000;
    // The window's face banks show only while the message window does (sscript 0x0c0e8a /
    // 0x0c14d5: banks 75-76 kept hidden unless m[0x28668], which the window's show / hide sets,
    // 0x000c78; Amakano 0x091031: 77-78, m[0x2b9a8]).
    for (const auto& [k, img] : stage.layer(cs2::Layer::Mpl)) {
        if (k >= mplFaceFrom_ && !facesShown_) continue;
        float d = !std::isnan(img.depth) ? img.depth : cs2::defaultDepth(cs2::Layer::Mpl, k, mplFaceFrom_);
        order.push_back({d, kBankKey - k});
    }
    // Particle layers are render nodes of their own, not drawn through their plane: the load
    // places the layer's node at (0, 0, depth) (Happiness 0x4fd660 -> 0x582480, as a plane's
    // base z), so it goes among the planes by its depth; Happiness 2's scripts lift planes over
    // one with `pri` (planes at 16600 / 16700 over a layer at 16500).
    constexpr int32_t kParticleKey = -2000000;
    for (const auto& [slot, p] : stage.particles) {
        if (p.effect) order.push_back({p.depth, kParticleKey - slot});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& [d, n] : order) {
        if (n <= kParticleKey) {
            drawParticles(stage, stage.particles.at(kParticleKey - n), frame);
            continue;
        }
        if (n <= kBankKey) {
            int32_t k = kBankKey - n;
            drawImage(stage.layer(cs2::Layer::Mpl).at(k), frame, PlaneXform{}, 1.0f, 0, 0, 0, 0);
            continue;
        }
        if (planeFade && n == tr.plane) drawPlaneFade(stage, n, byPlane[n], frame);
        else drawPlane(stage, n, byPlane[n], frame);
    }
    for (auto it = planeVideos_.begin(); it != planeVideos_.end();) {
        auto pit = stage.planes.find(it->first);
        bool keep = (pit != stage.planes.end() && !pit->second.video.empty()) ||
                    (tr.from && tr.from->planes.count(it->first) && !tr.from->planes.at(it->first).video.empty());
        if (keep) {
            ++it;
            continue;
        }
        if (it->second.tex) glDeleteTextures(1, &it->second.tex);
        it = planeVideos_.erase(it);
    }
    flush();
}

void StageRenderer::drawPlaneFade(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items, uint64_t frame) {
    // Task_Eff2D_Fade: the plane's surface goes from its old pixels to its new ones, each pixel
    // old * (1 - t) + new * t, and the plane shows that. Both are drawn into surfaces of their own
    // (premultiplied, as drawing over clear black leaves them), mixed, then composited. The 2D
    // wipes on a plane (Task_Eff2D_Wipe / Wipe2) mix them per pixel through the mask, as the
    // screen's wipes do (kTransitionFs), the mask over the whole screen (the planes the games
    // wipe are screen-sized).
    const auto& tr = stage.transition;
    const cs2::Stage& old = *tr.from;
    auto oldItems = planeItems(old);
    float t = std::clamp(static_cast<float>(frame - tr.start) / static_cast<float>(std::max<uint32_t>(1, tr.frames)), 0.0f, 1.0f);
    const int W = opt_.virtualWidth, H = opt_.virtualHeight;
    flush();
    auto into = [&](const Target& target) {
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        begin(W, H);
    };
    into(fadeOld_);
    if (auto it = oldItems.find(n); it != oldItems.end()) drawPlane(old, n, it->second, frame);
    flush();
    into(fadeNew_);
    drawPlane(stage, n, items, frame);
    flush();
    into(fadeMix_);
    if (tr.kind == cs2::Transition::Kind::Wipe) {
        const Texture* mask = tr.mask.empty() ? nullptr : texture(tr.mask);
        glDisable(GL_BLEND);
        glUseProgram(transitionProgram_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fadeOld_.tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fadeNew_.tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, mask ? mask->id : fadeNew_.tex);
        glUniform1i(uPrev_, 0);
        glUniform1i(uNext_, 1);
        glUniform1i(uMask_, 2);
        glUniform1f(uT_, t);
        glUniform1i(uKind_, 2);
        glUniform1i(uHasMask_, mask ? 1 : 0);
        glUniform1i(uHard_, tr.hard ? 1 : 0);
        glBindVertexArray(emptyVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
        begin(W, H);
    } else {
        glBlendFunc(GL_ONE, GL_ONE);
        auto weighted = [&](const Target& src, float k) {
            uint32_t c = static_cast<uint32_t>(std::lround(std::clamp(k, 0.0f, 1.0f) * 255.0f));
            quad(Mode::Texture, src.tex, {0, 0, static_cast<float>(W), static_cast<float>(H)}, 0, 1, 1, 0,
                 (c << 24) | (c << 16) | (c << 8) | c, 1.0f);
            flush();
        };
        weighted(fadeOld_, 1.0f - t);
        weighted(fadeNew_, t);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, scene_.fbo);
    glViewport(0, 0, W, H);
    begin(W, H);
    premul_ = true;
    quad(Mode::Texture, fadeMix_.tex, {0, 0, static_cast<float>(W), static_cast<float>(H)}, 0, 1, 1, 0, 0xFFFFFFFF, 1.0f);
    flush();
    premul_ = false;
}

std::optional<std::array<int32_t, 4>> StageRenderer::vramWindow(const cs2::Stage& stage, int32_t plane) const {
    auto def = opt_.planeVram.find(plane);
    std::array<int32_t, 4> w{0, 0, 0, 0};
    if (def != opt_.planeVram.end()) w = def->second;
    auto it = stage.planes.find(plane);
    if (it != stage.planes.end()) {
        const cs2::Plane& p = it->second;
        if (p.hasVram) w[0] = p.vramX, w[1] = p.vramY;
        if (p.width > 0 && p.height > 0) w[2] = p.width, w[3] = p.height;
        if (!p.hasVram && def == opt_.planeVram.end()) return std::nullopt;
    } else if (def == opt_.planeVram.end()) {
        return std::nullopt;
    }
    if (w[2] <= 0 || w[3] <= 0) return std::nullopt;
    return w;
}

void StageRenderer::setBlendMode(uint8_t mode) {
    // The engine's D3D blend table (kclib 0x618870): src/dest colour factors per `bmode`.
    static const GLenum kFactors[6][2] = {
        {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA}, {GL_SRC_ALPHA, GL_ONE},  {GL_ONE, GL_ONE_MINUS_SRC_ALPHA},
        {GL_ZERO, GL_SRC_COLOR},                {GL_ONE_MINUS_DST_COLOR, GL_ZERO}, {GL_DST_COLOR, GL_ONE},
    };
    const auto& f = kFactors[mode < 6 ? mode : 0];
    glBlendFuncSeparate(f[0], f[1], GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    blendMode_ = mode;
}

StageRenderer::PlaneXform StageRenderer::afterXform(const cs2::Stage& stage, int32_t plane, uint64_t frame,
                                                    int32_t i) const {
    const cs2::AfterImage& a = stage.planes.at(plane).after;
    // Snapshot i: the plane i frames ago, pushed i steps further; before the history is full, the
    // engine's initial snapshots trail back from where the plane was when it started.
    auto at = [&](int32_t k) {
        int64_t age = static_cast<int64_t>(frame - std::min(frame, a.start));
        PlaneXform x;
        float steps;
        if (k <= age) {
            x = planeXform(stage, plane, frame - static_cast<uint64_t>(k));
            steps = static_cast<float>(k);
        } else {
            x = planeXform(stage, plane, a.start);
            steps = static_cast<float>(2 * age - k);
        }
        x.posX += a.dx * steps;
        x.posY += a.dy * steps;
        return x;
    };
    PlaneXform x = at(i);
    float t = static_cast<float>(i) / static_cast<float>(std::max(1, a.steps()));
    x.alpha = (x.alpha * 255.0f + (static_cast<float>(a.alpha) - x.alpha * 255.0f) * t) / 255.0f;
    x.scaleX *= 1.0f + (a.scaleX - 1.0f) * t;
    x.scaleY *= 1.0f + (a.scaleY - 1.0f) * t;
    if (a.mode == 2) {
        // Each copy is also turned the way the plane travelled through it (its neighbours'
        // positions), on top of its own rotation.
        PlaneXform prev = at(i - 1), next = at(i + 1);
        float dx = next.posX - prev.posX, dy = next.posY - prev.posY;
        if (dx != 0 || dy != 0) {
            float r = std::atan2(dy, dx);
            float c = std::cos(r), sn = std::sin(r);
            // times a z turn by r: [c sn; -sn c]
            float m00 = x.m00 * c - x.m01 * sn, m01 = x.m00 * sn + x.m01 * c;
            float m10 = x.m10 * c - x.m11 * sn, m11 = x.m10 * sn + x.m11 * c;
            x.m00 = m00, x.m01 = m01, x.m10 = m10, x.m11 = m11;
        }
    }
    return x;
}

void StageRenderer::drawPlane(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items, uint64_t frame) {
    auto pit = stage.planes.find(n);
    const cs2::Plane* p = pit == stage.planes.end() ? nullptr : &pit->second;
    PlaneXform xf = planeXform(stage, n, frame);
    if (!xf.visible) return;
    uint8_t mode = p ? p->blendMode : 0;
    if (mode) {
        flush();
        setBlendMode(mode);
    }
    // Afterimages: copies of the plane from the last frames, oldest first, under the plane.
    if (p && p->after.count > 0) {
        const cs2::AfterImage& a = p->after;
        for (int32_t i = a.steps(); i >= 1; --i) {
            if (i % a.every() == 0) drawPlaneContent(stage, n, items, afterXform(stage, n, frame, i), frame);
        }
    }
    const cs2::Kx2Mesh* wave = p && p->raster && p->wave.dir >= 0 && !p->shape.empty() ? mesh(p->shape) : nullptr;
    if (wave) drawRaster(stage, n, items, xf, *wave, p->wave, frame);
    else drawPlaneContent(stage, n, items, xf, frame);
    if (mode) {
        flush();
        setBlendMode(0);
    }
}

void StageRenderer::drawPlaneContent(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items,
                                     const PlaneXform& xf, uint64_t frame, bool clip) {
    auto pit = stage.planes.find(n);
    const cs2::Plane* p = pit == stage.planes.end() ? nullptr : &pit->second;
    if (p && p->fill) {
        // A colour plane (ScenePlayer: `pl N $AARRGGBB`) is the engine's kcFEScriptObjCPlane in place
        // of the plane: a rectangle of its size from the plane's origin, in the plane's colour at its
        // opacity (one ARGB word, so `blend` / `fade` set the fill's alpha); the plane's images are
        // not shown.
        Vec2 c[4] = {xf.apply(0, 0), xf.apply(static_cast<float>(p->fillWidth), 0),
                     xf.apply(static_cast<float>(p->fillWidth), static_cast<float>(p->fillHeight)),
                     xf.apply(0, static_cast<float>(p->fillHeight))};
        quadCorners(Mode::Solid, 0, c, 0, 0, 0, 0, (xf.color & 0xFFFFFFu) | 0xFF000000u, xf.alpha);
        return;
    }
    bool stencil = false, scissor = false;
    if (clip && p) {
        // Shaped planes (`pl N <mesh>`) draw only inside their mesh's triangles.
        if (const cs2::Kx2Mesh* shape = p->shape.empty() ? nullptr : mesh(p->shape)) {
            flush();
            glEnable(GL_STENCIL_TEST);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            for (const auto& v : shape->vertices) {
                Vec2 s = xf.apply(v.x, v.y);
                batch_.push_back({s.x, s.y, 0, 0, 1, 1, 1, 1});
            }
            batchMode_ = Mode::Solid;
            flush();
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            stencil = true;
        }
        // Planes with their own size clip what is drawn on them to that surface.
        if (p->width > 0 && p->height > 0) {
            flush();
            float w = static_cast<float>(p->width), h = static_cast<float>(p->height);
            Vec2 c[4] = {xf.apply(0, 0), xf.apply(w, 0), xf.apply(w, h), xf.apply(0, h)};
            float x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
            for (const auto& v : c) {
                x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
            }
            // The scene target is drawn top row first; GL scissor counts from the bottom.
            glEnable(GL_SCISSOR_TEST);
            glScissor(static_cast<GLint>(std::floor(x0)), static_cast<GLint>(std::floor(targetH_ - y1)),
                      static_cast<GLsizei>(std::ceil(x1 - x0)), static_cast<GLsizei>(std::ceil(y1 - y0)));
            scissor = true;
        }
    }
    if (p && !p->video.empty()) {
        // The clip is the plane's picture, its top-left at the plane's local origin.
        if (PlaneVideo* v = planeVideo(n, *p, frame)) {
            float w = static_cast<float>(v->video->width()), h = static_cast<float>(v->video->height());
            Vec2 c[4] = {xf.apply(0, 0), xf.apply(w, 0), xf.apply(w, h), xf.apply(0, h)};
            quadCorners(Mode::Texture, v->tex, c, 0, 0, 1, 1, xf.color | 0xFF000000u,
                        xf.alpha * static_cast<float>((xf.color >> 24) & 0xFF) / 255.0f);
        }
    }
    // A mask image on the plane (`\\ma`, Amakano 2's split screens and round portrait cut-outs) is
    // not drawn: its alpha limits where the plane's other images show. Only the alpha channel is
    // used by the games (`\\mr` `\\mg` `\\mb` read as alpha too).
    const GLuint savedMask = screenMask_;
    const Rect savedMaskRect = screenMaskRect_;
    for (const auto& it : items) {
        if (it.img->maskChannel == 0) continue;
        auto sh = opt_.imageShift.find({it.layer, it.index});
        float sx = sh == opt_.imageShift.end() ? 0.0f : static_cast<float>(sh->second.first);
        float sy = sh == opt_.imageShift.end() ? 0.0f : static_cast<float>(sh->second.second);
        ImageMask m;
        drawImage(*it.img, frame, xf, 1.0f, 0, 0, sx + it.offX, sy + it.offY, &m);
        flush();
        screenMask_ = m.tex;
        screenMaskRect_ = m.rect;
        if (!m.tex) screenMaskRect_ = {0, 0, 0, 0};
        break;
    }
    for (const auto& it : items) {
        if (it.img->maskChannel != 0) continue;
        auto sh = opt_.imageShift.find({it.layer, it.index});
        float sx = sh == opt_.imageShift.end() ? 0.0f : static_cast<float>(sh->second.first);
        float sy = sh == opt_.imageShift.end() ? 0.0f : static_cast<float>(sh->second.second);
        drawImage(*it.img, frame, xf, 1.0f, 0, 0, sx + it.offX, sy + it.offY);
    }
    if (screenMask_ != savedMask || screenMaskRect_.w != savedMaskRect.w) {
        flush();
        screenMask_ = savedMask;
        screenMaskRect_ = savedMaskRect;
    }
    if (stencil || scissor) {
        flush();
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
    }
}

void StageRenderer::drawParticles(const cs2::Stage& stage, const cs2::ParticleLayer& p, uint64_t frame) {
    // The engine's draw callback (Happiness 0x4fcf70, Amakano 2 0x586b90) works in screen terms
    // (cs2::projectParticles) and applies only what the layer is attached to: sscript attaches
    // every layer to the camera (native 1068 -> message 0x250, Happiness sscript 0x09f8e1), whose
    // position, scale and matrix (0x580d00 / 0x580db0 / 0x580d50) give C - P + S R (v - C). The
    // plane is not applied: the plugins follow it themselves from the fields the engine fills
    // (ParticleEffect::setPlanePos). Nor are the plane's alpha, colour or visibility.
    const Texture* t = texture(p.texture + "|canvas");
    if (!t) return;
    PlaneXform xf;
    cameraXform(stage, frame, xf);
    int32_t alpha = std::clamp(p.alpha.value(frame), 0, 255);
    auto quads = cs2::projectParticles(*p.effect, static_cast<float>(opt_.virtualWidth), static_cast<float>(opt_.virtualHeight), alpha);
    // The layer's own blend (the plugin's, cs2::ParticleEffect::blend()), then the one before.
    const uint8_t before = blendMode_;
    const uint8_t blend = static_cast<uint8_t>(std::clamp(p.effect->blend(), 0, 5));
    if (blend != before) {
        flush();
        setBlendMode(blend);
    }
    // The plugin's sampler states (cs2::ParticleEffect::samplerStates): the texture's address
    // modes for this layer's draw only. Otherwise the engine's MIRROR, which within the texture's
    // [0, 1] samples as the renderer's CLAMP_TO_EDGE does.
    const auto& sampler = p.effect->samplerStates();
    bool addressed = false;
    for (const auto& s : sampler) {
        if (s.type != 1 && s.type != 2) continue;
        const GLint mode = s.value == 1 ? GL_REPEAT : s.value == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
        if (!addressed) flush();
        addressed = true;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t->id);
        glTexParameteri(GL_TEXTURE_2D, s.type == 1 ? GL_TEXTURE_WRAP_S : GL_TEXTURE_WRAP_T, mode);
    }
    for (const auto& q : quads) {
        if ((q.alpha[0] | q.alpha[1] | q.alpha[2] | q.alpha[3]) == 0) continue;
        // Record corners are top-left, top-right, bottom-left, bottom-right; each keeps its own
        // colour and alpha (the engine's vertices, a two-triangle strip: 0x580bf0 type 5).
        Vec2 c[4] = {xf.apply(q.c[0][0], q.c[0][1]), xf.apply(q.c[1][0], q.c[1][1]), xf.apply(q.c[3][0], q.c[3][1]),
                     xf.apply(q.c[2][0], q.c[2][1])};
        auto argb = [&](int k) { return static_cast<uint32_t>(q.alpha[k]) << 24 | q.rgb[k]; };
        const uint32_t col[4] = {argb(0), argb(1), argb(3), argb(2)};
        quadCorners(Mode::Texture, t->id, c, q.u0, q.v0, q.u1, q.v1, col);
    }
    if (addressed) {
        flush();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t->id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (blend != before) {
        flush();
        setBlendMode(before);
    }
}

void StageRenderer::drawRaster(const cs2::Stage& stage, int32_t n, const std::vector<PlaneItem>& items,
                               const PlaneXform& xf, const cs2::Kx2Mesh& grid, const cs2::Raster& raster,
                               uint64_t frame) {
    const cs2::Raster wave = raster.at(frame);  // through a `raster fade`
    // The plane is drawn flat into its own surface, then through its mesh with the vertices swayed
    // (Task_Eff_Raster's vertex callback) and the texture coordinates left in place.
    int w = std::clamp(static_cast<int>(std::ceil(grid.x1)), 1, 4096);
    int h = std::clamp(static_cast<int>(std::ceil(grid.y1)), 1, 4096);
    flush();
    if (!surface_.fbo || surfaceW_ != w || surfaceH_ != h) {
        if (surface_.fbo) {
            glDeleteFramebuffers(1, &surface_.fbo);
            glDeleteTextures(1, &surface_.tex);
        }
        glGenTextures(1, &surface_.tex);
        glBindTexture(GL_TEXTURE_2D, surface_.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        setTexParams(GL_LINEAR);
        glGenFramebuffers(1, &surface_.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, surface_.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface_.tex, 0);
        surfaceW_ = w;
        surfaceH_ = h;
    }
    GLint prevFbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    float prevW = targetW_, prevH = targetH_;
    glBindFramebuffer(GL_FRAMEBUFFER, surface_.fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    begin(w, h);
    PlaneXform flat;
    drawPlaneContent(stage, n, items, flat, frame, false);
    flush();
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    begin(static_cast<int>(prevW), static_cast<int>(prevH));
    if (auto p = stage.planes.find(n); p != stage.planes.end() && p->second.blendMode) setBlendMode(p->second.blendMode);

    // The wave scrolls one pixel per 60 fps frame and wraps every whole <cycle> pixels.
    const float kTwoPi = 6.2831853f;
    int32_t cycle = std::max(1, static_cast<int32_t>(wave.cycle));
    // Task_Eff_Raster counts its time at the `effspd` speed (setEffectSpeed).
    float phase = std::fmod(std::floor(static_cast<float>(frame - std::min(frame, wave.start)) * effectSpeed_), static_cast<float>(cycle));
    for (const auto& v : grid.vertices) {
        float x = v.x, y = v.y;
        if (wave.dir == 1) x += std::sin(std::fmod(phase + v.y, wave.cycle) * kTwoPi / wave.cycle) * wave.amplitude;
        else y += std::sin(std::fmod(phase + v.x, wave.cycle) * kTwoPi / wave.cycle) * wave.amplitude;
        Vec2 s = xf.apply(x, y);
        batch_.push_back({s.x, s.y, v.x / static_cast<float>(w), 1.0f - v.y / static_cast<float>(h), 1, 1, 1, xf.alpha});
    }
    batchMode_ = Mode::Texture;
    batchTex_ = surface_.tex;
    premul_ = true;
    flush();
    premul_ = false;
}

void StageRenderer::composite(const cs2::Stage& stage, uint64_t frame) {
    const auto& tr = stage.transition;
    if (tr.id != lastTransitionId_) {
        // A new commit: what was on screen becomes the transition's starting image.
        std::swap(prev_, display_);
        // Any screen shown before counts, whatever its id: loading a save restarts the stage's
        // commit count at 0, and the next transition (a quick load's first `rdraw`) still fades
        // out of what is on screen.
        havePrev_ = displayed_;
        lastTransitionId_ = tr.id;
    }
    float t = 1.0f;
    int kind = 0;
    // A `draw` on one plane fades that plane in drawScene; the screen itself cuts.
    if (havePrev_ && tr.plane < 0 && tr.kind != cs2::Transition::Kind::Cut && tr.frames > 0 && frame < tr.start + tr.frames) {
        t = static_cast<float>(frame - tr.start) / static_cast<float>(tr.frames);
        kind = tr.kind == cs2::Transition::Kind::Fade ? 1 : tr.kind == cs2::Transition::Kind::Mosaic ? 3
             : tr.kind == cs2::Transition::Kind::FadeMosaic ? 4 : 2;
    }
    // The page turn draws the new screen whole, then the old one on the turning page; the break
    // the new screen whole, then the old one's pieces.
    const bool pageFlip = kind != 0 && tr.kind == cs2::Transition::Kind::PageFlip;
    const bool breaking = kind != 0 && tr.kind == cs2::Transition::Kind::Break;
    const bool cellFlip = kind != 0 && tr.kind == cs2::Transition::Kind::CellFlip;
    if (pageFlip || breaking || cellFlip) kind = 0;
    const Texture* mask = kind == 2 && !tr.mask.empty() ? texture(tr.mask) : nullptr;

    glBindFramebuffer(GL_FRAMEBUFFER, display_.fbo);
    glViewport(0, 0, opt_.virtualWidth, opt_.virtualHeight);
    glDisable(GL_BLEND);
    glUseProgram(transitionProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, prev_.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scene_.tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, mask ? mask->id : scene_.tex);
    glUniform1i(uPrev_, 0);
    glUniform1i(uNext_, 1);
    glUniform1i(uMask_, 2);
    glUniform1f(uT_, pageFlip || breaking || cellFlip ? 1.0f : t);
    glUniform1i(uKind_, kind);
    if (kind == 4) {
        // kcTaskRenderFMosaic (cs2.exe 0x504a30): one level for each frame i where the block size
        // (int)(i / frames * (size - 1) + 1) changes, its alpha 255 - (int)(i / frames * 255) (the
        // first level: blocks of 1, 255); each frame shows the last level begun (0x505410).
        const int32_t n = static_cast<int32_t>(tr.frames), i = static_cast<int32_t>(frame - tr.start);
        auto block = [&](int32_t j) {
            return static_cast<int32_t>(static_cast<float>(j) / static_cast<float>(n) * (static_cast<float>(tr.mosaic) - 1.0f) + 1.0f);
        };
        int32_t from = i;
        while (from > 0 && block(from - 1) == block(i)) --from;
        const int32_t alpha = from == 0 ? 255 : 255 - static_cast<int32_t>(static_cast<float>(from) / static_cast<float>(n) * 255.0f);
        glUniform1f(uT_, static_cast<float>(alpha) / 255.0f);
        const float b = static_cast<float>(block(i));
        glUniform2f(uMosaic_, b / opt_.virtualWidth, b / opt_.virtualHeight);
    } else {
        // Block size: 1 -> max over the first half, max -> 1 over the second, whole pixels.
        float half = t < 0.5f ? t / 0.5f : (1.0f - t) / 0.5f;
        float block = std::floor(1.0f + half * static_cast<float>(std::max(1, tr.mosaic) - 1) + 0.5f);
        glUniform2f(uMosaic_, block / opt_.virtualWidth, block / opt_.virtualHeight);
    }
    glUniform1i(uHasMask_, mask ? 1 : 0);
    glUniform1i(uHard_, tr.hard ? 1 : 0);
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
    if (pageFlip) drawPageFlip(tr, t);
    if (cellFlip) drawCellFlip(tr, t);
    if (breaking) drawBreak(tr, frame - tr.start);
    displayed_ = true;
}

void StageRenderer::drawBreak(const cs2::Transition& tr, uint64_t elapsed) {
    // cs2.exe kcTaskRenderWBreak. Pass 0 clears to black and draws the layers to depth 45000 (the
    // new screen, drawn above), pass 1 the old screen's pieces, pass 2 the layers past it (the
    // window, drawn later here). Coordinates are D3D's pretransformed pixels (a corner at -0.5).
    const float W = static_cast<float>(opt_.virtualWidth), H = static_cast<float>(opt_.virtualHeight);
    using M3 = std::array<float, 9>;
    auto mul = [](const M3& a, const M3& b) {
        M3 r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        return r;
    };
    // D3DXMatrixRotationX / Y / Z, for row vectors (v' = v M).
    auto rotX = [](float a) { float c = std::cos(a), s = std::sin(a); return M3{1, 0, 0, 0, c, s, 0, -s, c}; };
    auto rotY = [](float a) { float c = std::cos(a), s = std::sin(a); return M3{c, 0, -s, 0, 1, 0, s, 0, c}; };
    auto rotZ = [](float a) { float c = std::cos(a), s = std::sin(a); return M3{c, s, 0, -s, c, 0, 0, 0, 1}; };
    auto apply = [](const std::array<float, 3>& v, const M3& m) {
        return std::array<float, 3>{v[0] * m[0] + v[1] * m[3] + v[2] * m[6], v[0] * m[1] + v[1] * m[4] + v[2] * m[7],
                                    v[0] * m[2] + v[1] * m[5] + v[2] * m[8]};
    };
    if (tr.id != breakId_ || elapsed < breakElapsed_) {
        // Message 0x209 -> 0x5fda00: the screen in cells, w x h pixels from the top left (the last
        // ones run past the edge), each two triangles (corners 0 1 2 and 1 2 3 of x0 y0, x1 y0,
        // x0 y1, x1 y1). Each piece: its centre (the corners' mean); a speed, random 0-1 times
        // `speed`, along (0, 0, 1) turned about x by (its distance from the screen's centre / the
        // centre's from the corner) x 45 degrees and about z by atan2(dx, -dy) of that offset,
        // then flattened (z 0): straight outwards, faster the farther out; a spin about each axis
        // of `spin` x (0.5 - random 0-1) degrees a frame. The randoms are the engine's MT19937
        // (0x5c1fa0, the output / 2^32), shared with everything else it draws randoms for, so the
        // pieces cannot match a PC run: here a generator of its own, seeded as the engine's first
        // (4357). Fitted: not compared with PC.
        breakId_ = tr.id;
        breakElapsed_ = 0;
        breakPieces_.clear();
        cs2::MersenneTwister rng(4357);
        auto rnd = [&] { return static_cast<float>(static_cast<double>(rng.next()) * 2.3283064365386963e-10); };
        const int cw = std::max(1, tr.breakCellW), ch = std::max(1, tr.breakCellH);
        const float cx = W * 0.5f, cy = H * 0.5f, reach = std::sqrt(cx * cx + cy * cy);
        const float kRad = 3.14159274f / 180.0f;
        for (int y = 0; static_cast<float>(y) < H; y += ch) {
            for (int x = 0; static_cast<float>(x) < W; x += cw) {
                const float xs[4] = {static_cast<float>(x), static_cast<float>(x + cw), static_cast<float>(x), static_cast<float>(x + cw)};
                const float ys[4] = {static_cast<float>(y), static_cast<float>(y), static_cast<float>(y + ch), static_cast<float>(y + ch)};
                for (const auto& tri : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{1, 2, 3}}) {
                    BreakPiece p;
                    for (int k = 0; k < 3; ++k) {
                        const int i = tri[static_cast<size_t>(k)];
                        p.corner[static_cast<size_t>(k)] = {xs[i] - 0.5f, ys[i] - 0.5f, xs[i] / W, ys[i] / H};
                    }
                    for (int a = 0; a < 2; ++a)
                        p.centre[static_cast<size_t>(a)] = (p.corner[0][static_cast<size_t>(a)] + p.corner[1][static_cast<size_t>(a)] +
                                                            p.corner[2][static_cast<size_t>(a)]) / 3.0f;
                    const float dx = p.centre[0] - cx, dy = p.centre[1] - cy;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    const float speed = rnd() * tr.breakSpeed;
                    const M3 aim = mul(rotX(dist / reach * 0.785398185f), rotZ(static_cast<float>(std::atan2(static_cast<double>(dx), -static_cast<double>(dy)))));
                    p.vel = apply({0, 0, speed}, aim);
                    p.vel[2] = 0;
                    for (float& w : p.spin) w = tr.breakSpin * 0.5f * 3.14159274f / 180.0f - rnd() * (tr.breakSpin * kRad);
                    breakPieces_.push_back(p);
                }
            }
        }
    }
    // Each frame (0x5fe640 pass 1): the frames since the last, dt, turn each piece on by its spin x
    // dt about x, then y, then z, and move it by its velocity x dt; a corner is its offset from the
    // centre turned, plus the centre and the movement. Alpha 255 less elapsed x 255 / frames.
    const float dt = static_cast<float>(elapsed - breakElapsed_);
    breakElapsed_ = elapsed;
    if (dt > 0) {
        for (auto& p : breakPieces_) {
            p.turn = mul(mul(mul(p.turn, rotX(p.spin[0] * dt)), rotY(p.spin[1] * dt)), rotZ(p.spin[2] * dt));
            for (int a = 0; a < 3; ++a) p.pos[static_cast<size_t>(a)] += p.vel[static_cast<size_t>(a)] * dt;
        }
    }
    const uint64_t frames = std::max<uint64_t>(1, tr.frames);
    const float alpha = static_cast<float>(255 - std::min<uint64_t>(255, elapsed * 255 / frames)) / 255.0f;
    // Seen from 1000 in front (0x5fea60): a corner with z 0 stays where it is; others scale by
    // 1000 / (1000 - z) about the texture's far corner (W, H; its size, the screen's where the
    // device takes any size), as the engine does. Nearer pieces drawn last (the engine tests depth).
    struct V { float x, y, u, v; };
    std::vector<std::pair<float, std::array<V, 3>>> tris;
    tris.reserve(breakPieces_.size());
    for (const auto& p : breakPieces_) {
        std::array<V, 3> t{};
        float depth = 0;
        for (size_t k = 0; k < 3; ++k) {
            const auto& c = p.corner[k];
            auto q = apply({c[0] - p.centre[0], c[1] - p.centre[1], 0.0f}, p.turn);
            q[0] += p.centre[0] + p.pos[0], q[1] += p.centre[1] + p.pos[1], q[2] += p.pos[2];
            float x = q[0], y = q[1];
            if (q[2] != 0.0f) {
                const float den = 1000.0f - q[2];
                const float s = std::max(0.0f, 1000.0f / (den != 0.0f ? den : 1.0f));
                x = (x - W) * s + W, y = (y - H) * s + H;
            }
            depth += q[2];
            t[k] = {x + 0.5f, y + 0.5f, c[2], 1.0f - c[3]};
        }
        tris.push_back({depth, t});
    }
    std::stable_sort(tris.begin(), tris.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    begin(static_cast<int>(W), static_cast<int>(H));
    glEnable(GL_BLEND);
    batchMode_ = Mode::Texture;
    batchTex_ = prev_.tex;
    for (const auto& [d, t] : tris) {
        for (const auto& v : t) batch_.push_back({v.x, v.y, v.u, v.v, 1, 1, 1, alpha});
    }
    flush();
}

void StageRenderer::drawCellFlip(const cs2::Transition& tr, float t) {
    // cs2.exe kcTaskRenderCFlip (Amakano 2). On start (message 400, 0x592ea0) the screen is cut into
    // w x h cells (0x5925a0), and their corners and centres are turned by sweep = angle + axis
    // about the screen's centre (0x592370: x cos + y sin, y cos - x sin, 0x60fc00's turn); the line
    // runs from the largest turned centre y to the smallest less the band. Each frame (0x5932e0) a
    // cell whose centre is d past the line is turned over by min(d / band, 1) x pi: none, the old
    // screen flat; up to pi / 2 the old screen's cell, from there the new screen's cell by that
    // less pi; pi, the new screen flat. A turn takes the corners relative to the cell's centre,
    // turns them by -axis, about x by the angle (0x60fae0: y cos, z = -y sin), back by axis, sees
    // them from 2000 in front (x and y times 2000 / (2000 + z), 0x608880 / 0x60fe40), adds the
    // centre and turns back by -sweep. The draw (0x593080) fills the screen with the colour, then the
    // old screen's cells, then the new one's.
    const float W = static_cast<float>(opt_.virtualWidth), H = static_cast<float>(opt_.virtualHeight);
    const float cx = W * 0.5f, cy = H * 0.5f;
    const int cw = std::max(1, tr.flipCellW), ch = std::max(1, tr.flipCellH);
    const int cols = (static_cast<int>(W) + cw - 1) / cw, rows = (static_cast<int>(H) + ch - 1) / ch;
    const float kPi = 3.14159274f;
    struct P { float x, y; };
    auto turn = [](P p, float a) { const float c = std::cos(a), s = std::sin(a); return P{p.x * c + p.y * s, p.y * c - p.x * s}; };
    const float sweep = tr.flipAngle + tr.flipAxis;
    auto turned = [&](float x, float y) { return turn(P{x - cx, y - cy}, sweep); };
    float maxY = -10000.0f, minY = 10000.0f;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const P c = turned(static_cast<float>(i * cw) + cw * 0.5f, static_cast<float>(j * ch) + ch * 0.5f);
            maxY = std::max(maxY, c.y), minY = std::min(minY, c.y);
        }
    }
    const float band = std::max(1.0f, tr.flipBand);
    const float line = t * ((minY - band) - maxY) + maxY;
    struct V { float x, y, u, v; };
    std::vector<V> front, back;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const float x0 = static_cast<float>(i * cw), y0 = static_cast<float>(j * ch);
            const float x1 = std::min(W, x0 + cw), y1 = std::min(H, y0 + ch);
            const P c = turned(x0 + cw * 0.5f, y0 + ch * 0.5f);
            float a = 0;
            if (c.y >= line) a = c.y - line >= band ? kPi : (c.y - line) / band * kPi;
            const bool old = a < kPi * 0.5f;
            const float by = old ? a : a - kPi;  // the angle the shown face is turned by
            auto place = [&](float x, float y) {
                V out{x, y, x / W, 1.0f - y / H};
                if (a == 0 || a >= kPi) return out;
                const P v = turned(x, y);
                P d = turn(P{v.x - c.x, v.y - c.y}, -tr.flipAxis);
                const float z = -d.y * std::sin(by);
                d.y *= std::cos(by);
                d = turn(d, tr.flipAxis);
                const float s = std::max(0.0f, 2000.0f / (2000.0f + z));
                const P back = turn(P{d.x * s + c.x, d.y * s + c.y}, -sweep);
                out.x = back.x + cx, out.y = back.y + cy;
                return out;
            };
            const V a0 = place(x0, y0), b0 = place(x1, y0), c0 = place(x0, y1), d0 = place(x1, y1);
            auto& to = old ? front : back;
            to.insert(to.end(), {a0, b0, c0, b0, d0, c0});
        }
    }
    begin(static_cast<int>(W), static_cast<int>(H));
    glDisable(GL_BLEND);
    const Vec2 screen[4] = {{0, 0}, {W, 0}, {W, H}, {0, H}};
    quadCorners(Mode::Solid, 0, screen, 0, 0, 1, 1, tr.flipColor | 0xFF000000u, 1.0f);
    flush();
    for (const auto* cells : {&front, &back}) {
        batchMode_ = Mode::Texture;
        batchTex_ = cells == &front ? prev_.tex : scene_.tex;
        for (const auto& v : *cells) batch_.push_back({v.x, v.y, v.u, v.v, 1, 1, 1, 1});
        flush();
    }
    glEnable(GL_BLEND);
}

void StageRenderer::drawPageFlip(const cs2::Transition& tr, float t) {
    // cs2.exe kcTaskRenderPFlip. The page is the screen in 32-pixel cells (0x591090). On start
    // (message 0x18c, 0x5916e0) each point is turned by the angle about the screen's centre and the
    // span of its turned y noted; each frame (0x591d30) the fold line runs from the largest y to
    // the smallest less the radius, and points past it wrap round a cylinder of that radius (an
    // arc of a / r for the distance a past the line), going on flat beyond the fold's arc (fold x
    // radius) in the turned-over direction. Points are then seen from 2000 in front of the page
    // (x and y times 2000 / (2000 + depth), 0x608880) and turned back.
    const float W = static_cast<float>(opt_.virtualWidth), H = static_cast<float>(opt_.virtualHeight);
    const float cx = W * 0.5f, cy = H * 0.5f, r = tr.flipRadius;
    const float ca = std::cos(tr.flipAngle), sa = std::sin(tr.flipAngle);
    const float cf = std::cos(tr.flipFold), sf = std::sin(tr.flipFold);
    const int cell = 32;
    const int cols = (static_cast<int>(W) + cell - 1) / cell, rows = (static_cast<int>(H) + cell - 1) / cell;
    struct P { float X, Y; };
    auto turned = [&](float x, float y) { float rx = x - cx, ry = y - cy; return P{rx * ca + ry * sa, ry * ca - rx * sa}; };
    float maxY = -100000.0f, minY = 100000.0f;
    for (int j = 0; j <= rows; ++j) {
        for (int i = 0; i <= cols; ++i) {
            P p = turned(std::min(W, static_cast<float>(i * cell)), std::min(H, static_cast<float>(j * cell)));
            maxY = std::max(maxY, p.Y), minY = std::min(minY, p.Y);
        }
    }
    const float line = ((minY - r) - maxY) * t + maxY;
    struct V { float x, y, depth, u, v; };
    auto place = [&](float x, float y) {
        P p = turned(x, y);
        float px = p.X, py = p.Y, dz = 0;
        if (p.Y >= line) {
            float a = p.Y - line;
            if (a >= tr.flipFold * r) {
                float a2 = a - tr.flipFold * r;
                py = line + r * sf + cf * a2;
                dz = r * (cf - 1.0f) - sf * a2;
            } else {
                float b = r > 0 ? a / r : 0.0f;
                py = line + r * std::sin(b);
                dz = r * std::cos(b) - r;
            }
            float s = std::max(0.0f, 2000.0f / (dz + 2000.0f));
            px *= s, py *= s;
        }
        // Back from the turned frame (0x60fc00 with -angle).
        return V{cx + px * ca - py * sa, cy + py * ca + px * sa, dz, x / W, 1.0f - y / H};
    };
    std::vector<std::array<V, 3>> tris;
    tris.reserve(static_cast<size_t>(cols * rows * 2));
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            float x0 = static_cast<float>(i * cell), y0 = static_cast<float>(j * cell);
            float x1 = std::min(W, x0 + cell), y1 = std::min(H, y0 + cell);
            V a = place(x0, y0), b = place(x1, y0), c = place(x0, y1), d = place(x1, y1);
            tris.push_back({a, b, c});
            tris.push_back({b, d, c});
        }
    }
    // The engine draws with depth; here far triangles first (the turned part is nearer).
    std::stable_sort(tris.begin(), tris.end(), [](const auto& p, const auto& q) {
        return p[0].depth + p[1].depth + p[2].depth > q[0].depth + q[1].depth + q[2].depth;
    });
    begin(static_cast<int>(W), static_cast<int>(H));
    glDisable(GL_BLEND);
    batchMode_ = Mode::Texture;
    batchTex_ = prev_.tex;
    for (const auto& tri : tris) {
        for (const auto& v : tri) batch_.push_back({v.x, v.y, v.u, v.v, 1, 1, 1, 1});
    }
    flush();
    glEnable(GL_BLEND);
}

FontAtlas* StageRenderer::atlasFor(const std::string& key, int32_t want) {
    // A font a script chose (`layout font` / `fontind`), loaded once; the game's font otherwise.
    // Bold (the config's `fontweight`, or the string's `layout weight`): the family's bold face
    // where there is one, else the face emboldened as Windows does (FontAtlas fakeBold); the game's
    // font too.
    const int32_t weight = (want > 0 ? want : textWeight_) > 550 ? 700 : 400;
    if (weight == 400 && (key.empty() || !opt_.fontLoader)) return font_.get();
    auto it = fonts_.find({key, weight});
    if (it == fonts_.end()) {
        int face = 0;
        bool fake = false;
        Bytes data = opt_.fontLoader ? opt_.fontLoader(key, weight, face, fake) : Bytes{};
        if (data.empty() && key.empty() && font_->valid()) {
            // The stand-in for a game font Nyanroid lacks (MS Gothic), emboldened.
            data = font_->data();
            face = font_->face();
            fake = !cs2::fontFamilyInfo(data, face).bold;
        }
        std::unique_ptr<FontAtlas> atlas;
        if (!data.empty()) {
            atlas = std::make_unique<FontAtlas>(std::move(data), face, fake);
            if (!atlas->valid()) atlas.reset();
        }
        it = fonts_.emplace(std::make_pair(key, weight), std::move(atlas)).first;
    }
    if (it->second) return it->second.get();
    return key.empty() ? font_.get() : atlasFor({});
}

float StageRenderer::measure(const std::string& text, float sizeV) const {
    int px = std::max(1, static_cast<int>(std::lround(sizeV * viewScale_)));
    float w = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = nextCodepoint(text, i);
        if (prev) w += cur_->kern(prev, cp, px);
        w += cur_->glyph(cp, px).advance;
        prev = cp;
    }
    return w / viewScale_;
}

void StageRenderer::drawText(const std::string& text, float x, float y, float sizeV, uint32_t argb, float alpha,
                             size_t maxChars) {
    int px = std::max(1, static_cast<int>(std::lround(sizeV * viewScale_)));
    float penX = viewX_ + x * viewScale_;
    float baseY = viewY_ + y * viewScale_ + cur_->ascent(px);
    size_t n = 0;
    uint32_t prev = 0;
    float shadow = std::max(1.0f, px / 14.0f);
    for (size_t i = 0; i < text.size() && n < maxChars; ++n) {
        uint32_t cp = nextCodepoint(text, i);
        if (prev) penX += cur_->kern(prev, cp, px);
        const Glyph& g = cur_->glyph(cp, px);
        if (g.x1 > g.x0) {
            Rect r{penX + g.x0, baseY + g.y0, g.x1 - g.x0, g.y1 - g.y0};
            Rect s{r.x + shadow, r.y + shadow, r.w, r.h};
            quad(Mode::Glyph, cur_->texture(), s, g.u0, g.v0, g.u1, g.v1, 0xB0000000, alpha);
            quad(Mode::Glyph, cur_->texture(), r, g.u0, g.v0, g.u1, g.v1, argb, alpha);
        }
        penX += g.advance;
        prev = cp;
    }
}

std::vector<StageRenderer::LaidGlyph> StageRenderer::layoutMessage(const cs2::MessageText& text, const Rect& area,
                                                                   const float sizes[5], float lineHeight,
                                                                   const Rect* alignBox) const {
    // Work in window pixels so wrapping matches the rasterised glyph metrics exactly.
    std::vector<LaidGlyph> out;
    float left = viewX_ + area.x * viewScale_;
    float right = left + area.w * viewScale_;
    float lineH = lineHeight * viewScale_;
    float penX = left;
    float penY = viewY_ + area.y * viewScale_;
    const float startY = penY;
    size_t lineStart = 0;  // index in `out` of the first glyph on the current line

    struct Item {
        uint32_t cp;
        int px;
        float size;
        uint32_t color;
        uint8_t align;
        bool dbcs;      // double-byte in Shift-JIS
        int16_t group;  // `[...]` word group, -1 none
        bool italic;
    };
    std::vector<Item> items;
    for (const auto& span : text.spans) {
        // `\f<n>`: pixels outright; `\f+<n>` / `\f-<n>`: from the level's size (cs2.exe 0x6937a0).
        float size = span.px >= 0 ? static_cast<float>(span.px)
                                  : std::max(1.0f, sizes[std::clamp(static_cast<int>(span.size) + 2, 0, 4)] + span.dpx);
        int px = std::max(1, static_cast<int>(std::lround(size * viewScale_)));
        for (size_t i = 0; i < span.text.size();) {
            const uint32_t cp = nextCodepoint(span.text, i);
            items.push_back({cp, px, size, span.color, span.align, sjisDoubleByte(cp), span.group, span.italic});
        }
    }

    // CatSystem2 lays text out on a fixed grid: a full-width character occupies one cell of
    // the font size, a half-width (ASCII/half-width kana) character half a cell. Glyphs are
    // centred in their cell.
    auto cellOf = [&](const Item& it) {
        float cell = it.size * viewScale_;
        // Proportional text (`layout fontfix 0`): each glyph its own advance, rounded to whole pixels
        // as the engine's FreeType renderer does (Yukikoi's English lines measure exactly so).
        if (proportional_ && !isUserGlyph(it.cp)) return std::round(cur_->glyph(it.cp, it.px).advance);
        // A double-byte character is `size` wide in fixed mode, whatever its glyph's width (grisaia.exe
        // 0x546a30, the GDI measure): Amakano 1's ’ in Meiryo takes a whole cell on PC.
        if (isWide(it.cp) || it.dbcs || isUserGlyph(it.cp)) return cell;
        // A single-byte character in fixed mode: its own width, at least (size + 1) / 2 whole pixels
        // (grisaia.exe 0x546a30, the GDI measure: GetTextExtentPoint32 raised to that minimum). GDI's
        // widths are the hinted advances, the scaled ones rounded to whole pixels: Amakano 1's Meiryo
        // lines on PC run 9-10 pixels longer than floored widths give (MS Gothic's and Grisaia's
        // half-width letters are the same either way).
        return std::max(std::floor((it.size + 1.0f) / 2.0f) * viewScale_, std::round(cur_->glyph(it.cp, it.px).advance));
    };
    auto advanceOf = [&](const Item& it) { return cellOf(it) + letterSpacing_ * viewScale_; };
    // `layout indent` (grisaia.exe 0x54a670 with 0x549ba0 / 0x549af0): when the text opens with a
    // bracket, every later line (wrapped or after a newline) starts that bracket's width in, unless
    // it begins with a bracket itself. The engine's sets: （［｛「『([{｢, and the same without 『.
    auto opensBracket = [](uint32_t cp) {
        switch (cp) {
        case 0xFF08: case 0xFF3B: case 0xFF5B: case 0x300C: case 0x300E: case '(': case '[': case '{': case 0xFF62:
            return true;
        default:
            return false;
        }
    };
    float hang = 0;
    if (hanging_ && !items.empty() && opensBracket(items[0].cp)) hang = advanceOf(items[0]);
    auto lineLeft = [&](size_t next) { return next < items.size() && opensBracket(items[next].cp) ? left : left + hang; };

    // Line breaks (cs2.exe 0x695640 with 0x694b90 / 0x694c50 / 0x694ec0; grisaia.exe 0x54a670 with
    // 0x549c80 / 0x549d40 / 0x549fb0). A glyph that would end past width - margin right ends the
    // line, and the break is then adjusted with the text object's sets (cs2::FesInstance::
    // kKinsokuDefaults, or the script's `layout prof`):
    // - an opener (set 1) is not left at a line's end: it moves down (0x694b90);
    // - a no-line-start character (set 0) that overflows stays on the line when it fits in the box's
    //   full width (the right margin is room for it; a full-width space always stays), else it pulls
    //   up to three characters back down with it so the next line does not start with it (0x694c50);
    // - a group the break falls inside stays whole on the line when its rest fits in the full width,
    //   else it moves down whole (0x694ec0; set 2's …‥― runs too). The engine's groups are `[...]`
    //   (0x696010 / 0x692a60); English text breaks only between words on PC, so each run of
    //   characters other than spaces holding a single-byte one (with the punctuation on it) is a
    //   group here (fitted: Nekopara 3 and Yukikoi's English lines against PC screenshots).
    const float fullRight = right + wrapSlack_ * viewScale_;
    std::u32string sets[3];
    static const std::u32string punct = [] {
        std::u32string all;
        for (const char* src : cs2::FesInstance::kKinsokuDefaults) {
            const std::string t = src;
            for (size_t p = 0; p < t.size();) all.push_back(nextCodepoint(t, p));
        }
        return all;
    }();
    for (int k = 0; k < 3; ++k) {
        const std::string src = kinsoku_ ? kinsoku_[k] : std::string(cs2::FesInstance::kKinsokuDefaults[k]);
        for (size_t p = 0; p < src.size();) sets[k].push_back(nextCodepoint(src, p));
    }
    // The sets hold Shift-JIS codes: Unicode's other readings of the same code count as them.
    auto norm = [](uint32_t cp) -> uint32_t {
        switch (cp) {
        case 0x2014: return 0x2015;  // ― (0x815C)
        case 0x301C: return 0xFF5E;  // ～ (0x8160)
        case 0x2212: return 0xFF0D;  // － (0x817C)
        default: return cp;
        }
    };
    auto inSet = [&](int k, uint32_t cp) { return sets[k].find(norm(cp)) != std::u32string::npos; };
    auto attaches = [&](uint32_t cp) { return !isWide(cp) || punct.find(norm(cp)) != std::u32string::npos; };
    // The glyphs laid out together from item i on: a `[...]` group, a run of set 2, or one character
    // (0x694ec0: a glyph's group is set only between brackets, 0x696010 / 0x692a60). Proportional
    // text also keeps unbracketed words whole (Fitted: Nekopara's and Yukikoi's English, all
    // proportional, break only between words on PC, by a mechanism not found; fixed-pitch text
    // breaks inside them, as Amakano 1's translation does on PC).
    auto unitEnd = [&](size_t i) {
        const uint32_t cp = items[i].cp;
        if (items[i].group >= 0) {
            size_t j = i;
            while (j < items.size() && items[j].group == items[i].group) ++j;
            return j;
        }
        if (cp == ' ' || cp == '\n') return i + 1;
        size_t j = i;
        bool narrow = false;
        while (proportional_ && j < items.size() && items[j].group < 0 && items[j].cp != ' ' && items[j].cp != '\n' && attaches(items[j].cp))
            narrow |= !isWide(items[j++].cp);
        if (narrow) return j;
        if (inSet(2, cp)) {
            j = i;
            while (j < items.size() && inSet(2, items[j].cp)) ++j;
            return j;
        }
        return i + 1;
    };
    size_t unit = 0;  // the end of the unit being placed
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (it.cp == '\n') {
            out.push_back({0, penX, penY, it.size, it.color, -1, penX, 0, it.align});
            penX = lineLeft(i + 1);
            penY += lineH;
            lineStart = out.size();
            unit = i + 1;
            continue;
        }
        if (i >= unit) {
            // A unit starts: does it go on this line?
            unit = unitEnd(i);
            float w = 0;
            for (size_t j = i; j < unit; ++j) w += advanceOf(items[j]);
            size_t breakAt = SIZE_MAX;  // the item the next line starts with
            if (it.cp != ' ' && out.size() > lineStart && penX + w > right) {
                const bool group = unit - i > 1 || !isWide(it.cp);
                if (group) {
                    if (!(penX + advanceOf(it) <= right && penX + w < fullRight)) breakAt = i;
                } else if (inSet(0, it.cp)) {
                    if (it.cp != 0x3000 && penX + w >= fullRight) {
                        // It does not fit: up to three characters before it come down too, so the
                        // line below starts with one that may.
                        breakAt = i;
                        for (size_t e = i, n = 0; e > lineStart + 1 && n <= 3; ++n) {
                            --e;
                            const uint32_t c = items[e].cp;
                            if (c != ' ' && !inSet(0, c)) {
                                breakAt = e;
                                break;
                            }
                        }
                    }
                } else {
                    breakAt = i;
                }
                // An opener never ends a line.
                while (breakAt != SIZE_MAX && breakAt > lineStart + 1 && inSet(1, items[breakAt - 1].cp)) --breakAt;
                // A break pulled back into a group goes before the group (0x694ec0 moves it down).
                while (breakAt != SIZE_MAX && breakAt > lineStart + 1 && items[breakAt].group >= 0 &&
                       items[breakAt - 1].group == items[breakAt].group)
                    --breakAt;
            }
            if (breakAt != SIZE_MAX) {
                out.resize(breakAt);
                penX = lineLeft(breakAt);
                penY += lineH;
                lineStart = out.size();
                i = breakAt;
                unit = unitEnd(i);
            }
        }
        const Item& g = items[i];
        if (g.cp == ' ' && out.size() == lineStart && !out.empty()) {
            out.push_back({0, penX, penY, g.size, g.color, -1, penX, 0, g.align});  // swallow leading space after a wrap
            continue;
        }
        float cellW = advanceOf(g);
        // A full-width space opening a line other than the first takes no room (cs2.exe 0x695640:
        // before any glyph of the line, a 0x8140's width is set to 0; the first line keeps it):
        // Yukikoi's novel pages centre such a second line without it.
        if (g.cp == 0x3000 && penY > startY &&
            std::all_of(out.begin() + static_cast<long>(lineStart), out.end(), [](const LaidGlyph& lg) { return lg.cp == 0; })) {
            out.push_back({0, penX, penY, g.size, g.color, -1, penX, 0, g.align});
            continue;
        }
        float glyphW = isUserGlyph(g.cp) ? cellW : cur_->glyph(g.cp, g.px).advance;
        out.push_back({g.cp, penX + (cellW - glyphW) / 2.0f, penY, g.size, g.color, -1, penX, cellW, g.align, g.italic});
        penX += cellW;
    }

    // `\pl` / `\pc` / `\pr` (cs2.exe 0x695640): the lines are broken first, then each line is placed
    // left to right in runs of one alignment. A centred run moves into the middle of the room left
    // on the line, a right-aligned run to its end; text after a run follows it. A run that starts
    // the line measures its room from the align box.
    const float boxLeft = alignBox ? viewX_ + alignBox->x * viewScale_ : left;
    const float boxRight = alignBox ? boxLeft + alignBox->w * viewScale_ : right;
    for (size_t a = 0; a < out.size();) {
        size_t b = a;
        while (b < out.size() && out[b].y == out[a].y) ++b;
        float pen = out[a].pen;
        bool lineBegin = true;
        for (size_t r = a; r < b;) {
            size_t e = r;
            float w = 0;
            while (e < b && out[e].align == out[r].align) w += out[e++].adv;
            float at = pen;
            if (out[r].align != 0) {
                const float from = lineBegin ? boxLeft + (pen - left) : pen;
                const float room = std::max(0.0f, boxRight - from - w);
                at = from + (out[r].align == 1 ? room / 2 : room);
            }
            const float shift = at - out[r].pen;
            for (size_t k = r; k < e; ++k) {
                out[k].x += shift;
                out[k].pen += shift;
            }
            pen = at + w;
            if (w > 0) lineBegin = false;
            r = e;
        }
        a = b;
    }
    return out;
}

std::vector<Rect> StageRenderer::choiceRects(const cs2::Stage& stage) const {
    std::vector<Rect> rects;
    float W = static_cast<float>(opt_.virtualWidth), H = static_cast<float>(opt_.virtualHeight);
    float w = W * 0.6f, h = H * 0.075f, gap = H * 0.025f;
    float total = stage.choices.size() * h + (stage.choices.empty() ? 0 : (stage.choices.size() - 1) * gap);
    float y = H * 0.42f - total / 2;
    for (size_t i = 0; i < stage.choices.size(); ++i) {
        rects.push_back({(W - w) / 2, y, w, h});
        y += h + gap;
    }
    return rects;
}

int StageRenderer::choiceAt(const cs2::Stage& stage, float vx, float vy) const {
    if (choicesHidden_) return -1;
    auto rects = choiceRects(stage);
    for (size_t i = 0; i < rects.size(); ++i) {
        if (rects[i].contains(vx, vy)) return static_cast<int>(i);
    }
    return -1;
}

void StageRenderer::toVirtual(float wx, float wy, float& vx, float& vy) const {
    vx = (wx - viewX_) / viewScale_;
    vy = (wy - viewY_) / viewScale_;
}

void StageRenderer::drawGlyphs(const std::vector<LaidGlyph>& glyphs, size_t count, float alpha, bool outline,
                               uint32_t defaultColor, uint32_t edgeColor) {
    if (!edgeColor) edgeColor = 0xFF000000;
    if (!defaultColor) defaultColor = 0xFFFFFFFF;
    struct Upright {
        float& s;
        ~Upright() { s = 0; }
    } upright{shear_};
    for (size_t i = 0; i < count && i < glyphs.size(); ++i) {
        const LaidGlyph& lg = glyphs[i];
        shear_ = 0;
        if (lg.cp == 0 || lg.cp == ' ') continue;
        int px = std::max(1, static_cast<int>(std::lround(lg.size * viewScale_)));
        if (const Texture* ug = userGlyph(lg.cp)) {
            drawUserGlyph(*ug, lg.x, lg.y, lg.size * viewScale_, alpha);
            continue;
        }
        // The edge first: looking up the glyph may clear a full atlas, so it is fetched again then.
        // Glyph and edge are both made at the game's pixel size and scaled with the screen, as the
        // engine draws its text images at that size (FreeType's cache at the layout size: LSG
        // 0x66ce70; GDI's GetGlyphOutline: 0x691a60) into the frame it then stretches to the window.
        const bool edged = outline && (textAround_ || textRightDown_);
        const int gamePx = std::max(1, static_cast<int>(std::lround(lg.size)));
        const uint32_t gen = cur_->generation();
        Glyph e = edged ? cur_->edge(lg.cp, gamePx, textAround_, textRightDown_) : Glyph{};
        const Glyph g = cur_->glyph(lg.cp, gamePx);
        if (edged && cur_->generation() != gen) e = cur_->edge(lg.cp, gamePx, textAround_, textRightDown_);
        if (g.x1 <= g.x0) continue;
        float baseY = lg.y + cur_->ascent(px);
        Rect r{lg.x + g.x0 * viewScale_, baseY + g.y0 * viewScale_, (g.x1 - g.x0) * viewScale_, (g.y1 - g.y0) * viewScale_};
        // `\ti;`: the engine asks GDI for the face in italic (Yukikoi 0x5e82b0, lfItalic); for a face
        // with no italic, as Yukikoi's, Windows slants the glyph itself: sheared by 1/4 about the
        // baseline, the advance kept (Wine's reproduction of it). Its edge goes with it.
        shear_ = lg.italic ? 0.25f : 0.0f;
        shearY_ = baseY;
        float d = std::max(1.0f, viewScale_);
        // Under a wipe reveal the opacity runs across the glyph with the wipe's ramp.
        const bool wiped = wipeOn_ && lg.wipe >= 0;
        auto emitUV = [&](const Rect& q, const Glyph& t, uint32_t color) {
            if (!wiped) {
                quad(Mode::Glyph, cur_->texture(), q, t.u0, t.v0, t.u1, t.v1, color, alpha);
                return;
            }
            float l = wipeAlpha(lg.wipe + (q.x - lg.x)), rr = wipeAlpha(lg.wipe + (q.x + q.w - lg.x));
            quadLR(Mode::Glyph, cur_->texture(), q, t.u0, t.v0, t.u1, t.v1, color, alpha * l, alpha * rr);
        };
        if (wiped && wipeAlpha(lg.wipe) <= 0.0f) continue;
        if (edged) {
            // The engine's edge in the layout's edge colour (FontAtlas::edge), worked out at the
            // game's pixel size and scaled with the screen.
            if (e.x1 > e.x0) {
                emitUV({lg.x + e.x0 * viewScale_, baseY + e.y0 * viewScale_, (e.x1 - e.x0) * viewScale_,
                        (e.y1 - e.y0) * viewScale_},
                       e, edgeColor);
            }
        } else if (!outline) {
            emitUV({r.x + d, r.y + d, r.w, r.h}, g, 0xB0000000);
        }
        emitUV(r, g, lg.color ? lg.color : defaultColor);
    }
}

void StageRenderer::setFrameScissor() {
    float w = opt_.virtualWidth * viewScale_, h = opt_.virtualHeight * viewScale_;
    glScissor(static_cast<GLint>(viewX_), static_cast<GLint>(std::floor(targetH_ - viewY_ - h)),
              static_cast<GLsizei>(std::ceil(w)), static_cast<GLsizei>(std::ceil(h)));
}

void StageRenderer::drawUi(const cs2::Stage& stage, uint64_t frame) {
    float W = static_cast<float>(opt_.virtualWidth), H = static_cast<float>(opt_.virtualHeight);
    auto toScreen = [&](Rect r) {
        return Rect{viewX_ + r.x * viewScale_, viewY_ + r.y * viewScale_, r.w * viewScale_, r.h * viewScale_};
    };
    const auto& w = stage.window;
    novelEnd_.reset();
    // Fade-outs (`frameoff fade N`) keep drawing until the alpha tween reaches zero.
    float winAlpha = std::clamp(w.alpha.value(frame), 0, 255) / 255.0f;
    // Novel mode uses its own style's window (`view 0 novel`: Fruit's novelwnd.fes, an NVSTRING page).
    const cs2::WindowLayout* L = opt_.theme ? opt_.theme->find(w.style) : nullptr;
    if (L && w.novel != L->novel) L = nullptr;  // a novel page in an ADV style (or the reverse): generic

    if (windowHidden_) winAlpha = 0;
    if (winAlpha > 0) {
        Rect textArea;
        float sizes[5];
        float lineHeight;
        float textSlack = 0;  // the layout's right margin (see wrapSlack_)
        if (L) {
            // The game's own window: its background layers (window-image frames) at the window origin.
            // The first layer is the tinted panel and takes the window opacity; the rest are decoration.
            if (windowQuads_) drawWindowQuads(false);  // the window script draws its own art
            for (size_t b = 0; !windowQuads_ && b < L->backgroundFrames.size(); ++b) {
                const Texture* bg = texture(L->image + "#" + std::to_string(L->backgroundFrames[b]));
                if (!bg) continue;
                float layerAlpha = b == 0 ? ((opt_.windowColor >> 24) & 0xFF) / 255.0f : 1.0f;
                quad(Mode::Texture, bg->id,
                     toScreen({static_cast<float>(L->originX + bg->offsetX), static_cast<float>(L->originY + bg->offsetY),
                               static_cast<float>(bg->width), static_cast<float>(bg->height)}),
                     0, 0, 1, 1, 0xFFFFFFFF, winAlpha * layerAlpha);
            }
            const bool live = liveLayout_.sizes[0] >= 0;
            for (int i = 0; i < 5; ++i) sizes[i] = static_cast<float>(live ? liveLayout_.sizes[i] : L->fontSizes[i]);
            const int32_t ruby = liveLayout_.ruby >= 0 ? liveLayout_.ruby : L->rubySize;
            const int32_t moveY = liveLayout_.moveY >= 0 ? liveLayout_.moveY : L->moveY;
            // Each line reserves ruby space above the glyphs: pitch = size + rsize.
            // Lines are the font size plus the layout's `move` gap apart (as measured against the
            // original's window and backlog); older layouts without `move` fall back to the ruby space.
            lineHeight = sizes[2] + static_cast<float>(moveY >= 0 ? moveY : ruby > 0 ? ruby : L->lineSpacing);
            // The first line starts the layout's top margin into the box (the older windows' margin and
            // ruby space were within a pixel or two; Yukikoi's English layout, margin 0 with ruby 10,
            // tells them apart).
            const int32_t top = liveLayout_.margin[1] >= 0 ? liveLayout_.margin[1] : ruby;
            // Lines run from the layout's left margin to its right margin short of the box's width
            // (grisaia.exe 0x54a670 / Hatsumira 0x63cf20: start x = margin left, a glyph that would end
            // past width - margin right starts a new line). Hatsumira's earlier full-width wrapping
            // made up for a wider stand-in font; its own face is not found.
            const int32_t left = liveLayout_.margin[0] >= 0 ? liveLayout_.margin[0] : 0;
            const int32_t right = liveLayout_.margin[2] >= 0 ? liveLayout_.margin[2] : L->marginRight;
            textArea = {static_cast<float>(L->text.x + left), static_cast<float>(L->text.y + top),
                        static_cast<float>(std::max(1, L->text.w - left - right)), static_cast<float>(L->text.h)};
            textSlack = static_cast<float>(std::max(0, right));
            if (L->novel) {
                // NVSTRING: the name column, then the text from the layout's left margin past it (the
                // margin once: the script's as it runs, else the static reading's); the first line its
                // top margin into the box.
                const int32_t marginLeft = liveLayout_.margin[0] >= 0 ? liveLayout_.margin[0] : L->marginLeft;
                const int32_t marginTop = liveLayout_.margin[1] >= 0 ? liveLayout_.margin[1] : L->marginTop;
                textArea.y = static_cast<float>(L->text.y + marginTop);
                textArea.x = static_cast<float>(L->text.x + L->nameColumn + marginLeft);
                textArea.w = static_cast<float>(std::max(1, L->text.w - L->nameColumn - marginLeft - right));
            }
            const std::string& shownName = scriptName_ ? *scriptName_ : w.name;
            if (!L->novel && !w.name.empty() && !shownName.empty() && w.textVisible && L->nameText) {
                cs2::MessageText name;
                name.spans.push_back({shownName});
                // A `\pc` / `\pr` name is centred / right-aligned in its box (the layout's span
                // alignment: 1 centre, 2 right).
                const int align = scriptName_ ? scriptNameAlign_ : 0;
                name.spans.back().align = static_cast<uint8_t>(align == 2 ? 1 : align == 1 ? 2 : 0);
                const float boxW = align != 0 && L->name.w > 0 ? static_cast<float>(L->name.w) : 10000.0f;
                float nsz[5];
                for (float& v : nsz) v = static_cast<float>(L->nameSize);
                cur_ = atlasFor(nameFont_);
                proportional_ = nameProportional_;
                auto glyphs = layoutMessage(name, {static_cast<float>(L->name.x), static_cast<float>(L->name.y), boxW, 100},
                                            nsz, static_cast<float>(L->nameSize));
                drawGlyphs(glyphs, glyphs.size(), winAlpha, true, nameColor_, nameEdge_);
                cur_ = atlasFor({});
                proportional_ = false;
            }
            // Face sprites: anchored at the window's face base point, clipped to the face plane.
            if (!stage.layer(cs2::Layer::Fw).empty()) {
                flush();
                // Face plane clip, intersected with the game frame (which is already the scissor).
                Rect clip = toScreen({static_cast<float>(L->faceClip.x), static_cast<float>(L->faceClip.y),
                                      static_cast<float>(L->faceClip.w), static_cast<float>(L->faceClip.h)});
                Rect frameRect{viewX_, viewY_, W * viewScale_, H * viewScale_};
                float x0 = std::max(clip.x, frameRect.x), y0 = std::max(clip.y, frameRect.y);
                float x1 = std::min(clip.x + clip.w, frameRect.x + frameRect.w);
                float y1 = std::min(clip.y + clip.h, frameRect.y + frameRect.h);
                glScissor(static_cast<GLint>(std::floor(x0)), static_cast<GLint>(std::floor(targetH_ - y1)),
                          static_cast<GLsizei>(std::max(0.0f, std::ceil(x1 - x0))),
                          static_cast<GLsizei>(std::max(0.0f, std::ceil(y1 - y0))));
                // The mask's alpha shapes the window (Cyanotype's soft-edged face panel).
                if (L->faceMaskId >= 0) {
                    const Texture* m = texture(L->faceMask + "#" + std::to_string(L->faceMaskId));
                    if (m) screenMask_ = m->id, screenMaskRect_ = clip;
                }
                PlaneXform faceXf;
                faceXf.posX = static_cast<float>(L->faceX);
                faceXf.posY = static_cast<float>(L->faceY);
                for (const auto& [index, img] : stage.layer(cs2::Layer::Fw)) {
                    // Re-based on an attribute point of its frame: draw it as if that were its base.
                    float sx = 0, sy = 0;
                    if (L->faceAttr >= 0 && !img.parts.empty()) {
                        if (const Texture* t = texture(img.parts.front())) {
                            if (auto a = t->attributes.find(static_cast<uint32_t>(L->faceAttr)); a != t->attributes.end()) {
                                sx = static_cast<float>(t->baseX - a->second.first);
                                sy = static_cast<float>(t->baseY - a->second.second);
                            }
                        }
                    }
                    drawImage(img, frame, faceXf, viewScale_, viewX_, viewY_, sx, sy);
                }
                flush();
                screenMask_ = 0;
                setFrameScissor();
            }
        } else {
            float textSize = H * 0.042f;
            for (int i = 0; i < 5; ++i) sizes[i] = textSize * (i == 0 ? 0.65f : i == 1 ? 0.8f : i == 3 ? 1.25f : i == 4 ? 1.5f : 1.0f);
            lineHeight = textSize * 1.5f;
            if (w.novel) {
                quad(Mode::Solid, 0, toScreen({0, 0, W, H}), 0, 0, 0, 0, 0xA0000000, winAlpha);
                textArea = {W * 0.08f, H * 0.08f, W * 0.84f, H * 0.84f};
            } else {
                Rect win{W * 0.03f, H * 0.725f, W * 0.94f, H * 0.255f};
                quad(Mode::Solid, 0, toScreen(win), 0, 0, 0, 0, opt_.windowColor, winAlpha);
                textArea = {win.x + W * 0.035f, win.y + H * 0.035f, win.w - W * 0.07f, win.h - H * 0.05f};
                if (!w.name.empty() && w.textVisible) {
                    drawText(w.name, win.x + W * 0.035f, win.y - H * 0.05f, textSize, 0xFFFFE6A0, winAlpha, SIZE_MAX);
                }
            }
            for (const auto& [index, img] : stage.layer(cs2::Layer::Fw)) {
                drawImage(img, frame, PlaneXform{}, viewScale_, viewX_, viewY_);
            }
        }

        if (w.textVisible) {
            if (L) {
                cur_ = atlasFor(textFont_);
                proportional_ = textProportional_;
                // `layout move <x> <y>`: x pixels after each character (fitted: Ikinari Anata ni
                // Koishiteiru's `move 2 7`, the only non-zero x, puts its half-width letters 12 pixels
                // apart at size 19 on PC screenshots; every other game says 0).
                letterSpacing_ = static_cast<float>(std::max(0, liveLayout_.moveX >= 0 ? liveLayout_.moveX : L->moveX));
                hanging_ = (liveLayout_.indent >= 0 ? liveLayout_.indent : L->indent) != 0;
                wrapSlack_ = textSlack;
                kinsoku_ = liveLayout_.kinsokuSet ? liveLayout_.kinsoku : nullptr;
            }
            if (L && L->novel) {
                drawNovelPage(w, *L, textArea, sizes, lineHeight, winAlpha);
            } else {
                auto glyphs = layoutMessage(w.text, textArea, sizes, lineHeight);
                const bool wipe = L && liveLayout_.scrollType == 1 && liveLayout_.wipeWidth > 0;
                if (wipe && w.revealed < w.text.length()) {
                    // `layout scrolltype 1` (grisaia.exe 0x54c2b0): the new text appears behind a wipe
                    // running along it line after line, size x frames x 2 / frame pixels in.
                    const int32_t speed = liveLayout_.frame >= 0 ? liveLayout_.frame : messageSpeed_;
                    const uint64_t elapsed = frame > w.wipeStart ? frame - w.wipeStart : 0;
                    const float pos = speed <= 0 ? 1e9f : static_cast<float>(static_cast<uint64_t>(sizes[2]) * elapsed * 2 / static_cast<uint64_t>(speed));
                    const float total = placeWipe(glyphs, w.wipeFrom);
                    wipeWidth_ = static_cast<float>(liveLayout_.wipeWidth) * viewScale_;
                    wipeHead_ = pos * viewScale_ - wipeWidth_;
                    wipeOn_ = true;
                    wipeDone_ = wipeHead_ >= total;
                    wipeDoneStart_ = w.wipeStart;
                    drawGlyphs(glyphs, glyphs.size(), winAlpha, true, textColor_, textEdge_);
                    wipeOn_ = false;
                } else {
                    drawGlyphs(glyphs, std::min(glyphs.size(), w.revealed), winAlpha, L != nullptr, L ? textColor_ : 0, textEdge_);
                }
                if (L && !w.subText.empty() && subLayout_.sizes[0] >= 0) {
                    // The sub language under the text (meswnd.fes MES_SHOW: `str endpos -1` then
                    // `strsub pos <x> <y + end + 32>`; MES_DRAW draws both at the same speed): its own
                    // layout and font, from the top of the main text's last line.
                    float subSizes[5];
                    for (int i = 0; i < 5; ++i) subSizes[i] = static_cast<float>(subLayout_.sizes[i]);
                    const float endY = glyphs.empty() ? 0.0f : glyphs.back().y - glyphs.back().size - static_cast<float>(L->text.y);
                    const int32_t sl = subLayout_.margin[0] >= 0 ? subLayout_.margin[0] : 0;
                    const int32_t st = subLayout_.margin[1] >= 0 ? subLayout_.margin[1] : 0;
                    const int32_t sr = subLayout_.margin[2] >= 0 ? subLayout_.margin[2] : 0;
                    const Rect subArea{static_cast<float>(L->text.x + sl), static_cast<float>(L->text.y) + endY + 32.0f + static_cast<float>(st),
                                       static_cast<float>(std::max(1, L->text.w - sl - sr)), static_cast<float>(L->text.h)};
                    const float subLine = subSizes[2] + static_cast<float>(subLayout_.moveY >= 0 ? subLayout_.moveY : 0);
                    cur_ = atlasFor(subFont_);
                    proportional_ = subProportional_;
                    hanging_ = subLayout_.indent > 0;
                    wrapSlack_ = static_cast<float>(sr);
                    kinsoku_ = subLayout_.kinsokuSet ? subLayout_.kinsoku : nullptr;
                    auto subGlyphs = layoutMessage(w.subText, subArea, subSizes, subLine);
                    drawGlyphs(subGlyphs, std::min(subGlyphs.size(), w.revealed), winAlpha, true, textColor_, textEdge_);
                }
            }
            cur_ = atlasFor({});
            proportional_ = false;
            hanging_ = false;
            wrapSlack_ = 0;
            kinsoku_ = nullptr;
            letterSpacing_ = 0;
        }
        if (L && windowQuads_) drawWindowQuads(true);  // what the script puts over its text
    }
    // The scripts beside the window (touch.fes's paw) show and hide themselves.
    drawQuads(sideQuads_);

    float textSize = H * 0.042f;
    auto rects = choiceRects(stage);
    if (choicesHidden_) rects.clear();
    for (size_t i = 0; i < rects.size(); ++i) {
        bool hot = static_cast<int>(i) == hovered_;
        quad(Mode::Solid, 0, toScreen(rects[i]), 0, 0, 0, 0, hot ? 0xE0306080 : 0xD0101820, 1.0f);
        quad(Mode::Solid, 0, toScreen({rects[i].x, rects[i].y + rects[i].h - H * 0.004f, rects[i].w, H * 0.004f}), 0,
             0, 0, 0, hot ? 0xFFFFE6A0 : 0x80FFFFFF, 1.0f);
        const std::string& p = stage.choices[i].prompt;
        float size = textSize;
        float tw = measure(p, size);
        drawText(p, rects[i].x + (rects[i].w - tw) / 2, rects[i].y + (rects[i].h - size * 1.2f) / 2, size,
                 0xFFFFFFFF, 1.0f, SIZE_MAX);
    }
}

void StageRenderer::drawNovelPage(const cs2::MessageWindow& w, const cs2::WindowLayout& L, const Rect& area,
                                  const float sizes[5], float lineHeight, float alpha) {
    // An NVSTRING page: each message starts on a new line under the last, its speaker right-aligned
    // in the name column (the box's first `DIV` pixels) on the message's first line. The page reveals
    // as one text, message after message.
    std::vector<cs2::MessageWindow::NovelLine> paras = w.novelLines;
    if (paras.empty()) paras.push_back({0, w.name});
    float endX = area.x, endY = area.y;
    size_t reveal = w.revealed;
    float y = area.y;
    for (size_t p = 0; p < paras.size(); ++p) {
        size_t from = std::min(paras[p].at, w.text.spans.size());
        size_t to = p + 1 < paras.size() ? std::min(paras[p + 1].at, w.text.spans.size()) : w.text.spans.size();
        cs2::MessageText part;
        for (size_t s = from; s < to; ++s) part.spans.push_back(w.text.spans[s]);
        // The separator before the next message is its own "\n" span; it counts as revealed text.
        size_t sep = 0;
        if (!part.spans.empty() && part.spans.back().text == "\n" && p + 1 < paras.size()) {
            part.spans.pop_back();
            sep = 1;
        }
        // `\pc` lines are centred in the text column, the room right of the name column up to the
        // right margin (the alignment takes the middle of the room left on the line, 0x695640;
        // Yukikoi's opening on PC: the line's middle at the column's, x 650 of 1280, not the box's).
        const Rect box{area.x, y, area.w, area.h};
        auto glyphs = layoutMessage(part, {area.x, y, area.w, area.h}, sizes, lineHeight, &box);
        size_t shown = std::min(glyphs.size(), reveal);
        if (shown > 0 && !paras[p].name.empty()) {
            cs2::MessageText name;
            name.spans.push_back({paras[p].name});
            // Right-aligned: the name ends at the column's edge.
            float endX = static_cast<float>(L.text.x + L.nameColumn);
            float width = measure(paras[p].name, sizes[2]);
            auto ng = layoutMessage(name, {endX - width, y, width + sizes[2], lineHeight}, sizes, lineHeight);
            drawGlyphs(ng, ng.size(), alpha, true, textColor_, textEdge_);
        }
        drawGlyphs(glyphs, shown, alpha, true, textColor_, textEdge_);
        reveal -= shown;
        reveal -= std::min(reveal, sep + (glyphs.size() < part.length() ? part.length() - glyphs.size() : 0));
        // The next message starts on the line after this one's last.
        float lines = 1;
        if (!glyphs.empty()) lines = std::round((glyphs.back().y - glyphs.front().y) / (lineHeight * viewScale_)) + 1;
        if (!glyphs.empty()) {
            const LaidGlyph& g = glyphs.back();
            std::string ch;
            uint32_t c = g.cp;
            if (c < 0x80) ch += static_cast<char>(c);
            else if (c < 0x800) ch += static_cast<char>(0xC0 | c >> 6), ch += static_cast<char>(0x80 | (c & 0x3F));
            else ch += static_cast<char>(0xE0 | c >> 12), ch += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), ch += static_cast<char>(0x80 | (c & 0x3F));
            endX = (g.x - viewX_) / viewScale_ + measure(ch, g.size);
            endY = y + (lines - 1) * lineHeight;
        }
        y += lines * lineHeight;
    }
    novelEnd_ = std::pair<int32_t, int32_t>{static_cast<int32_t>(std::lround(endX - static_cast<float>(L.text.x))),
                                            static_cast<int32_t>(std::lround(endY - static_cast<float>(L.text.y)))};
}

StageRenderer::BoxLayout StageRenderer::layoutTextBox(const cs2::FesQuad& q) {
    // Text laid out on the engine's cell grid (full-width = size, half-width = size / 2), wrapped
    // at the line width, cut after `maxLines`, aligned per line; glyphs start at the line's top.
    BoxLayout L;
    const float size = q.textSize > 0 ? q.textSize : 12;
    L.boxW = q.x[2] - q.x[0];
    const float boxH = q.y[2] - q.y[0];
    const float lineW = q.lineChars > 0 ? std::min(L.boxW, q.lineChars * size) : L.boxW;
    const int maxLines = std::max(1, q.maxLines);
    L.lineH = q.lineHeight > 0 ? q.lineHeight : std::max(size, boxH / maxLines);
    // `\f<n>` switches the size for what follows (Fruit's choice labels: `\f26Run\f10　\f26away`).
    auto& cps = L.cps;
    auto& sizes = L.sizes;
    float curSize = size;
    for (size_t i = 0; i < q.text.size();) {
        if (q.text[i] == '\\' && i + 2 < q.text.size() && q.text[i + 1] == 'f' &&
            std::isdigit(static_cast<unsigned char>(q.text[i + 2]))) {
            size_t j = i + 2;
            int n = 0;
            while (j < q.text.size() && std::isdigit(static_cast<unsigned char>(q.text[j]))) n = n * 10 + (q.text[j++] - '0');
            curSize = static_cast<float>(std::max(1, n));
            i = j;
            continue;
        }
        cps.push_back(nextCodepoint(q.text, i));
        sizes.push_back(curSize);
    }
    auto pxOf = [&](size_t i) { return std::max(1, static_cast<int>(std::lround(sizes[i] * viewScale_))); };
    L.cells.assign(cps.size(), 0.0f);
    for (size_t i = 0; i < cps.size(); ++i) {
        const uint32_t cp = cps[i];
        if (cp == '\n') continue;
        if (q.proportional && !isUserGlyph(cp)) L.cells[i] = std::round(cur_->glyph(cp, pxOf(i)).advance) / viewScale_;
        else if (isWide(cp) || isUserGlyph(cp)) L.cells[i] = sizes[i];
        else L.cells[i] = std::max(sizes[i] / 2, std::floor(cur_->glyph(cp, pxOf(i)).advance) / viewScale_);
    }

    // Break into lines: [begin, end) code point ranges and their widths.
    BoxLayout::Line cur{0, 0, 0};
    auto& lines = L.lines;
    for (size_t i = 0; i < cps.size() && static_cast<int>(lines.size()) < maxLines; ++i) {
        uint32_t cp = cps[i];
        if (cp == '\n') {
            cur.end = i;
            lines.push_back(cur);
            cur = {i + 1, i + 1, 0};
            continue;
        }
        bool wordStart = i == 0 || cps[i - 1] == ' ' || isWide(cp) || isWide(cps[i - 1]);
        if (wordStart && cp != ' ') {
            float w = 0;
            for (size_t j = i; j < cps.size() && cps[j] != ' ' && cps[j] != '\n'; ++j) {
                w += L.cells[j];
                if (isWide(cps[j])) break;
            }
            if (cur.width + w > lineW && cur.width > 0) {
                cur.end = i;
                lines.push_back(cur);
                cur = {i, i, 0};
            }
        }
        if (cp == ' ' && cur.width == 0) {
            cur.begin = i + 1;
            continue;
        }
        cur.width += L.cells[i];
    }
    if (static_cast<int>(lines.size()) < maxLines && cur.begin < cps.size()) {
        cur.end = cps.size();
        lines.push_back(cur);
    }
    return L;
}

std::optional<int32_t> StageRenderer::textLineCount(const cs2::FesQuad& q) {
    // cs2.exe's count (0x5a1860): 1 plus each break, the wrap pass's included (0x5a2700 inserts
    // one where it wraps), less a break that ends the text; all of the text, not the lines shown.
    if (viewScale_ <= 0) return std::nullopt;  // nothing drawn yet
    cs2::FesQuad all = q;
    all.maxLines = 1 << 20;
    cur_ = atlasFor(all.font, all.weight);
    const BoxLayout L = layoutTextBox(all);
    cur_ = atlasFor({});
    return std::max<int32_t>(1, static_cast<int32_t>(L.lines.size()));
}

std::optional<std::pair<float, float>> StageRenderer::textDrawArea(const cs2::FesQuad& q) {
    // The laid-out text's extent within the box: the widest line, and the last line's top plus the
    // size (cs2.exe 0x59db80: the largest x + width and y + height of the glyphs).
    if (viewScale_ <= 0) return std::nullopt;  // nothing drawn yet
    cs2::FesQuad all = q;
    all.maxLines = 1 << 20;
    cur_ = atlasFor(all.font, all.weight);
    const BoxLayout L = layoutTextBox(all);
    cur_ = atlasFor({});
    if (L.lines.empty()) return std::make_pair(0.0f, 0.0f);
    float w = 0;
    for (const auto& ln : L.lines) w = std::max(w, ln.width);
    return std::make_pair(w, (L.lines.size() - 1) * L.lineH + q.textSize);
}

std::optional<float> StageRenderer::textWipeLength(const cs2::FesQuad& q) {
    if (viewScale_ <= 0) return std::nullopt;  // nothing drawn yet
    cur_ = atlasFor(q.font, q.weight);
    const BoxLayout L = layoutTextBox(q);
    cur_ = atlasFor({});
    float total = 0;
    for (const auto& ln : L.lines) total += ln.width;
    return total;
}

void StageRenderer::drawTextBox(const cs2::FesQuad& q) {
    cur_ = atlasFor(q.font, q.weight);
    const BoxLayout L = layoutTextBox(q);
    const auto& cps = L.cps;
    const auto& sizes = L.sizes;
    const auto& lines = L.lines;
    const float size = q.textSize > 0 ? q.textSize : 12;
    const int px = std::max(1, static_cast<int>(std::lround(size * viewScale_)));
    const float boxW = L.boxW, lineH = L.lineH;
    auto pxOf = [&](size_t i) { return std::max(1, static_cast<int>(std::lround(sizes[i] * viewScale_))); };

    // A reveal after the script's `draw` (FesQuad::revealElapsed). `layout scrolltype 1`: the wipe
    // of grisaia.exe 0x54c2b0, as the message window's: size x frames x 2 / speed pixels along the
    // lines (each glyph at its x from the line's start plus the lengths of the lines before), a
    // pixel k past the trailing edge (wipewidth behind) keeping (255 - k x 255 / wipewidth + 1) /
    // 256 of its opacity (0x54cc70). Otherwise glyph by glyph, frames-per-glyph apart.
    const bool revealing = q.revealElapsed >= 0 && q.revealSpeed > 0;
    const bool wipe = revealing && q.scrollType == 1 && q.wipeWidth > 0;
    const float ww = static_cast<float>(q.wipeWidth);
    float head = 0;
    if (wipe) {
        const auto s = static_cast<uint64_t>(std::max(1.0f, size));
        head = static_cast<float>(s * static_cast<uint64_t>(q.revealElapsed) * 2 / static_cast<uint64_t>(q.revealSpeed)) - ww;
    }
    auto ramp = [&](float off) {
        const float k = off - head;
        if (k < 0) return 1.0f;
        if (k >= ww) return 0.0f;
        return (255.0f - std::floor(k * 255.0f / ww) + 1.0f) / 256.0f;
    };
    size_t shown = SIZE_MAX;
    if (revealing && !wipe && q.revealFramesPerGlyph > 0)
        shown = static_cast<size_t>(static_cast<double>(q.revealElapsed) / q.revealFramesPerGlyph);
    std::vector<size_t> order(cps.size());  // glyphs before each one, line breaks left out
    for (size_t i = 0, n = 0; i < cps.size(); ++i) {
        order[i] = n;
        if (cps[i] != '\n') ++n;
    }

    float done = 0;  // the lines before, along the wipe
    for (size_t l = 0; l < lines.size(); ++l) {
        const BoxLayout::Line& ln = lines[l];
        float width = 0;
        int linePx = px;  // mixed sizes share the baseline of the largest
        for (size_t i = ln.begin; i < ln.end; ++i) {
            width += L.cells[i];
            linePx = std::max(linePx, pxOf(i));
        }
        float penX = q.align == 1 ? boxW - width : q.align == 2 ? (boxW - width) / 2 : 0;
        const float lineStart = penX;
        for (size_t i = ln.begin; i < ln.end; ++i) {
            uint32_t cp = cps[i];
            float cw = L.cells[i];
            const float off = done + (penX - lineStart);
            const float cellX = viewX_ + (q.x[0] + penX) * viewScale_;
            if ((wipe && ramp(off) <= 0.0f) || (!wipe && order[i] >= shown)) {
                penX += cw;
                continue;
            }
            if (const Texture* ug = userGlyph(cp)) {
                drawUserGlyph(*ug, cellX, viewY_ + (q.y[0] + l * lineH) * viewScale_, sizes[i] * viewScale_,
                              q.alpha * (wipe ? ramp(off + cw / 2) : 1.0f));
                penX += cw;
                continue;
            }
            // The edge switches: the string's own (`layout shadow`), else the config's.
            const bool around = q.edgeAround >= 0 ? q.edgeAround != 0 : textAround_;
            const bool rightDown = q.edgeRightDown >= 0 ? q.edgeRightDown != 0 : textRightDown_;
            const bool edged = q.outlineColor && (around || rightDown);
            // Glyph and edge at the game's pixel size, scaled with the screen (see drawGlyphs).
            const int gamePx = std::max(1, static_cast<int>(std::lround(sizes[i])));
            const uint32_t gen = cur_->generation();
            Glyph e = edged ? cur_->edge(cp, gamePx, around, rightDown) : Glyph{};
            const Glyph g = cur_->glyph(cp, gamePx);
            if (edged && cur_->generation() != gen) e = cur_->edge(cp, gamePx, around, rightDown);
            if (g.x1 > g.x0) {
                float gx = cellX + (cw - g.advance) * viewScale_ / 2;
                float gy = viewY_ + (q.y[0] + l * lineH) * viewScale_ + cur_->ascent(linePx);
                Rect r{gx + g.x0 * viewScale_, gy + g.y0 * viewScale_, (g.x1 - g.x0) * viewScale_, (g.y1 - g.y0) * viewScale_};
                auto emit = [&](const Rect& rr, const Glyph& t, uint32_t color) {
                    if (!wipe) {
                        quad(Mode::Glyph, cur_->texture(), rr, t.u0, t.v0, t.u1, t.v1, color, q.alpha);
                        return;
                    }
                    const float a0 = ramp(off + (rr.x - cellX) / viewScale_), a1 = ramp(off + (rr.x + rr.w - cellX) / viewScale_);
                    quadLR(Mode::Glyph, cur_->texture(), rr, t.u0, t.v0, t.u1, t.v1, color, q.alpha * a0, q.alpha * a1);
                };
                // The engine's edge (FontAtlas::edge) in the layout's edge colour, at the game's size.
                if (edged && e.x1 > e.x0) {
                    emit({gx + e.x0 * viewScale_, gy + e.y0 * viewScale_, (e.x1 - e.x0) * viewScale_, (e.y1 - e.y0) * viewScale_},
                         e, q.outlineColor);
                }
                emit(r, g, q.color);
            }
            penX += cw;
        }
        done += width;
    }
    cur_ = atlasFor({});
    if (!q.subText.empty()) {
        // The sub-language text (a log entry's): its own string under the main one (cs2.exe
        // 0x75fe90 / 0x7603a0), at the entry's y plus `shift_sub` plus, when there is main text,
        // where that text ends: the string plane's answer to message 0x1e0 (0x69cf50 -> 0x699630,
        // kept by 0x7536a0 and read by 0x7536e0) is the last character's x + width and its line's
        // top (0x69b5f0 +0x2bc / +0x2c0), no size added. Same size and pitch, its own font. (The
        // LOGSTRING keeps one such answer, from its plane's last sync; here each entry's own.)
        const float mainH = lines.empty() ? 0.0f : static_cast<float>(lines.size() - 1) * lineH;
        cs2::FesQuad s = q;
        s.text = q.subText;
        s.font = q.subFont.empty() ? q.font : q.subFont;
        s.subText.clear();
        s.maxLines = 8;
        for (int k = 0; k < 4; ++k) {
            s.x[k] += q.subX;
            s.y[k] += mainH + q.subY;
        }
        s.y[2] = s.y[3] = s.y[0] + s.maxLines * lineH;
        drawTextBox(s);
    }
}

Bytes StageRenderer::captureThumbnail(int w, int h) {
    if (w <= 0 || h <= 0 || !display_.tex) return {};
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    setTexParams(GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    begin(w, h);
    // Same orientation as on screen; rows are read back bottom-up and flipped below.
    quad(Mode::Texture, display_.tex, {0, 0, static_cast<float>(w), static_cast<float>(h)}, 0, 1, 1, 0, 0xFFFFFFFF,
         1.0f);
    // The screen over the stage (a scene's FE screen, e.g. Amakano 2's map) is part of the picture.
    if (!lastOverlay_.empty()) {
        const float vs = viewScale_, vx = viewX_, vy = viewY_;
        viewScale_ = static_cast<float>(w) / static_cast<float>(opt_.virtualWidth);
        viewX_ = viewY_ = 0;
        drawQuads(lastOverlay_);
        flush();
        viewScale_ = vs, viewX_ = vx, viewY_ = vy;
    }
    flush();
    Bytes px(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    Bytes out(px.size());
    size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; ++y) std::memcpy(&out[y * row], &px[(h - 1 - y) * row], row);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;  // opaque
    return out;
}

std::vector<uint32_t> StageRenderer::captureImage(const cs2::StageImage& img, uint64_t frame) {
    // Drawn into the scene target, which the next frame draws afresh.
    const int w = opt_.virtualWidth, h = opt_.virtualHeight;
    glBindFramebuffer(GL_FRAMEBUFFER, scene_.fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    begin(w, h);
    cs2::StageImage shown = img;
    shown.visible = true;
    drawImage(shown, frame, PlaneXform{}, 1.0f, 0, 0, 0, 0);
    flush();
    Bytes px(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::vector<uint32_t> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &px[static_cast<size_t>(h - 1 - y) * w * 4];  // bottom-up
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            out[static_cast<size_t>(y) * w + x] = (uint32_t{p[3]} << 24) | (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
        }
    }
    return out;
}

void StageRenderer::drawWindowQuads(bool overText) {
    std::vector<cs2::FesQuad> part;
    for (const auto& q : *windowQuads_) {
        if ((q.layer > windowTextLayer_) == overText) part.push_back(q);
    }
    drawQuads(part);
}

void StageRenderer::drawQuads(const std::vector<cs2::FesQuad>& quads) {
    for (const auto& q : quads) {
        // A quad clipped to its container's VIEWPORT: the scissor narrowed to that rectangle (within
        // the game frame) while it draws.
        bool scissorWas = false;
        if (q.clip) {
            flush();
            scissorWas = glIsEnabled(GL_SCISSOR_TEST);
            glEnable(GL_SCISSOR_TEST);
            const float fx0 = viewX_, fy0 = viewY_;
            const float fx1 = fx0 + opt_.virtualWidth * viewScale_, fy1 = fy0 + opt_.virtualHeight * viewScale_;
            const float x0 = std::max(fx0, viewX_ + q.clipX * viewScale_), y0 = std::max(fy0, viewY_ + q.clipY * viewScale_);
            const float x1 = std::min(fx1, viewX_ + (q.clipX + q.clipW) * viewScale_);
            const float y1 = std::min(fy1, viewY_ + (q.clipY + q.clipH) * viewScale_);
            glScissor(static_cast<GLint>(std::floor(x0)), static_cast<GLint>(std::floor(targetH_ - y1)),
                      static_cast<GLsizei>(std::max(0.0f, std::ceil(x1 - x0))),
                      static_cast<GLsizei>(std::max(0.0f, std::ceil(y1 - y0))));
        }
        auto unclip = [&] {
            if (!q.clip) return;
            flush();
            setFrameScissor();
            if (!scissorWas) glDisable(GL_SCISSOR_TEST);
        };
        if (!q.text.empty()) {
            drawTextBox(q);
            unclip();
            continue;
        }
        Vec2 c[4];
        for (int k = 0; k < 4; ++k) c[k] = {viewX_ + q.x[k] * viewScale_, viewY_ + q.y[k] * viewScale_};
        if (q.texture.empty()) {
            quadCorners(Mode::Solid, 0, c, 0, 0, 0, 0, q.color, q.alpha);
            unclip();
            continue;
        }
        if (q.texture.starts_with("@wipe:")) {  // drawScreenWipe
            unclip();
            continue;
        }
        const Texture* t = texture(q.texture);
        if (t) quadCorners(Mode::Texture, t->id, c, q.u0, q.v0, q.u1, q.v1, 0xFFFFFFFF, q.alpha);
        unclip();
    }
}

void StageRenderer::drawScreenWipe(const std::vector<cs2::FesQuad>& overlay, uint64_t frame, float vw, float vh) {
    const cs2::FesQuad* w = nullptr;
    for (const auto& q : overlay) {
        if (q.texture.starts_with("@wipe:")) w = &q;
    }
    if (!w) {
        wipeStart_ = UINT64_MAX;
        return;
    }
    // "@wipe:<start>:<frames>:<mask>"
    unsigned long long start = 0, frames = 1;
    int used = 0;
    if (std::sscanf(w->texture.c_str() + 6, "%llu:%llu:%n", &start, &frames, &used) < 2) return;
    const std::string mask = w->texture.substr(6 + static_cast<size_t>(used));
    if (start != wipeStart_) {
        // It starts from the frame shown just before; without one (the script's first frame) from
        // the stage beneath it.
        wipeStart_ = start;
        wipeFromSnapshot_ = haveLastFrame_ && lastFrameAt_ + 1 >= start;
    }
    const float t = std::clamp(static_cast<float>(frame - start) / static_cast<float>(std::max(1ULL, frames)), 0.0f, 1.0f);
    const Texture* m = mask.empty() ? nullptr : texture(mask);
    flush();
    glViewport(static_cast<GLint>(viewX_), static_cast<GLint>(viewY_), static_cast<GLsizei>(vw), static_cast<GLsizei>(vh));
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(wipeProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wipeFromSnapshot_ ? lastFrame_.tex : prev_.tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m ? m->id : lastFrame_.tex);
    glUniform1i(uWipePrev_, 0);
    glUniform1i(uWipeMask_, 2);
    glUniform1f(uWipeT_, t);
    glUniform1i(uWipeHasMask_, m ? 1 : 0);
    glBindVertexArray(emptyVao_);
    if (w->clip) {
        // A plane's `draw` (FesInstance's ScreenWipe box): the fade only inside the plane.
        const float x0 = std::max(viewX_, viewX_ + w->clipX * viewScale_), y0 = std::max(viewY_, viewY_ + w->clipY * viewScale_);
        const float x1 = std::min(viewX_ + vw, viewX_ + (w->clipX + w->clipW) * viewScale_);
        const float y1 = std::min(viewY_ + vh, viewY_ + (w->clipY + w->clipH) * viewScale_);
        glScissor(static_cast<GLint>(std::floor(x0)), static_cast<GLint>(std::floor(targetH_ - y1)),
                  static_cast<GLsizei>(std::max(0.0f, std::ceil(x1 - x0))), static_cast<GLsizei>(std::max(0.0f, std::ceil(y1 - y0))));
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (w->clip) setFrameScissor();
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    glViewport(0, 0, static_cast<GLsizei>(targetW_), static_cast<GLsizei>(targetH_));
    begin(static_cast<int>(targetW_), static_cast<int>(targetH_));
}

void StageRenderer::render(const cs2::Stage& stage, uint64_t frame, int width, int height, GLuint target,
                           const std::vector<cs2::FesQuad>& overlay, const std::vector<cs2::FesQuad>* window,
                           const std::vector<cs2::FesQuad>* top) {
    windowQuads_ = window;
    lastOverlay_ = overlay;
    drawScene(stage, frame);
    composite(stage, frame);

    // Letterbox the virtual screen into the window.
    viewScale_ = std::min(static_cast<float>(width) / opt_.virtualWidth, static_cast<float>(height) / opt_.virtualHeight);
    float vw = opt_.virtualWidth * viewScale_, vh = opt_.virtualHeight * viewScale_;
    viewX_ = std::floor((width - vw) / 2);
    viewY_ = std::floor((height - vh) / 2);

    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    begin(width, height);
    // Framebuffer textures are bottom-up, hence the flipped V.
    if (!storyHidden_) quad(Mode::Texture, display_.tex, {viewX_, viewY_, vw, vh}, 0, 1, 1, 0, 0xFFFFFFFF, 1.0f);
    flush();
    glDisable(GL_BLEND);
    begin(width, height);
    // Nothing may draw outside the game frame (letterbox margins stay black).
    glEnable(GL_SCISSOR_TEST);
    setFrameScissor();
    if (!storyHidden_) {
        drawQuads(overlay);
        drawUi(stage, frame);
        drawScreenWipe(overlay, frame, vw, vh);
    }
    if (top) drawQuads(*top);
    if (screenFade_ > 0) quad(Mode::Solid, 0, {viewX_, viewY_, vw, vh}, 0, 0, 1, 1, 0xFF000000, screenFade_);
    flush();
    glDisable(GL_SCISSOR_TEST);
    // While an FE script shows (and no wipe runs), keep the finished game area: a `rwipe` starts
    // from it.
    if (!overlay.empty() && wipeStart_ == UINT64_MAX) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lastFrame_.fbo);
        glBlitFramebuffer(static_cast<GLint>(viewX_), static_cast<GLint>(viewY_), static_cast<GLint>(viewX_ + vw),
                          static_cast<GLint>(viewY_ + vh), 0, 0, opt_.virtualWidth, opt_.virtualHeight, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        haveLastFrame_ = true;
        lastFrameAt_ = frame;
    }
}

void StageRenderer::renderMovie(const uint8_t* rgba, int w, int h, bool changed, int width, int height, GLuint target) {
    if (!movieTex_) glGenTextures(1, &movieTex_);
    glBindTexture(GL_TEXTURE_2D, movieTex_);
    if (w != movieW_ || h != movieH_) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        setTexParams(GL_LINEAR);
        movieW_ = w, movieH_ = h;
    } else if (changed) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    // The game's screen in the window (as render() letterboxes it), the movie fitted inside it.
    viewScale_ = std::min(static_cast<float>(width) / opt_.virtualWidth, static_cast<float>(height) / opt_.virtualHeight);
    float vw = opt_.virtualWidth * viewScale_, vh = opt_.virtualHeight * viewScale_;
    viewX_ = std::floor((width - vw) / 2);
    viewY_ = std::floor((height - vh) / 2);
    float s = std::min(vw / static_cast<float>(w), vh / static_cast<float>(h));
    float mw = w * s, mh = h * s;
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    begin(width, height);
    quad(Mode::Texture, movieTex_, {viewX_ + std::floor((vw - mw) / 2), viewY_ + std::floor((vh - mh) / 2), mw, mh}, 0, 0, 1, 1,
         0xFFFFFFFF, 1.0f);
    flush();
}

}  // namespace nyan::render
