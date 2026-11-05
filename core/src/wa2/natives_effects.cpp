// The script functions of shakes, weather, free pictures and the date card (WA2.exe natives 0x455090..,
// 0x455600.., 0x4556a0..0x455900, 0x455aa0, 0x456250.., 0x4562d0).

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

void Game::registerEffectNatives() {
    registerNative("SetShake", &Game::nSetShake);
    registerNative("SetShake2", &Game::nSetShake);
    registerNative("StopShake", &Game::nStopShake);
    registerNative("SetWeather", &Game::nSetWeather);
    registerNative("SetWeather2", &Game::nSetWeather);
    registerNative("ChangeWeather", &Game::nChangeWeather);
    registerNative("ChangeWeather2", &Game::nChangeWeather);
    registerNative("ResetWeather", &Game::nResetWeather);
    registerNative("LoadBmp", &Game::nLoadBmp);
    registerNative("LoadBmpAnime", &Game::nLoadBmpAnime);
    registerNative("SetBmpAvi", &Game::nSetBmpAvi);
    registerNative("WaitBmpAvi", &Game::nWaitBmpAvi);
    registerNative("WaitBmpAnime", &Game::nWaitBmpAnime);
    registerNative("ReleaseBmp", &Game::nReleaseBmp);
    for (const char* n : {"SetBmpAnimePlay", "SetBmpDisp", "SetBmpLayer", "SetBmpParam", "SetBmpRevParam", "SetBmpBright",
                          "SetBmpMove", "SetBmpPos", "SetBmpZoom", "SetBmpZoom2", "SetBmpRoll"}) {
        registerNative(n, &Game::nSetBmp);
    }
    registerNative("Calender", &Game::nCalender);
}

// SetShake(type, amp, dur, dir, speed) / SetShake2(..., wait) (0x455090 / 0x4562d0): waits while a
// finite shake runs (SetShake2 only with `wait`).
int Game::nSetShake(ScriptVm&, CallContext& c) {
    if (c.phase == 0) effects_.shake(c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4));
    c.phase = 1;
    if (nativeNames_[static_cast<size_t>(c.function)] == "SetShake2" && c.argInt(5) == 0) return 0;
    c.waiting = effects_.shaking();
    return 1;
}

int Game::nStopShake(ScriptVm&, CallContext&) {
    effects_.stopShake();
    return 0;
}

// SetWeather(type, wind, fall, count, layer, mode) / SetWeather2(..., margin) (0x455600 / 0x456250).
int Game::nSetWeather(ScriptVm&, CallContext& c) {
    int margin = nativeNames_[static_cast<size_t>(c.function)] == "SetWeather2" ? c.argInt(6) : 0;
    effects_.weather(c.argInt(0), c.argInt(1), c.argInt(2), margin, c.argInt(3), c.argInt(4), c.argInt(5));
    return 0;
}

// ChangeWeather(wind, fall, count, layer, mode) / ChangeWeather2(..., margin) (0x455630 / 0x456280):
// -1000 keeps a value.
int Game::nChangeWeather(ScriptVm&, CallContext& c) {
    int margin = nativeNames_[static_cast<size_t>(c.function)] == "ChangeWeather2" ? c.argInt(5) : 0;
    effects_.changeWeather(c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4), margin);
    return 0;
}

// ResetWeather() (0x455670): the particles leave by themselves; waits until they are gone.
int Game::nResetWeather(ScriptVm&, CallContext& c) {
    if (c.phase == 0) effects_.resetWeather(false);
    c.phase = 1;
    bool gone = effects_.weatherGone();
    if (gone && effects_.weatherFading()) effects_.resetWeather(true);  // 0x406f60
    c.waiting = !gone;
    return 1;
}

// LoadBmp(slot, file, layer, colour, amp, level, group, fixed) (0x4556a0 -> 0x424f90).
int Game::nLoadBmp(ScriptVm&, CallContext& c) {
    effects_.loadPicture(c.argInt(0), c.argString(1), c.argInt(2), c.argInt(3), c.argString(4), c.argInt(5), c.argString(6),
                         c.argInt(7));
    return 0;
}

// LoadBmpAnime(slot, file, layer, amp, level, group, fixed) (0x4556e0 -> 0x4252d0).
int Game::nLoadBmpAnime(ScriptVm&, CallContext& c) {
    effects_.loadAnime(c.argInt(0), c.argString(1), c.argInt(2), c.argString(3), c.argInt(4), c.argString(5), c.argInt(6));
    return 0;
}

// SetBmpAvi (0x455710): a movie as a picture; no script calls it.
int Game::nSetBmpAvi(ScriptVm&, CallContext& c) {
    log("SetBmpAvi(" + c.argString(1) + "): movies as pictures are not played");
    return 0;
}

// WaitBmpAvi (0x455730): yields one frame, never waits.
int Game::nWaitBmpAvi(ScriptVm&, CallContext&) {
    return 1;
}

// WaitBmpAnime (0x455770): returns 0, a no-op.
int Game::nWaitBmpAnime(ScriptVm&, CallContext&) {
    return 0;
}

int Game::nReleaseBmp(ScriptVm&, CallContext& c) {
    effects_.releasePicture(c.argInt(0));
    return 0;
}

// The SetBmp... setters (0x455790..0x455900).
int Game::nSetBmp(ScriptVm&, CallContext& c) {
    const std::string& n = nativeNames_[static_cast<size_t>(c.function)];
    int s = c.argInt(0);
    if (n == "SetBmpAnimePlay") effects_.animePlay(s, c.argInt(1), c.argInt(2), c.argInt(3));
    else if (n == "SetBmpDisp") effects_.pictureVisible(s, c.argInt(1) != 0);
    else if (n == "SetBmpLayer") effects_.pictureLayer(s, c.argInt(1));
    else if (n == "SetBmpParam") effects_.pictureParam(s, c.argInt(1), c.argInt(2), c.argInt(3));
    else if (n == "SetBmpRevParam") effects_.pictureMirror(s, c.argInt(1));
    else if (n == "SetBmpBright") effects_.pictureBright(s, c.argInt(1), c.argInt(2), c.argInt(3));
    else if (n == "SetBmpMove") effects_.pictureMove(s, c.argInt(1), c.argInt(2));
    else if (n == "SetBmpPos") effects_.picturePos(s, c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4), c.argInt(5), c.argInt(6));
    else if (n == "SetBmpZoom") effects_.pictureZoom(s, c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4));
    else if (n == "SetBmpZoom2") effects_.pictureZoom2(s, c.argInt(1), c.argInt(2), c.argFloat(3));
    else if (n == "SetBmpRoll") effects_.pictureRoll(s, c.argInt(1), c.argInt(2), c.argInt(3));
    return 0;
}

// Calender(year, month, day, weekday, show) (0x455aa0 -> 0x401320): waits while the card shows.
int Game::nCalender(ScriptVm&, CallContext& c) {
    if (c.phase == 0) effects_.calendar(c.argInt(0), c.argInt(1), c.argInt(2), c.argInt(3), c.argInt(4));
    c.phase = 1;
    c.waiting = effects_.calendarShown();
    return 1;
}

}  // namespace nyan::wa2
