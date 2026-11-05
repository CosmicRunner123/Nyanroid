// The script functions of control flow, timing, flags and choices (WA2.exe natives 0x4545a0..,
// tables 0x4c25c0 / 0x4c2610).

#include <cmath>

#include "nyan/wa2/game.hpp"

namespace nyan::wa2 {

namespace {

int32_t ftol(float f) {
    if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) return INT32_MIN;
    return static_cast<int32_t>(f);
}

// A script number from its name (0x44e4c0: atoi over the leading digits, a leading '-' negates).
int scriptNumberOf(const std::string& name) {
    size_t i = 0;
    int sign = 1;
    if (!name.empty() && name[0] == '-') sign = -1, i = 1;
    int v = 0;
    for (; i < name.size() && name[i] >= '0' && name[i] <= '9'; ++i) v = v * 10 + (name[i] - '0');
    return v * sign;
}

// Scripts outside Game::chosenTable keep their "chosen before" bits in this flag (0x4046a0).
constexpr int kChosenFlagDefault = 0x3e0;

}  // namespace

void Game::registerNative(const char* name, Native fn) {
    const ScriptFunction* f = functions_.find(std::string_view(name));
    if (!f) return log(std::string("no script function ") + name);
    natives_[static_cast<size_t>(f->id)] = fn;
    nativeNames_[static_cast<size_t>(f->id)] = name;
}

void Game::registerNatives() {
    auto reg = [&](const char* name, Native fn) { registerNative(name, fn); };
    reg("run", &Game::nRun);
    reg("print", &Game::nPrint);
    reg("printEx", &Game::nPrint);
    reg("printEx2", &Game::nPrint);
    reg("ret", &Game::nRet);
    reg("_int", &Game::nInt);
    reg("_float", &Game::nFloat);
    reg("rand", &Game::nRand);
    for (const char* m : {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt"}) reg(m, &Game::nMath);
    reg("timeGetTime", &Game::nTimeGetTime);
    reg("Wait", &Game::nWait);
    reg("Wait2", &Game::nWait2);
    reg("StartTimer", &Game::nStartTimer);
    reg("GetTimer", &Game::nGetTimer);
    reg("WaitTimer", &Game::nWaitTimer);
    reg("GetGameFlag", &Game::nGetGameFlag);
    reg("SetGameFlag", &Game::nSetGameFlag);
    reg("V_Flag", &Game::nVFlag);
    reg("H_Flag", &Game::nHFlag);
    reg("GetSkip", &Game::nGetSkip);
    reg("GetSkip2", &Game::nGetSkip2);
    reg("IfSkip", &Game::nIfSkip);
    reg("SkipOFF", &Game::nSkipOff);
    reg("runEX", &Game::nRunEx);
    reg("GetReplayMode", &Game::nGetReplayMode);
    reg("SetDemoMode", &Game::nSetDemoMode);
    reg("SetSelectMess", &Game::nSetSelectMess);
    reg("SetSelect", &Game::nSetSelect);
    reg("GoTitle", &Game::nGoTitle);
    if (assets_.build() == Build::Special) {
        reg("VWait", &Game::nVWait);
        for (const char* m : {"EyeCatch", "InfoAdditionalContent", "Trophy"}) reg(m, &Game::nDebugBox);
        reg("GetTrialFlag", &Game::nGetTrialFlag);
    }
    registerSoundNatives();
    registerSceneNatives();
    registerMessageNatives();
    registerEffectNatives();
}

// run (0x4545a0): ends the frame.
int Game::nRun(ScriptVm&, CallContext&) {
    return 1;
}

// print / printEx (0x4545b0 / 0x4548c0): the engine's debug log.
int Game::nPrint(ScriptVm& vm, CallContext& c) {
    const Value& v = c.args[0];
    if (v.type == 3) log("print[" + std::to_string(v.i) + "] in " + vm.name());
    else if (v.type == 4) log("print[" + std::to_string(v.f) + "] in " + vm.name());
    else if (v.type == 5) log("print[" + v.str + "] in " + vm.name());
    return 0;
}

// ret (0x454650): nothing; a called label ends with the script's end instruction.
int Game::nRet(ScriptVm&, CallContext&) {
    return 0;
}

int Game::nInt(ScriptVm&, CallContext& c) {
    if (c.args[0].type == 3) c.returnInt = c.args[0].i;
    else if (c.args[0].type == 4) c.returnInt = ftol(c.args[0].f);
    return 0;
}

int Game::nFloat(ScriptVm&, CallContext& c) {
    if (c.args[0].type == 3) c.returnFloat = static_cast<float>(c.args[0].i);
    else if (c.args[0].type == 4) c.returnFloat = c.args[0].f;
    return 0;
}

// rand(n) (0x4546d0): rand() % n, 0 for n < 1.
int Game::nRand(ScriptVm&, CallContext& c) {
    int n = c.argInt(0);
    c.returnInt = n < 1 ? 0 : rand.next() % n;
    return 0;
}

// sin, cos, tan take degrees (x (float)pi/180 through a float, 0x454700); asin, acos, atan, atan2
// return degrees (x 57.2957763671875, 0x454790); results are floats.
int Game::nMath(ScriptVm&, CallContext& c) {
    const ScriptFunction* f = functions_.find(c.function);
    std::string name = f ? f->name : "";
    constexpr double kDegToRad = 0.01745329238474369;  // 0x4a22f0
    constexpr double kRadToDeg = 57.2957763671875;     // 0x4a6b40
    double a = c.argFloat(0), b = c.argFloat(1);
    auto rad = [&] { return static_cast<double>(static_cast<float>(a * kDegToRad)); };
    auto deg = [&](double r) { return static_cast<float>(static_cast<double>(static_cast<float>(r)) * kRadToDeg); };
    if (name == "sin") c.returnFloat = static_cast<float>(std::sin(rad()));
    else if (name == "cos") c.returnFloat = static_cast<float>(std::cos(rad()));
    else if (name == "tan") c.returnFloat = static_cast<float>(std::tan(rad()));
    else if (name == "asin") c.returnFloat = deg(std::asin(a));
    else if (name == "acos") c.returnFloat = deg(std::acos(a));
    else if (name == "atan") c.returnFloat = deg(std::atan(a));
    else if (name == "atan2") c.returnFloat = deg(std::atan2(a, b));  // atan2(arg0, arg1), 0x454820
    else if (name == "pow") c.returnFloat = static_cast<float>(std::pow(a, b));
    else if (name == "sqrt") c.returnFloat = static_cast<float>(std::sqrt(a));
    return 0;
}

int Game::nTimeGetTime(ScriptVm&, CallContext& c) {
    c.returnInt = static_cast<int32_t>(milliseconds());
    return 0;
}

// 0x40af60: the shared wait counter. Frames (counted from 1 on the first call) or milliseconds;
// skipping ends it unless `noSkip`.
bool Game::waitFrames(uint32_t amount, int ms, int noSkip, int extra) {
    uint32_t target;
    if (waitCounter == 0) {
        waitCounter = 1;
        target = ms ? milliseconds() + amount : amount;
    } else {
        ++waitCounter;
        target = waitTarget;
        if (ms == 0) {
            if (static_cast<int>(waitTarget) <= waitCounter) {
                waitCounter = 0;
                return true;
            }
        } else if (waitTarget <= milliseconds()) {
            waitCounter = 0;
            return true;
        }
    }
    waitTarget = target;
    // 0x401250 is the advance key (0xa39770); 0xa397a8 scrolling down.
    bool anyKey = keys.advance || modes.forceSkip || modes.skip;
    if (noSkip == 0 &&
        ((modes.demo == 0 && (skipActive() || (extra == 1 && (keys.advance || keys.scrollDown) && currentTextRead() != 0))) ||
         (anyKey && modes.demo == 1 && currentTextRead() != 0))) {
        waitCounter = 0;
        return true;
    }
    return false;
}

// Wait(amount, ms, noSkip) / Wait2(..., extra) (0x455960 / 0x4563f0).
int Game::nWait(ScriptVm&, CallContext& c) {
    c.waiting = !waitFrames(static_cast<uint32_t>(c.argInt(0)), c.argInt(1), c.argInt(2), 0);
    return 1;
}

int Game::nWait2(ScriptVm&, CallContext& c) {
    c.waiting = !waitFrames(static_cast<uint32_t>(c.argInt(0)), c.argInt(1), c.argInt(2), c.argInt(3));
    return 1;
}

int Game::nStartTimer(ScriptVm&, CallContext&) {
    timerStart = milliseconds();
    return 0;
}

int Game::nGetTimer(ScriptVm&, CallContext& c) {
    c.returnInt = static_cast<int32_t>(milliseconds() - timerStart);
    return 0;
}

// WaitTimer(ms) (0x4559a0 -> 0x40b330): until the timer reaches ms; the skip keys end it on read text.
int Game::nWaitTimer(ScriptVm&, CallContext& c) {
    bool done = timerStart + static_cast<uint32_t>(c.argInt(0)) <= milliseconds() ||
                ((keys.advance || modes.forceSkip || modes.skip) && currentTextRead() != 0);
    c.waiting = !done;
    return 1;
}

int Game::nGetGameFlag(ScriptVm&, CallContext& c) {
    c.returnInt = system.gameFlag(c.argInt(0));
    return 0;
}

// SetGameFlag (0x455a00 -> 0x405370): flag 99 is refused (logged and set to 0); the system data is
// written at once.
int Game::nSetGameFlag(ScriptVm&, CallContext& c) {
    int n = c.argInt(0), v = c.argInt(1);
    if (n == 99) {
        log("SetGameFlag: flag 99 is the engine's");
        v = 0;
    }
    system.setGameFlag(n, v);
    writeSystem();
    return 0;
}

// V_Flag / H_Flag(a, b, value) (0x455a40 / 0x455a70 -> 0x4052e0): the gallery's flag a * 10 + b.
int Game::nVFlag(ScriptVm&, CallContext& c) {
    system.setGalleryFlag(0, c.argInt(1) + c.argInt(0) * 10, static_cast<uint8_t>(c.argInt(2)));
    writeSystem();
    return 0;
}

int Game::nHFlag(ScriptVm&, CallContext& c) {
    system.setGalleryFlag(1, c.argInt(1) + c.argInt(0) * 10, static_cast<uint8_t>(c.argInt(2)));
    writeSystem();
    return 0;
}

int Game::nGetSkip(ScriptVm&, CallContext& c) {
    c.returnInt = skipActive() ? 1 : 0;
    return 0;
}

int Game::nGetSkip2(ScriptVm&, CallContext& c) {
    c.returnInt = skipActive2() ? 1 : 0;
    return 0;
}

// IfSkip (0x456420 -> 0x405220(2)).
int Game::nIfSkip(ScriptVm&, CallContext&) {
    ifSkip = 2;
    return 0;
}

// SkipOFF (0x455dc0): skip and auto off.
int Game::nSkipOff(ScriptVm&, CallContext&) {
    modes.skip = false;
    modes.autoMode = false;
    return 0;
}

// runEX (0x455b50): `mov eax, 1` into 0x4c223c and as the return value, so it ends the frame (the
// scripts' busy loops call it once per frame).
int Game::nRunEx(ScriptVm&, CallContext&) {
    runEx = 1;
    return 1;
}

// VWait(ms) (Special 0x458590 -> 0x40e6d0): waits until `ms` milliseconds have passed since the last
// recorded voice started on channel 0, or until the advance test (0x40a910) lets go.
int Game::nVWait(ScriptVm&, CallContext& c) {
    bool done = static_cast<uint32_t>(c.argInt(0)) <= milliseconds() - sound_->voiceStartTime ||
                message_.advanceInput(0);
    c.waiting = !done;
    return 1;
}

// EyeCatch, InfoAdditionalContent, Trophy (Special 0x4585c0 / 0x4585e0 / 0x458600): the console
// version's hooks, which on the PC put up a message box ("Trophy[ 3 ]", 0x44dde0). Only the debug
// scripts 4030 and LF3 call them; Nyanroid logs the message.
int Game::nDebugBox(ScriptVm&, CallContext& c) {
    log(nativeNames_[static_cast<size_t>(c.function)] + "[ " + std::to_string(c.argInt(0)) + " ]");
    return 0;
}

// GetTrialFlag (Special 0x458620): returns without setting a result (not a trial).
int Game::nGetTrialFlag(ScriptVm&, CallContext&) {
    return 0;
}

int Game::nGetReplayMode(ScriptVm&, CallContext& c) {
    c.returnInt = modes.replay;
    return 0;
}

// SetDemoMode(mode, b) (0x454b10 -> 0x401230).
int Game::nSetDemoMode(ScriptVm&, CallContext& c) {
    modes.demo = c.argInt(0);
    modes.demoArg = c.argInt(1);
    return 0;
}

// SetSelectMess(text, flag, value, always) (0x455b60 -> 0x404530): an option, shown when `always` is
// set or game flag `flag` equals `value`; up to 8.
int Game::nSetSelectMess(ScriptVm&, CallContext& c) {
    addChoice(c.argString(0), c.argInt(1), c.argInt(2), c.argInt(3));
    return 0;
}

void Game::addChoice(const std::string& text, int flag, int value, int always) {
    // 0x404530
    if (choiceOptions.size() >= 8) return;
    ChoiceOption o{text, flag, value, always};
    int index = static_cast<int>(choiceOptions.size());
    bool shown = o.always != 0 || system.gameFlag(o.flag) == o.value;
    choiceOptions.push_back(std::move(o));
    if (shown && visibleChoices.size() < 8) visibleChoices.push_back(index);
}

// SetSelect(id) (0x455b80): shows the options (0x4046a0) and waits for the pick (0x405150); returns
// the option's index among all the SetSelectMess calls. A pick sets the option's bit in the
// "chosen before" game flag (0x413400) and clears the options (0x404d10).
int Game::nSetSelect(ScriptVm& vm, CallContext& c) {
    if (c.phase == 0) {
        choiceShown = true;
        message_.showChoices(c.argInt(0), scriptNumberOf(currentScript()));
    }
    c.phase = 1;
    if (choiceResult == -1 && choose && choiceShown) {
        std::vector<int> options = message_.selectableChoices();
        std::vector<std::string> texts;
        for (int i : options) texts.push_back(choiceOptions[static_cast<size_t>(i)].text);
        int pick = choose(texts);
        if (pick >= 0 && pick < static_cast<int>(options.size())) pickChoice(options[static_cast<size_t>(pick)]);
    }
    c.returnInt = choiceResult;
    c.waiting = choiceResult == -1;
    (void)vm;
    return 1;
}

void Game::pickChoice(int option) {
    message_.pick(option);
}

int Game::choiceFlagIndex() const {
    for (size_t j = 0; j < chosenTable.size(); ++j) {
        if (chosenTable[j] == choiceScript) return choiceId + 900 + static_cast<int>(j) * 4;
    }
    return kChosenFlagDefault;
}

// GoTitle (0x4559c0 -> 0x413ac0): back to the title; the call never returns.
int Game::nGoTitle(ScriptVm&, CallContext& c) {
    // 0x4559c0: 0x413ac0 once; the call never ends (the title takes over).
    if (c.phase == 0) goTitle();
    c.phase = 1;
    c.waiting = true;
    return 1;
}

}  // namespace nyan::wa2
