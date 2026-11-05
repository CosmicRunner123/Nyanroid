#pragma once

#include <map>
#include <vector>

#include "nyan/render/gl.hpp"
#include "nyan/wa2/gfx.hpp"

namespace nyan::render::wa2r {

// Runs White Album 2's frame on OpenGL ES 3: the draw commands of wa2::Gfx::buildFrame into a
// 1280 x 720 back buffer and the engine's render targets, then the back buffer onto the screen,
// letterboxed (WA2.exe 0x43b590 device states, 0x43dde0 present). Needs a current GL context.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void execute(const std::vector<wa2::DrawCommand>& commands, const wa2::Gfx& gfx);
    // A render target's texels as drawn so far (0x4391f0), top row first. False if it has none.
    bool readTarget(int slot, wa2::RgbaImage& out);
    // The back buffer scaled to fit `width` x `height` of framebuffer `target`, black around it.
    void present(int width, int height, GLuint target);
    // A movie picture (RGBA, top row first; `changed`: upload it) at 16:9 in `target`, letterboxed as
    // the engine's video window (0x4138a0), whatever the picture's own size.
    void presentMovie(const uint8_t* rgba, int w, int h, bool changed, int width, int height, GLuint target);

private:
    struct Texture {
        GLuint tex = 0;
        GLuint fbo = 0;  // render targets
        uint32_t version = 0;
        int w = 0, h = 0;
    };
    Texture* texture(const wa2::Gfx& gfx, int slot);
    void bindTarget(int target);
    void setBlend(uint32_t mode);
    void flush();

    GLuint progTex_ = 0, progColor_ = 0, progMask_ = 0, progPresent_ = 0;
    GLint texRtAlpha_ = -1, texSize_ = -1, colorSize_ = -1, colorPoint_ = -1, maskSize_ = -1, maskShader_ = -1,
          maskC0_ = -1, maskOn_ = -1, maskRtAlpha_ = -1;
    GLuint vao_ = 0, vbo_ = 0, ibo_ = 0;
    GLuint samplerNearest_ = 0, samplerLinear_ = 0;
    GLuint backTex_ = 0, backFbo_ = 0;
    GLuint movieTex_ = 0;
    int movieW_ = 0, movieH_ = 0;
    void drawFitted(GLuint tex, float aspect, int width, int height, GLuint target);
    std::map<int, Texture> textures_;
    int boundTarget_ = -2;
    int targetW_ = 0, targetH_ = 0;
};

}  // namespace nyan::render::wa2r
