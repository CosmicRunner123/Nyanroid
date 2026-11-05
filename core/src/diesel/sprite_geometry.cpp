#include "nyan/diesel/sprite_geometry.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace nyan::diesel {

namespace {

using Points = std::array<std::array<float, 2>, 4>;

// The 3x3 projective map (column vectors, h[8] = 1) taking each src point to its dst point; false if
// the points are degenerate.
bool solveHomography(const Points& src, const Points& dst, std::array<double, 9>& h) {
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        double x = src[static_cast<size_t>(i)][0], y = src[static_cast<size_t>(i)][1];
        double u = dst[static_cast<size_t>(i)][0], v = dst[static_cast<size_t>(i)][1];
        double* r0 = a[2 * i];
        double* r1 = a[2 * i + 1];
        r0[0] = x, r0[1] = y, r0[2] = 1.0, r0[6] = -x * u, r0[7] = -y * u, r0[8] = u;
        r1[3] = x, r1[4] = y, r1[5] = 1.0, r1[6] = -x * v, r1[7] = -y * v, r1[8] = v;
    }
    for (int c = 0; c < 8; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 8; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        }
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c) {
            for (int k = 0; k < 9; ++k) std::swap(a[c][k], a[pivot][k]);
        }
        for (int r = 0; r < 8; ++r) {
            if (r == c) continue;
            double f = a[r][c] / a[c][c];
            if (f == 0.0) continue;
            for (int k = c; k < 9; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int i = 0; i < 8; ++i) h[static_cast<size_t>(i)] = a[i][8] / a[i][i];
    h[8] = 1.0;
    return true;
}

}  // namespace

Mat4 Mat4::translation(float x, float y, float z) {
    Mat4 r;
    r.at(3, 0) = x, r.at(3, 1) = y, r.at(3, 2) = z;
    return r;
}

Mat4 Mat4::scaling(float x, float y, float z) {
    Mat4 r;
    r.at(0, 0) = x, r.at(1, 1) = y, r.at(2, 2) = z;
    return r;
}

Mat4 Mat4::rotationX(float a) {
    Mat4 r;
    float s = std::sin(a), c = std::cos(a);
    r.at(1, 1) = c, r.at(1, 2) = s, r.at(2, 1) = -s, r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotationY(float a) {
    Mat4 r;
    float s = std::sin(a), c = std::cos(a);
    r.at(0, 0) = c, r.at(0, 2) = -s, r.at(2, 0) = s, r.at(2, 2) = c;
    return r;
}

Mat4 Mat4::rotationZ(float a) {
    Mat4 r;
    float s = std::sin(a), c = std::cos(a);
    r.at(0, 0) = c, r.at(0, 1) = s, r.at(1, 0) = -s, r.at(1, 1) = c;
    return r;
}

Mat4 Mat4::rotationZXY(float x, float y, float z) { return rotationZ(z) * rotationX(x) * rotationY(y); }

Mat4 Mat4::homographic2D(const Points& src, const Points& offsets) {
    Points dst;
    for (size_t i = 0; i < 4; ++i) dst[i] = {src[i][0] + offsets[i][0], src[i][1] + offsets[i][1]};
    std::array<double, 9> h{};
    Mat4 r;
    if (!solveHomography(src, dst, h)) return r;
    // Row vectors: x' = x h0 + y h1 + h2, and so on, w' = x h6 + y h7 + 1.
    r.at(0, 0) = static_cast<float>(h[0]), r.at(1, 0) = static_cast<float>(h[1]), r.at(3, 0) = static_cast<float>(h[2]);
    r.at(0, 1) = static_cast<float>(h[3]), r.at(1, 1) = static_cast<float>(h[4]), r.at(3, 1) = static_cast<float>(h[5]);
    r.at(0, 3) = static_cast<float>(h[6]), r.at(1, 3) = static_cast<float>(h[7]), r.at(3, 3) = 1.0f;
    return r;
}

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += at(i, k) * b.at(k, j);
            r.at(i, j) = sum;
        }
    }
    return r;
}

std::array<float, 4> Mat4::apply(float x, float y, float z, float w) const {
    std::array<float, 4> r{};
    for (int j = 0; j < 4; ++j) r[static_cast<size_t>(j)] = x * at(0, j) + y * at(1, j) + z * at(2, j) + w * at(3, j);
    return r;
}

std::array<float, 2> spriteSpaceSize(const SpriteImage& s, int screenW, int screenH) {
    if (!s.parent) return {static_cast<float>(screenW), static_cast<float>(screenH)};
    return {static_cast<float>(s.parent->srcW), static_cast<float>(s.parent->srcH)};
}

namespace {
// Mware's rotations take degrees: Matrix4x4::RotationX / RotationZXY (0x10039d20 / 0x1003a020) and
// Quaternion::RotationAxis (0x1003cff0) multiply the angle by 0x1019f558 first.
constexpr float kDegrees = 0.0174532924f;
}  // namespace

Mat4 placementMatrix(const Placement& p, float spaceW, float spaceH) {
    float w = p.width, h = p.height;
    float ax = (p.centerX - 0.5f) * w + p.pivotX;
    float ay = (p.centerY - 0.5f) * h + p.pivotY;
    // The negated angles (0x455b10), in degrees.
    float rx = -p.rotate[0] * kDegrees, ry = -p.rotate[1] * kDegrees, rz = -p.rotate[2] * kDegrees;
    Mat4 m = Mat4::translation(-ax, ay, 0.0f);
    m *= Mat4::scaling(p.scaleX, p.scaleY, 1.0f);
    if (p.quaternions) {
        // Quaternions about X, then Y, then Z (Fitted: Mware's quaternion product taken as
        // DirectXMath's "this, then that").
        m *= Mat4::rotationX(rx) * Mat4::rotationY(ry) * Mat4::rotationZ(rz);
    } else {
        m *= Mat4::rotationZXY(rx, ry, rz);
    }
    m *= Mat4::translation(ax, -ay, 0.0f);
    m *= Mat4::translation(p.position[0], -p.position[1], p.position[2]);
    m *= Mat4::translation((spaceW - w) * -0.5f, (spaceH - h) * 0.5f, 0.0f);
    return m;
}

std::array<float, 3> toScreen(const Mat4& world, float x, float y, int screenW, int screenH) {
    auto q = world.apply(x, y, 0.0f);
    float w = q[3] != 0.0f ? q[3] : 1.0f;
    return {q[0] / w + static_cast<float>(screenW) * 0.5f, static_cast<float>(screenH) * 0.5f - q[1] / w, w};
}

Mat4 spriteLocalMatrix(const SpriteImage& s, float spaceW, float spaceH) {
    Placement p;
    p.width = static_cast<float>(s.srcW), p.height = static_cast<float>(s.srcH);
    p.centerX = s.centerX, p.centerY = s.centerY, p.pivotX = s.pivotX, p.pivotY = s.pivotY;
    p.scaleX = s.scale.now[0], p.scaleY = s.scale.now[1];
    p.rotate = s.rotate.now;
    p.position = s.move.now;
    // A shake moves the position (0x455b10: its offset added before the translation).
    if (s.shake) {
        auto o = s.shake->offset();
        for (size_t i = 0; i < 3; ++i) p.position[i] += o[i];
    }
    p.quaternions = s.actionCompatible == 1;
    return placementMatrix(p, spaceW, spaceH);
}

Mat4 spriteWorldMatrix(const SpriteImage& s, int screenW, int screenH) {
    auto size = spriteSpaceSize(s, screenW, screenH);
    Mat4 m = spriteLocalMatrix(s, size[0], size[1]);
    // Up the parents (a chain that loops is cut off where the engine's would never end).
    int depth = 0;
    for (const SpriteImage* p = s.parent; p && depth < 64; p = p->parent, ++depth) {
        auto ps = spriteSpaceSize(*p, screenW, screenH);
        m *= spriteLocalMatrix(*p, ps[0], ps[1]);
    }
    return m;
}

Mat4 spriteVertexMatrix(const SpriteImage& s) {
    float sum = 0.0f;
    for (const auto& t : s.vertexMove) sum += t.now[0] * t.now[0] + t.now[1] * t.now[1];
    if (!(sum > FLT_EPSILON)) return Mat4{};
    float w = static_cast<float>(s.srcW), h = static_cast<float>(s.srcH);
    float hw = w * 0.5f, hh = h * 0.5f;
    Points src = {{{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}}};
    Points off;
    for (size_t i = 0; i < 4; ++i) off[i] = {s.vertexMove[i].now[0], s.vertexMove[i].now[1]};
    Mat4 m = Mat4::scaling(1.0f, -1.0f, 1.0f);
    m *= Mat4::translation(hw, hh, 0.0f);
    m *= Mat4::homographic2D(src, off);
    m *= Mat4::translation(-hw, -hh, 0.0f);
    m *= Mat4::scaling(1.0f, -1.0f, 1.0f);
    return m;
}

Mat4 Mat4::lookAtLH(const std::array<float, 3>& eye, const std::array<float, 3>& at, const std::array<float, 3>& up) {
    auto sub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    auto unit = [&](const std::array<float, 3>& a) {
        float l = std::sqrt(dot(a, a));
        return l > 0.0f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : a;
    };
    auto z = unit(sub(at, eye));
    auto x = unit(cross(up, z));
    auto y = cross(z, x);
    Mat4 m;
    for (int i = 0; i < 3; ++i) {
        m.at(i, 0) = x[static_cast<size_t>(i)];
        m.at(i, 1) = y[static_cast<size_t>(i)];
        m.at(i, 2) = z[static_cast<size_t>(i)];
        m.at(i, 3) = 0.0f;
    }
    m.at(3, 0) = -dot(x, eye), m.at(3, 1) = -dot(y, eye), m.at(3, 2) = -dot(z, eye), m.at(3, 3) = 1.0f;
    return m;
}

Mat4 Mat4::perspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    float ys = 1.0f / std::tan(fovY * 0.5f);
    float range = zf / (zf - zn);
    Mat4 m;
    m.m = {ys / aspect, 0.0f, 0.0f, 0.0f, 0.0f, ys, 0.0f, 0.0f, 0.0f, 0.0f, range, 1.0f, 0.0f, 0.0f, -range * zn, 0.0f};
    return m;
}

Mat4 Mat4::orthographicLH(float w, float h, float zn, float zf) {
    float range = 1.0f / (zf - zn);
    Mat4 m;
    m.m = {2.0f / w, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f / h, 0.0f, 0.0f, 0.0f, 0.0f, range, 0.0f, 0.0f, 0.0f, -range * zn, 1.0f};
    return m;
}

std::optional<SpriteQuad> spriteQuad(const SpriteImage& s, int screenW, int screenH) {
    float hw = static_cast<float>(s.srcW) * 0.5f * s.sourceScale[0], hh = static_cast<float>(s.srcH) * 0.5f * s.sourceScale[1];
    return projectQuad(s, spriteVertexMatrix(s), {{{-hw, hh}, {hw, hh}, {-hw, -hh}, {hw, -hh}}}, screenW, screenH);
}

std::vector<PieceQuad> splitPieceQuads(const SpriteImage& s, int screenW, int screenH) {
    std::vector<PieceQuad> out;
    const float hw = static_cast<float>(s.srcW) * 0.5f, hh = static_cast<float>(s.srcH) * 0.5f;
    const float sx = s.sourceScale[0], sy = s.sourceScale[1];
    const Mat4 v = spriteVertexMatrix(s);
    for (const auto& p : s.split.pieces) {
        PieceQuad q;
        const float px[4] = {p.x, p.x + p.w, p.x, p.x + p.w};
        const float py[4] = {p.y, p.y, p.y + p.h, p.y + p.h};
        std::array<std::array<float, 2>, 4> corners{};
        for (size_t k = 0; k < 4; ++k) {
            q.pixel[k] = {px[k] + p.jag[k][0], py[k] + p.jag[k][1]};
            corners[k] = {(px[k] - hw + p.jag[k][0]) * sx, (hh - py[k] - p.jag[k][1]) * sy};
        }
        float cx = p.center[0] * sx, cy = p.center[1] * sy;
        Mat4 m = Mat4::translation(-cx, cy, 0.0f);
        m *= Mat4::rotationZXY(-p.angle[0] * kDegrees, -p.angle[1] * kDegrees, -p.angle[2] * kDegrees);
        m.at(3, 0) += cx;
        m.at(3, 1) -= cy;
        m *= v;
        m.at(3, 0) = p.offset[0] * sx + m.at(3, 0);
        m.at(3, 1) = m.at(3, 1) - p.offset[1] * sy;
        m.at(3, 2) = p.offset[2] + m.at(3, 2);
        auto quad = projectQuad(s, m, corners, screenW, screenH);
        if (!quad) continue;
        q.quad = *quad;
        out.push_back(q);
    }
    return out;
}

std::optional<SpriteQuad> projectQuad(const SpriteImage& s, const Mat4& local,
                                      const std::array<std::array<float, 2>, 4>& pts, int screenW, int screenH) {
    Mat4 m = local * spriteWorldMatrix(s, screenW, screenH);
    if (s.camera && s.camera->made) {
        // The camera's view and projection, then the viewport (NDC to the target's pixels, y down).
        const CameraView& c = *s.camera;
        const float sw = static_cast<float>(screenW), sh = static_cast<float>(screenH);
        Mat4 vp = Mat4::lookAtLH(c.eye, c.target, c.up);
        if (c.ortho) vp *= Mat4::orthographicLH(c.width, c.height, c.nearZ, c.farZ);
        else vp *= Mat4::perspectiveFovLH(c.fov * 0.0174532924f, sw / sh, c.nearZ, c.farZ);
        Mat4 full = m * vp;
        SpriteQuad q;
        for (size_t i = 0; i < 4; ++i) {
            auto p = full.apply(pts[i][0], pts[i][1], 0.0f);
            if (!(p[3] > 0.0f)) return std::nullopt;  // behind the eye (the exe's GPU clips it)
            q.corner[i] = {(p[0] / p[3] + 1.0f) * 0.5f * sw, (1.0f - p[1] / p[3]) * 0.5f * sh, p[3]};
        }
        return q;
    }
    // The view (eye at z = -1000) and the orthographic projection leave x and y; the viewport puts
    // the origin at the screen's centre with y down.
    SpriteQuad q;
    for (size_t i = 0; i < 4; ++i) {
        auto p = m.apply(pts[i][0], pts[i][1], 0.0f);
        if (!(p[3] > 0.0f)) return std::nullopt;
        q.corner[i] = {p[0] / p[3] + static_cast<float>(screenW) * 0.5f,
                       static_cast<float>(screenH) * 0.5f - p[1] / p[3], p[3]};
    }
    return q;
}

std::optional<std::array<float, 2>> quadPointToUv(const SpriteQuad& q, float x, float y) {
    // The square's map onto the quad; its inverse takes the point back.
    Points unit = {{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}}};
    Points corners;
    for (size_t i = 0; i < 4; ++i) corners[i] = {q.corner[i][0], q.corner[i][1]};
    std::array<double, 9> h{};
    if (!solveHomography(unit, corners, h)) return std::nullopt;
    // Inverse by the adjugate.
    double a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7], l = h[8];
    double A = e * l - f * k, B = c * k - b * l, C = b * f - c * e;
    double D = f * g - d * l, E = a * l - c * g, F = c * d - a * f;
    double G = d * k - e * g, H = b * g - a * k, I = a * e - b * d;
    double det = a * A + b * D + c * G;
    if (std::fabs(det) < 1e-12) return std::nullopt;
    double u = A * x + B * y + C, v = D * x + E * y + F, w = G * x + H * y + I;
    if (std::fabs(w) < 1e-12) return std::nullopt;
    u /= w, v /= w;
    // The exe keeps |X| <= 1 and |Y| <= 1 in the [-1, 1] square: u and v within [0, 1].
    const double eps = 1e-6;
    if (u < -eps || u > 1.0 + eps || v < -eps || v > 1.0 + eps) return std::nullopt;
    return std::array<float, 2>{static_cast<float>(std::clamp(u, 0.0, 1.0)), static_cast<float>(std::clamp(v, 0.0, 1.0))};
}

}  // namespace nyan::diesel
