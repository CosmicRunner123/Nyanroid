// Global natives (muramasa_en.exe registers them at 0x4902b0).

#include <cstring>

#include "natives.hpp"

namespace nyan::diesel {

namespace {

// GetScreenWidth() / GetScreenHeight() (0x488380, 0x488390): the app's screen size, setting.ini's
// SCREEN_WIDTH / SCREEN_HEIGHT.
SQInteger getScreenWidth(HSQUIRRELVM v) {
    sq_pushinteger(v, host(v).game.screenWidth());
    return 1;
}
SQInteger getScreenHeight(HSQUIRRELVM v) {
    sq_pushinteger(v, host(v).game.screenHeight());
    return 1;
}

// RandomI(low, high): int / RandomF(low, high): float (0x487fc0 / 0x487fe0): Mware's generator.
SQInteger randomI(HSQUIRRELVM v) {
    sq_pushinteger(v, host(v).random.nextInt(static_cast<int32_t>(argInt(v, 2)), static_cast<int32_t>(argInt(v, 3))));
    return 1;
}
SQInteger randomF(HSQUIRRELVM v) {
    sq_pushfloat(v, host(v).random.nextFloat(argFloat(v, 2), argFloat(v, 3)));
    return 1;
}

// SystemCall(target): int (0x4883f0): leaves full screen, then has the window open `target` (a URL:
// the official site, the manual), and answers -1 whatever happens. The request is kept for the
// session to open.
SQInteger systemCall(HSQUIRRELVM v) {
    auto& h = host(v);
    if (h.fullScreen && h.config.fullScreen) {
        h.config.fullScreen = 0;
        h.applyConfig();
    }
    std::string target = argString(v, 2);
    h.systemCalls.push_back(target);
    if (h.log) h.log("SystemCall: " + target);
    sq_pushinteger(v, -1);
    return 1;
}

// CompareSqObject(a, b): int (0x489560, registered without types): 1 if b is a by a's type: a
// float's bits as floats, a value or reference of the other types by identity (strings are
// interned: equal strings are one object); for null (and any other type) whether b is null. b's
// own type is not looked at (the exe compares the raw words).
SQInteger compareSqObject(HSQUIRRELVM v) {
    HSQOBJECT a, b;
    sq_resetobject(&a);
    sq_resetobject(&b);
    if (sq_gettop(v) >= 2) sq_getstackobj(v, 2, &a);
    if (sq_gettop(v) >= 3) sq_getstackobj(v, 3, &b);
    auto low = [](const HSQOBJECT& o) { return static_cast<uint32_t>(o._unVal.raw); };
    bool same = false;
    switch (a._type) {
    case OT_FLOAT: {
        uint32_t x = low(a), y = low(b);
        float fx, fy;
        std::memcpy(&fx, &x, 4), std::memcpy(&fy, &y, 4);
        same = fx == fy;
        break;
    }
    case OT_INTEGER:
    case OT_BOOL:
    case OT_USERPOINTER: same = low(a) == low(b); break;
    case OT_STRING:
    case OT_ARRAY:
    case OT_CLOSURE:
    case OT_NATIVECLOSURE:
    case OT_GENERATOR:
    case OT_THREAD:
    case OT_FUNCPROTO:
    case OT_CLASS:
    case OT_WEAKREF:
    case OT_TABLE:
    case OT_USERDATA:
    case OT_INSTANCE: same = a._unVal.raw == b._unVal.raw; break;
    default: same = b._type == OT_NULL; break;
    }
    sq_pushinteger(v, same ? 1 : 0);
    return 1;
}

}  // namespace

std::vector<NativeImpl> globalNatives() {
    std::vector<NativeImpl> out = {{"GetScreenWidth", getScreenWidth}, {"GetScreenHeight", getScreenHeight},
                                   {"RandomI", randomI},           {"RandomF", randomF},
                                   {"SystemCall", systemCall},     {"CompareSqObject", compareSqObject}};
    for (const auto& n : fileGlobals()) out.push_back(n);
    for (const auto& n : textGlobals()) out.push_back(n);
    for (const auto& n : vmStateGlobals()) out.push_back(n);
    for (const auto& n : mathGlobals()) out.push_back(n);
    return out;
}

ClassNatives classNatives(const std::string& className) {
    if (className == "SQAction1D") return action1DNatives();
    if (className == "SQProcess") return processNatives();
    if (className == "SQCamera") return cameraNatives();
    if (className == "SQScene") return sceneNatives();
    if (className == "SQParticle") return particleNatives();
    if (className == "SQApp") return appNatives();
    if (className == "SQSprite") return spriteNatives();
    if (className == "SQSpriteMask") return spriteMaskNatives();
    if (className == "SQSpriteRenderTarget") return spriteRenderTargetNatives();
    if (className == "SQDisplacementMap") return displacementMapNatives();
    if (className == "SQSound") return soundNatives();
    if (className == "SQText") return textObjectNatives();
    if (className == "SQFile") return fileNatives();
    if (className == "SQPhysics") return physicsNatives();
    if (className == "SQTrophy") return trophyNatives();
    if (className == "SQN1ScriptReader") return n1ScriptReaderNatives();
    if (className == "SQIniFileReader") return iniFileReaderNatives();
    return {};
}

}  // namespace nyan::diesel
