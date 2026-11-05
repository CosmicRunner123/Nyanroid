#include "wa2/renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

#include "nyan/common/error.hpp"

namespace nyan::render::wa2r {

namespace {

using wa2::DrawCommand;
using wa2::DrawVertex;

// Positions are target pixels (y down); every offscreen buffer keeps row 0 at the top, so a target
// pixel row y is texture row y when the buffer is sampled later.
const char* kVs = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aDiffuse;
layout(location = 3) in vec3 aSpecular;
uniform vec2 uSize;
out vec2 vUv;
out vec4 vDiffuse;
out vec3 vSpecular;
void main() {
    gl_Position = vec4(aPos.x / uSize.x * 2.0 - 1.0, aPos.y / uSize.y * 2.0 - 1.0, 0.0, 1.0);
    gl_PointSize = 1.0;
    vUv = aUv;
    vDiffuse = aDiffuse;
    vSpecular = aSpecular;
}
)";

// Texture stage 0 (0x43c303): rgb = tex * diffuse + specular, alpha = tex.a * diffuse.a; a render
// target's texture takes the diffuse alpha twice (ALPHAARG2 left at the diffuse).
const char* kTexFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
in vec4 vDiffuse;
in vec3 vSpecular;
uniform sampler2D uTex;
uniform bool uRtAlpha;
out vec4 oColor;
void main() {
    vec4 t = texture(uTex, vUv);
    vec3 rgb = min(t.rgb * vDiffuse.rgb + vSpecular, vec3(1.0));
    float a = uRtAlpha ? vDiffuse.a * vDiffuse.a : t.a * vDiffuse.a;
    oColor = vec4(rgb, a);
}
)";

// Untextured primitives (0x43dd9c): the vertex colour.
const char* kColorFs = R"(#version 300 es
precision highp float;
in vec4 vDiffuse;
out vec4 oColor;
void main() {
    oColor = vDiffuse;
}
)";

// Two textures (0x43c57a). With a shader (grp\pf00..pf03.psh, ps.1.1): colour of the picture, alpha
// saturate((mask.a + c0.a) * 1, 2, 4 or 32), vertex colours ignored. Without: stage 0 alone.
const char* kMaskFs = R"(#version 300 es
precision highp float;
in vec2 vUv;
in vec4 vDiffuse;
in vec3 vSpecular;
uniform sampler2D uTex;
uniform sampler2D uMask;
uniform bool uShaderOn;
uniform int uShader;
uniform float uC0;
uniform bool uRtAlpha;
out vec4 oColor;
void main() {
    vec4 t0 = texture(uTex, vUv);
    if (!uShaderOn) {
        vec3 rgb = min(t0.rgb * vDiffuse.rgb + vSpecular, vec3(1.0));
        oColor = vec4(rgb, uRtAlpha ? vDiffuse.a * vDiffuse.a : t0.a * vDiffuse.a);
        return;
    }
    float m = texture(uMask, vUv).a + uC0;
    float a;
    if (uShader == 0) a = m;
    else if (uShader == 1) a = m * 2.0;
    else if (uShader == 2) a = m * 4.0;
    else a = clamp(m * 4.0, -1.0, 1.0) * 8.0;
    oColor = vec4(t0.rgb, clamp(a, 0.0, 1.0));
}
)";

const char* kPresentVs = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
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

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        fail(std::string("WA2 shader compile failed: ") + buf);
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
        fail(std::string("WA2 shader link failed: ") + buf);
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

Renderer::Renderer() {
    progTex_ = link(kVs, kTexFs);
    progColor_ = link(kVs, kColorFs);
    progMask_ = link(kVs, kMaskFs);
    progPresent_ = link(kPresentVs, kPresentFs);
    texSize_ = glGetUniformLocation(progTex_, "uSize");
    texRtAlpha_ = glGetUniformLocation(progTex_, "uRtAlpha");
    colorSize_ = glGetUniformLocation(progColor_, "uSize");
    maskSize_ = glGetUniformLocation(progMask_, "uSize");
    maskShader_ = glGetUniformLocation(progMask_, "uShader");
    maskOn_ = glGetUniformLocation(progMask_, "uShaderOn");
    maskC0_ = glGetUniformLocation(progMask_, "uC0");
    maskRtAlpha_ = glGetUniformLocation(progMask_, "uRtAlpha");
    glUseProgram(progMask_);
    glUniform1i(glGetUniformLocation(progMask_, "uTex"), 0);
    glUniform1i(glGetUniformLocation(progMask_, "uMask"), 1);
    glUseProgram(progTex_);
    glUniform1i(glGetUniformLocation(progTex_, "uTex"), 0);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ibo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(DrawVertex), reinterpret_cast<void*>(offsetof(DrawVertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DrawVertex), reinterpret_cast<void*>(offsetof(DrawVertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DrawVertex), reinterpret_cast<void*>(offsetof(DrawVertex, diffuse)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DrawVertex), reinterpret_cast<void*>(offsetof(DrawVertex, specular)));
    glBindVertexArray(0);

    samplerNearest_ = makeSampler(GL_NEAREST);
    samplerLinear_ = makeSampler(GL_LINEAR);

    glGenTextures(1, &backTex_);
    glBindTexture(GL_TEXTURE_2D, backTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, wa2::kScreenWidth, wa2::kScreenHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &backFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, backFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, backTex_, 0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Renderer::~Renderer() {
    for (auto& [slot, t] : textures_) {
        if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
        if (t.tex) glDeleteTextures(1, &t.tex);
    }
    glDeleteFramebuffers(1, &backFbo_);
    glDeleteTextures(1, &backTex_);
    if (movieTex_) glDeleteTextures(1, &movieTex_);
    glDeleteSamplers(1, &samplerNearest_);
    glDeleteSamplers(1, &samplerLinear_);
    glDeleteBuffers(1, &vbo_);
    glDeleteBuffers(1, &ibo_);
    glDeleteVertexArrays(1, &vao_);
    for (GLuint p : {progTex_, progColor_, progMask_, progPresent_}) glDeleteProgram(p);
}

Renderer::Texture* Renderer::texture(const wa2::Gfx& gfx, int slot) {
    if (slot < 0 || slot >= wa2::kSlotCount) return nullptr;
    const wa2::ImageSlot& s = gfx.slots[static_cast<size_t>(slot)];
    Texture& t = textures_[slot];
    if (t.version == s.version && t.tex) return &t;
    // The slot changed: a new picture, a render target, or nothing.
    if (s.rtState != 0) {
        if (t.tex && t.fbo && t.w == s.texW && t.h == s.texH) {
            t.version = s.version;
            return &t;
        }
    }
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = Texture{};
    t.version = s.version;
    if (!s.loaded) return &t;
    t.w = s.texW;
    t.h = s.texH;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    if (s.rtState != 0) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &t.fbo);
        GLint prev = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
    } else if (s.hasTexture && !s.pixels.pixels.empty()) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s.pixels.pixels.data());
    }
    return &t;
}

bool Renderer::readTarget(int slot, wa2::RgbaImage& out) {
    auto it = textures_.find(slot);
    if (it == textures_.end() || !it->second.fbo) return false;
    const Texture& t = it->second;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    out.width = t.w;
    out.height = t.h;
    out.pixels.assign(static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, out.pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
    boundTarget_ = -2;
    return true;
}

void Renderer::bindTarget(int target) {
    if (target == boundTarget_) return;
    boundTarget_ = target;
    if (target < 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, backFbo_);
        targetW_ = wa2::kScreenWidth;
        targetH_ = wa2::kScreenHeight;
    } else {
        auto it = textures_.find(target);
        if (it == textures_.end() || !it->second.fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, backFbo_);
            targetW_ = wa2::kScreenWidth;
            targetH_ = wa2::kScreenHeight;
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
        targetW_ = it->second.w;
        targetH_ = it->second.h;
    }
    glViewport(0, 0, targetW_, targetH_);
}

void Renderer::setBlend(uint32_t mode) {
    // 0x43dd70: one recipe for colour and alpha alike.
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case 1:
    case 0xb: glEnable(GL_BLEND), glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case 2: glEnable(GL_BLEND), glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    case 3: glEnable(GL_BLEND), glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
    case 4: glEnable(GL_BLEND), glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_COLOR); break;
    case 5: glEnable(GL_BLEND), glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR); break;
    case 6: glEnable(GL_BLEND), glBlendFunc(GL_ZERO, GL_SRC_COLOR); break;
    case 7:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBlendEquation(GL_FUNC_SUBTRACT);
        break;
    case 8: glEnable(GL_BLEND), glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR); break;
    case 9:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        break;
    case 10: glEnable(GL_BLEND), glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR); break;
    default: glDisable(GL_BLEND); break;
    }
}

void Renderer::execute(const std::vector<DrawCommand>& commands, const wa2::Gfx& gfx) {
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    boundTarget_ = -2;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    std::vector<DrawVertex> verts;
    std::vector<uint32_t> indices;
    for (size_t ci = 0; ci < commands.size(); ++ci) {
        const DrawCommand& c = commands[ci];
        if (c.prim == DrawCommand::Prim::MakeTarget) {
            Texture* t = texture(gfx, c.target);  // made and cleared here
            (void)t;
            continue;
        }
        bindTarget(c.target);
        if (c.prim == DrawCommand::Prim::Clear) {
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            continue;
        }
        // Quads of the same state in a row go out as one draw.
        verts.clear();
        indices.clear();
        size_t last = ci;
        auto appendQuads = [&](const DrawCommand& d) {
            for (size_t q = 0; q + 3 < d.vertices.size(); q += 4) {
                auto base = static_cast<uint32_t>(verts.size());
                verts.insert(verts.end(), d.vertices.begin() + static_cast<std::ptrdiff_t>(q),
                             d.vertices.begin() + static_cast<std::ptrdiff_t>(q + 4));
                for (uint32_t k : {0u, 1u, 2u, 1u, 3u, 2u}) indices.push_back(base + k);
            }
        };
        if (c.prim == DrawCommand::Prim::Quads) {
            appendQuads(c);
            while (last + 1 < commands.size()) {
                const DrawCommand& n = commands[last + 1];
                if (n.prim != c.prim || n.target != c.target || n.texture != c.texture || n.mask != c.mask ||
                    n.shader != c.shader || n.c0w != c.c0w || n.blend != c.blend || n.linear != c.linear ||
                    n.alphaFromDiffuseSquared != c.alphaFromDiffuseSquared) {
                    break;
                }
                appendQuads(n);
                ++last;
            }
        } else {
            verts = c.vertices;
        }
        ci = last;
        if (verts.empty()) continue;

        setBlend(c.blend);
        GLuint sampler = c.linear ? samplerLinear_ : samplerNearest_;
        if (c.texture < 0) {
            glUseProgram(progColor_);
            glUniform2f(colorSize_, static_cast<float>(targetW_), static_cast<float>(targetH_));
        } else if (c.mask >= 0) {
            Texture* t0 = texture(gfx, c.texture);
            Texture* t1 = texture(gfx, c.mask);
            if (!t0 || !t1 || !t0->tex || !t1->tex) continue;
            glUseProgram(progMask_);
            glUniform2f(maskSize_, static_cast<float>(targetW_), static_cast<float>(targetH_));
            glUniform1i(maskOn_, c.shader >= 0);
            glUniform1i(maskShader_, c.shader);
            glUniform1f(maskC0_, c.c0w);
            glUniform1i(maskRtAlpha_, c.alphaFromDiffuseSquared);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, t1->tex);
            glBindSampler(1, sampler);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t0->tex);
            glBindSampler(0, sampler);
        } else {
            Texture* t = texture(gfx, c.texture);
            if (!t || !t->tex) continue;
            glUseProgram(progTex_);
            glUniform2f(texSize_, static_cast<float>(targetW_), static_cast<float>(targetH_));
            glUniform1i(texRtAlpha_, c.alphaFromDiffuseSquared);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t->tex);
            glBindSampler(0, sampler);
        }
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(DrawVertex)), verts.data(), GL_STREAM_DRAW);
        switch (c.prim) {
        case DrawCommand::Prim::Quads:
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(),
                         GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, nullptr);
            break;
        case DrawCommand::Prim::Lines: glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size())); break;
        case DrawCommand::Prim::Triangles: glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size())); break;
        case DrawCommand::Prim::Points: glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(verts.size())); break;
        case DrawCommand::Prim::TriangleFan: glDrawArrays(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(verts.size())); break;
        case DrawCommand::Prim::LineStrip: glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(verts.size())); break;
        default: break;
        }
    }
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
}

void Renderer::present(int width, int height, GLuint target) {
    drawFitted(backTex_, static_cast<float>(wa2::kScreenWidth) / wa2::kScreenHeight, width, height, target);
}

void Renderer::presentMovie(const uint8_t* rgba, int w, int h, bool changed, int width, int height, GLuint target) {
    if (!movieTex_) glGenTextures(1, &movieTex_);
    glBindTexture(GL_TEXTURE_2D, movieTex_);
    if (changed && rgba && w > 0 && h > 0) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (w != movieW_ || h != movieH_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            movieW_ = w;
            movieH_ = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
    }
    if (movieW_ == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    drawFitted(movieTex_, 16.0f / 9.0f, width, height, target);
}

void Renderer::drawFitted(GLuint tex, float aspect, int width, int height, GLuint target) {
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (width <= 0 || height <= 0) return;
    // Aspect kept, centred (the fullscreen letterbox of 0x43dde0).
    float fw = static_cast<float>(width), fh = static_cast<float>(height);
    float w = 1, h = 1;
    if (fw / fh > aspect) w = fh * aspect / fw;
    else h = fw / aspect / fh;
    // Back buffer row 0 is the top of the picture: the top of the screen.
    DrawVertex v[4];
    float xs[4] = {-w, w, -w, w}, ys[4] = {h, h, -h, -h}, us[4] = {0, 1, 0, 1}, vs[4] = {0, 0, 1, 1};
    for (int i = 0; i < 4; ++i) v[i].x = xs[i], v[i].y = ys[i], v[i].u = us[i], v[i].v = vs[i];
    glUseProgram(progPresent_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, samplerLinear_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindSampler(0, 0);
    glBindVertexArray(0);
}

}  // namespace nyan::render::wa2r
