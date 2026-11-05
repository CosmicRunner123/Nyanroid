// Mware's value types as the exes bind them for the scripts with SqPlus (muramasa_en.exe 0x4902b0,
// after the globals): Vector2D (x, y), Vector3D (x, y, z), ColorF (r, g, b, a) and Rect2DF (x, y, w,
// h). Each is a class whose instances hold the C++ value; SqPlus reaches the members through the
// class's _get / _set (floats: a number set is converted, so 1 reads back as 1.0), and a new instance
// has every member 0 (Mware's default constructors). Natives take them by value (SqPlus's
// ClassTypeCopyImpl), refusing anything else.

#include <array>
#include <cmath>
#include <cstring>

#include "natives.hpp"

namespace nyan::diesel {

namespace {

struct MathClass {
    const char* name;
    std::array<const char*, 4> members;
    int count;
};

constexpr MathClass kMathClasses[] = {
    {"Vector2D", {"x", "y", nullptr, nullptr}, 2},
    {"Vector3D", {"x", "y", "z", nullptr}, 3},
    {"ColorF", {"r", "g", "b", "a"}, 4},
    {"Rect2DF", {"x", "y", "w", "h"}, 4},
};

struct MathValue final : NativeObject {
    const MathClass* cls = nullptr;
    std::array<float, 4> v{};
};

SQInteger releaseMath(SQUserPointer p, SQInteger) {
    delete static_cast<NativeObject*>(p);
    return 1;
}

MathValue* mathSelf(HSQUIRRELVM v) {
    SQUserPointer p = nullptr;
    if (SQ_FAILED(sq_getinstanceup(v, 1, &p, nullptr)) || !p) return nullptr;
    return dynamic_cast<MathValue*>(static_cast<NativeObject*>(p));
}

int memberIndex(const MathValue& m, HSQUIRRELVM v, SQInteger idx) {
    const SQChar* name = nullptr;
    if (SQ_FAILED(sq_getstring(v, idx, &name)) || !name) return -1;
    for (int i = 0; i < m.cls->count; ++i) {
        if (std::strcmp(m.cls->members[static_cast<size_t>(i)], name) == 0) return i;
    }
    return -1;
}

// constructor(): every member 0. The class is the closure's free variable.
SQInteger mathConstructor(HSQUIRRELVM v) {
    SQUserPointer up = nullptr;
    sq_getuserpointer(v, sq_gettop(v), &up);
    auto* m = new MathValue();
    m->cls = static_cast<const MathClass*>(up);
    sq_setinstanceup(v, 1, m);
    sq_setreleasehook(v, 1, releaseMath);
    return 0;
}

// _get(name) / _set(name, value): SqPlus's variable access (an unknown name is an error).
SQInteger mathGet(HSQUIRRELVM v) {
    MathValue* m = mathSelf(v);
    int i = m ? memberIndex(*m, v, 2) : -1;
    if (i < 0) return sq_throwerror(v, "the index does not exist");
    sq_pushfloat(v, m->v[static_cast<size_t>(i)]);
    return 1;
}
SQInteger mathSet(HSQUIRRELVM v) {
    MathValue* m = mathSelf(v);
    int i = m ? memberIndex(*m, v, 2) : -1;
    if (i < 0) return sq_throwerror(v, "the index does not exist");
    SQFloat f = 0;
    if (SQ_FAILED(sq_getfloat(v, 3, &f))) return sq_throwerror(v, "a member takes a number");
    m->v[static_cast<size_t>(i)] = static_cast<float>(f);
    return 0;
}
// _cloned(original): the copy SqPlus's copy constructor makes.
SQInteger mathCloned(HSQUIRRELVM v) {
    SQUserPointer p = nullptr;
    if (SQ_FAILED(sq_getinstanceup(v, 2, &p, nullptr)) || !p) return 0;
    auto* from = dynamic_cast<MathValue*>(static_cast<NativeObject*>(p));
    if (!from) return 0;
    auto* m = new MathValue(*from);
    sq_setinstanceup(v, 1, m);
    sq_setreleasehook(v, 1, releaseMath);
    return 0;
}

void defineMethod(HSQUIRRELVM v, const char* name, SQFUNCTION fn) {
    sq_pushstring(v, name, -1);
    sq_newclosure(v, fn, 0);
    sq_setnativeclosurename(v, -1, name);
    sq_createslot(v, -3);
}

}  // namespace

void registerMathClasses(HSQUIRRELVM v, const ExeInfo& exe) {
    sq_pushroottable(v);
    for (const MathClass& c : kMathClasses) {
        if (!exe.holdsName(c.name)) continue;
        sq_pushstring(v, c.name, -1);
        sq_newclass(v, SQFalse);
        sq_settypetag(v, -1, const_cast<MathClass*>(&c));
        sq_pushstring(v, "constructor", -1);
        sq_pushuserpointer(v, const_cast<MathClass*>(&c));
        sq_newclosure(v, mathConstructor, 1);
        sq_createslot(v, -3);
        defineMethod(v, "_get", mathGet);
        defineMethod(v, "_set", mathSet);
        defineMethod(v, "_cloned", mathCloned);
        sq_createslot(v, -3);  // root[name] = class
    }
    sq_pop(v, 1);
}

std::optional<std::array<float, 4>> argMath(HSQUIRRELVM v, SQInteger idx, const char* className) {
    SQUserPointer p = nullptr;
    if (SQ_FAILED(sq_getinstanceup(v, idx, &p, nullptr)) || !p) return std::nullopt;
    auto* m = dynamic_cast<MathValue*>(static_cast<NativeObject*>(p));
    if (!m || std::strcmp(m->cls->name, className) != 0) return std::nullopt;
    return m->v;
}


// --- SQMATH_V3_* / SQMATH_Q_* (muramasa_en.exe 0x550ec0 .. 0x551f50) ---------------------------------
// Each reads the host's SQMATH variables (V3_1 +0xe4, V3_2 +0xf0, Q_1 +0xfc, Q_2 +0x10c) and writes
// SQMATH_RET_1.. (+0x11c) through Mware's Vector3D / Quaternion / Matrix4x4, DirectXMath's SSE2 code
// underneath; sums follow its operation order. Angles are degrees (x 0x1019f558). Sines and cosines
// are the C library's (Fitted: DirectXMath's polynomial agrees to float rounding).
namespace {

using V3f = std::array<float, 3>;
using Qf = std::array<float, 4>;
constexpr float kDegToRad = 0.0174532924f;

float* mv(HSQUIRRELVM v) { return host(v).mathVars; }
V3f v3(const float* m, int at) { return {m[at], m[at + 1], m[at + 2]}; }
Qf q4(const float* m, int at) { return {m[at], m[at + 1], m[at + 2], m[at + 3]}; }
void retV3(HSQUIRRELVM v, const V3f& r) { host(v).setResult(r[0], r[1], r[2], host(v).mathVars[ScriptHost::Impl::kMathRet + 3]); }
void retQ(HSQUIRRELVM v, const Qf& r) { host(v).setResult(r[0], r[1], r[2], r[3]); }
void retF(HSQUIRRELVM v, float f) { host(v).mathVars[ScriptHost::Impl::kMathRet] = f; }
constexpr int kV1 = 0, kV2 = 3, kQ1 = 6, kQ2 = 10;

// XMVector3Dot (SSE2): (x + y) + z of the products.
float dot3(const V3f& a, const V3f& b) { return (a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]; }
// XMVector4Dot (SSE2): (y + w) + (x + z) of the products.
float dot4(const Qf& a, const Qf& b) {
    return (a[1] * b[1] + a[3] * b[3]) + (a[0] * b[0] + a[2] * b[2]);
}
// XMVector3Normalize: by the length ((x + y) + z of the squares); 0 for a zero length, NaN for an
// infinite one.
V3f normalize3(const V3f& a) {
    float lenSq = (a[0] * a[0] + a[1] * a[1]) + a[2] * a[2];
    float len = std::sqrt(lenSq);
    if (std::isinf(lenSq)) return {NAN, NAN, NAN};
    if (len == 0.0f) return {0.0f, 0.0f, 0.0f};
    return {a[0] / len, a[1] / len, a[2] / len};
}
// XMVector4Normalize: the same with (x + z) + (y + w).
Qf normalize4(const Qf& a) {
    float lenSq = (a[0] * a[0] + a[2] * a[2]) + (a[1] * a[1] + a[3] * a[3]);
    float len = std::sqrt(lenSq);
    if (std::isinf(lenSq)) return {NAN, NAN, NAN, NAN};
    if (len == 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
    return {a[0] / len, a[1] / len, a[2] / len, a[3] / len};
}
// XMVector3Cross: (a.yzx * b.zxy) - (a.zxy * b.yzx).
V3f cross3(const V3f& a, const V3f& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// Row vector times a 4x4 (row-major m[r][c]): with w = 1, divided by w (TransformCoord) or not.
V3f transform(const V3f& p, const float m[4][4], bool coord) {
    float r[4];
    for (int c = 0; c < 4; ++c) r[c] = ((p[0] * m[0][c] + p[1] * m[1][c]) + p[2] * m[2][c]) + m[3][c];
    if (!coord) return {r[0], r[1], r[2]};
    return {r[0] / r[3], r[1] / r[3], r[2] / r[3]};
}
void identity(float m[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = r == c ? 1.0f : 0.0f;
}
void multiply(const float a[4][4], const float b[4][4], float out[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = ((a[r][0] * b[0][c] + a[r][1] * b[1][c]) + a[r][2] * b[2][c]) + a[r][3] * b[3][c];
}
// XMMatrixRotationX / Y / Z (radians), the left-handed rotations Mware's RotationZXY combines.
void rotX(float a, float m[4][4]) {
    identity(m);
    float s = std::sin(a), c = std::cos(a);
    m[1][1] = c, m[1][2] = s, m[2][1] = -s, m[2][2] = c;
}
void rotY(float a, float m[4][4]) {
    identity(m);
    float s = std::sin(a), c = std::cos(a);
    m[0][0] = c, m[0][2] = -s, m[2][0] = s, m[2][2] = c;
}
void rotZ(float a, float m[4][4]) {
    identity(m);
    float s = std::sin(a), c = std::cos(a);
    m[0][0] = c, m[0][1] = s, m[1][0] = -s, m[1][1] = c;
}
// Matrix4x4::RotationZXY (0x1003a020): degrees; Z, then X, then Y (identity for all zero).
void rotationZXY(float x, float y, float z, float m[4][4]) {
    float a[4][4], b[4][4], t[4][4];
    rotZ(z * kDegToRad, a), rotX(x * kDegToRad, b), multiply(a, b, t);
    rotY(y * kDegToRad, a), multiply(t, a, m);
}
// XMMatrixRotationQuaternion.
void fromQuaternion(const Qf& q, float m[4][4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    identity(m);
    m[0][0] = 1.0f - 2.0f * (y * y + z * z), m[0][1] = 2.0f * (x * y + z * w), m[0][2] = 2.0f * (x * z - y * w);
    m[1][0] = 2.0f * (x * y - z * w), m[1][1] = 1.0f - 2.0f * (x * x + z * z), m[1][2] = 2.0f * (y * z + x * w);
    m[2][0] = 2.0f * (x * z + y * w), m[2][1] = 2.0f * (y * z - x * w), m[2][2] = 1.0f - 2.0f * (x * x + y * y);
}
// XMMatrixRotationAxis (Matrix4x4::RotationAxis 0x1003a140: degrees): about the normalised axis.
void rotationAxis(const V3f& axisIn, float degrees, float m[4][4]) {
    V3f n = normalize3(axisIn);
    float a = degrees * kDegToRad, s = std::sin(a), c = std::cos(a), t = 1.0f - c;
    identity(m);
    m[0][0] = t * n[0] * n[0] + c, m[0][1] = t * n[0] * n[1] + s * n[2], m[0][2] = t * n[0] * n[2] - s * n[1];
    m[1][0] = t * n[0] * n[1] - s * n[2], m[1][1] = t * n[1] * n[1] + c, m[1][2] = t * n[1] * n[2] + s * n[0];
    m[2][0] = t * n[0] * n[2] + s * n[1], m[2][1] = t * n[1] * n[2] - s * n[0], m[2][2] = t * n[2] * n[2] + c;
}
// XMQuaternionMultiply(q1, q2) (Mware's Mul 0x1003cce0): q2 then q1, as
// (w2 q1 + x2 (w1, z1, y1, x1) (+ - + -)) + (y2 (z1, w1, x1, y1) (+ + - -) + z2 (y1, x1, w1, z1) (- + + -)).
Qf quatMul(const Qf& a, const Qf& b) {
    float x1 = a[0], y1 = a[1], z1 = a[2], w1 = a[3], x2 = b[0], y2 = b[1], z2 = b[2], w2 = b[3];
    return {(w2 * x1 + x2 * w1) + (y2 * z1 + -(z2 * y1)), (w2 * y1 + -(x2 * z1)) + (y2 * w1 + z2 * x1),
            (w2 * z1 + x2 * y1) + (-(y2 * x1) + z2 * w1), (w2 * w1 + -(x2 * x1)) + (-(y2 * y1) + -(z2 * z1))};
}

SQInteger v3Add(HSQUIRRELVM v) {  // 0x550ec0
    float* m = mv(v);
    V3f a = v3(m, kV1), b = v3(m, kV2);
    retV3(v, {a[0] + b[0], a[1] + b[1], a[2] + b[2]});
    return 0;
}
SQInteger v3Sub(HSQUIRRELVM v) {  // 0x550fc0
    float* m = mv(v);
    V3f a = v3(m, kV1), b = v3(m, kV2);
    retV3(v, {a[0] - b[0], a[1] - b[1], a[2] - b[2]});
    return 0;
}
SQInteger v3Dot(HSQUIRRELVM v) {  // 0x5510c0
    float* m = mv(v);
    retF(v, dot3(v3(m, kV1), v3(m, kV2)));
    return 0;
}
SQInteger v3Cross(HSQUIRRELVM v) {  // 0x551190
    float* m = mv(v);
    retV3(v, cross3(v3(m, kV1), v3(m, kV2)));
    return 0;
}
SQInteger v3MulS(HSQUIRRELVM v) {  // 0x5512a0: V3_1 times V3_2's x
    float* m = mv(v);
    V3f a = v3(m, kV1);
    float s = m[kV2];
    retV3(v, {a[0] * s, a[1] * s, a[2] * s});
    return 0;
}
SQInteger v3DivS(HSQUIRRELVM v) {  // 0x551370: times 1 / V3_2's x (Vector3D::operator/ 0x10002070)
    float* m = mv(v);
    V3f a = v3(m, kV1);
    float r = 1.0f / m[kV2];
    retV3(v, {r * a[0], a[1] * r, a[2] * r});
    return 0;
}
SQInteger v3Norm(HSQUIRRELVM v) {  // 0x551440
    retV3(v, normalize3(v3(mv(v), kV1)));
    return 0;
}
SQInteger v3LenSq(HSQUIRRELVM v) {  // 0x551500: x x + y y + z z (the scalar load path)
    V3f a = v3(mv(v), kV1);
    retF(v, a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return 0;
}
SQInteger v3Len(HSQUIRRELVM v) {  // 0x551590: XMVector3Length, (x + z) + y of the squares
    V3f a = v3(mv(v), kV1);
    retF(v, std::sqrt((a[0] * a[0] + a[2] * a[2]) + a[1] * a[1]));
    return 0;
}
SQInteger v3RotZXY(HSQUIRRELVM v) {  // 0x551620: V3_1 by RotationZXY(V3_2), TransformCoord
    float* m = mv(v);
    float r[4][4];
    rotationZXY(m[kV2], m[kV2 + 1], m[kV2 + 2], r);
    retV3(v, transform(v3(m, kV1), r, true));
    return 0;
}
SQInteger v3RotAxis(HSQUIRRELVM v) {  // 0x551720: V3_1 about V3_2 by Q_1's x degrees (Transform)
    float* m = mv(v);
    float r[4][4];
    rotationAxis(v3(m, kV2), m[kQ1], r);
    retV3(v, transform(v3(m, kV1), r, false));
    return 0;
}
SQInteger v3RotQ(HSQUIRRELVM v) {  // 0x551830: V3_1 by Q_1's matrix, TransformCoord
    float* m = mv(v);
    float r[4][4];
    fromQuaternion(q4(m, kQ1), r);
    retV3(v, transform(v3(m, kV1), r, true));
    return 0;
}
SQInteger qAdd(HSQUIRRELVM v) {  // 0x551930
    float* m = mv(v);
    Qf a = q4(m, kQ1), b = q4(m, kQ2);
    retQ(v, {a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]});
    return 0;
}
SQInteger qSub(HSQUIRRELVM v) {  // 0x551a10
    float* m = mv(v);
    Qf a = q4(m, kQ1), b = q4(m, kQ2);
    retQ(v, {a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]});
    return 0;
}
SQInteger qMul(HSQUIRRELVM v) {  // 0x551af0
    float* m = mv(v);
    retQ(v, quatMul(q4(m, kQ1), q4(m, kQ2)));
    return 0;
}
SQInteger qDot(HSQUIRRELVM v) {  // 0x551bd0
    float* m = mv(v);
    retF(v, dot4(q4(m, kQ1), q4(m, kQ2)));
    return 0;
}
SQInteger qNorm(HSQUIRRELVM v) {  // 0x551c70
    retQ(v, normalize4(q4(mv(v), kQ1)));
    return 0;
}
SQInteger qLenSq(HSQUIRRELVM v) {  // 0x551d20
    Qf a = q4(mv(v), kQ1);
    retF(v, dot4(a, a));
    return 0;
}
SQInteger qLen(HSQUIRRELVM v) {  // 0x551d90
    Qf a = q4(mv(v), kQ1);
    retF(v, std::sqrt(dot4(a, a)));
    return 0;
}
SQInteger qConj(HSQUIRRELVM v) {  // 0x551e00
    Qf a = q4(mv(v), kQ1);
    retQ(v, {-a[0], -a[1], -a[2], a[3]});
    return 0;
}
// 0x551eb0: Quaternion::RotationXYZ(V3_1) (degrees: XMQuaternionRotationRollPitchYaw, x pitch,
// y yaw, z roll): ((P1 sign) Y1) R1 + (P0 Y0) R0 of the half angles' sines and cosines.
SQInteger qRotXYZ(HSQUIRRELVM v) {
    float* m = mv(v);
    float hp = m[kV1] * kDegToRad * 0.5f, hy = m[kV1 + 1] * kDegToRad * 0.5f, hr = m[kV1 + 2] * kDegToRad * 0.5f;
    float sp = std::sin(hp), cp = std::cos(hp), sy = std::sin(hy), cy = std::cos(hy), sr = std::sin(hr), cr = std::cos(hr);
    Qf p0{sp, cp, cp, cp}, y0{cy, sy, cy, cy}, r0{cr, cr, sr, cr};
    Qf p1{cp, -sp, -sp, sp}, y1{sy, cy, sy, sy}, r1{sr, sr, cr, sr};
    Qf out;
    for (size_t i = 0; i < 4; ++i) out[i] = (p1[i] * y1[i]) * r1[i] + (p0[i] * y0[i]) * r0[i];
    retQ(v, out);
    return 0;
}
// 0x551f50: Quaternion::RotationAxis(V3_1, V3_2's x degrees): XMQuaternionRotationAxis, the
// normalised axis times sin(a / 2), w cos(a / 2).
SQInteger qRotAxis(HSQUIRRELVM v) {
    float* m = mv(v);
    V3f n = normalize3(v3(m, kV1));
    float h = m[kV2] * kDegToRad * 0.5f, s = std::sin(h), c = std::cos(h);
    retQ(v, {n[0] * s, n[1] * s, n[2] * s, c});
    return 0;
}

}  // namespace

std::vector<NativeImpl> mathGlobals() {
    return {{"SQMATH_V3_Add", v3Add},       {"SQMATH_V3_Sub", v3Sub},     {"SQMATH_V3_Dot", v3Dot},
            {"SQMATH_V3_Cross", v3Cross},   {"SQMATH_V3_MulS", v3MulS},   {"SQMATH_V3_DivS", v3DivS},
            {"SQMATH_V3_Norm", v3Norm},     {"SQMATH_V3_LenSq", v3LenSq}, {"SQMATH_V3_Len", v3Len},
            {"SQMATH_V3_RotZXY", v3RotZXY}, {"SQMATH_V3_RotAxis", v3RotAxis}, {"SQMATH_V3_RotQ", v3RotQ},
            {"SQMATH_Q_Add", qAdd},         {"SQMATH_Q_Sub", qSub},       {"SQMATH_Q_Mul", qMul},
            {"SQMATH_Q_Dot", qDot},         {"SQMATH_Q_Norm", qNorm},     {"SQMATH_Q_LenSq", qLenSq},
            {"SQMATH_Q_Len", qLen},         {"SQMATH_Q_Conj", qConj},     {"SQMATH_Q_RotXYZ", qRotXYZ},
            {"SQMATH_Q_RotAxis", qRotAxis}};
}

}  // namespace nyan::diesel
