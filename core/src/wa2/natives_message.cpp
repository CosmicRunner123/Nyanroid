// The script functions of messages, names, the window and novel mode (WA2.exe natives 0x454970..,
// 0x454c00.., 0x455a20, 0x455b30, 0x455dd0, 0x456350..).

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

constexpr int kInt = 3;
constexpr int kString = 5;

}  // namespace

void Game::registerMessageNatives() {
    registerNative("SetMessage", &Game::nSetMessage);
    registerNative("SetMessageE", &Game::nSetMessage);
    registerNative("SetMessage2", &Game::nSetMessage2);
    registerNative("WaitMessage2", &Game::nWaitMessage);
    registerNative("EndMessage", &Game::nEndMessage);
    registerNative("K", &Game::nK);
    registerNative("WN", &Game::nWN);
    registerNative("WNS", &Game::nWN);
    registerNative("WN2", &Game::nWN2);
    registerNative("WNS2", &Game::nWN2);
    registerNative("W", &Game::nW);
    registerNative("WR", &Game::nWR);
    registerNative("NB", &Game::nNB);
    registerNative("NBR", &Game::nNB);
    registerNative("NevelMode", &Game::nNevelMode);
    registerNative("LogOut", &Game::nLogOut);
    registerNative("GetClick", &Game::nGetClick);
    registerNative("SetMovie", &Game::nSetMovie);
}

// SetMessage / SetMessageE(text, line, mode) (0x454970 / 0x4549e0): kind 1 goes on once shown, kind 2
// waits at the end; both wait for the message to be done.
int Game::nSetMessage(ScriptVm&, CallContext& c) {
    if (c.phase == 0) {
        int kind = nativeNames_[static_cast<size_t>(c.function)] == "SetMessage" ? 1 : 2;
        message_.say(c.argString(0), c.argInt(1), c.argInt(2), kind);
    }
    c.phase = 1;
    c.waiting = !message_.done();
    return 1;
}

// SetMessage2(text, line, mode) (0x454a60): kind 2, no wait (WaitMessage2 waits).
int Game::nSetMessage2(ScriptVm&, CallContext& c) {
    message_.say(c.argString(0), c.argInt(1), c.argInt(2), 2);
    return 0;
}

// WaitMessage2() (0x454ad0).
int Game::nWaitMessage(ScriptVm&, CallContext& c) {
    c.waiting = !message_.done();
    return 1;
}

// EndMessage(x) (0x454a50 -> 0x40e6f0): stops a shake started to end with the line.
int Game::nEndMessage(ScriptVm&, CallContext&) {
    effects_.endMessageShake();
    return 0;
}

// K() (0x454af0): waits for a click (or skip, auto, demo).
int Game::nK(ScriptVm&, CallContext& c) {
    c.waiting = !message_.advanceInput(0);
    return 1;
}

// WN(name or n) (0x454c40): a speaker by name, or by index into the exe's (Japanese) table; -1 clears
// the name. Always ends the frame.
int Game::nWN(ScriptVm&, CallContext& c) {
    if (c.args[0].type == kInt) {
        if (c.argInt(0) == -1) message_.setName(nullptr, 0, -1);
        else message_.openWindow(c.argInt(0), 0, -1);
    } else if (c.args[0].type == kString) {
        message_.speak(c.argString(0), -1);
    }
    return 1;
}

// WN2(name or -1, speaker) (0x455e30): as WN with the speaker given.
int Game::nWN2(ScriptVm&, CallContext& c) {
    if (c.args[0].type == kString) {
        message_.speak(c.argString(0), c.argInt(1));
    } else if (c.argInt(0) == -1) {
        message_.setName(nullptr, 0, c.argInt(1));
    }
    return 1;
}

// W(name, fade, style) (0x454c00).
int Game::nW(ScriptVm&, CallContext& c) {
    message_.openWindow(c.argInt(0), c.argInt(2), c.argInt(1));
    return 1;
}

// WR(frames, style) (0x454c20 -> 0x403240).
int Game::nWR(ScriptVm&, CallContext& c) {
    message_.closeWindow(c.argInt(0), c.argInt(1));
    return 1;
}

// NB / NBR(frames, wait) (0x456350 / 0x456390): the novel backdrop out / in.
int Game::nNB(ScriptVm&, CallContext& c) {
    if (c.phase == 0) message_.novelFade(c.argInt(0), nativeNames_[static_cast<size_t>(c.function)] == "NB" ? 1 : 0);
    c.phase = 1;
    if (c.argInt(1) == 0) return 0;
    c.waiting = message_.novelFading();
    return 1;
}

int Game::nNevelMode(ScriptVm&, CallContext& c) {
    message_.novelMode(c.argInt(0));
    return 0;
}

int Game::nLogOut(ScriptVm&, CallContext& c) {
    message_.logOut = c.argInt(0);
    return 0;
}

// GetClick() (0x455b30): advanceInput(1), the auto counter left alone.
int Game::nGetClick(ScriptVm&, CallContext& c) {
    c.returnInt = message_.advanceInput(1) ? 1 : 0;
    return 0;
}

// SetMovie(name, id) (0x455920): starts the movie and waits while it plays.
int Game::nSetMovie(ScriptVm&, CallContext& c) {
    if (c.phase == 0) startMovie(c.argString(0), c.argInt(1));
    c.phase = 1;
    c.waiting = movieActive_;
    return 1;
}

void Game::startMovie(const std::string& name, int id) {
    // 0x4138a0: `name` + 0 / 1 / 2 by the movie quality (mov_lv), from IC/ when the current chapter
    // digit is 1 and the introductory chapter is bundled (the separate install's registry path does
    // not exist here); voice 0 and the effects stop.
    int volume = settings.bgm != 0 && settings.allSound != 0 ? (settings.allVol * settings.bgmVol) >> 8 : 0;
    sound_->stopVoice(0, 0);
    sound_->stopAllSe();
    const char* suffix = settings.movieLevel == 1 ? "1.pak" : settings.movieLevel == 2 ? "0.pak" : "2.pak";
    MovieRequest r;
    r.file = (chapterDigit == 1 && assets_.introductoryChapterBundled() ? "IC/" : "") + name + suffix;
    r.id = id;
    r.volume = volume;
    movieId_ = r.id;
    movieActive_ = true;
    if (!playMovie) {
        endMovie();
    } else if (!playMovie(r)) {
        log("SetMovie: " + r.file + " cannot be played");
        movieActive_ = false;
    }
}

bool Game::movieSkippable() const {
    return movieId_ < 200 || system.gameFlag(movieId_) != 0;
}

void Game::endMovie() {
    modes.skip = false;
    if (movieActive_ && movieId_ >= 200) {
        system.setGameFlag(movieId_, 1);
        writeSystem();
    }
    movieActive_ = false;
}

}  // namespace nyan::wa2
