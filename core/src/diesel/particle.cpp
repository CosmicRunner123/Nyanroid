#include "nyan/diesel/particle.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nyan/diesel/action.hpp"
#include "nyan/diesel/camera.hpp"

namespace nyan::diesel {

namespace {

#include "particle_layout.inc"

using V3 = std::array<float, 3>;

constexpr float kEpsilon = 1.1920929e-07f;      // 0x5eedc8
constexpr float kNearlyOne = 0.99998999f;       // 0x5eee54
constexpr float kFramesPerMs = 0.06f;           // 0x5eedec
constexpr float kDegrees = 0.0174532924f;       // Mware's rotations take degrees (0x1019f558)
constexpr float kRadiansToDegrees = 57.2957802f;  // 0x5eeef8
constexpr float kMaxStep = 166.666672f;         // 0x5eef08

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 mul(const V3& a, const V3& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
V3 scale(const V3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float lengthSq(const V3& a) { return a[0] * a[0] + a[1] * a[1] + a[2] * a[2]; }
V3 normalized(const V3& a) {
    float l2 = lengthSq(a);
    if (l2 <= 0.0f) return a;
    return scale(a, 1.0f / std::sqrt(l2));
}
V3 cross(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// Mware's Vector3D::Transform / TransformCoord with a matrix (row vector).
V3 transform(const V3& v, const Mat4& m) {
    auto p = m.apply(v[0], v[1], v[2]);
    return {p[0], p[1], p[2]};
}
V3 transformCoord(const V3& v, const Mat4& m) {
    auto p = m.apply(v[0], v[1], v[2]);
    float w = p[3] != 0.0f ? p[3] : 1.0f;
    return {p[0] / w, p[1] / w, p[2] / w};
}
// Mware's GetRotationX / Y / Z / ZXY (degrees; a zero angle is the identity).
Mat4 rotationX(float deg) { return deg == 0.0f ? Mat4{} : Mat4::rotationX(deg * kDegrees); }
Mat4 rotationY(float deg) { return deg == 0.0f ? Mat4{} : Mat4::rotationY(deg * kDegrees); }
Mat4 rotationZ(float deg) { return deg == 0.0f ? Mat4{} : Mat4::rotationZ(deg * kDegrees); }
Mat4 rotationZXY(float x, float y, float z) { return Mat4::rotationZXY(x * kDegrees, y * kDegrees, z * kDegrees); }
// GetRotationAxis(degrees, axis) (Mware 0x10003eb0: XMMatrixRotationAxis).
Mat4 rotationAxis(float deg, const V3& axis) {
    V3 n = normalized(axis);
    float a = deg * kDegrees, s = std::sin(a), c = std::cos(a), t = 1.0f - c;
    Mat4 m;
    m.m = {t * n[0] * n[0] + c,        t * n[0] * n[1] + s * n[2], t * n[0] * n[2] - s * n[1], 0.0f,
           t * n[0] * n[1] - s * n[2], t * n[1] * n[1] + c,        t * n[1] * n[2] + s * n[0], 0.0f,
           t * n[0] * n[2] + s * n[1], t * n[1] * n[2] - s * n[0], t * n[2] * n[2] + c,        0.0f,
           0.0f, 0.0f, 0.0f, 1.0f};
    return m;
}
Mat4 inverse(const Mat4& a) {
    // General 4x4 inverse (Mware's Inverse: XMMatrixInverse).
    const auto& m = a.m;
    std::array<float, 16> inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (det == 0.0f) return r;
    float id = 1.0f / det;
    for (size_t i = 0; i < 16; ++i) r.m[i] = inv[i] * id;
    return r;
}

// The keyframes' curves of files before version 4 in the numbering later files use (0x47e8e0).
int32_t convertTempo(int32_t t) {
    static const int32_t kFrom[] = {3,    4,    5,    6,    0x13, 0x14, 0x15, 0x16, 0x43, 0x44, 0x45,
                                    0x46, 0x53, 0x54, 0x55, 0x56, 0x63, 0x64, 0x65, 0x66, 0x73, 0x74,
                                    0x75, 0x76, 0x83, 0x84, 0x85, 0x86, 0x93, 0x94, 0x95, 0x96};
    static const int32_t kTo[] = {5,    6,    0xd,  0xe,  0x15, 0x16, 0x1d, 0x1e, 0x45, 0x46, 0x4d,
                                  0x4e, 0x55, 0x56, 0x5d, 0x5e, 0x65, 0x66, 0x6d, 0x6e, 0x75, 0x76,
                                  0x7d, 0x7e, 0x87, 0x88, 0x8f, 0x90, 0x97, 0x98, 0x9f, 0xa0};
    for (size_t i = 0; i < std::size(kFrom); ++i) {
        if (kFrom[i] == t) return kTo[i];
    }
    return t;
}

// A keyframe's curve between two values over `duration`, at `time` (the tables 0x5e57a8 / 0x5e6910
// / 0x5e6688 / 0x5e7b28, one formula per curve applied to each component; curves above 160 as 0).
float ease(int32_t tempo, float a, float b, float duration, float time) {
    return easing(tempo < 0 || tempo > 0xa0 ? 0 : tempo, a, b, duration, time);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The file

int32_t ParticleFile::i(size_t off) const {
    int32_t v = 0;
    if (off + 4 <= raw.size()) std::memcpy(&v, raw.data() + off, 4);
    return v;
}
float ParticleFile::f(size_t off) const {
    float v = 0.0f;
    if (off + 4 <= raw.size()) std::memcpy(&v, raw.data() + off, 4);
    return v;
}
std::string ParticleFile::s(size_t off, size_t max) const {
    if (off >= raw.size()) return {};
    size_t end = off;
    while (end < raw.size() && end < off + max && raw[end]) ++end;
    return std::string(reinterpret_cast<const char*>(raw.data() + off), end - off);
}

void ParticleFile::setI(size_t off, int32_t v) {
    if (off + 4 <= raw.size()) std::memcpy(raw.data() + off, &v, 4);
}
void ParticleFile::setF(size_t off, float v) {
    if (off + 4 <= raw.size()) std::memcpy(raw.data() + off, &v, 4);
}
void ParticleFile::setS(size_t off, const std::string& v) {
    if (off + v.size() + 1 <= raw.size()) {
        std::memcpy(raw.data() + off, v.data(), v.size());
        raw[off + v.size()] = 0;
    }
}

const ParticleField kParticleFields[] = {
#include "particle_fields.inc"
};
const size_t kParticleFieldCount = sizeof(kParticleFields) / sizeof(kParticleFields[0]);

std::optional<ParticleFile> parseParticleFile(const uint8_t* data, size_t size) {
    if (size < 0x14f4) return std::nullopt;
    ParticleFile f;
    f.raw.assign(ParticleFile::kSize, 0);
    auto put = [&f](size_t off, int32_t v) { std::memcpy(f.raw.data() + off, &v, 4); };
    if (size == 0x14f4) {
        // The record before the texture list: version 0, one texture used.
        std::memcpy(f.raw.data(), data, 0x14f4);
        put(kVersion, 0);
        put(kTextureCount, 1);
        put(kTextureUseCount, 1);
        f.textures.push_back(f.s(kTextureFilename));
    } else {
        std::memcpy(f.raw.data(), data, std::min(size, ParticleFile::kSize));
        if (f.i(kVersion) == 2) {
            // Version 2 had no font size and polyline fields yet.
            for (size_t off = kFontSize; off <= kPolylineSyncLifeTime; off += 4) put(off, 0);
            put(0x15b0, 0);
        }
        f.textures.push_back(f.s(kTextureFilename));
        size_t pos = ParticleFile::kSize;
        uint32_t count = static_cast<uint32_t>(f.i(kTextureCount));
        for (uint32_t t = 1; t < count; ++t) {
            if (pos + 2 > size) break;
            uint16_t len = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
            pos += 2;
            if (static_cast<uint16_t>(len - 1u) > 0xff || pos + len > size) break;
            std::string name(reinterpret_cast<const char*>(data + pos), len);
            pos += len;
            name = name.substr(0, name.find('\0'));
            f.textures.push_back(name);
        }
        if (f.i(kTextureUseCount) == 0) put(kTextureUseCount, 1);
    }
    if (f.i(kVersion) < 4) {
        for (size_t off : {kSpriteSizeMiddleTempo, kMoveAttractMiddleTempo, kTurnRadiusMiddleTempo,
                           kTurnAngularVelocityMiddleTempo, kSpriteSizeEndTempo, kMoveAttractEndTempo,
                           kTurnRadiusEndTempo, kTurnAngularVelocityEndTempo, kSpriteColorMiddleTempo,
                           kSpriteColorEndTempo}) {
            put(off, convertTempo(f.i(off)));
        }
    }
    return f;
}

// ---------------------------------------------------------------------------------------------
// The particles

// One particle (Diesel::Particle::CParticleObject, 0x66c bytes; offsets are its).
struct ParticleSystem::Particle {
    uint32_t index = 0;        // +0xb4
    int32_t alive = 0;         // +0xb8
    int32_t started = 0;       // +0xbc: its delay is over
    float time = 0.0f;         // +0xc0: ms since it started this life
    float age = -1.0f;         // +0xc4: its life (-1: for ever)
    int32_t limit = 0;         // +0xc8: lives (-1: for ever)
    int32_t repeats = 0;       // +0xcc
    float delay = 0.0f;        // +0xd0
    V3 base{};                 // +0x10c (left at zero by everything here)
    uint32_t texture = 0;      // +0x118
    // CLocalRotate (+0x130, +0x170, +0x1b4): its own rotation about X, Y and Z, degrees.
    struct Spin {
        int32_t on = 0;
        float angle = 0.0f, velocity = 0.0f, accel = 0.0f, resist = 1.0f, addResist = 0.0f;
        float time = 0.0f, frame = 0.0f, accum = 0.0f;
        uint32_t ticks = 0;
    };
    std::array<Spin, 3> spin;
    int32_t faceMotion = 0;    // +0x1b0: rotate.z.type 1, its Z rotation follows its motion
    // CGlobalRotate (+0x1f4): the turn about the place it would be (radii and angles per axis).
    struct Turn {
        V3 radius{}, start{}, middle{}, end{};      // +0x1f8, +0x204, +0x214, +0x22c
        int32_t useMiddle = 0, useEnd = 0;          // +0x210, +0x228
        float timing = 0.5f;                        // +0x220
        int32_t middleTempo = 0, endTempo = 0;      // +0x224, +0x238
        float time = 0.0f, frame = 0.0f;            // +0x23c, +0x240
        V3 angle{}, velocity{}, accel{}, resist{1.0f, 1.0f, 1.0f}, addResist{};  // +0x244 ...
        V3 frameTime{}, times{}, accum{};           // +0x2b0, +0x2bc, +0x308
        std::array<uint32_t, 3> ticks{};            // +0x314
    } turn;
    // CScale (+0x320) and CColor (+0x35c): keyframes over the life (or a frame of it, repeating).
    struct Size {
        std::array<float, 2> now{1.0f, 1.0f}, start{1.0f, 1.0f}, middle{1.0f, 1.0f}, end{1.0f, 1.0f};
        int32_t useMiddle = 0;
        float timing = 1.0f;
        int32_t middleTempo = 0, endTempo = 0;
        float time = 0.0f, frame = 0.0f;
    } size;
    struct Color {
        std::array<float, 4> now{1.0f, 1.0f, 1.0f, 1.0f}, start{1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 4> middle{1.0f, 1.0f, 1.0f, 1.0f}, end{1.0f, 1.0f, 1.0f, 1.0f};
        int32_t useMiddle = 0;
        float timing = 1.0f;
        int32_t middleTempo = 0, endTempo = 0;
        float time = 0.0f, frame = 0.0f;
    } color;
    // CMove (+0x3b8): the motion from where it was made.
    struct Move {
        V3 prev{}, pos{}, last{};                   // +0x3bc, +0x3c8, +0x3d4
        V3 velocity{}, accel{}, resist{1.0f, 1.0f, 1.0f}, addResist{};  // +0x3e0 ...
        float gravityAccel = 0.0f, gravity = 0.0f, gravityFactor = 0.0f;  // +0x410, +0x414, +0x418
        V3 direction{};                             // +0x420
        V3 attractStart{}, attractMiddle{}, attractEnd{};  // +0x42c, +0x438, +0x444
        int32_t useMiddle = 0;                      // +0x450
        float timing = 0.5f;                        // +0x454
        int32_t middleTempo = 0, endTempo = 0;      // +0x458, +0x45c
        float power = 0.0f;                         // +0x460
        float time = 0.0f, frame = 0.0f;            // +0x464, +0x468
        uint32_t ticks = 0;                         // +0x46c
        V3 tickPos{};                               // +0x470
        V3 velocityNow{};                           // +0x47c
    } move;
    V3 next{}, now{};          // +0x488 (after the step), +0x494 (before it)
    int32_t first = 1;         // +0x4a0
    V3 emitterOffset{};        // +0x4a4
    std::array<float, 4> uv{};  // +0x4c0: the texture's part (pixels)
};

// The emitter (Diesel::Particle::CParticleEmitter, 0x124 bytes; 0x4acac0 reads the location).
struct ParticleSystem::Emitter {
    int32_t type = 0;           // +0xb4: 1 point, 2 circle, 3 sphere, 4 box
    float circleRadius = 0.0f;  // +0xb8
    V3 circleAxis{0.0f, 0.0f, 1.0f};  // +0xbc
    int32_t circleRandom = 0;   // +0xc8
    int32_t circleSpectrum = 0; // +0xcc
    float circleFirst = 0.0f, circleInterval = 0.0f, circleJitter = 0.0f;  // +0xd0 ...
    float sphereRadius = 0.0f;  // +0xdc
    int32_t sphereRandom = 0, sphereSpectrum = 0;  // +0xe0, +0xe4
    float latitude = 0.0f, longitude = 0.0f, latitudeEnd = 180.0f, longitudeEnd = 360.0f;  // +0xe8 ...
    float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f, front = 0.0f, back = 0.0f;  // +0xf8 ...
    V3 boxRotate{};             // +0x110
    int32_t boxMake = 0;        // +0x11c: 0 faces, 1 inside, 2 edges
    uint32_t aspects = 0x3f;    // +0x120: left, right, top, bottom, front, back
};

namespace {

using Particle = ParticleSystem::Particle;

// A random float between two values (the exe's pattern: equal values stay; GetFloat(min, max)).
float between(MwareRandom& r, float a, float b) {
    if (a == b) return a;
    return r.nextFloat(std::min(a, b), std::max(a, b));
}
// Between 0 and `x` (the uniform turns and circle jitter).
float towards(MwareRandom& r, float x) {
    if (x == 0.0f) return 0.0f;
    return r.nextFloat(std::min(x, 0.0f), std::max(x, 0.0f));
}
bool coin(MwareRandom& r) { return r.nextInt(0, 1) != 0; }  // 0x477000
float unsignedToFloat(uint32_t v) { return static_cast<float>(static_cast<double>(v)); }

// The velocity (3D: 0x4adac0; 1D: inside 0x4ae0d0) at frame t of a motion with initial velocity v0,
// acceleration a and a resistance factor R0 growing by `add` a frame: R = R0 + add t, E = e^((R-1) t),
// v = a R / (1 - R) (1 - E) + v0 E.
float velocityAt(float v0, float a, float r0, float add, float t) {
    float r = add * t + r0;
    float e = std::exp((r - 1.0f) * t);
    return a * r / (1.0f - r) * (1.0f - e) + v0 * e;
}
// The distance covered between frames t0 and t1: Simpson's rule over ten parts (0x4add10, 0x4ae0d0).
float distance(float v0, float a, float r0, float add, float t0, float t1) {
    float h = (t1 - t0) / 10.0f;
    float sum = velocityAt(v0, a, r0, add, h * 10.0f + t0) + velocityAt(v0, a, r0, add, t0);
    for (uint32_t i = 1; i < 10; i += 2) sum += 4.0f * velocityAt(v0, a, r0, add, unsignedToFloat(i) * h + t0);
    for (uint32_t i = 2; i < 10; i += 2) sum += 2.0f * velocityAt(v0, a, r0, add, unsignedToFloat(i) * h + t0);
    return sum * h * 0.333333343f;
}
// A resistance of exactly 1 would divide by zero: the callers use 0.99999 then.
float clampResist(float r) { return std::fabs(r - 1.0f) < kEpsilon ? kNearlyOne : r; }

// Three keyframes over a life (CScale 0x4afa70, CColor 0x4afcf0; the turn's radius 0x4af2a0): the
// fraction s of the frame; start to middle until the timing, middle to end after (its own fraction
// of the rest), or start to end; each with its curve.
template <size_t N>
std::array<float, N> keys(const std::array<float, N>& start, const std::array<float, N>& middle,
                          const std::array<float, N>& end, bool useMiddle, float timing, int32_t middleTempo,
                          int32_t endTempo, float s) {
    std::array<float, N> out{};
    const std::array<float, N>* from = &start;
    const std::array<float, N>* to = &end;
    float duration = 1.0f;
    int32_t tempo = endTempo;
    if (useMiddle) {
        if (s <= timing) {
            to = &middle;
            duration = timing;
            tempo = middleTempo;
        } else {
            from = &middle;
            duration = 1.0f - timing;
            s -= timing;
        }
    }
    for (size_t i = 0; i < N; ++i) out[i] = ease(tempo, (*from)[i], (*to)[i], duration, s);
    return out;
}

// CLocalRotate's step (0x4aec70): the angle integrated in ten-frame parts; at the end of its frame
// the time starts again from the angle reached.
void stepSpin(Particle::Spin& s, float dt) {
    if (!s.on) return;
    s.time = dt + s.time;
    float frames = s.time * kFramesPerMs;
    uint32_t n = static_cast<uint32_t>(std::fabs(frames)) / 10;
    float r = clampResist(s.resist);
    for (uint32_t k = s.ticks; k < n; ++k) {
        s.accum += distance(s.velocity, s.accel, r, s.addResist, unsignedToFloat(k * 10), unsignedToFloat(k * 10 + 10));
    }
    s.angle = s.accum;
    s.ticks = n;
    s.angle = distance(s.velocity, s.accel, r, s.addResist, unsignedToFloat(n * 10), frames) + s.angle;
    if (s.frame > 0.0f && s.frame - s.time < kEpsilon) {
        s.time = 0.0f;
        s.accum = s.angle;
        s.ticks = 0;
    }
}

// CGlobalRotate's step (0x4af2a0): the radius by its keyframes; each axis's angle integrated as the
// spin's, while that axis's radius is not zero.
void stepTurn(Particle::Turn& t, float dt) {
    t.time = dt + t.time;
    if (t.frame <= 0.0f) {
        t.radius = t.start;
    } else {
        float s = t.time / t.frame;
        if (!t.useMiddle) {
            if (t.useEnd) t.radius = keys<3>(t.start, t.middle, t.end, false, t.timing, t.middleTempo, t.endTempo, s);
        } else if (s <= t.timing || t.useEnd) {
            t.radius = keys<3>(t.start, t.middle, t.end, true, t.timing, t.middleTempo, t.endTempo, s);
        }
    }
    for (size_t i = 0; i < 3; ++i) t.times[i] = dt + t.times[i];
    for (size_t i = 0; i < 3; ++i) {
        if (!(std::fabs(t.radius[i]) > kEpsilon)) continue;
        float frames = t.times[i] / 1000.0f * 60.0f;
        float r = clampResist(t.resist[i]);
        uint32_t n = static_cast<uint32_t>(frames) / 10;
        for (uint32_t k = t.ticks[i]; k < n; ++k) {
            t.accum[i] += distance(t.velocity[i], t.accel[i], r, t.addResist[i], unsignedToFloat(k * 10), unsignedToFloat(k * 10 + 10));
        }
        t.angle[i] = t.accum[i];
        t.ticks[i] = n;
        t.angle[i] = distance(t.velocity[i], t.accel[i], r, t.addResist[i], unsignedToFloat(n * 10), frames) + t.angle[i];
    }
    for (size_t i = 0; i < 3; ++i) {
        if (t.frameTime[i] > 0.0f && t.frameTime[i] - t.times[i] < kEpsilon) {
            t.times[i] = 0.0f;
            t.accum[i] = t.angle[i];
            t.ticks[i] = 0;
        }
    }
}

// The attraction's value at frame t (the attract keyframes over the life in frames, times the power)
// and its integral (0x4ae400 / 0x4ae830: Simpson's rule over ten parts).
V3 attractAt(const Particle::Move& m, float ageFrames, float t) {
    float s = t / ageFrames;
    V3 v = m.useMiddle ? keys<3>(m.attractStart, m.attractMiddle, m.attractEnd, true, m.timing, m.middleTempo, m.endTempo, s)
                       : keys<3>(m.attractStart, m.attractMiddle, m.attractEnd, false, m.timing, m.middleTempo, m.endTempo, s);
    return scale(v, m.power);
}
V3 attractDistance(const Particle::Move& m, float ageFrames, float t0, float t1) {
    if (ageFrames <= 0.0f) return scale(m.attractStart, m.power * (t1 - t0));  // Guess: an endless life
    float h = (t1 - t0) / 10.0f;
    V3 sum = add(attractAt(m, ageFrames, h * 10.0f + t0), attractAt(m, ageFrames, t0));
    for (uint32_t i = 1; i < 10; i += 2) sum = add(sum, scale(attractAt(m, ageFrames, unsignedToFloat(i) * h + t0), 4.0f));
    for (uint32_t i = 2; i < 10; i += 2) sum = add(sum, scale(attractAt(m, ageFrames, unsignedToFloat(i) * h + t0), 2.0f));
    return scale(sum, h / 3.0f);
}

// CMove's step (0x4b0170): the distance integrated in ten-frame parts from where the particle was
// made, the attraction along its direction, gravity's fall (1/2 g factor t^2, frames).
void stepMove(Particle::Move& m, float dt) {
    m.time = dt + m.time;
    m.last = m.pos;
    float frames = m.time * kFramesPerMs;
    float ageFrames = m.frame * kFramesPerMs;
    V3 r{clampResist(m.resist[0]), clampResist(m.resist[1]), clampResist(m.resist[2])};
    auto travel = [&m, &r](float t0, float t1) {
        V3 d{};
        for (size_t i = 0; i < 3; ++i) d[i] = distance(m.velocity[i], m.accel[i], r[i], m.addResist[i], t0, t1);
        return d;
    };
    uint32_t n = static_cast<uint32_t>(frames) / 10;
    for (uint32_t k = m.ticks; k < n; ++k) {
        float t0 = unsignedToFloat(k * 10), t1 = unsignedToFloat(k * 10 + 10);
        m.tickPos = add(m.tickPos, travel(t0, t1));
        m.tickPos = add(m.tickPos, mul(m.direction, attractDistance(m, ageFrames, t0, t1)));
    }
    m.ticks = n;
    float t0 = unsignedToFloat(n * 10);
    m.pos = add(m.tickPos, travel(t0, frames));
    m.pos = add(m.pos, mul(m.direction, attractDistance(m, ageFrames, t0, frames)));
    m.pos[1] = m.pos[1] - m.gravity * 0.5f * m.gravityFactor * frames * frames;
    for (size_t i = 0; i < 3; ++i) m.velocityNow[i] = velocityAt(m.velocity[i], m.accel[i], r[i], m.addResist[i], frames);
}

// The turn's offset from the particle's place: (0, rx, 0) about X, (0, 0, ry) about Y, (rz, 0, 0)
// about Z, each by its angle (0x4b5a80, 0x4b5f20).
V3 turnOffset(const Particle::Turn& t) {
    V3 a = transform({0.0f, t.radius[0], 0.0f}, rotationX(t.angle[0]));
    V3 b = transform({0.0f, 0.0f, t.radius[1]}, rotationY(t.angle[1]));
    V3 c = transform({t.radius[2], 0.0f, 0.0f}, rotationZ(t.angle[2]));
    return add(add(a, b), c);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The system

ParticleSystem::ParticleSystem(ParticleFile file, std::vector<Texture> textures, int32_t seed)
    : file_(std::move(file)), textures_(std::move(textures)), random_(seed), emitter_(std::make_unique<Emitter>()) {
    const ParticleFile& p = file_;
    // The emitter (0x4acac0).
    Emitter& e = *emitter_;
    if (p.i(kLocationUseBoundingPoint)) {
        e.type = 1;
    } else if (p.i(kLocationUseBoundingCircle)) {
        e.type = 2;
        e.circleRadius = p.f(kLocationCircleRadius);
        e.circleAxis = {p.f(kLocationCircleRotateX), p.f(kLocationCircleRotateY), p.f(kLocationCircleRotateZ)};
        if (lengthSq(e.circleAxis) < kEpsilon) e.circleAxis = {0.0f, 0.0f, 1.0f};
        e.circleRandom = p.i(kLocationCircleMakeType) != 0;
        e.circleSpectrum = p.i(kLocationCircleSpectrumType);
        e.circleFirst = p.f(kLocationCircleFirstAngle);
        e.circleInterval = p.f(kLocationCircleIntervalAngle);
        e.circleJitter = p.f(kLocationCircleIntervalDurationAngle);
    } else if (p.i(kLocationUseBoundingSphere)) {
        e.type = 3;
        e.sphereRadius = p.f(kLocationSphereRadius);
        e.sphereRandom = p.i(kLocationSphereMakeType) != 0;
        e.sphereSpectrum = p.i(kLocationSphereSpectrumType);
        e.latitude = p.f(kLocationSphereLatitudeAngle) + 90.0f;
        e.longitude = p.f(kLocationSphereLongitudeAngle);
        if (p.i(kLocationSphereUseBasePointSelectRange)) {
            e.latitudeEnd = p.f(kLocationSphereLatitudeSelectRange) + 90.0f;
            e.longitudeEnd = p.f(kLocationSphereLongitudeSelectRange);
        }
    } else if (p.i(kLocationUseBoundingBox)) {
        e.type = 4;
        e.left = p.f(kLocationBoxSizeLeft), e.right = p.f(kLocationBoxSizeRight);
        e.top = p.f(kLocationBoxSizeTop), e.bottom = p.f(kLocationBoxSizeBottom);
        e.front = p.f(kLocationBoxSizeFront), e.back = p.f(kLocationBoxSizeBack);
        e.boxRotate = {p.f(kLocationBoxRotateX), p.f(kLocationBoxRotateY), p.f(kLocationBoxRotateZ)};
        int32_t make = p.i(kLocationBoxMakeType);
        e.boxMake = make == 0 ? 0 : (make != 1) + 1;
        e.aspects = 0x3f;
        if (!p.i(kLocationBoxAspectLeft)) e.aspects = 0x3e;
        if (!p.i(kLocationBoxAspectRight)) e.aspects &= ~2u;
        if (!p.i(kLocationBoxAspectTop)) e.aspects &= ~4u;
        if (!p.i(kLocationBoxAspectBottom)) e.aspects &= ~8u;
        if (!p.i(kLocationBoxAspectFront)) e.aspects &= ~0x10u;
        if (!p.i(kLocationBoxAspectBack)) e.aspects &= ~0x20u;
    }
    billboard_ = p.i(kSpriteUseBillboard) != 0;
    // The number of particles (0x4776b0): the file's, or one between it and the range.
    int32_t count = p.i(kOccurParticleNumNum);
    if (p.i(kOccurParticleNumUseRandom)) {
        int32_t other = p.i(kOccurParticleNumRandomRange);
        if (count != other) count = random_.nextInt(std::min(count, other), std::max(count, other));
    }
    count = std::clamp(count, 0, 0x3fff);  // the exe refuses sets of 0xffff / 4 vertices or more
    for (int32_t i = 0; i < count; ++i) {
        auto part = std::make_unique<Particle>();
        part->index = static_cast<uint32_t>(i);
        initParticle(*part);
        particles_.push_back(std::move(part));
    }
}

ParticleSystem::~ParticleSystem() = default;

size_t ParticleSystem::aliveCount() const {
    size_t n = 0;
    for (const auto& p : particles_) n += p->alive && p->started ? 1 : 0;
    return n;
}

void ParticleSystem::initParticle(Particle& o) {
    const ParticleFile& p = file_;
    MwareRandom& r = random_;
    o.first = 1;
    // The texture (0x47aa80): the first, or one of the others when the file uses several (Guess:
    // the exe's pool of textures in turn is not followed).
    o.texture = 0;
    if (textures_.size() > 1 && p.i(kTextureUseCount) > 1) {
        o.texture = static_cast<uint32_t>(r.nextInt(0, static_cast<int32_t>(textures_.size()) - 1));
    }
    o.alive = 1;
    o.time = 0.0f;
    if (!p.i(kOccurUseParticleAge)) {
        o.age = -1.0f;
    } else {
        float a = static_cast<float>(p.i(kOccurParticleAgeTime));
        if (p.i(kOccurParticleAgeUseRandom)) a = between(r, a, static_cast<float>(p.i(kOccurParticleAgeRandomRange)));
        o.age = a;
    }
    o.limit = p.i(kOccurPatturn) ? -1 : p.i(kOccurCount);
    if (!o.started) {
        o.repeats = 0;
        float suspend = static_cast<float>(p.i(kOccurSuspendTime));
        if (!p.i(kOccurUseDelay)) {
            o.delay = suspend;
        } else {
            float d = static_cast<float>(p.i(kOccurDelayTime));
            if (p.i(kOccurDelayUseRandom)) d = between(r, d, static_cast<float>(p.i(kOccurDelayRandomRange)));
            if (p.i(kOccurDelayOrder) == 0) d = d * unsignedToFloat(o.index + 1);
            o.delay = d + suspend;
        }
    }
    // Where it is made (0x4ace10); the place is the motion's start.
    auto place = [&o](const V3& at) {  // 0x4b7450
        o.next = o.now = at;
        o.move.pos = o.move.prev = o.move.last = at;
        o.move.ticks = 0;
        o.move.tickPos = at;
    };
    const Emitter& e = *emitter_;
    if (e.type == 1) {
        place({0.0f, 0.0f, 0.0f});
    } else if (e.type == 2) {
        float angle = e.circleFirst;
        float n = unsignedToFloat(static_cast<uint32_t>(particles_.size()));
        if (e.circleSpectrum == 0) {
            angle = angle + (360.0f / n) * unsignedToFloat(o.index) + e.circleInterval;
        } else if (e.circleSpectrum == 1) {
            angle = unsignedToFloat(o.index) * e.circleInterval + angle;
        } else if (e.circleSpectrum == 2) {
            angle = r.nextFloat(0.0f, 360.0f) + e.circleInterval + angle;  // Guess: the range drawn from
        }
        V3 axis = normalized(e.circleAxis);
        V3 side = cross({0.0f, 0.0f, 1.0f}, axis);
        if (lengthSq(side) < 9.99999975e-05f) side = {1.0f, 0.0f, 0.0f};
        side = normalized(side);
        float radius = e.circleRandom ? towards(r, e.circleRadius) : e.circleRadius;
        V3 at = scale(side, radius);
        float jitter = towards(r, e.circleJitter);
        place(transformCoord(at, rotationAxis(jitter + angle, axis)));
    } else if (e.type == 3) {
        float lat = e.latitude, lon = e.longitude;
        uint32_t n = static_cast<uint32_t>(particles_.size());
        if (e.sphereSpectrum == 0) {
            if (o.index != 0) {
                if (o.index + 1 != n) {
                    // Rows and columns of a grid (Guess: the grid's side as the square root of the count).
                    uint32_t side = std::max<uint32_t>(1, static_cast<uint32_t>(std::sqrt(static_cast<float>(n))));
                    uint32_t row = (o.index - 1) / side;
                    uint32_t col = o.index - row - 1;
                    float span = e.latitudeEnd - e.latitude, step = 1.0f / unsignedToFloat(side);
                    float edge = step * span;
                    lat = (span - (edge + edge)) * step * unsignedToFloat(row) + edge + lat;
                    lon = (e.longitudeEnd - e.longitude) * step * unsignedToFloat(col) + lon;
                } else {
                    lat = e.latitudeEnd;
                }
            }
        } else if (e.sphereSpectrum == 1) {
            lat = (e.latitudeEnd - e.latitude) / unsignedToFloat(n) * unsignedToFloat(o.index) + lat;
            lon = (e.longitudeEnd - e.longitude) / unsignedToFloat(n) * unsignedToFloat(o.index) + lon;
        } else if (e.sphereSpectrum == 2) {
            lat = between(r, e.latitude, e.latitudeEnd);
            lon = between(r, e.longitude, e.longitudeEnd);
        }
        V3 at = transformCoord({0.0f, 1.0f, 0.0f}, rotationX(lat) * rotationY(lon));
        if (!e.sphereRandom) {
            at = scale(at, e.sphereRadius);
        } else {
            at[0] = towards(r, e.sphereRadius) * at[0];
            at[1] = towards(r, e.sphereRadius) * at[1];
            at[2] = towards(r, e.sphereRadius) * at[2];
        }
        place(at);
    } else if (e.type == 4) {
        V3 at{};
        auto x = [&] { return between(r, e.left, e.right); };
        auto y = [&] { return between(r, e.top, e.bottom); };
        auto z = [&] { return between(r, e.front, e.back); };
        uint32_t a = e.aspects;
        enum Face { None, X, Y, Z } face = None;
        if (e.boxMake == 1) {
            at[0] = x(), at[1] = y(), at[2] = z();
        } else {
            int32_t axis = r.nextInt(1, 3);  // 0x476fc0
            if (axis == 1) {
                if (a & 3) face = X;
                else if ((a & 0xc) && (a & 0x30)) face = coin(r) ? Y : Z;
                else if (a & 0xc) face = Y;
                else if (a & 0x30) face = Z;
            } else if (axis == 2) {
                if (a & 0xc) face = Y;
                else if ((a & 3) && (a & 0x30)) face = coin(r) ? X : Z;
                else if (a & 3) face = X;
                else if (a & 0x30) face = Z;
            } else {
                if (a & 0x30) face = Z;
                else if ((a & 3) && (a & 0xc)) face = coin(r) ? X : Y;
                else if (a & 3) face = X;
                else if (a & 0xc) face = Y;
            }
            // On the face (both sides allowed: either); the others inside (faces) or one of them on
            // an edge too (edges).
            if (face == X) {
                at[0] = (a & 3) == 3 ? (coin(r) ? e.left : e.right) : ((a & 1) ? e.left : e.right);
                if (e.boxMake == 0) {
                    at[1] = y(), at[2] = z();
                } else if (coin(r)) {
                    at[1] = coin(r) ? e.top : e.bottom;
                    at[2] = z();
                } else {
                    at[2] = coin(r) ? e.front : e.back;
                    at[1] = y();
                }
            } else if (face == Y) {
                at[1] = (a & 0xc) == 0xc ? (coin(r) ? e.top : e.bottom) : ((a & 4) ? e.top : e.bottom);
                if (e.boxMake == 0) {
                    at[0] = x(), at[2] = z();
                } else if (coin(r)) {
                    at[0] = coin(r) ? e.left : e.right;
                    at[2] = z();
                } else {
                    at[2] = coin(r) ? e.front : e.back;
                    at[0] = x();
                }
            } else if (face == Z) {
                at[2] = (a & 0x30) == 0x30 ? (coin(r) ? e.front : e.back) : ((a & 0x10) ? e.front : e.back);
                if (e.boxMake == 0) {
                    at[0] = x(), at[1] = y();
                } else if (coin(r)) {
                    at[1] = coin(r) ? e.top : e.bottom;
                    at[0] = x();
                } else {
                    at[0] = coin(r) ? e.left : e.right;
                    at[1] = y();
                }
            }
        }
        place(transformCoord(at, rotationZXY(e.boxRotate[0], e.boxRotate[1], e.boxRotate[2])));
    }
    // Its own rotations (rotate.x / y / z: type 0 turns by velocity, acceleration and resistance;
    // rotate.z type 1 follows the motion).
    struct SpinKeys {
        size_t type, angle, angleRandom, angleRange, vel, velRandom, velRange, acc, accRandom, accRange;
        size_t res, resRandom, resRange, resAdd, useFrame, frame, frameRandom, frameRange;
    };
    static const SpinKeys kSpins[3] = {
        {kRotateXType, kRotateXAngleValue, kRotateXAngleUseRandom, kRotateXAngleRandomRange, kRotateXVelocityValue,
         kRotateXVelocityUseRandom, kRotateXVelocityRandomRange, kRotateXAccelerationValue, kRotateXAccelerationUseRandom,
         kRotateXAccelerationRandomRange, kRotateXResistValue, kRotateXResistUseRandom, kRotateXResistRandomRange,
         kRotateXResistAddResist, kRotateXUseFrame, kRotateXFrameTime, kRotateXFrameUseRandom, kRotateXFrameRandomRange},
        {kRotateYType, kRotateYAngleValue, kRotateYAngleUseRandom, kRotateYAngleRandomRange, kRotateYVelocityValue,
         kRotateYVelocityUseRandom, kRotateYVelocityRandomRange, kRotateYAccelerationValue, kRotateYAccelerationUseRandom,
         kRotateYAccelerationRandomRange, kRotateYResistValue, kRotateYResistUseRandom, kRotateYResistRandomRange,
         kRotateYResistAddResist, kRotateYUseFrame, kRotateYFrameTime, kRotateYFrameUseRandom, kRotateYFrameRandomRange},
        {kRotateZType, kRotateZAngleValue, kRotateZAngleUseRandom, kRotateZAngleRandomRange, kRotateZVelocityValue,
         kRotateZVelocityUseRandom, kRotateZVelocityRandomRange, kRotateZAccelerationValue, kRotateZAccelerationUseRandom,
         kRotateZAccelerationRandomRange, kRotateZResistValue, kRotateZResistUseRandom, kRotateZResistRandomRange,
         kRotateZResistAddResist, kRotateZUseFrame, kRotateZFrameTime, kRotateZFrameUseRandom, kRotateZFrameRandomRange},
    };
    auto value = [&](size_t v, size_t useRandom, size_t range) {
        float x = p.f(v);
        return p.i(useRandom) ? between(r, x, p.f(range)) : x;
    };
    auto frameOf = [&](size_t use, size_t time, size_t useRandom, size_t range) {
        if (!p.i(use)) return o.age;
        float t = static_cast<float>(p.i(time));
        return p.i(useRandom) ? between(r, t, static_cast<float>(p.i(range))) : t;
    };
    for (auto& s : o.spin) s.on = 0;
    o.faceMotion = 0;
    for (size_t i = 0; i < 3; ++i) {
        const SpinKeys& k = kSpins[i];
        Particle::Spin& s = o.spin[i];
        if (p.i(k.type) == 0) {
            s.on = 1;
            s.angle = value(k.angle, k.angleRandom, k.angleRange);
            s.accum = s.angle;
            s.velocity = value(k.vel, k.velRandom, k.velRange);
            s.accel = value(k.acc, k.accRandom, k.accRange);
            float res = 1.0f - p.f(k.res);
            s.resist = p.i(k.resRandom) ? between(r, res, p.f(k.resRange)) : res;
            s.addResist = p.f(k.resAdd);
            s.time = 0.0f;
            s.frame = frameOf(k.useFrame, k.frame, k.frameRandom, k.frameRange);
        } else if (i == 2 && p.i(k.type) == 1) {
            o.faceMotion = 1;
        }
    }
    // The turn: radii (start, middle, end; exe types 1 and 2 multiply by the start and the middle).
    Particle::Turn& t = o.turn;
    t.start = {value(kTurnRadiusXValue, kTurnRadiusXUseRandom, kTurnRadiusXRandomRange),
               value(kTurnRadiusYValue, kTurnRadiusYUseRandom, kTurnRadiusYRandomRange),
               value(kTurnRadiusZValue, kTurnRadiusZUseRandom, kTurnRadiusZRandomRange)};
    t.useMiddle = p.i(kTurnRadiusUseMiddle) != 0;
    if (!t.useMiddle) {
        t.middle = t.start;
    } else {
        t.middle = {value(kTurnRadiusMiddleXValue, kTurnRadiusMiddleXUseRandom, kTurnRadiusMiddleXRandomRange),
                    value(kTurnRadiusMiddleYValue, kTurnRadiusMiddleYUseRandom, kTurnRadiusMiddleYRandomRange),
                    value(kTurnRadiusMiddleZValue, kTurnRadiusMiddleZUseRandom, kTurnRadiusMiddleZRandomRange)};
        if (p.i(kTurnRadiusMiddleExeType) == 1) t.middle = mul(t.start, t.middle);
        t.timing = p.f(kTurnRadiusMiddleTiming);
        t.middleTempo = p.i(kTurnRadiusMiddleTempo);
    }
    t.useEnd = p.i(kTurnRadiusUseEnd) != 0;
    if (!t.useEnd) {
        t.end = t.middle;
    } else {
        t.end = {value(kTurnRadiusEndXValue, kTurnRadiusEndXUseRandom, kTurnRadiusEndXRandomRange),
                 value(kTurnRadiusEndYValue, kTurnRadiusEndYUseRandom, kTurnRadiusEndYRandomRange),
                 value(kTurnRadiusEndZValue, kTurnRadiusEndZUseRandom, kTurnRadiusEndZRandomRange)};
        int32_t type = p.i(kTurnRadiusEndExeType);
        if (type == 1) t.end = mul(t.start, t.end);
        else if (type == 2) t.end = mul(t.middle, t.end);
        t.endTempo = p.i(kTurnRadiusEndTempo);
    }
    t.time = 0.0f;
    t.frame = o.age;
    t.radius = t.start;
    // The first angles: absolute, evenly spread over the set plus up to `duration`, or random.
    t.angle = {};
    if (p.i(kTurnFirstAngleUseAbsoluteVector)) {
        t.angle = {p.f(kTurnFirstAngleX), p.f(kTurnFirstAngleY), p.f(kTurnFirstAngleZ)};
    } else if (p.i(kTurnFirstAngleUseUniform)) {
        float u = (360.0f / unsignedToFloat(static_cast<uint32_t>(particles_.size() > o.index ? particles_.size() : o.index + 1))) *
                      unsignedToFloat(o.index) +
                  p.f(kTurnFirstAngleUniformFirstValue);
        float d = p.f(kTurnFirstAngleUniformDuration);
        float jz = towards(r, d), jy = towards(r, d), jx = towards(r, d);
        t.angle = {u + jx, u + jy, u + jz};
    } else if (p.i(kTurnFirstAngleUseRandom)) {
        float a0 = r.nextFloat(0.0f, 360.0f), a1 = r.nextFloat(0.0f, 360.0f), a2 = r.nextFloat(0.0f, 360.0f);
        t.angle = {a2, a1, a0};
    }
    t.accum = t.angle;
    struct TurnAxis {
        size_t vel, velRandom, velRange, velReverse, acc, accRandom, accRange, accReverse;
        size_t res, resRandom, resRange, resAdd, useFrame, frame, frameRandom, frameRange;
    };
    static const TurnAxis kTurnAxes[3] = {
        {kTurnAngularVelocityXVelocityValue, kTurnAngularVelocityXVelocityUseRandom, kTurnAngularVelocityXVelocityRandomRange,
         kTurnAngularVelocityXVelocityIsReverseValue, kTurnAngularVelocityXAccelerationValue,
         kTurnAngularVelocityXAccelerationUseRandom, kTurnAngularVelocityXAccelerationRandomRange,
         kTurnAngularVelocityXAccelerationIsReverseValue, kTurnAngularVelocityXVelocityResistValue,
         kTurnAngularVelocityXVelocityResistUseRandom, kTurnAngularVelocityXVelocityResistRandomRange,
         kTurnAngularVelocityXVelocityResistAddResist, kTurnAngularVelocityXUseFrame, kTurnAngularVelocityXFrameTime,
         kTurnAngularVelocityXFrameUseRandom, kTurnAngularVelocityXFrameRandomRange},
        {kTurnAngularVelocityYVelocityValue, kTurnAngularVelocityYVelocityUseRandom, kTurnAngularVelocityYVelocityRandomRange,
         kTurnAngularVelocityYVelocityIsReverseValue, kTurnAngularVelocityYAccelerationValue,
         kTurnAngularVelocityYAccelerationUseRandom, kTurnAngularVelocityYAccelerationRandomRange,
         kTurnAngularVelocityYAccelerationIsReverseValue, kTurnAngularVelocityYVelocityResistValue,
         kTurnAngularVelocityYVelocityResistUseRandom, kTurnAngularVelocityYVelocityResistRandomRange,
         kTurnAngularVelocityYVelocityResistAddResist, kTurnAngularVelocityYUseFrame, kTurnAngularVelocityYFrameTime,
         kTurnAngularVelocityYFrameUseRandom, kTurnAngularVelocityYFrameRandomRange},
        {kTurnAngularVelocityZVelocityValue, kTurnAngularVelocityZVelocityUseRandom, kTurnAngularVelocityZVelocityRandomRange,
         kTurnAngularVelocityZVelocityIsReverseValue, kTurnAngularVelocityZAccelerationValue,
         kTurnAngularVelocityZAccelerationUseRandom, kTurnAngularVelocityZAccelerationRandomRange,
         kTurnAngularVelocityZAccelerationIsReverseValue, kTurnAngularVelocityZVelocityResistValue,
         kTurnAngularVelocityZVelocityResistUseRandom, kTurnAngularVelocityZVelocityResistRandomRange,
         kTurnAngularVelocityZVelocityResistAddResist, kTurnAngularVelocityZUseFrame, kTurnAngularVelocityZFrameTime,
         kTurnAngularVelocityZFrameUseRandom, kTurnAngularVelocityZFrameRandomRange},
    };
    auto reversed = [&](float v, size_t flag) { return p.i(flag) && coin(r) ? -v : v; };
    for (size_t i = 0; i < 3; ++i) {
        const TurnAxis& k = kTurnAxes[i];
        t.velocity[i] = reversed(value(k.vel, k.velRandom, k.velRange), k.velReverse);
        t.accel[i] = reversed(value(k.acc, k.accRandom, k.accRange), k.accReverse);
        float res = 1.0f - p.f(k.res);
        t.resist[i] = p.i(k.resRandom) ? between(r, res, p.f(k.resRange)) : res;
        t.addResist[i] = p.f(k.resAdd);
        t.times[i] = 0.0f;
        t.frameTime[i] = frameOf(k.useFrame, k.frame, k.frameRandom, k.frameRange);
    }
    // Size (texture pixels times the keys unless exe_type says otherwise; the fixed rate keeps the
    // start's aspect).
    Particle::Size& sz = o.size;
    sz.start = {p.f(kSpriteSizeStartX), p.f(kSpriteSizeStartY)};
    if (p.i(kSpriteSizeStartUseRandom)) {
        float y = between(r, sz.start[1], p.f(kSpriteSizeStartRandomRangeY));
        float x = between(r, sz.start[0], p.f(kSpriteSizeStartRandomRangeX));
        sz.start = {x, y};
        if (p.i(kSpriteSizeStartUseFixRate) && p.f(kSpriteSizeStartX) * p.f(kSpriteSizeStartY) != 0.0f) {
            sz.start[1] = sz.start[0] * (p.f(kSpriteSizeStartY) / p.f(kSpriteSizeStartX));
        }
    }
    const Texture* tex = o.texture < textures_.size() ? &textures_[o.texture] : nullptr;
    float tw = tex ? static_cast<float>(tex->width) : 0.0f, th = tex ? static_cast<float>(tex->height) : 0.0f;
    if (p.i(kSpriteSizeStartExeType) == 0) sz.start = {sz.start[0] * tw, th * sz.start[1]};
    auto sizeKey = [&](size_t x, size_t y, size_t useRandom, size_t rx, size_t ry, size_t fix, size_t type) {
        std::array<float, 2> v{p.f(x), p.f(y)};
        if (p.i(useRandom)) {
            float vy = between(r, v[1], p.f(ry));
            float vx = between(r, v[0], p.f(rx));
            v = {vx, vy};
            if (p.i(fix)) {
                if (p.i(type) == 0) {
                    if (p.f(x) * p.f(y) != 0.0f) v[1] = (p.f(y) / p.f(x)) * v[0];
                } else {
                    v[1] = v[0];
                }
            }
        }
        return v;
    };
    sz.useMiddle = p.i(kSpriteSizeUseMiddle) != 0;
    if (!sz.useMiddle) {
        sz.middle = sz.start;
        sz.timing = 1.0f;
    } else {
        auto m = sizeKey(kSpriteSizeMiddleX, kSpriteSizeMiddleY, kSpriteSizeMiddleUseRandom, kSpriteSizeMiddleRandomRangeX,
                         kSpriteSizeMiddleRandomRangeY, kSpriteSizeMiddleUseFixRate, kSpriteSizeMiddleExeType);
        sz.middle = p.i(kSpriteSizeMiddleExeType) == 0 ? m : std::array<float, 2>{sz.start[0] * m[0], sz.start[1] * m[1]};
        sz.timing = p.f(kSpriteSizeMiddleTiming);
        sz.middleTempo = p.i(kSpriteSizeMiddleTempo);
    }
    if (!p.i(kSpriteSizeUseEnd)) {
        sz.end = sz.useMiddle ? sz.middle : sz.start;
    } else {
        auto m = sizeKey(kSpriteSizeEndX, kSpriteSizeEndY, kSpriteSizeEndUseRandom, kSpriteSizeEndRandomRangeX,
                         kSpriteSizeEndRandomRangeY, kSpriteSizeEndUseFixRate, kSpriteSizeEndExeType);
        int32_t type = p.i(kSpriteSizeEndExeType);
        const auto& by = type == 1 ? sz.start : sz.middle;
        sz.end = type == 0 ? m : std::array<float, 2>{by[0] * m[0], by[1] * m[1]};
        sz.endTempo = p.i(kSpriteSizeEndTempo);
    }
    sz.time = 0.0f;
    sz.frame = frameOf(kSpriteSizeUseFrame, kSpriteSizeFrameTime, kSpriteSizeFrameUseRandom, kSpriteSizeFrameRandomRange);
    sz.now = sz.start;
    // The motion: velocity (the exe flips y where z's reverse is asked: velocity.y's flag read
    // twice), attraction, gravity, acceleration, resistance.
    Particle::Move& m = o.move;
    m.velocity[0] = reversed(value(kMoveVelocityXValue, kMoveVelocityXUseRandom, kMoveVelocityXRandomRange), kMoveVelocityXIsReverseValue);
    m.velocity[1] = reversed(value(kMoveVelocityYValue, kMoveVelocityYUseRandom, kMoveVelocityYRandomRange), kMoveVelocityYIsReverseValue);
    m.velocity[2] = value(kMoveVelocityZValue, kMoveVelocityZUseRandom, kMoveVelocityZRandomRange);
    m.velocity[1] = reversed(m.velocity[1], kMoveVelocityYIsReverseValue);
    m.attractStart = {reversed(value(kMoveAttractXValue, kMoveAttractXUseRandom, kMoveAttractXRandomRange), kMoveAttractXIsReverseValue),
                      reversed(value(kMoveAttractYValue, kMoveAttractYUseRandom, kMoveAttractYRandomRange), kMoveAttractYIsReverseValue),
                      reversed(value(kMoveAttractZValue, kMoveAttractZUseRandom, kMoveAttractZRandomRange), kMoveAttractZIsReverseValue)};
    m.useMiddle = p.i(kMoveAttractUseMiddle) != 0;
    if (!m.useMiddle) {
        m.attractMiddle = m.attractStart;
    } else {
        m.attractMiddle = {
            reversed(value(kMoveAttractMiddleXValue, kMoveAttractMiddleXUseRandom, kMoveAttractMiddleXRandomRange), kMoveAttractMiddleXIsReverseValue),
            reversed(value(kMoveAttractMiddleYValue, kMoveAttractMiddleYUseRandom, kMoveAttractMiddleYRandomRange), kMoveAttractMiddleYIsReverseValue),
            reversed(value(kMoveAttractMiddleZValue, kMoveAttractMiddleZUseRandom, kMoveAttractMiddleZRandomRange), kMoveAttractMiddleZIsReverseValue)};
        if (p.i(kMoveAttractMiddleExeType) == 1) m.attractMiddle = mul(m.attractStart, m.attractMiddle);
        m.timing = p.f(kMoveAttractMiddleTiming);
        m.middleTempo = p.i(kMoveAttractMiddleTempo);
    }
    if (!p.i(kMoveAttractUseEnd)) {
        m.attractEnd = m.attractMiddle;
    } else {
        m.attractEnd = {
            reversed(value(kMoveAttractEndXValue, kMoveAttractEndXUseRandom, kMoveAttractEndXRandomRange), kMoveAttractEndXIsReverseValue),
            reversed(value(kMoveAttractEndYValue, kMoveAttractEndYUseRandom, kMoveAttractEndYRandomRange), kMoveAttractEndYIsReverseValue),
            reversed(value(kMoveAttractEndZValue, kMoveAttractEndZUseRandom, kMoveAttractEndZRandomRange), kMoveAttractEndZIsReverseValue)};
        int32_t type = p.i(kMoveAttractEndExeType);
        if (type == 1) m.attractEnd = mul(m.attractStart, m.attractEnd);
        else if (type == 2) m.attractEnd = mul(m.attractMiddle, m.attractEnd);
        m.endTempo = p.i(kMoveAttractEndTempo);
    }
    m.power = reversed(value(kMoveAttractPowerValue, kMoveAttractPowerUseRandom, kMoveAttractPowerRandomRange), kMoveAttractPowerIsReverseValue);
    if (!p.i(kMoveUseGravity)) {
        m.gravityAccel = 0.0f;
    } else {
        m.gravityAccel = std::fabs(p.f(kMoveGravityAcceleration));
        m.gravity = m.gravityAccel;
        m.gravityFactor = p.f(kMoveGravityFactor);
    }
    m.accel = {reversed(value(kMoveAccelerationXValue, kMoveAccelerationXUseRandom, kMoveAccelerationXRandomRange), kMoveAccelerationXIsReverseValue),
               reversed(value(kMoveAccelerationYValue, kMoveAccelerationYUseRandom, kMoveAccelerationYRandomRange), kMoveAccelerationYIsReverseValue),
               reversed(value(kMoveAccelerationZValue, kMoveAccelerationZUseRandom, kMoveAccelerationZRandomRange), kMoveAccelerationZIsReverseValue)};
    auto resist = [&](size_t v, size_t useRandom, size_t range) {
        float x = 1.0f - p.f(v);
        return p.i(useRandom) ? between(r, x, p.f(range)) : x;
    };
    m.resist[0] = resist(kMoveRegistXValue, kMoveRegistXUseRandom, kMoveRegistXRandomRange);
    m.addResist[0] = p.f(kMoveRegistXAddResist);
    m.resist[1] = resist(kMoveRegistYValue, kMoveRegistYUseRandom, kMoveRegistYRandomRange);
    m.addResist[1] = p.f(kMoveRegistYAddResist);
    m.resist[2] = resist(kMoveRegistZValue, kMoveRegistZUseRandom, kMoveRegistZRandomRange);
    m.addResist[2] = p.f(kMoveRegistZAddResist);
    m.time = 0.0f;
    m.frame = o.age;
    // Colour (bytes to 0..1; only_alpha keeps the colour of the key before).
    constexpr float kByte = 0.00392156886f;  // 0x5eeddc
    auto colorKey = [&](size_t base, size_t useRandom, size_t range) {
        std::array<float, 4> c{static_cast<float>(p.i(base)) * kByte, static_cast<float>(p.i(base + 4)) * kByte,
                               static_cast<float>(p.i(base + 8)) * kByte, static_cast<float>(p.i(base + 12)) * kByte};
        if (p.i(useRandom)) {
            for (size_t i = 0; i < 4; ++i) c[i] = between(r, c[i], static_cast<float>(p.i(range + 4 * i)) * kByte);
        }
        return c;
    };
    Particle::Color& col = o.color;
    col.start = colorKey(kSpriteColorStartR, kSpriteColorStartUseRandom, kSpriteColorStartRandomRangeR);
    col.useMiddle = p.i(kSpriteColorUseMiddle) != 0;
    if (!col.useMiddle) {
        col.middle = col.start;
    } else {
        col.timing = p.f(kSpriteColorMiddleTiming);
        col.middleTempo = p.i(kSpriteColorMiddleTempo);
        col.middle = colorKey(kSpriteColorMiddleR, kSpriteColorMiddleUseRandom, kSpriteColorMiddleRandomRangeR);
        if (p.i(kSpriteColorMiddleOnlyAlpha)) col.middle = {col.start[0], col.start[1], col.start[2], col.middle[3]};
    }
    if (!p.i(kSpriteColorUseEnd)) {
        col.end = col.useMiddle ? col.middle : col.start;
    } else {
        col.endTempo = p.i(kSpriteColorEndTempo);
        col.end = colorKey(kSpriteColorEndR, kSpriteColorEndUseRandom, kSpriteColorEndRandomRangeR);
        if (p.i(kSpriteColorEndOnlyAlpha)) col.end = {col.middle[0], col.middle[1], col.middle[2], col.end[3]};
    }
    col.time = 0.0f;
    col.frame = frameOf(kSpriteColorUseFrame, kSpriteColorFrameTime, kSpriteColorFrameUseRandom, kSpriteColorFrameRandomRange);
    col.now = col.start;
    // The direction the attraction pulls along.
    m.direction = {};
    if (p.i(kMoveUseBasePoint)) {
        if (lengthSq(m.pos) > 0.0f) m.direction = normalized(m.pos);
    } else if (p.i(kMoveUseAbsoluteVector)) {
        V3 v{p.f(kMoveAbsoluteVectorXValue), p.f(kMoveAbsoluteVectorYValue), p.f(kMoveAbsoluteVectorZValue)};
        if (p.i(kMoveAbsoluteVectorXUseRandom)) v[0] = between(r, v[0], p.f(kMoveAbsoluteVectorXRandomRange));
        if (p.i(kMoveAbsoluteVectorYUseRandom)) v[1] = between(r, v[1], p.f(kMoveAbsoluteVectorYRandomRange));
        if (p.i(kMoveAbsoluteVectorZUseRandom)) v[2] = between(r, v[2], p.f(kMoveAbsoluteVectorZRandomRange));
        m.direction = normalized(sub(v, m.pos));
    } else if (p.i(kMoveUseCircle)) {
        float jitter = towards(r, p.f(kMoveCircleDiffuseDurationAngle));
        float idx = unsignedToFloat(o.index);
        float angle = (p.f(kMoveCircleDiffuseAngle) + jitter) * idx + p.f(kMoveCircleFirstDiffuseAngle);
        if (p.i(kMoveCircleDiffuseOrder) == 0) {
            if (p.i(kMoveCircleDiffuseType) == 0) {
                angle = angle + (360.0f / unsignedToFloat(static_cast<uint32_t>(std::max<size_t>(particles_.size(), 1)))) * idx;
            }
        } else if (p.i(kMoveCircleDiffuseOrder) == 1) {
            angle = r.nextFloat(0.0f, 360.0f) + angle;  // Guess: the range drawn from
        }
        V3 axis = normalized({p.f(kMoveCircleRotateX), p.f(kMoveCircleRotateY), p.f(kMoveCircleRotateZ)});
        if (lengthSq(axis) < 1.17549435e-38f) axis = {0.0f, 0.0f, 1.0f};
        float a = p.i(kMoveCircleDiffuseType) == 0 ? angle : r.nextFloat(0.0f, 360.0f);
        m.direction = transform({1.0f, 0.0f, 0.0f}, rotationAxis(a, axis));
    } else if (p.i(kMoveUseSphere)) {
        float lat = p.f(kMoveSphereLatitudeAngleValue);
        if (p.i(kMoveSphereLatitudeAngleUseRandom)) lat = between(r, lat, p.f(kMoveSphereLatitudeAngleRandomRange));
        lat += 90.0f;
        float lon = p.f(kMoveSphereLongitudeAngleValue);
        if (p.i(kMoveSphereLongitudeAngleUseRandom)) lon = between(r, lon, p.f(kMoveSphereLongitudeAngleRandomRange));
        if (p.i(kMoveSphereDiffuseType) == 0) {
            uint32_t n = static_cast<uint32_t>(particles_.size());
            if (o.index == 0) {
                lat = 0.0f, lon = 0.0f;
            } else if (o.index + 1 == n) {
                lat = 180.0f, lon = 0.0f;
            } else {
                uint32_t side = std::max<uint32_t>(1, static_cast<uint32_t>(std::sqrt(static_cast<float>(n))));  // Guess
                uint32_t row = (o.index - 1) / side;
                uint32_t col2 = o.index - row - 1;
                float step = 1.0f / unsignedToFloat(side);
                float edge = step * 180.0f;
                lat = (180.0f - (edge + edge)) * step * unsignedToFloat(row) + edge;
                lon = unsignedToFloat(col2) * step * 360.0f;
            }
        }
        m.direction = transform({0.0f, 1.0f, 0.0f}, rotationZXY(lat, lon, 0.0f));
    }
    // The texture's part: all of it.
    o.uv = {0.0f, 0.0f, tw, th};
    // Where it is drawn first.
    o.now = o.next = add(add(turnOffset(t), m.pos), o.base);
}

void ParticleSystem::reset() {
    visible_ = 1;
    begun_ = 0;
    longestLife_ = 0.0f;
    for (auto& part : particles_) {
        part->started = 0;
        initParticle(*part);
        if (longestLife_ >= 0.0f) longestLife_ = part->age >= 0.0f ? std::max(part->age, longestLife_) : -1.0f;
    }
}

void ParticleSystem::start() { begun_ = 1; }

void ParticleSystem::end() {
    visible_ = 0;
    begun_ = 0;
}

ParticleSystem::Saved ParticleSystem::saved() const {
    return {visible_, begun_, subActive_, fade, totalTime_, timeScale, seekDelay_};
}

void ParticleSystem::restore(const Saved& s) {
    reset();
    if (s.begun) {
        start();
        timeScale = s.timeScale;
        if (s.totalTime > 0.0f) advance(s.totalTime);
    }
    visible_ = s.visible;
    begun_ = s.begun;
    subActive_ = s.subActive;
    fade = s.fade;
    totalTime_ = s.totalTime;
    timeScale = s.timeScale;
    seekDelay_ = s.seekDelay;
}

void ParticleSystem::seek(float ms) {
    if (!begun_) seekDelay_ = -ms;
    if (ms > 0.0f) advance(ms);
}

void ParticleSystem::advance(float ms) {
    if (!visible_ || !begun_) {
        subActive_ = 0;
        return;
    }
    float dt = ms;
    float d = seekDelay_;
    if (d > kEpsilon) seekDelay_ = d - ms;
    d = seekDelay_;
    if (d > 0.0f) return;
    if (d < 0.0f) {
        seekDelay_ = 0.0f;
        dt = -d;
    }
    subActive_ = 0;
    int32_t any = 0;
    for (auto& part : particles_) {
        if (!part->alive) continue;
        stepParticle(*part, dt);
        any = any || part->alive;
    }
    visible_ = any;
    totalTime_ += dt;
}

int ParticleSystem::stepParticle(Particle& o, float dt) {
    if (!o.alive) return -1;
    if (!o.started) {
        o.delay = o.delay - dt;
        if (o.delay < 0.0f) {
            o.started = 1;
            dt = -o.delay;
            // An endless particle skips whole lives of the time past its start.
            if (o.age > 0.0f && o.repeats >= o.limit && o.limit <= 0) dt = std::fmod(dt, o.age);
            o.delay = 0.0f;
        }
    }
    if (!o.started) return 0;
    o.time = o.time + dt;
    o.now = add(add(turnOffset(o.turn), o.move.pos), o.base);
    if (dt > 0.0f && integrate(o, dt) < 0) return -1;
    if (o.age > 0.0f && o.age <= o.time) {
        ++o.repeats;
        if (o.limit < 1 || o.repeats < o.limit) {
            float rest = std::fmod(o.time, o.age);
            initParticle(o);
            stepParticle(o, rest);
        } else {
            o.alive = 0;
        }
    }
    return 0;
}

int ParticleSystem::integrate(Particle& o, float dt) {
    float remaining = dt;
    // Each step stops at the next end of a rotation's frame (and, with collisions, at 1/6 s).
    for (int guard = 0; guard < 100000; ++guard) {
        float s = remaining;
        for (const auto& sp : o.spin) {
            if (sp.on && sp.frame > 0.0f) s = std::min(s, sp.frame - sp.time);
        }
        for (size_t i = 0; i < 3; ++i) {
            float d = o.turn.frameTime[i] > 0.0f ? o.turn.frameTime[i] - o.turn.times[i] : s;
            s = std::min(s, d);
        }
        if (!(s > 0.0f)) s = remaining;
        for (auto& sp : o.spin) stepSpin(sp, s);
        stepTurn(o.turn, s);
        // Size and colour: their keys over their frame, then the system's colour and fade.
        {
            auto& z = o.size;
            z.time = s + z.time;
            if (z.frame <= 0.0f) z.now = z.start;
            else z.now = keys<2>(z.start, z.middle, z.end, z.useMiddle, z.timing, z.middleTempo, z.endTempo, z.time / z.frame);
            if (z.frame > 0.0f && z.frame <= z.time) z.time = 0.0f;
        }
        {
            auto& c = o.color;
            c.time = s + c.time;
            if (c.frame <= 0.0f) c.now = c.start;
            else c.now = keys<4>(c.start, c.middle, c.end, c.useMiddle, c.timing, c.middleTempo, c.endTempo, c.time / c.frame);
            c.now = {c.now[0] * color[0], c.now[1] * color[1], c.now[2] * color[2], c.now[3] * fade};
            if (c.frame > 0.0f && c.frame <= c.time) c.time = 0.0f;
        }
        stepMove(o.move, s);
        remaining = remaining - s;
        if (remaining <= kEpsilon) break;
    }
    o.next = add(add(turnOffset(o.turn), o.move.pos), o.base);
    if (o.faceMotion) {
        V3 d = sub(o.next, o.now);
        if (lengthSq(d) > kEpsilon) {
            d = normalized(d);
            float c = std::clamp(d[0], -1.0f, 1.0f);  // dot((1, 0, 0), d)
            o.spin[2].angle = std::acos(c) * kRadiansToDegrees - 90.0f;
            if (cross({1.0f, 0.0f, 0.0f}, d)[2] < 0.0f) o.spin[2].angle = 180.0f - o.spin[2].angle;
        }
    }
    return 0;
}

void ParticleSystem::setCamera(std::shared_ptr<const CameraView> camera, float depth, float fov, int screenW, int screenH) {
    camera_ = std::move(camera);
    positionScale_ = {1.0f, 1.0f, 1.0f};
    particleScale_ = {1.0f, 1.0f, 1.0f};
    // The default view's height at its distance: 2 x 512 tan(22.5 degrees).
    float tanDefault = std::tan(0.392699093f);
    float defaultHeight = tanDefault * 512.0f + tanDefault * 512.0f;
    if (camera_ && camera_->made && depth > 0.0f && !camera_->ortho) {
        float f = fov > 0.0f ? fov : camera_->fov;
        float t = std::tan(f * 0.00872664619f);
        float h = t * depth + t * depth;
        float s = h / defaultHeight;
        particleScale_ = {s, s, s};
        float ps = h / static_cast<float>(screenH);
        positionScale_ = {ps, ps, ps};
        return;
    }
    (void)screenW;
    float ps = defaultHeight / static_cast<float>(screenH);
    positionScale_ = {ps, ps, ps};
}

Mat4 ParticleSystem::world() const {
    Mat4 w = local;
    w.at(3, 0) *= positionScale_[0];
    w.at(3, 1) *= positionScale_[1];
    w.at(3, 2) *= positionScale_[2];
    if (!parent) return w;
    w = w * parent->world;
    // The parent's position from its units into this system's: (scale / parent's scale) - 1 of it.
    const V3 at{parent->world.at(3, 0), parent->world.at(3, 1), parent->world.at(3, 2)};
    for (size_t k = 0; k < 3; ++k) {
        const float ratio = positionScale_[k] / parent->scale[k];
        w.at(3, static_cast<int>(k)) += at[k] * ratio - at[k];
    }
    return w;
}

std::shared_ptr<ParticleBatch> ParticleSystem::build(int targetW, int targetH) {
    if (!visible_ || !(fade > 0.0f) || textures_.empty()) return nullptr;
    // The system's world (0x478930).
    const Mat4 world = this->world();
    // The view and projection (0x478e30): the camera's, or the eye at z = -512 looking at the
    // origin, 45 degrees, near 10, far 10^7, the target's aspect.
    float aspect = static_cast<float>(targetW) / static_cast<float>(targetH);
    Mat4 view;
    Mat4 proj;
    if (camera_ && camera_->made) {
        view = Mat4::lookAtLH(camera_->eye, camera_->target, camera_->up);
        if (camera_->ortho) proj = Mat4::orthographicLH(camera_->width, camera_->height, camera_->nearZ, camera_->farZ);
        else proj = Mat4::perspectiveFovLH(camera_->fov * kDegrees, aspect, camera_->nearZ, camera_->farZ);
    } else {
        view = Mat4::lookAtLH({0.0f, 0.0f, -512.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        proj = Mat4::perspectiveFovLH(0.785398185f, aspect, 10.0f, 1.0e7f);
    }
    Mat4 wvp = world * view * proj;
    // A billboard turns the particles to face the eye (the view's rotation and the world's, undone).
    Mat4 board;
    if (billboard_) {
        Mat4 v = view;
        v.at(3, 0) = v.at(3, 1) = v.at(3, 2) = 0.0f;
        Mat4 w = world;
        float sx = std::sqrt(w.at(0, 0) * w.at(0, 0) + w.at(0, 1) * w.at(0, 1) + w.at(0, 2) * w.at(0, 2));
        float sy = std::sqrt(w.at(1, 0) * w.at(1, 0) + w.at(1, 1) * w.at(1, 1) + w.at(1, 2) * w.at(1, 2));
        float sz = std::sqrt(w.at(2, 0) * w.at(2, 0) + w.at(2, 1) * w.at(2, 1) + w.at(2, 2) * w.at(2, 2));
        w = inverse(Mat4::scaling(sx, sy, sz)) * w;
        w.at(3, 0) = w.at(3, 1) = w.at(3, 2) = 0.0f;
        board = inverse(v) * inverse(w);
    }
    auto batch = std::make_shared<ParticleBatch>();
    const Texture& tex = textures_.front();
    batch->texture = tex.path;
    batch->textureWidth = tex.width;
    batch->textureHeight = tex.height;
    batch->blendMode = file_.i(kSpriteBlendMode);
    batch->colorMode = file_.i(kSpriteColorCulcMode);
    float iw = tex.width > 0 ? 1.0f / static_cast<float>(tex.width) : 0.0f;
    float ih = tex.height > 0 ? 1.0f / static_cast<float>(tex.height) : 0.0f;
    for (auto& up : particles_) {
        Particle& o = *up;
        // 0x4b6630: its rotation (degrees) and the billboard; placed at its position.
        Mat4 m;
        if (o.spin[0].angle != 0.0f || o.spin[1].angle != 0.0f || o.spin[2].angle != 0.0f) {
            m = rotationZXY(o.spin[0].angle, o.spin[1].angle, o.spin[2].angle);
        }
        m *= board;
        if (o.first) {
            // Where the emitter is in the system's space when the particle first draws: its world
            // position through the system's inverse, rescaled by the system's scale over the
            // emitter's (its parent's). Hung from the system (the default) that is the origin
            // (Fitted: exactly 0 here; the exe's inverse leaves rounding).
            o.emitterOffset = {};
            if (emitterParent) {
                const V3 at{emitterParent->world.at(3, 0), emitterParent->world.at(3, 1), emitterParent->world.at(3, 2)};
                const V3 local = transformCoord(at, inverse(world));
                for (size_t k = 0; k < 3; ++k) o.emitterOffset[k] = local[k] * (positionScale_[k] / emitterParent->scale[k]);
            }
            o.first = 0;
        }
        V3 at = add(mul(o.next, particleScale_), o.emitterOffset);
        m.at(3, 0) = at[0], m.at(3, 1) = at[1], m.at(3, 2) = at[2];
        // 0x4b6d30: drawn while alive and started.
        if (!o.alive || !o.started) continue;
        float w = o.size.now[0] * particleScale_[0], h = o.size.now[1] * particleScale_[1];
        std::array<float, 4> c{};
        for (size_t i = 0; i < 4; ++i) {
            // Bytes (truncated): the vertex colour is RGBA8.
            float v = std::clamp(o.color.now[i], 0.0f, 1.0f);
            c[i] = static_cast<float>(static_cast<uint8_t>(static_cast<int32_t>(v * 255.0f))) / 255.0f;
        }
        // Mware's CParticle3D (0x1000dd10): the corners (-w/2, h/2), (w/2, h/2), (-w/2, -h/2),
        // (w/2, -h/2) through the particle's matrix; the texture's part inset by half a texel.
        const float corners[4][2] = {{-0.5f, 0.5f}, {0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
        float u0 = (o.uv[0] + 0.5f) * iw, v0 = (o.uv[1] + 0.5f) * ih;
        float u1 = (o.uv[2] + o.uv[0] - 0.5f) * iw, v1 = (o.uv[3] + o.uv[1] - 0.5f) * ih;
        const float uvs[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
        for (size_t k = 0; k < 4; ++k) {
            V3 pt = transformCoord({w * corners[k][0], h * corners[k][1], 0.0f}, m);
            auto clip = wvp.apply(pt[0], pt[1], pt[2]);
            ParticleVertex vtx;
            vtx.x = clip[0], vtx.y = clip[1], vtx.z = clip[2], vtx.w = clip[3];
            vtx.u = uvs[k][0], vtx.v = uvs[k][1];
            vtx.color = c;
            batch->vertices.push_back(vtx);
        }
    }
    return batch;
}

}  // namespace nyan::diesel
