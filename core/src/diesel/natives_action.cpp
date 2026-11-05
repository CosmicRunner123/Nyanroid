// SQAction1D: the scripts' 1D tween (muramasa_en.exe registers it at 0x54d6b0; object 0xe8 bytes,
// the tween at +0xb8). Types as Muramasa's SqPlus dispatchers check and push them.
// SQProcess: the engine half of the framework's CNutProcess (a script run as a process), a task
// object made with the process's name; `create` keeps a function's name, and Totono's natives are the
// task object's lock and disused flags.

#include "natives.hpp"

namespace nyan::diesel {

namespace {

struct Action1DObject final : NativeObject, FrameTask {
    ScriptHost::Impl* host;
    std::string name;  // the constructor's argument (the task object's name)
    Action1D a;
    int32_t wasActive = 0;  // +0xe4: active before this frame's step

    Action1DObject(ScriptHost::Impl* h, std::string n) : host(h), name(std::move(n)) { host->addTask(this); }
    ~Action1DObject() override { host->removeTask(this); }
    // The task update (vtable slot 14, 0x54d930).
    void update(ScriptHost::Impl& h) override {
        wasActive = a.active;
        a.step(h.clock);
    }
};

NativeObject* make(HSQUIRRELVM v) { return new Action1DObject(&host(v), argString(v, 2)); }

Action1DObject* obj(HSQUIRRELVM v) { return self<Action1DObject>(v); }

#define ACTION_SELF                         \
    Action1DObject* o = obj(v);             \
    if (!o) return sq_throwerror(v, "SQAction1D: not an instance");

// setAction(src: float, dst: float, time: int, tempo: int) (0x54d950).
SQInteger setAction(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.set(argFloat(v, 2), argFloat(v, 3), static_cast<int32_t>(argInt(v, 4)), static_cast<int32_t>(argInt(v, 5)));
    return 0;
}
// finish(): int (0x54da20).
SQInteger finish(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushinteger(v, o->a.finish() ? 1 : 0);
    return 1;
}
SQInteger resetNow(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.refresh();
    return 0;
}
SQInteger resetTimer(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.resetTimer();
    return 0;
}
SQInteger setTimer(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setTimer(argFloat(v, 2));
    return 0;
}
SQInteger getTimer(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.timer);
    return 1;
}
SQInteger setActionTime(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setActionTime(static_cast<int32_t>(argInt(v, 2)));
    return 0;
}
SQInteger getActionTime(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.actionTime);
    return 1;
}
SQInteger setSrc(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setSrc(argFloat(v, 2));
    return 0;
}
SQInteger setDst(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setDst(argFloat(v, 2));
    return 0;
}
SQInteger setNow(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setNow(argFloat(v, 2));
    return 0;
}
// getSrc / getDst / getNow / getNowNoPower: a SquirrelObject holding the float (0x54dcb0..).
SQInteger getSrc(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.src);
    return 1;
}
SQInteger getDst(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.dst);
    return 1;
}
SQInteger getNow(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.now);
    return 1;
}
SQInteger getNowNoPower(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.nowNoPower);
    return 1;
}
// getRate(): float, timer / actionTime (0x54dd70).
SQInteger getRate(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.timer / o->a.actionTime);
    return 1;
}
// isActive(): int, active or active before this frame's step (0x54dd80).
SQInteger isActive(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushinteger(v, (o->a.active || o->wasActive) ? 1 : 0);
    return 1;
}
SQInteger setActive(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.active = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger setPower(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setPower(argFloat(v, 2));
    return 0;
}
SQInteger getPower(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.power);
    return 1;
}
SQInteger setTempo(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.setTempo(static_cast<int32_t>(argInt(v, 2)));
    return 0;
}
SQInteger getTempo(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushinteger(v, o->a.tempo);
    return 1;
}
SQInteger getTimeScale(HSQUIRRELVM v) {
    ACTION_SELF
    sq_pushfloat(v, o->a.timeScale);
    return 1;
}
SQInteger setTimeScale(HSQUIRRELVM v) {
    ACTION_SELF
    o->a.timeScale = argFloat(v, 2);
    return 0;
}

// SQProcess: setLock / getLock / setDisused / getDisused (totono_en.exe 0x4be5f0, 0x4be5f5,
// 0x4be5fa, 0x4be5eb) call the task object's vtable +0x40 / +0x44 / +0x48 / +0x4c, the flags every
// task object keeps (as SQSprite's and SQSound's). serialize / deserialize are the exe's empty
// void(string) (0x4040f0).
struct ProcessObject final : NativeObject {
    std::string name;
    std::string function;  // +0xb8 (Totono +0xbc, Phantom +0xe0): create's; nothing else reads it
    int32_t lock = 0;
    int32_t disused = 0;
    explicit ProcessObject(std::string n) : name(std::move(n)) {}
};

NativeObject* makeProcess(HSQUIRRELVM v) { return new ProcessObject(argString(v, 2)); }

#define PROCESS_SELF                                \
    ProcessObject* o = self<ProcessObject>(v);      \
    if (!o) return sq_throwerror(v, "SQProcess: not an instance");

SQInteger processSetLock(HSQUIRRELVM v) {
    PROCESS_SELF
    o->lock = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger processGetLock(HSQUIRRELVM v) {
    PROCESS_SELF
    sq_pushinteger(v, o->lock);
    return 1;
}
SQInteger processSetDisused(HSQUIRRELVM v) {
    PROCESS_SELF
    o->disused = static_cast<int32_t>(argInt(v, 2));
    return 0;
}
SQInteger processGetDisused(HSQUIRRELVM v) {
    PROCESS_SELF
    sq_pushinteger(v, o->disused);
    return 1;
}
// create(function) (muramasa_en.exe 0x5307f0, totono_en.exe 0x502f50, phantom.exe 0x1401688d0): the
// name kept (up to its first NUL); answers 0. The framework's CNutProcess.create keeps the name on
// the script side instead, so no script calls it.
SQInteger processCreate(HSQUIRRELVM v) {
    PROCESS_SELF
    o->function = argString(v, 2);
    sq_pushinteger(v, 0);
    return 1;
}
// serialize / deserialize: empty functions in the exes (0x403d00, totono_en.exe 0x4040f0, phantom.exe
// 0x140044030).
SQInteger processNoOp(HSQUIRRELVM) { return 0; }

}  // namespace

ClassNatives processNatives() {
    return {{{"create", processCreate},
             {"setLock", processSetLock},
             {"getLock", processGetLock},
             {"setDisused", processSetDisused},
             {"getDisused", processGetDisused},
             {"serialize", processNoOp},
             {"deserialize", processNoOp}},
            makeProcess};
}

ClassNatives action1DNatives() {
    return {{{"setAction", setAction},
             {"finish", finish},
             {"resetNow", resetNow},
             {"resetTimer", resetTimer},
             {"setTimer", setTimer},
             {"getTimer", getTimer},
             {"setActionTime", setActionTime},
             {"getActionTime", getActionTime},
             {"setSrc", setSrc},
             {"setDst", setDst},
             {"setNow", setNow},
             {"getSrc", getSrc},
             {"getDst", getDst},
             {"getNow", getNow},
             {"getNowNoPower", getNowNoPower},
             {"getRate", getRate},
             {"isActive", isActive},
             {"setActive", setActive},
             {"setPower", setPower},
             {"getPower", getPower},
             {"setTempo", setTempo},
             {"getTempo", getTempo},
             {"getTimeScale", getTimeScale},
             {"setTimeScale", setTimeScale}},
            make};
}

}  // namespace nyan::diesel
