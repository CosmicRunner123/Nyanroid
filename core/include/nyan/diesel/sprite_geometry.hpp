#pragma once

#include <array>
#include <optional>

#include "nyan/diesel/sprite.hpp"

namespace nyan::diesel {

// A 4x4 matrix as Mware's Matrix4x4 (DirectXMath underneath): row-major, points as row vectors
// (p' = p M), so A * B applies A first.
struct Mat4 {
    std::array<float, 16> m{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    float& at(int r, int c) { return m[static_cast<size_t>(r * 4 + c)]; }
    float at(int r, int c) const { return m[static_cast<size_t>(r * 4 + c)]; }

    static Mat4 translation(float x, float y, float z);
    static Mat4 scaling(float x, float y, float z);
    static Mat4 rotationX(float radians);  // XMMatrixRotationX
    static Mat4 rotationY(float radians);
    static Mat4 rotationZ(float radians);
    // Mware's RotationZXY(x, y, z): about Z, then X, then Y (XMMatrixRotationRollPitchYaw).
    static Mat4 rotationZXY(float x, float y, float z);
    // Mware's Homographic2D(src, offsets) (Mware.dll 0x1003ab50): the projective map of the plane
    // taking each src point to itself plus its offset (z kept).
    static Mat4 homographic2D(const std::array<std::array<float, 2>, 4>& src,
                              const std::array<std::array<float, 2>, 4>& offsets);

    // DirectXMath's XMMatrixLookAtLH, XMMatrixPerspectiveFovLH and XMMatrixOrthographicLH (Mware's
    // Matrix4x4::LookAt, Perspective, Ortho).
    static Mat4 lookAtLH(const std::array<float, 3>& eye, const std::array<float, 3>& at, const std::array<float, 3>& up);
    static Mat4 perspectiveFovLH(float fovY, float aspect, float zn, float zf);
    static Mat4 orthographicLH(float w, float h, float zn, float zf);

    Mat4 operator*(const Mat4& b) const;
    Mat4& operator*=(const Mat4& b) { return *this = *this * b; }
    std::array<float, 4> apply(float x, float y, float z, float w = 1.0f) const;
};

// What places a picture in its space: CSprite's and CText's world matrices (0x455b10, 0x4d3fb0)
// share it (the text's picture is its field).
struct Placement {
    float width = 0.0f, height = 0.0f;
    float centerX = 0.5f, centerY = 0.5f, pivotX = 0.0f, pivotY = 0.0f;
    float scaleX = 1.0f, scaleY = 1.0f;
    std::array<float, 3> rotate{}, position{};
    bool quaternions = false;  // "action compatible" mode
};
Mat4 placementMatrix(const Placement& p, float spaceW, float spaceH);
// A point of a picture's centred space (y up) through a world matrix to the screen (game pixels,
// y down), with the projective w.
std::array<float, 3> toScreen(const Mat4& world, float x, float y, int screenW, int screenH);

// The size of the space a sprite is placed in (CSprite vtable +0x20, 0x4a2a50): the screen for one
// hung from the task's root, else its parent's part size.
std::array<float, 2> spriteSpaceSize(const SpriteImage& s, int screenW, int screenH);

// The sprite's local matrix (CSprite vtable +0x18, 0x455b10): about its anchor ((centre - 0.5) *
// size + pivot) it scales and rotates, then moves to its position, the y axis flipped for the 3D
// space whose origin is the centre of the space it is placed in.
Mat4 spriteLocalMatrix(const SpriteImage& s, float spaceW, float spaceH);
// Local times the parent's world matrix, up to the task's root (whose world is the identity).
Mat4 spriteWorldMatrix(const SpriteImage& s, int screenW, int screenH);
// The corner offsets' projective map (the vertex build 0x456400, used when the four offsets'
// squared lengths add up to more than FLT_EPSILON), in the quad's centred space.
Mat4 spriteVertexMatrix(const SpriteImage& s);

// Where the sprite's quad lands on the screen (the vertex build 0x456400): with the task's camera a
// view from z = -1000 and an orthographic projection of the screen's size; with its own camera, that
// camera's view and projection (the target's aspect), then the viewport. The quad is the part's size
// times the source scale.
std::optional<SpriteQuad> spriteQuad(const SpriteImage& s, int screenW, int screenH);
// The same for any corners of the sprite's centred space (y up) through `local` instead of the
// corner offsets' map: what the split's pieces use.
std::optional<SpriteQuad> projectQuad(const SpriteImage& s, const Mat4& local,
                                      const std::array<std::array<float, 2>, 4>& corners, int screenW, int screenH);

// One piece of a split sprite as the vertex build draws it (0x456400's split branch): its quad, and
// its corners in the sprite's pixels (y down), which pick its part of the texture and its colour.
struct PieceQuad {
    SpriteQuad quad;
    std::array<std::array<float, 2>, 4> pixel{};
};
// Each piece: its jagged cell about its group's centre turned by its angle (Z, X, Y, negated), through
// the corner offsets' map, moved by its offset; then the sprite's world and view. A piece behind the
// eye is left out.
std::vector<PieceQuad> splitPieceQuads(const SpriteImage& s, int screenW, int screenH);

// Where a screen point falls in a quad, as (u, v) from its top-left corner: the exe's hit test
// (0x4584a0) inverts the matrix taking the quad's [-1, 1] square to the screen and keeps points
// within it. nullopt outside (or for a quad with no area).
std::optional<std::array<float, 2>> quadPointToUv(const SpriteQuad& q, float x, float y);

}  // namespace nyan::diesel
