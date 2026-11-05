// SQScene: the 3D scene's settings (Diesel::Squirrel::SQScene; natives muramasa_en.exe 0x53d600 ff.,
// constructor 0x53b330, update 0x53d1c0). The framework makes one (named "Scene")
// and its saves read every tween's timer.
//
// Nyanroid renders no 3D scene; this keeps the scene's state as the exe does: the post effects' and
// the light's tweens (all 0 at first, power and time scale 1), their values, timers and time scales,
// the effects' switches (the app's system state) and what the renderer alone reads.
//
// The renderer's parameters differ by exe. muramasa_en.exe and phantom.exe drop most of them (the
// natives are empty functions shared with others: 0x403d00, 0x439230, 0x439290, 0x140044030);
// totono_en.exe hands them to Mware's scene manager (+0x4b8 = app+0x30), whose world / screen
// transforms answer through SQMATH_RET.

#include <cfloat>
#include <climits>
#include <cmath>

#include "natives.hpp"
#include "nyan/diesel/sprite_geometry.hpp"

namespace nyan::diesel {

namespace {

using V3 = std::array<float, 3>;

// --- Mware's vector math under setLightPosition (DirectXMath in Mware.dll; the same code in each
// game's Mware) ---

// cvttss2si: NaN and values out of range give INT_MIN.
int32_t truncateToInt(float f) {
    if (!(f > -2147483904.0f && f < 2147483648.0f)) return INT_MIN;
    return static_cast<int32_t>(f);
}

// XMScalarSinCos (Mware.dll 0x1003c3d0): the angle brought into [-pi/2, pi/2], then an 11-degree
// polynomial for the sine and a 10-degree one for the cosine.
void scalarSinCos(float value, float& s, float& c) {
    float q = value * 0.159154937f;
    q = value >= 0.0f ? q + 0.5f : q - 0.5f;
    float y = value - static_cast<float>(truncateToInt(q)) * 6.28318548f;
    float sign = 1.0f;
    if (y > 1.57079637f) {
        y = 3.14159274f - y;
        sign = -1.0f;
    } else if (-1.57079637f > y) {
        y = -3.14159274f - y;
        sign = -1.0f;
    }
    float y2 = y * y;
    float sp = 2.75255616e-06f - y2 * 2.38898590e-08f;
    sp = sp * y2 - 0.000198408743f;
    sp = sp * y2 + 0.00833333097f;
    sp = sp * y2 - 0.166666672f;
    s = (sp * y2 + 1.0f) * y;
    float cp = 2.47604949e-05f - y2 * 2.60516146e-07f;
    cp = cp * y2 - 0.00138883782f;
    cp = cp * y2 + 0.0416666381f;
    cp = cp * y2 - 0.5f;
    c = (cp * y2 + 1.0f) * sign;
}

// XMVector3Normalize (Vector3D::Normalize, 0x1003e1e0): zero stays zero, an infinite length gives NaN.
V3 normalize3(const V3& v) {
    float l2 = (v[0] * v[0] + v[1] * v[1]) + v[2] * v[2];
    if (l2 == INFINITY) return {NAN, NAN, NAN};
    float len = std::sqrt(l2);
    if (len == 0.0f) return {0.0f, 0.0f, 0.0f};
    return {v[0] / len, v[1] / len, v[2] / len};
}

// Matrix4x4::RotationAxis(degrees, axis) (0x1003a140): degrees times 0.0174532924, then
// XMMatrixRotationAxis (0x1003c020: the axis normalized; Mware asserts on a zero axis, which here
// gives the angle's cosine on the diagonal) and XMMatrixRotationNormal (0x1003bf50).
Mat4 rotationAxis(float degrees, const V3& axis) {
    V3 n = normalize3(axis);
    float s = 0.0f, c = 0.0f;
    scalarSinCos(degrees * 0.0174532924f, s, c);
    float c2 = 1.0f - c;
    V3 v0{(c2 * n[1]) * n[2], (c2 * n[2]) * n[0], (c2 * n[0]) * n[1]};
    V3 r0, r1, r2;
    for (size_t i = 0; i < 3; ++i) {
        r0[i] = (c2 * n[i]) * n[i] + c;
        r1[i] = s * n[i] + v0[i];
        r2[i] = v0[i] - s * n[i];
    }
    Mat4 m;
    m.m = {r0[0], r1[2], r2[1], 0.0f, r2[2], r0[1], r1[0], 0.0f, r1[1], r2[0], r0[2], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    return m;
}

// 0x446da0 (totono_en.exe 0x461ce0, phantom.exe 0x1400551b0): a rotation matrix's angles in degrees
// (x = -asin(m[9]); near the poles y from m[0] and m[2] and no z). natives_camera.cpp's copy clamps
// its input, the matrix there being a guess.
V3 matrixAngles(const Mat4& m) {
    constexpr float kDeg = 57.2957802f;  // 0x5eeef8
    float r = std::asin(m.m[9]);
    float nr = -r;
    float c = std::cos(nr);
    if (0.001f > std::fabs(c)) {
        float y = static_cast<float>(std::atan2(static_cast<double>(-m.m[2]), static_cast<double>(m.m[0])));
        return {nr * kDeg, y * kDeg, 0.0f};
    }
    float y = static_cast<float>(std::atan2(static_cast<double>(m.m[8]), static_cast<double>(m.m[10])));
    float z = std::asin(m.m[1] / c);
    if (0.0f > m.m[5]) z = 3.14159274f - z;
    return {nr * kDeg, y * kDeg, z * kDeg};
}

// --- totono_en.exe's world / screen transforms (its Mware: CSceneManagerDX11 vtable +0x70 / +0x74,
// 0x10052fc0 / 0x10053070) ---

// XMVector3TransformCoord (0x10070a80): (x, y, z, 1) M over its w.
V3 transformCoord(const V3& p, const Mat4& m) {
    float r[4];
    for (int i = 0; i < 4; ++i) r[i] = p[2] * m.at(2, i) + m.at(3, i);
    for (int i = 0; i < 4; ++i) r[i] = p[1] * m.at(1, i) + r[i];
    for (int i = 0; i < 4; ++i) r[i] = p[0] * m.at(0, i) + r[i];
    return {r[0] / r[3], r[1] / r[3], r[2] / r[3]};
}

// Matrix4x4's inverse (0x1006dc30, XMMatrixInverse; by cofactors here: Fitted, the last bits may
// differ). A singular matrix divides by zero as the exe's does.
Mat4 inverse(const Mat4& a) {
    const auto& m = a.m;
    std::array<float, 16> inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    for (size_t i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
    return r;
}

// The scene camera's matrix as Mware's camera answers it (vtable +0x34: +0x104, the view times the
// projection its update makes, 0x100556c0). Guess: as the camera stands now (the exe's is from the
// camera's last update). Mware's own camera (no setSceneCamera) keeps identities; a camera not
// created yet has an identity projection.
Mat4 sceneViewProjection(const ScriptHost::Impl& h) {
    const CameraView* c = h.sceneCamera.get();
    if (!c) return Mat4{};
    Mat4 m = Mat4::lookAtLH(c->eye, c->target, c->up);
    if (!c->made) return m;
    if (c->ortho) return m * Mat4::orthographicLH(c->width, c->height, c->nearZ, c->farZ);
    return m * Mat4::perspectiveFovLH(c->fov * 0.0174532924f, c->aspect, c->nearZ, c->farZ);
}

// The scene manager's viewport (+0x7dc: 0, 0 and the back buffer's size from its constructor; the
// exe never changes it). Guess: the back buffer is the game's screen.
struct Viewport {
    float x, y, halfW, halfH;
};
Viewport sceneViewport(const ScriptHost::Impl& h) {
    return {0.0f, 0.0f, static_cast<float>(h.screenWidth() / 2), static_cast<float>(h.screenHeight() / 2)};
}

struct SceneObject final : NativeObject, FrameTask {
    ScriptHost::Impl* host;
    int32_t enable = 0;  // +0xc0
    // The effects (setEffectTimeScale 0x53d6b0): one value each, then the fog's colour.
    Tween softFocusBlur{1, 0.0f};   // +0xc8 (timer +0xcc, value +0xdc)
    Tween softFocusBlend{1, 0.0f};  // +0xf4
    Tween dofMiddle{1, 0.0f};       // +0x124
    Tween dofFar{1, 0.0f};          // +0x150
    Tween hdrLowLimit{1, 0.0f};     // +0x180 (setHDRBrightPassLowLimit)
    Tween hdrBrightPower{1, 0.0f};  // +0x1ac
    Tween hdrBloom{1, 0.0f};        // +0x1d8
    Tween toneBias{1, 0.0f};        // +0x208
    Tween toneDark{1, 0.0f};        // +0x234
    Tween toneLight{1, 0.0f};       // +0x260
    Tween fogNear{1, 0.0f};         // +0x3a8
    Tween fogFar{1, 0.0f};          // +0x3d4
    Tween fogColor{3, 0.0f, 0.0f, 0.0f};  // +0x400 (value +0x424)
    Tween radialBlur{1, 0.0f};      // +0x450
    std::array<float, 2> radialCenter{};  // +0x47c
    // setShadowMapOffsetRotate's vector (+0x39c, Phantom +0x3cc; only the renderer reads it).
    V3 shadowMapOffsetRotate{};
    // The light (setLightTimeScale 0x53d730).
    Tween lightLookAt{3, 0.0f, 0.0f, 0.0f};  // +0x28c (moveLightLookAt; value +0x2b0)
    Tween lightRotate{3, 0.0f, 0.0f, 0.0f};  // +0x2d8 (rotateLight; value +0x2fc)
    Tween light1D{1, 0.0f};                  // +0x324 (no native reads it)
    Tween lightColor{3, 0.0f, 0.0f, 0.0f};   // +0x350 (setLightColor; value +0x374)

    SceneObject(ScriptHost::Impl* h, std::string name) : host(h) {
        taskName = std::move(name);
        host->addTask(this);
    }
    ~SceneObject() override { host->removeTask(this); }

    std::array<Tween*, 14> effects() {
        return {&softFocusBlur, &softFocusBlend, &dofMiddle, &dofFar, &hdrLowLimit, &hdrBrightPower, &hdrBloom,
                &toneBias, &toneDark, &toneLight, &fogNear, &fogFar, &fogColor, &radialBlur};
    }
    std::array<Tween*, 4> lights() { return {&lightLookAt, &lightRotate, &light1D, &lightColor}; }
    // The update (0x53d1c0): every tween steps.
    void update(ScriptHost::Impl& h) override {
        for (Tween* t : effects()) t->step(h.clock);
        for (Tween* t : lights()) t->step(h.clock);
    }
};

NativeObject* make(HSQUIRRELVM v) { return new SceneObject(&host(v), argString(v, 2)); }

#define SCENE_SELF                               \
    SceneObject* o = self<SceneObject>(v);       \
    if (!o) return sq_throwerror(v, "SQScene: not an instance");

SQInteger pushFloat(HSQUIRRELVM v, float f) {
    sq_pushfloat(v, f);
    return 1;
}
int32_t argI(HSQUIRRELVM v, SQInteger idx) { return static_cast<int32_t>(argInt(v, idx)); }

// setEnable(on) (0x53d600): +0xc0.
SQInteger setEnable(HSQUIRRELVM v) {
    SCENE_SELF
    o->enable = argI(v, 2);
    return 0;
}
// setTimeScale(s) (0x53d610): every tween's; setEffectTimeScale (0x53d6b0) the effects',
// setLightTimeScale (0x53d730) the light's.
SQInteger setTimeScale(HSQUIRRELVM v) {
    SCENE_SELF
    float s = argFloat(v, 2);
    for (Tween* t : o->effects()) t->timeScale = s;
    for (Tween* t : o->lights()) t->timeScale = s;
    return 0;
}
SQInteger setEffectTimeScale(HSQUIRRELVM v) {
    SCENE_SELF
    float s = argFloat(v, 2);
    for (Tween* t : o->effects()) t->timeScale = s;
    return 0;
}
SQInteger setLightTimeScale(HSQUIRRELVM v) {
    SCENE_SELF
    float s = argFloat(v, 2);
    for (Tween* t : o->lights()) t->timeScale = s;
    return 0;
}

// A one-value tween: set<Name>(value, time, curve) from where it is (0x42fa80), get<Name>() its
// value (now), get<Name>Timer(), set<Name>Timer(ms) (clamped, eased: 0x53dcd0).
#define SCENE_TWEEN1(Set, Get, member)                                       \
    SQInteger Set(HSQUIRRELVM v) {                                           \
        SCENE_SELF                                                           \
        o->member.moveTo({argFloat(v, 2), 0.0f, 0.0f}, argI(v, 3), argI(v, 4)); \
        return 0;                                                            \
    }                                                                        \
    SQInteger Get(HSQUIRRELVM v) {                                           \
        SCENE_SELF                                                           \
        return pushFloat(v, o->member.now[0]);                               \
    }                                                                        \
    SQInteger Get##Timer(HSQUIRRELVM v) {                                    \
        SCENE_SELF                                                           \
        return pushFloat(v, o->member.timer);                                \
    }                                                                        \
    SQInteger Set##Timer(HSQUIRRELVM v) {                                    \
        SCENE_SELF                                                           \
        o->member.setTimer(argFloat(v, 2));                                  \
        return 0;                                                            \
    }
SCENE_TWEEN1(setSoftFocusBlurPower, getSoftFocusBlurPower, softFocusBlur)
SCENE_TWEEN1(setSoftFocusBlendPower, getSoftFocusBlendPower, softFocusBlend)
SCENE_TWEEN1(setDOFMiddle, getDOFMiddle, dofMiddle)
SCENE_TWEEN1(setDOFFar, getDOFFar, dofFar)
SCENE_TWEEN1(setHDRBrightPassLowLimit, getHDRBrightPassLowLimit, hdrLowLimit)
SCENE_TWEEN1(setHDRBrightPower, getHDRBrightPower, hdrBrightPower)
SCENE_TWEEN1(setHDRBloom, getHDRBloom, hdrBloom)
SCENE_TWEEN1(setToneMappingBias, getToneMappingBias, toneBias)
SCENE_TWEEN1(setToneMappingDarkClamp, getToneMappingDarkClamp, toneDark)
SCENE_TWEEN1(setToneMappingLightClamp, getToneMappingLightClamp, toneLight)
SCENE_TWEEN1(moveFogNear, getFogNear, fogNear)
SCENE_TWEEN1(moveFogFar, getFogFar, fogFar)
#undef SCENE_TWEEN1

// A three-value tween: <set>(x, y, z, time, curve) (0x45d720), <get>() puts its value in
// SQMATH_RET_1..3 (and 0 in _4: 0x53e710), the timer as above.
#define SCENE_TWEEN3(Set, Get, member)                                                                  \
    SQInteger Set(HSQUIRRELVM v) {                                                                      \
        SCENE_SELF                                                                                      \
        o->member.moveTo({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argI(v, 6));      \
        return 0;                                                                                       \
    }                                                                                                   \
    SQInteger Get(HSQUIRRELVM v) {                                                                      \
        SCENE_SELF                                                                                      \
        host(v).setResult(o->member.now[0], o->member.now[1], o->member.now[2], 0.0f);                  \
        return 0;                                                                                       \
    }                                                                                                   \
    SQInteger Get##Timer(HSQUIRRELVM v) {                                                               \
        SCENE_SELF                                                                                      \
        return pushFloat(v, o->member.timer);                                                           \
    }                                                                                                   \
    SQInteger Set##Timer(HSQUIRRELVM v) {                                                               \
        SCENE_SELF                                                                                      \
        o->member.setTimer(argFloat(v, 2));                                                             \
        return 0;                                                                                       \
    }
SCENE_TWEEN3(rotateLight, getRotateLight, lightRotate)
SCENE_TWEEN3(moveLightLookAt, getMoveLightLookAt, lightLookAt)
SCENE_TWEEN3(setLightColor, getLightColor, lightColor)
SCENE_TWEEN3(setFogColor, getFogColor, fogColor)
#undef SCENE_TWEEN3

// setFogParameter(near, far, r, g, b) (0x53ea30): the fog's three tweens set at once.
SQInteger setFogParameter(HSQUIRRELVM v) {
    SCENE_SELF
    o->fogNear.moveTo({argFloat(v, 2), 0.0f, 0.0f}, 0, 0);
    o->fogFar.moveTo({argFloat(v, 3), 0.0f, 0.0f}, 0, 0);
    o->fogColor.moveTo({argFloat(v, 4), argFloat(v, 5), argFloat(v, 6)}, 0, 0);
    return 0;
}
// setRadialBlur(cx, cy, power, time, curve) (0x53ed90): the power tweens, the centre is set;
// getRadialBlurCenter (0x53ee00) puts it in SQMATH_RET_1..2.
SQInteger setRadialBlur(HSQUIRRELVM v) {
    SCENE_SELF
    o->radialBlur.moveTo({argFloat(v, 4), 0.0f, 0.0f}, argI(v, 5), argI(v, 6));
    o->radialCenter = {argFloat(v, 2), argFloat(v, 3)};
    return 0;
}
SQInteger getRadialBlurPower(HSQUIRRELVM v) {
    SCENE_SELF
    return pushFloat(v, o->radialBlur.now[0]);
}
SQInteger getRadialBlurCenter(HSQUIRRELVM v) {
    SCENE_SELF
    host(v).setResult(o->radialCenter[0], o->radialCenter[1], 0.0f, 0.0f);
    return 0;
}
SQInteger getRadialBlurPowerTimer(HSQUIRRELVM v) {
    SCENE_SELF
    return pushFloat(v, o->radialBlur.timer);
}
SQInteger setRadialBlurPowerTimer(HSQUIRRELVM v) {
    SCENE_SELF
    o->radialBlur.setTimer(argFloat(v, 2));
    return 0;
}

// setLightLookAt(x, y, z) (0x53d950): the look-at point jumps there (moveLightLookAt with time 0).
SQInteger setLightLookAt(HSQUIRRELVM v) {
    SCENE_SELF
    o->lightLookAt.moveTo({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, 0, 0);
    return 0;
}
// setLightPosition(x, y, z) (0x53d760, totono_en.exe 0x50fcc0, phantom.exe 0x140179840): the
// rotation taking up (0, 1, 0) to the direction from the look-at point to the position, as angles,
// which the light's rotation jumps to. Fitted: asinf / acosf / cosf are the host's (the exe's SSE2
// runtime may round the last bit otherwise).
SQInteger setLightPosition(HSQUIRRELVM v) {
    SCENE_SELF
    const V3& at = o->lightLookAt.now;
    V3 d = normalize3({argFloat(v, 2) - at[0], argFloat(v, 3) - at[1], argFloat(v, 4) - at[2]});
    const V3 up{0.0f, 1.0f, 0.0f};
    float dot = (up[0] * d[0] + up[1] * d[1]) + up[2] * d[2];
    float degrees = std::acos(dot) * 57.2957802f;  // 0x495410 (acosf), 0x5eeef8
    // Vector3D::Cross(up, d) (0x1003e2d0).
    V3 axis{d[2] * up[1] - d[1] * up[2], d[0] * up[2] - d[2] * up[0], d[1] * up[0] - d[0] * up[1]};
    o->lightRotate.moveTo(matrixAngles(rotationAxis(degrees, axis)), 0, 0);
    return 0;
}
// setShadowMapOffsetRotate(x, y, z) (0x53d9f0, phantom.exe 0x140179a80): kept; totono_en.exe
// (0x50ff50) hands it to the scene manager.
SQInteger setShadowMapOffsetRotate(HSQUIRRELVM v) {
    SCENE_SELF
    V3 r{argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)};
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.shadowMapOffsetRotate = r;
    else o->shadowMapOffsetRotate = r;
    return 0;
}

// The effects' switches: the app's system state; Totono also hands the scene manager whether the
// effect runs (the setting's use flag and this).
#define SCENE_ENABLE(Name, field, use, managerField)                                       \
    SQInteger Name(HSQUIRRELVM v) {                                                        \
        SCENE_SELF                                                                         \
        ScriptHost::Impl& h = host(v);                                                     \
        h.config.field = argI(v, 2);                                                       \
        if (h.sceneForwards) h.sceneManager.managerField = h.config.use && h.config.field ? 1 : 0; \
        return 0;                                                                          \
    }
// setHDRBloomEnable (0x53da30, totono_en.exe 0x50ff90: scene manager +0xac)
SCENE_ENABLE(setHDRBloomEnable, hdrBloomEnable, useHDR, hdrBloom)
// setDOFEnable (0x53dd80, totono_en.exe 0x510360: +0xb8)
SCENE_ENABLE(setDOFEnable, dofEnable, useDOF, dof)
// setToneMappingEnable (0x53dfe0, totono_en.exe 0x510630: +0xc0)
SCENE_ENABLE(setToneMappingEnable, toneMappingEnable, useToneMapping, toneMapping)
// setSoftFocusEnable (0x53e330, totono_en.exe 0x5109d0: +0xc8)
SCENE_ENABLE(setSoftFocusEnable, softFocusEnable, useSoftFocus, softFocus)
// setFogEnable (0x53e8f0, totono_en.exe 0x511240: +0xe4)
SCENE_ENABLE(setFogEnable, fogEnable, useFog, fog)
#undef SCENE_ENABLE

// What only Totono's scene manager takes (empty functions in the later exes).
// setProjectionShadowOffsetRotate (totono_en.exe 0x50ff70): scene manager vtable +0x6c.
SQInteger setProjectionShadowOffsetRotate(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.projectionShadowOffsetRotate = {argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)};
    return 0;
}
// setHDRParameter (totono_en.exe 0x50ffe0): scene manager +0xb0.
SQInteger setHDRParameter(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.hdrParameter = {argFloat(v, 2), argFloat(v, 3)};
    return 0;
}
// setBloomParameter (totono_en.exe 0x510010): scene manager +0xb4.
SQInteger setBloomParameter(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.bloomParameter = argFloat(v, 2);
    return 0;
}
// setDOFParameter (totono_en.exe 0x5103b0): scene manager +0xbc.
SQInteger setDOFParameter(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.dofParameter = {argFloat(v, 2), argFloat(v, 3)};
    return 0;
}
// setToneMappingParameter (totono_en.exe 0x510680): scene manager +0xc4.
SQInteger setToneMappingParameter(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.toneMappingParameter = {argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)};
    return 0;
}
// setSoftFocusParameter (totono_en.exe 0x510a20): scene manager +0xcc.
SQInteger setSoftFocusParameter(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.softFocusParameter = {argFloat(v, 2), argFloat(v, 3)};
    return 0;
}
// setSceneStateShadow (totono_en.exe 0x510c50): scene manager +0xd0, the three ints as given.
SQInteger setSceneStateShadow(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.sceneStateShadow = {argI(v, 2), argI(v, 3), argI(v, 4)};
    return 0;
}
// setKeepSubstanceRenderingResult (totono_en.exe 0x510c70): scene manager +0xd4.
SQInteger setKeepSubstanceRenderingResult(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.keepSubstanceRenderingResult = argI(v, 2);
    return 0;
}
// setRadialBlurParameter (totono_en.exe 0x510c90): scene manager +0xd8 (Vector2D(x, y), power).
SQInteger setRadialBlurParameter(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) {
        h.sceneManager.radialBlurCenter = {argFloat(v, 2), argFloat(v, 3)};
        h.sceneManager.radialBlurPower = argFloat(v, 4);
    }
    return 0;
}
// setSoftParticleLength (totono_en.exe 0x510d10): scene manager +0xec.
SQInteger setSoftParticleLength(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (h.sceneForwards) h.sceneManager.softParticleLength = argFloat(v, 2);
    return 0;
}
// transformFromWorldToScreen(x, y, z) (0x510df0): the point through the scene camera, then the
// viewport (y down); z as projected. transformFromScreenToWorld (0x510d30): the screen point (z as
// given) back through the inverse. Both put the result in SQMATH_RET_1..3 (and 0 in _4). Guess: the
// camera's matrix as it stands now and the viewport as the screen (sceneViewProjection,
// sceneViewport); the later exes' are empty functions.
SQInteger transformFromWorldToScreen(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (!h.sceneForwards) return 0;
    V3 p = transformCoord({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, sceneViewProjection(h));
    Viewport vp = sceneViewport(h);
    float sx = (vp.x + vp.halfW) + p[0] * vp.halfW;
    float sy = (vp.y + vp.halfH) - vp.halfH * p[1];
    h.setResult(sx, sy, p[2], 0.0f);
    return 0;
}
// transformFromScreenToWorld (0x510d30): as above (Guess as above).
SQInteger transformFromScreenToWorld(HSQUIRRELVM v) {
    SCENE_SELF
    ScriptHost::Impl& h = host(v);
    if (!h.sceneForwards) return 0;
    Viewport vp = sceneViewport(h);
    float nx = (argFloat(v, 2) - (vp.x + vp.halfW)) / vp.halfW;
    float ny = -((argFloat(v, 3) - (vp.y + vp.halfH)) / vp.halfH);
    V3 p = transformCoord({nx, ny, argFloat(v, 4)}, inverse(sceneViewProjection(h)));
    h.setResult(p[0], p[1], p[2], 0.0f);
    return 0;
}
// The shadow map's settings: empty functions in all three exes (0x403d00 / 0x439230 / 0x439290,
// totono_en.exe 0x4040f0 / 0x435970 / 0x4359d0, phantom.exe 0x140044030).
SQInteger sceneNothing(HSQUIRRELVM v) {
    SCENE_SELF
    return 0;
}

}  // namespace

ClassNatives sceneNatives() {
    ClassNatives c;
    c.make = make;
#define T1(Set, Get) {#Set, Set}, {#Get, Get}, {#Get "Timer", Get##Timer}, {#Set "Timer", Set##Timer}
    c.methods = {
        {"setEnable", setEnable},
        {"setTimeScale", setTimeScale},
        {"setEffectTimeScale", setEffectTimeScale},
        {"setLightTimeScale", setLightTimeScale},
        T1(setSoftFocusBlurPower, getSoftFocusBlurPower),
        T1(setSoftFocusBlendPower, getSoftFocusBlendPower),
        T1(setDOFMiddle, getDOFMiddle),
        T1(setDOFFar, getDOFFar),
        T1(setHDRBrightPassLowLimit, getHDRBrightPassLowLimit),
        T1(setHDRBrightPower, getHDRBrightPower),
        T1(setHDRBloom, getHDRBloom),
        T1(setToneMappingBias, getToneMappingBias),
        T1(setToneMappingDarkClamp, getToneMappingDarkClamp),
        T1(setToneMappingLightClamp, getToneMappingLightClamp),
        {"moveFogNear", moveFogNear}, {"getFogNear", getFogNear}, {"getFogNearTimer", getFogNearTimer},
        {"setFogNearTimer", moveFogNearTimer},
        {"moveFogFar", moveFogFar}, {"getFogFar", getFogFar}, {"getFogFarTimer", getFogFarTimer},
        {"setFogFarTimer", moveFogFarTimer},
        {"rotateLight", rotateLight}, {"getRotateLight", getRotateLight}, {"getRotateLightTimer", getRotateLightTimer},
        {"setRotateLightTimer", rotateLightTimer},
        {"moveLightLookAt", moveLightLookAt}, {"getMoveLightLookAt", getMoveLightLookAt},
        {"getMoveLightLookAtTimer", getMoveLightLookAtTimer}, {"setMoveLightLookAtTimer", moveLightLookAtTimer},
        T1(setLightColor, getLightColor),
        T1(setFogColor, getFogColor),
        {"setFogParameter", setFogParameter},
        {"setRadialBlur", setRadialBlur},
        {"getRadialBlurPower", getRadialBlurPower},
        {"getRadialBlurCenter", getRadialBlurCenter},
        {"getRadialBlurPowerTimer", getRadialBlurPowerTimer},
        {"setRadialBlurPowerTimer", setRadialBlurPowerTimer},
        {"setLightPosition", setLightPosition},
        {"setLightLookAt", setLightLookAt},
        {"setShadowMapOffsetRotate", setShadowMapOffsetRotate},
        {"setHDRBloomEnable", setHDRBloomEnable},
        {"setDOFEnable", setDOFEnable},
        {"setToneMappingEnable", setToneMappingEnable},
        {"setSoftFocusEnable", setSoftFocusEnable},
        {"setFogEnable", setFogEnable},
        {"setProjectionShadowOffsetRotate", setProjectionShadowOffsetRotate},
        {"setHDRParameter", setHDRParameter},
        {"setBloomParameter", setBloomParameter},
        {"setDOFParameter", setDOFParameter},
        {"setToneMappingParameter", setToneMappingParameter},
        {"setSoftFocusParameter", setSoftFocusParameter},
        {"setSceneStateShadow", setSceneStateShadow},
        {"setKeepSubstanceRenderingResult", setKeepSubstanceRenderingResult},
        {"setRadialBlurParameter", setRadialBlurParameter},
        {"setSoftParticleLength", setSoftParticleLength},
        {"transformFromWorldToScreen", transformFromWorldToScreen},
        {"transformFromScreenToWorld", transformFromScreenToWorld},
        {"setShadowMapDistance", sceneNothing},
        {"setShadowMapCenterPosition", sceneNothing},
        {"setShadowMapFov", sceneNothing},
        {"setProjectionShadowFov", sceneNothing},
        {"setShadowMapFarClip", sceneNothing},
        {"setShadowMapNearClip", sceneNothing},
        {"setProjectionShadowFarClip", sceneNothing},
        {"setProjectionShadowNearClip", sceneNothing},
        {"setShadowMapCenterBone", sceneNothing},
        {"setProjectionShadowCenterBone", sceneNothing},
    };
#undef T1
    return c;
}

}  // namespace nyan::diesel
