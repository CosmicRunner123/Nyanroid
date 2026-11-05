// SQCamera (muramasa_en.exe natives 0x52b7d0 ff.; the task object 0x5eba48, update 0x52af20, the view
// 0x52b130): a task object holding Mware's camera, which sprites attached with SQSprite.setCamera are
// drawn through. Its position and rotation are tweens; a timeline
// (three timed splines: position, rotation, field of view) and a bezier can drive the position; the
// view goes to the camera after every change and every frame.

#include <algorithm>
#include <cmath>
#include <memory>

#include "natives.hpp"
#include "nyan/diesel/camera.hpp"
#include "nyan/diesel/spline.hpp"
#include "nyan/diesel/sprite.hpp"
#include "nyan/diesel/sprite_geometry.hpp"

namespace nyan::diesel {

namespace {

using V3 = std::array<float, 3>;
constexpr float kDegrees = 0.0174532924f;  // 0x5eede8

V3 rowTimes(const V3& v, const Mat4& m) {
    auto p = m.apply(v[0], v[1], v[2]);
    float w = p[3] != 0.0f ? p[3] : 1.0f;  // TransformCoord
    return {p[0] / w, p[1] / w, p[2] / w};
}

struct CameraObject final : NativeObject, FrameTask {
    ScriptHost::Impl* host;
    std::shared_ptr<CameraView> view = std::make_shared<CameraView>();
    float timeScale = 1.0f;                // +0x190
    V3 moveDelta{};                        // +0xd4: moveDelta's (each view adds half of it, turned)
    Tween move{3, 0.0f, 0.0f, 0.0f};       // +0xf8 (now +0x11c, without power +0x128)
    Tween rotate{3, 0.0f, 0.0f, 0.0f};     // +0x144 (now +0x168): degrees
    int32_t fixedLookAt = 0;               // +0x194
    SpriteImage::Bezier bezier;            // +0x198
    TimedSpline moveLine, rotateLine, fovLine;  // +0x1e8, +0x248, +0x2a8 (the timeline)
    // shake / randomShake (+0x300, 0x52bd70 / 0x52beb0): while it shakes the view moves eye and
    // target by its offset turned with the camera; the update drops it once it stops (0x52b0c8).
    std::unique_ptr<Shake> shake;
    int32_t lock = 0, disused = 0;         // Totono's task flags (vtable +0xb0 .. +0xbc: +0xdc's +0x20 / +0x24)

    CameraObject(ScriptHost::Impl* h, std::string name) : host(h) {
        taskName = std::move(name);
        host->addTask(this);
    }
    ~CameraObject() override { host->removeTask(this); }

    void setTimeScale(float s) {
        timeScale = s;
        for (Tween* t : {&move, &rotate, &bezier.t, &moveLine.t, &rotateLine.t, &fovLine.t}) t->timeScale = s;
        if (shake) shake->setTimeScale(s);
    }
    // The running shake's offset turned by the camera's rotation (TransformCoord), else none.
    V3 shakeOffset(const Mat4& r) const {
        if (!shake || !shake->shaking()) return {0.0f, 0.0f, 0.0f};
        auto o = shake->offset();
        return rowTimes({o[0], o[1], o[2]}, r);
    }
    void placeMove(const V3& p) {
        for (size_t i = 0; i < 3; ++i) move.nowNoPower[i] = p[i], move.now[i] = p[i] * move.power;
    }
    void placeRotate(const V3& p) {
        for (size_t i = 0; i < 3; ++i) rotate.nowNoPower[i] = p[i], rotate.now[i] = p[i] * rotate.power;
    }
    static void stop(Tween& t) {
        if (!t.active) return;
        t.active = 0;
        t.nowNoPower[0] = t.dst[0];
        t.now[0] = t.power * t.dst[0];
    }

    // 0x52b130: the projection, then the view. With a fixed look-at (setFixedLookAt) the eye is the
    // position, the target the look-at point and up (0, 1, 0) turned by the rotation (Z, X, Y). Else
    // the rotation (RotZ · RotX · RotY, degrees) gives the camera's axes; half the move delta along
    // them is added to the position; the eye looks along its forward axis (the target, eye + forward,
    // becomes the look-at point).
    void apply() {
        CameraView& c = *view;
        if (fixedLookAt) {
            // A running shake moves the eye and the target (the look-at point, kept) by its offset
            // turned the same way.
            Mat4 r = Mat4::rotationZXY(rotate.now[0] * kDegrees, rotate.now[1] * kDegrees, rotate.now[2] * kDegrees);
            c.up = rowTimes({0.0f, 1.0f, 0.0f}, r);
            V3 off = shakeOffset(r);
            for (size_t i = 0; i < 3; ++i) c.eye[i] = move.now[i] + off[i], c.target[i] = c.at[i] + off[i];
            return;
        }
        Mat4 r = Mat4::rotationZ(rotate.now[2] * kDegrees) * Mat4::rotationX(rotate.now[0] * kDegrees) *
                 Mat4::rotationY(rotate.now[1] * kDegrees);
        V3 right = rowTimes({1.0f, 0.0f, 0.0f}, r), up = rowTimes({0.0f, 1.0f, 0.0f}, r);
        V3 forward = rowTimes({0.0f, 0.0f, 1.0f}, r);
        for (size_t i = 0; i < 3; ++i) {
            float d = right[i] * (moveDelta[0] * 0.5f) + up[i] * (moveDelta[1] * 0.5f);
            d = d + forward[i] * (moveDelta[2] * 0.5f);
            move.nowNoPower[i] = move.now[i] + d;
            move.now[i] = move.nowNoPower[i] * move.power;
        }
        // The eye moves by a running shake's turned offset; the target (kept as the look-at point) is
        // the eye plus forward.
        V3 off = shakeOffset(r);
        for (size_t i = 0; i < 3; ++i) c.eye[i] = move.now[i] + off[i];
        for (size_t i = 0; i < 3; ++i) c.at[i] = forward[i] + c.eye[i];
        c.target = c.at;
        c.up = up;
    }

    // The task update (0x52af20): the tweens, the bezier, the timeline's three splines, the view.
    void update(ScriptHost::Impl& h) override {
        move.step(h.clock);
        rotate.step(h.clock);
        if (bezier.step(h.clock)) placeMove(bezier.point);
        if (moveLine.step(h.clock)) placeMove(moveLine.point);
        if (rotateLine.step(h.clock)) placeRotate(rotateLine.point);
        if (fovLine.step(h.clock)) view->fov = fovLine.point[0];
        if (shake) {
            if (shake->shaking()) shake->update(h.clock);
            else shake.reset();
        }
        apply();
    }
};

NativeObject* make(HSQUIRRELVM v) { return new CameraObject(&host(v), argString(v, 2)); }

#define CAMERA_SELF                                  \
    CameraObject* o = self<CameraObject>(v);         \
    if (!o) return sq_throwerror(v, "SQCamera: not an instance");

SQInteger pushFloat(HSQUIRRELVM v, float f) {
    sq_pushfloat(v, f);
    return 1;
}
SQInteger pushInt(HSQUIRRELVM v, SQInteger i) {
    sq_pushinteger(v, i);
    return 1;
}
int32_t argI(HSQUIRRELVM v, SQInteger idx) { return static_cast<int32_t>(argInt(v, idx)); }

// create(fov, aspect, near, far) (0x52b7d0): a perspective (degrees); createOrtho(width, height,
// near, far) (0x52b850).
SQInteger create(HSQUIRRELVM v) {
    CAMERA_SELF
    CameraView& c = *o->view;
    c.made = true, c.ortho = false;
    c.fov = argFloat(v, 2), c.aspect = argFloat(v, 3), c.nearZ = argFloat(v, 4), c.farZ = argFloat(v, 5);
    o->apply();
    return pushInt(v, 0);
}
SQInteger createOrtho(HSQUIRRELVM v) {
    CAMERA_SELF
    CameraView& c = *o->view;
    c.made = true, c.ortho = true;
    c.width = argFloat(v, 2), c.height = argFloat(v, 3), c.nearZ = argFloat(v, 4), c.farZ = argFloat(v, 5);
    o->apply();
    return pushInt(v, 0);
}
// lookAt(x, y, z) (0x52b8c0): the look-at point (a fixed look-at's target).
SQInteger lookAt(HSQUIRRELVM v) {
    CAMERA_SELF
    o->view->at = {argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)};
    o->apply();
    return 0;
}
// move3D(x, y, z, time, curve) (0x52b910): the position tween from where it is; the timeline's
// position and the bezier stop (their keys go; running tweens end where they were going).
SQInteger move3D(HSQUIRRELVM v) {
    CAMERA_SELF
    o->move.moveTo({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argI(v, 6));
    o->moveLine.reset();
    o->bezier.points.clear();
    CameraObject::stop(o->bezier.t);
    o->apply();
    return 0;
}
// rotate(x, y, z, time, curve) (0x52ba60): the rotation tween (degrees); the timeline's rotation goes.
SQInteger rotate(HSQUIRRELVM v) {
    CAMERA_SELF
    o->rotate.moveTo({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)}, argI(v, 5), argI(v, 6));
    o->rotateLine.reset();
    o->apply();
    return 0;
}
SQInteger setTimeScale(HSQUIRRELVM v) {  // 0x52bb70
    CAMERA_SELF
    o->setTimeScale(argFloat(v, 2));
    return 0;
}
SQInteger getMove(HSQUIRRELVM v) {  // 0x52bbd0: SQMATH_RET
    CAMERA_SELF
    host(v).setResult(o->move.now[0], o->move.now[1], o->move.now[2]);
    return 0;
}
SQInteger getRotate(HSQUIRRELVM v) {  // 0x52bc10
    CAMERA_SELF
    host(v).setResult(o->rotate.now[0], o->rotate.now[1], o->rotate.now[2]);
    return 0;
}
// moveDelta(x, y, z) (0x52bd00): kept; each view adds half of it along the camera's axes.
SQInteger moveDelta(HSQUIRRELVM v) {
    CAMERA_SELF
    o->moveDelta = {argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)};
    o->apply();
    return 0;
}
SQInteger getFov(HSQUIRRELVM v) {  // 0x52bc50
    CAMERA_SELF
    return pushFloat(v, o->view->fov);
}
// isMoving (0x52bc60): the position tween, the bezier or the timeline's position runs; isRotating
// (0x52bc90): the rotation tween or the timeline's rotation.
SQInteger isMoving(HSQUIRRELVM v) {
    CAMERA_SELF
    return pushInt(v, o->move.active || o->bezier.t.active || o->moveLine.t.active ? 1 : 0);
}
SQInteger isRotating(HSQUIRRELVM v) {
    CAMERA_SELF
    return pushInt(v, o->rotate.active || o->rotateLine.t.active ? 1 : 0);
}
// The bezier (0x52c000 / 0x52c050 / 0x52c0d0): as a sprite's, driving the position.
SQInteger resetBezier(HSQUIRRELVM v) {
    CAMERA_SELF
    o->bezier.points.clear();
    CameraObject::stop(o->bezier.t);
    o->apply();
    return 0;
}
SQInteger setBezier3D(HSQUIRRELVM v) {
    CAMERA_SELF
    o->bezier.points.push_back({argFloat(v, 2), argFloat(v, 3), argFloat(v, 4)});
    return 0;
}
SQInteger beginBezier(HSQUIRRELVM v) {
    CAMERA_SELF
    auto& b = o->bezier;
    size_t n = b.points.size();
    if (n == 0) return 0;
    if (n >= 4 && n % 4 == 0) {
        b.t.set({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, argI(v, 2), argI(v, 3));
        b.point = b.points.front();
    } else {
        b.point = b.points.back();
    }
    b.t.timeScale = o->timeScale;
    o->apply();
    return 0;
}
// resetTimeline (0x52c110): the timeline's three splines lose their keys.
SQInteger resetTimeline(HSQUIRRELVM v) {
    CAMERA_SELF
    o->moveLine.reset();
    o->rotateLine.reset();
    o->fovLine.reset();
    return 0;
}
// setTimeline(position, rotation, fov, time, weight) (0x52c210): a key on each spline (Vector3D
// position and rotation).
SQInteger setTimeline(HSQUIRRELVM v) {
    CAMERA_SELF
    auto p = argMath(v, 2, "Vector3D"), r = argMath(v, 3, "Vector3D");
    if (!p || !r) return sq_throwerror(v, "setTimeline: position and rotation must be Vector3D");
    int32_t ms = argI(v, 5);
    float weight = argFloat(v, 6);
    o->moveLine.add({(*p)[0], (*p)[1], (*p)[2]}, ms, weight);
    o->rotateLine.add({(*r)[0], (*r)[1], (*r)[2]}, ms, weight);
    o->fovLine.add({argFloat(v, 4), 0.0f, 0.0f}, ms, weight);
    return 0;
}
// beginTimeline(time, curve) (0x52c450): the bezier ends; the three splines start, the position,
// rotation and field of view put at their first keys.
SQInteger beginTimeline(HSQUIRRELVM v) {
    CAMERA_SELF
    o->bezier.points.clear();
    CameraObject::stop(o->bezier.t);
    o->apply();
    int32_t ms = argI(v, 2), curve = argI(v, 3);
    o->moveLine.begin(ms, curve);
    o->placeMove(o->moveLine.point);
    o->rotateLine.begin(ms, curve);
    o->placeRotate(o->rotateLine.point);
    o->fovLine.begin(ms, curve);
    o->view->fov = o->fovLine.point[0];
    o->setTimeScale(o->timeScale);
    o->apply();
    return 0;
}
// setFixedLookAt(flag) (0x52c5c0): turning it off keeps where the camera looks, as a rotation read
// back from the view (0x446da0).
SQInteger setFixedLookAt(HSQUIRRELVM v) {
    CAMERA_SELF
    int32_t flag = argI(v, 2);
    if (o->fixedLookAt && !flag) {
        const CameraView& c = *o->view;
        Mat4 m = Mat4::lookAtLH(c.eye, c.target, c.up);
        // 0x446da0: Euler angles (degrees) of the matrix's rotation (Guess: the camera's +0x38 taken
        // as its view).
        float x = -std::asin(std::clamp(m.m[9], -1.0f, 1.0f));
        float cx = std::cos(x);
        V3 e{};
        if (std::fabs(cx) < 0.001f) {
            e = {x * 57.2957802f, std::atan2(-m.m[2], m.m[0]) * 57.2957802f, 0.0f};
        } else {
            float z = std::asin(std::clamp(m.m[1] / cx, -1.0f, 1.0f));
            if (m.m[5] < 0.0f) z = 3.14159274f - z;
            e = {x * 57.2957802f, std::atan2(m.m[8], m.m[10]) * 57.2957802f, z * 57.2957802f};
        }
        o->rotate.set(e, e, 0, 0);
    }
    o->fixedLookAt = flag;
    o->apply();
    return 0;
}
// setSceneCamera() (0x52bcb0, totono_en.exe 0x4fe1e0, phantom.exe 0x140162e70): this camera's Mware
// camera becomes the 3D scene's (held by the render manager, or Totono's scene manager); only
// Totono's SQScene.transformFrom* read it back.
SQInteger setSceneCamera(HSQUIRRELVM v) {
    CAMERA_SELF
    host(v).sceneCamera = o->view;
    return 0;
}
SQInteger isFixedLookAt(HSQUIRRELVM v) {  // 0x52c680
    CAMERA_SELF
    return pushInt(v, o->fixedLookAt);
}
SQInteger getLookAt(HSQUIRRELVM v) {  // 0x52c690
    CAMERA_SELF
    host(v).setResult(o->view->at[0], o->view->at[1], o->view->at[2]);
    return 0;
}
// calcScreenSize(distance) (0x52c6d0): the size the view covers at that distance, the height
// 2 tan(fov / 2) distance and the width that times the aspect.
SQInteger calcScreenSize(HSQUIRRELVM v) {
    CAMERA_SELF
    float t = std::tan(o->view->fov * 0.00872664619f);
    float h = t * argFloat(v, 2) + t * argFloat(v, 2);
    host(v).setResult(o->view->aspect * h, h, 0.0f);
    return 0;
}
SQInteger getMoveTimer(HSQUIRRELVM v) {  // 0x52c780
    CAMERA_SELF
    return pushFloat(v, o->move.timer);
}
SQInteger getRotateTimer(HSQUIRRELVM v) {  // 0x52c790
    CAMERA_SELF
    return pushFloat(v, o->rotate.timer);
}
SQInteger getTimelineTimer(HSQUIRRELVM v) {  // 0x52c7a0
    CAMERA_SELF
    return pushFloat(v, o->moveLine.t.timer);
}
SQInteger getBezierTimer(HSQUIRRELVM v) {  // 0x52c7b0
    CAMERA_SELF
    return pushFloat(v, o->bezier.t.timer);
}
SQInteger setMoveTimer(HSQUIRRELVM v) {  // 0x52c7e0
    CAMERA_SELF
    o->move.setTimer(argFloat(v, 2));
    o->apply();
    return 0;
}
SQInteger setRotateTimer(HSQUIRRELVM v) {  // 0x52c850
    CAMERA_SELF
    o->rotate.setTimer(argFloat(v, 2));
    o->apply();
    return 0;
}
// setTimelineTimer(ms) (0x52c8c0): each begun spline jumps there; a running one puts its value.
SQInteger setTimelineTimer(HSQUIRRELVM v) {
    CAMERA_SELF
    float ms = argFloat(v, 2);
    if (o->moveLine.built()) {
        o->moveLine.setTimer(ms);
        if (o->moveLine.t.active) o->placeMove(o->moveLine.point);
    }
    if (o->rotateLine.built()) {
        o->rotateLine.setTimer(ms);
        if (o->rotateLine.t.active) o->placeRotate(o->rotateLine.point);
    }
    if (o->fovLine.built()) {
        o->fovLine.setTimer(ms);
        if (o->fovLine.t.active) o->view->fov = o->fovLine.point[0];
    }
    o->apply();
    return 0;
}
SQInteger setBezierTimer(HSQUIRRELVM v) {  // 0x52ca80
    CAMERA_SELF
    if (o->bezier.points.empty()) return 0;
    o->bezier.t.setTimer(argFloat(v, 2));
    o->bezier.evaluate();
    o->placeMove(o->bezier.point);
    o->apply();
    return 0;
}
// serialize / deserialize (0x52cc10 / 0x52ce70): the record holds the shake (as a sprite's).
SQInteger serialize(HSQUIRRELVM v) {
    CAMERA_SELF
    EngineState st;
    st.shake = &o->shake;
    return pushInt(v, writeEngineRecord(*o->host, argString(v, 2), EngineRecord::Camera, st) ? 1 : 0);
}
SQInteger deserialize(HSQUIRRELVM v) {
    CAMERA_SELF
    EngineState st;
    st.shake = &o->shake;
    bool ok = readEngineRecord(*o->host, argString(v, 2), EngineRecord::Camera, st);
    o->apply();
    return pushInt(v, ok ? 1 : 0);
}
// shake / randomShake(frequency, interpolation, time, tempo, beatTempo) (0x52bd70 / 0x52beb0): from
// SQMATH_V3_1 to SQMATH_V3_2 (the host's +0xe4 / +0xf0), a new shake with the camera's time scale.
SQInteger startCameraShake(HSQUIRRELVM v, bool random) {
    CAMERA_SELF
    auto& h = host(v);
    auto s = std::make_unique<Shake>(random, h.randomSeed + static_cast<int32_t>(h.clock.milliTime));
    s->set({h.mathVars[0], h.mathVars[1], h.mathVars[2]}, {h.mathVars[3], h.mathVars[4], h.mathVars[5]},
           static_cast<uint32_t>(argI(v, 2)), argI(v, 3), static_cast<uint32_t>(argI(v, 4)), argI(v, 5), argI(v, 6));
    s->setTimeScale(o->timeScale);
    o->shake = std::move(s);
    return 0;
}
SQInteger shake(HSQUIRRELVM v) { return startCameraShake(v, false); }
SQInteger randomShake(HSQUIRRELVM v) { return startCameraShake(v, true); }
SQInteger getShakeTimer(HSQUIRRELVM v) {  // 0x52c7c0
    CAMERA_SELF
    return pushFloat(v, o->shake ? o->shake->timer() : 0.0f);
}
// setShakeTimer(ms) (0x52cba0): within [0, the shake's time], then the view again.
SQInteger setShakeTimer(HSQUIRRELVM v) {
    CAMERA_SELF
    if (!o->shake) return 0;
    float t = argFloat(v, 2);
    float time = o->shake->time();
    o->shake->setTimer(t >= 0.0f ? (time < t ? time : t) : 0.0f);
    o->apply();
    return 0;
}
SQInteger setLock(HSQUIRRELVM v) {
    CAMERA_SELF
    o->lock = argI(v, 2);
    return 0;
}
SQInteger getLock(HSQUIRRELVM v) {
    CAMERA_SELF
    return pushInt(v, o->lock);
}
SQInteger setDisused(HSQUIRRELVM v) {
    CAMERA_SELF
    o->disused = argI(v, 2);
    return 0;
}
SQInteger getDisused(HSQUIRRELVM v) {
    CAMERA_SELF
    return pushInt(v, o->disused);
}

}  // namespace

std::shared_ptr<const CameraView> cameraView(ScriptHost::Impl& h, const std::string& name) {
    auto* c = dynamic_cast<CameraObject*>(h.findTask(name));
    return c ? c->view : nullptr;
}

ClassNatives cameraNatives() {
    ClassNatives c;
    c.make = make;
    c.methods = {{"create", create},
                 {"createOrtho", createOrtho},
                 {"lookAt", lookAt},
                 {"move3D", move3D},
                 {"rotate", rotate},
                 {"setTimeScale", setTimeScale},
                 {"getMove", getMove},
                 {"getRotate", getRotate},
                 {"moveDelta", moveDelta},
                 {"getFov", getFov},
                 {"isMoving", isMoving},
                 {"isRotating", isRotating},
                 {"setSceneCamera", setSceneCamera},
                 {"resetBezier", resetBezier},
                 {"setBezier3D", setBezier3D},
                 {"beginBezier", beginBezier},
                 {"resetTimeline", resetTimeline},
                 {"setTimeline", setTimeline},
                 {"beginTimeline", beginTimeline},
                 {"setFixedLookAt", setFixedLookAt},
                 {"isFixedLookAt", isFixedLookAt},
                 {"getLookAt", getLookAt},
                 {"calcScreenSize", calcScreenSize},
                 {"getMoveTimer", getMoveTimer},
                 {"getRotateTimer", getRotateTimer},
                 {"getTimelineTimer", getTimelineTimer},
                 {"getBezierTimer", getBezierTimer},
                 {"setMoveTimer", setMoveTimer},
                 {"setRotateTimer", setRotateTimer},
                 {"setTimelineTimer", setTimelineTimer},
                 {"setBezierTimer", setBezierTimer},
                 {"serialize", serialize},
                 {"deserialize", deserialize},
                 {"shake", shake},
                 {"randomShake", randomShake},
                 {"getShakeTimer", getShakeTimer},
                 {"setShakeTimer", setShakeTimer},
                 {"setLock", setLock},
                 {"getLock", getLock},
                 {"setDisused", setDisused},
                 {"getDisused", getDisused}};
    return c;
}

}  // namespace nyan::diesel
