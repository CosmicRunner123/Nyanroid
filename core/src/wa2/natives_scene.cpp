// The script functions of backgrounds, characters, tone, the camera and the colour settings (WA2.exe
// natives 0x454cd0.., 0x4550f0.., 0x455190.., 0x4555a0.., 0x455bf0.., 0x455ed0..).

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

constexpr int kFloat = 4;

}  // namespace

void Game::registerSceneNatives() {
    registerNative("B", &Game::nB);
    registerNative("BC", &Game::nB);
    registerNative("V", &Game::nB);
    registerNative("H", &Game::nB);
    registerNative("B2", &Game::nB2);
    registerNative("BC2", &Game::nB2);
    registerNative("V2", &Game::nB2);
    registerNative("H2", &Game::nB2);
    registerNative("C", &Game::nC);
    registerNative("CS", &Game::nCS);
    registerNative("CW", &Game::nCW);
    registerNative("CR", &Game::nCR);
    registerNative("CRS", &Game::nCR);
    registerNative("CRW", &Game::nCRW);
    registerNative("CM", &Game::nCM);
    registerNative("F", &Game::nF);
    registerNative("FB", &Game::nF);
    registerNative("SetTimeMode", &Game::nSetTimeMode);
    registerNative("SetChromaMode", &Game::nSetChromaMode);
    registerNative("SetEffctMode", &Game::nSetEffctMode);
    registerNative("S", &Game::nS);
    registerNative("Z", &Game::nZ);
    registerNative("R", &Game::nR);
    registerNative("WSZ", &Game::nWSZ);
    registerNative("StopSZR", &Game::nStopSZR);
}

// The picture number of V / H (0x454df0 / 0x454f40): a1 * 100 + a2 when the script global GameTitle is
// the string 王子 (another Leaf game's), else a1 * 10 + a2. White Album 2 has no such global.
int Game::cgNumber(int a1, int a2) {
    for (const GlobalVar& g : globals_) {
        if (g.name == "GameTitle") return g.type == 5 && g.str == "\x89\xa4\x8e\x71" ? a1 * 100 + a2 : a1 * 10 + a2;
    }
    return a1 * 10 + a2;
}

// B / BC / V / H(trans, a1, a2, frames, soft, x, y, w, h) (0x454cd0 / 0x454d60 / 0x454df0 / 0x454f40 ->
// 0x416650): zoom w / 1280, h / 720; waits for the transition.
int Game::nB(ScriptVm&, CallContext& c) {
    if (c.phase == 0) {
        const std::string& name = nativeNames_[static_cast<size_t>(c.function)];
        int kind = name == "V" ? 1 : name == "H" ? 2 : 0;
        int keep = name == "BC" ? 1 : 0;
        int num = kind == 0 ? c.argInt(2) + c.argInt(1) * 10 : cgNumber(c.argInt(1), c.argInt(2));
        scene_.show(num, c.argInt(0), keep, kind, c.argInt(3), c.argInt(4), c.argInt(5), c.argInt(6),
                    c.argFloat(7) / 1280.0f, c.argFloat(8) / 720.0f, 0, 0);
    }
    c.phase = 1;
    c.waiting = scene_.transitioning();
    return 1;
}

// B2 / BC2 / V2 / H2(trans, a1, a2, frames, soft, x, y, zx, zy, rot, wait) (0x455ed0..): zoom and
// rotation as given; waits only with `wait`.
int Game::nB2(ScriptVm&, CallContext& c) {
    if (c.phase == 0) {
        const std::string& name = nativeNames_[static_cast<size_t>(c.function)];
        int kind = name == "V2" ? 1 : name == "H2" ? 2 : 0;
        int keep = name == "BC2" ? 1 : 0;
        int num = kind == 0 ? c.argInt(2) + c.argInt(1) * 10 : cgNumber(c.argInt(1), c.argInt(2));
        scene_.show(num, c.argInt(0), keep, kind, c.argInt(3), c.argInt(4), c.argInt(5), c.argInt(6),
                    c.argFloat(7), c.argFloat(8), c.argFloat(9), 0);
    }
    if (c.argInt(10) == 0) return 0;
    c.phase = 1;
    c.waiting = scene_.transitioning();
    return 1;
}

// C(id, pic, pos, z, mode, frames, alpha, rgb) (0x455190). A shown character with mode 0 changes in a
// crossfade of the whole scene; otherwise charSet, waiting for the character unless mode 3.
int Game::nC(ScriptVm&, CallContext& c) {
    bool isFloat = c.args[2].type == kFloat;
    if (!scene_.cBusyFlag && scene_.charExists(c.argInt(0)) && c.argInt(4) == 0) {
        if (c.phase == 0) {
            int rgb = c.argInt(7);
            scene_.charSet(c.argInt(0), c.argInt(1), c.argInt(2), c.argFloat(2), isFloat, c.argInt(3), 3, rgb, rgb, rgb,
                           c.argInt(6), 0);
            scene_.show(-20, 1, 1, 0, c.argInt(5), 0, 10000, 10000, 10000.0f, 10000.0f, 10000.0f, 0);
        }
        c.phase = 1;
        c.waiting = scene_.transitioning();
        return 1;
    }
    if (c.phase == 0) {
        int rgb = c.argInt(7);
        scene_.cBusyFlag = true;
        scene_.charSet(c.argInt(0), c.argInt(1), c.argInt(2), c.argFloat(2), isFloat, c.argInt(3), c.argInt(4), rgb, rgb,
                       rgb, c.argInt(6), c.argInt(5));
    }
    c.phase = 1;
    if (c.argInt(4) == 3) {
        c.waiting = false;
        scene_.cBusyFlag = false;
        return 0;
    }
    if (scene_.charBusy(c.argInt(0))) {
        c.waiting = true;
        return 1;
    }
    c.waiting = false;
    scene_.cBusyFlag = false;
    return 1;
}

// CS(same arguments) (0x455cd0): charSet without waiting.
int Game::nCS(ScriptVm&, CallContext& c) {
    if (c.phase == 0) {
        int rgb = c.argInt(7);
        scene_.charSet(c.argInt(0), c.argInt(1), c.argInt(2), c.argFloat(2), c.args[2].type == kFloat, c.argInt(3),
                       c.argInt(4), rgb, rgb, rgb, c.argInt(6), c.argInt(5));
    }
    c.phase = 1;
    return 0;
}

// CW(id, pic, pos, z, alpha, rgb) (0x455330): a deferred change, applied by the next BC.
int Game::nCW(ScriptVm&, CallContext& c) {
    int rgb = c.argInt(5);
    scene_.charSet(c.argInt(0), c.argInt(1), c.argInt(2), c.argFloat(2), c.args[2].type == kFloat, c.argInt(3), 3, rgb,
                   rgb, rgb, c.argInt(4), 0);
    return 0;
}

// CR / CRS(id, mode, frames) (0x455380 / 0x455da0): remove; CR waits unless mode 3.
int Game::nCR(ScriptVm&, CallContext& c) {
    if (c.phase == 0) scene_.charRemove(c.argInt(0), c.argInt(1), c.argInt(2));
    c.phase = 1;
    if (nativeNames_[static_cast<size_t>(c.function)] == "CRS" || c.argInt(1) == 3) {
        c.waiting = false;
        return 0;
    }
    c.waiting = scene_.charBusy(c.argInt(0));
    return 1;
}

// CRW(id) (0x4553e0): a deferred removal.
int Game::nCRW(ScriptVm&, CallContext& c) {
    scene_.charRemove(c.argInt(0), 3, 0);
    return 0;
}

// CM(id, x) (0x455d60): a float position, an int argument converted.
int Game::nCM(ScriptVm&, CallContext& c) {
    scene_.charMove(c.argInt(0), c.args[1].type == kFloat ? c.argFloat(1) : static_cast<float>(c.argInt(1)));
    return 0;
}

// F(fin, fout, r, g, b) / FB(frames, r, g, b) (0x4550f0 / 0x455140 -> 0x401de0): wait for the tone.
int Game::nF(ScriptVm&, CallContext& c) {
    if (c.phase == 0) {
        if (nativeNames_[static_cast<size_t>(c.function)] == "F") {
            scene_.tone(1, c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4));
        } else {
            scene_.tone(0, c.argInt(0), 0, c.argInt(1), c.argInt(2), c.argInt(3));
        }
    }
    c.phase = 1;
    c.waiting = scene_.toneActive();
    return 1;
}

int Game::nSetTimeMode(ScriptVm&, CallContext& c) {
    scene_.timeMode = c.argInt(0);
    return 0;
}

int Game::nSetChromaMode(ScriptVm&, CallContext& c) {
    scene_.setChroma(c.argInt(0), c.argInt(1));
    return 0;
}

int Game::nSetEffctMode(ScriptVm&, CallContext& c) {
    scene_.setEffect(c.argString(0), c.argInt(1));
    return 0;
}

// S(x, y, frames, ease) (0x455bf0 -> 0x401a90).
int Game::nS(ScriptVm&, CallContext& c) {
    scene_.pan(c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3));
    return 0;
}

// Z(zx, zy, frames, ease, px, py) (0x455c10 -> 0x401b80).
int Game::nZ(ScriptVm&, CallContext& c) {
    scene_.zoom(c.argFloat(0), c.argFloat(1), c.argInt(4), c.argInt(5), c.argInt(2), c.argInt(3));
    return 0;
}

// R(angle, frames, ease, px, py) (0x455c40 -> 0x401c60).
int Game::nR(ScriptVm&, CallContext& c) {
    scene_.rotate(c.argFloat(0), c.argInt(3), c.argInt(4), c.argInt(1), c.argInt(2));
    return 0;
}

// WSZ() (0x455c70): waits while the camera moves.
int Game::nWSZ(ScriptVm&, CallContext& c) {
    c.waiting = scene_.cameraMoving();
    return 1;
}

// StopSZR(on, frames) (0x455c90 -> 0x401a70).
int Game::nStopSZR(ScriptVm&, CallContext& c) {
    scene_.pauseCamera(c.argInt(0), c.argInt(1));
    return 0;
}

}  // namespace nyan::wa2
