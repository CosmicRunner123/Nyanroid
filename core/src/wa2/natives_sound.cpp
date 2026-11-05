// The script functions of BGM, sound effects and voices (WA2.exe natives 0x454b30.., 0x455400..).

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

void Game::registerSoundNatives() {
    registerNative("M", &Game::nM);
    registerNative("M2", &Game::nM);
    registerNative("MS", &Game::nMS);
    registerNative("MP", &Game::nMP);
    registerNative("MV", &Game::nMV);
    registerNative("MW", &Game::nMW);
    registerNative("MLW", &Game::nMLW);
    registerNative("SE", &Game::nSE);
    registerNative("SEP", &Game::nSEP);
    registerNative("SES", &Game::nSES);
    registerNative("SEV", &Game::nSEV);
    registerNative("SEW", &Game::nSEW);
    registerNative("SEVW", &Game::nSEVW);
    registerNative("VI", &Game::nVI);
    registerNative("VV", &Game::nVV);
    registerNative("VX", &Game::nVX);
    registerNative("VW", &Game::nVW);
    registerNative("VS", &Game::nVS);
    registerNative("VA", &Game::nVA);
    registerNative("VXV", &Game::nVXV);
    registerNative("EroMode", &Game::nEroMode);
}

// M(num, fade, loop, vol) / M2(num, fade, loop, vol, variant) (0x455400 / 0x456320 -> 0x40fa50). Both
// natives pass the fifth argument word; M has four arguments, so its variant is 0.
int Game::nM(ScriptVm&, CallContext& c) {
    sound_->playBgm(c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3), 0, c.argInt(4));
    return 0;
}

// MS(fade) (0x455430 -> 0x406d50).
int Game::nMS(ScriptVm&, CallContext& c) {
    sound_->stopBgm(c.argInt(0));
    return 0;
}

// MP() (0x455450): nothing.
int Game::nMP(ScriptVm&, CallContext&) {
    return 0;
}

// MV(vol, fade) (0x455460 -> 0x406de0).
int Game::nMV(ScriptVm&, CallContext& c) {
    sound_->bgmVolume(c.argInt(0), c.argInt(1));
    return 0;
}

// MW() / MLW() (0x455480 / 0x4554a0): set the waiting word while the BGM fades but never clear it, so
// a wait that starts never ends. The scripts never call them.
int Game::nMW(ScriptVm&, CallContext& c) {
    if (!sound_->bgmSteady()) c.waiting = true;
    return 1;
}

int Game::nMLW(ScriptVm&, CallContext& c) {
    if (!sound_->bgmStopped()) c.waiting = true;
    return 1;
}

// SE(num, vol) (0x4554c0 -> 0x4064a0).
int Game::nSE(ScriptVm&, CallContext& c) {
    sound_->se(c.argInt(0), c.argInt(1));
    return 0;
}

// SEP(ch, num, fade, loop, vol, mode) (0x4554e0 -> 0x40f2d0).
int Game::nSEP(ScriptVm&, CallContext& c) {
    sound_->sep(c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4), c.argInt(5));
    return 0;
}

// SES(ch, fade) (0x455510 -> 0x40f460).
int Game::nSES(ScriptVm&, CallContext& c) {
    sound_->ses(c.argInt(0), c.argInt(1));
    return 0;
}

// SEV(ch, vol, fade) (0x455530 -> 0x4067f0).
int Game::nSEV(ScriptVm&, CallContext& c) {
    sound_->sev(c.argInt(0), c.argInt(1), c.argInt(2));
    return 0;
}

// SEW(ch, mode) (0x455550 -> 0x40f4c0): waits while the channel's sound plays.
int Game::nSEW(ScriptVm&, CallContext& c) {
    c.waiting = sound_->sewWaiting(c.argInt(0), c.argInt(1));
    return 1;
}

// SEVW(ch) (0x455580 -> 0x4068d0): waits while the volume has reached its target (inverted).
int Game::nSEVW(ScriptVm&, CallContext& c) {
    c.waiting = sound_->sevwSame(c.argInt(0));
    return 1;
}

// VI(x, scene) (0x454b30 -> 0x406190): x is not used.
int Game::nVI(ScriptVm&, CallContext& c) {
    sound_->setScene(c.argInt(1));
    return 0;
}

// VV(char, vol, loop, ch, num) (0x454b50 -> 0x40ecc0, scene -1).
int Game::nVV(ScriptVm&, CallContext& c) {
    sound_->playVoice(c.argInt(3), c.argInt(0), -1, c.argInt(4), c.argInt(1), c.argInt(2), 0);
    return 0;
}

// VX(char, num, scene, vol, loop, ch) (0x454b80 -> 0x40ecc0).
int Game::nVX(ScriptVm&, CallContext& c) {
    sound_->playVoice(c.argInt(5), c.argInt(0), c.argInt(2), c.argInt(1), c.argInt(3), c.argInt(4), 0);
    return 0;
}

// VW(ch, mode) (0x454bb0 -> 0x40ec40): waits until the voice is done.
int Game::nVW(ScriptVm&, CallContext& c) {
    c.waiting = !sound_->voiceDone(c.argInt(0), c.argInt(1), 0);
    return 1;
}

// VS(fade, ch) (0x454be0 -> 0x40eb80).
int Game::nVS(ScriptVm&, CallContext& c) {
    sound_->stopVoice(c.argInt(1), c.argInt(0));
    return 0;
}

// VA(n) (0x455cb0 -> 0x406170).
int Game::nVA(ScriptVm&, CallContext& c) {
    sound_->addOffset(c.argInt(0));
    return 0;
}

// VXV(vol, frames, ch) (0x4563d0 -> 0x4061b0).
int Game::nVXV(ScriptVm&, CallContext& c) {
    sound_->fadeVoice(c.argInt(2), c.argInt(1), c.argInt(0));
    return 0;
}

// EroMode(on, x) (0x455df0 -> 0x4034b0): with the ero_voice setting, character 0 goes unvoiced.
int Game::nEroMode(ScriptVm&, CallContext& c) {
    sound_->eroMode = c.argInt(0) != 0;
    return 0;
}

}  // namespace nyan::wa2
