#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nyan/wa2/script.hpp"
#include "nyan/wa2/serial.hpp"

namespace nyan::wa2 {

// A value on the VM stack (20 bytes in the engine: kind, type, string, float, int).
struct Value {
    enum Kind : int32_t { GlobalRef = 1, LocalRef = 2, StringRef = 3, Literal = 5 };
    int32_t kind = Literal;
    int32_t type = 3;  // 3 int, 4 float, 5 string; a LocalRef keeps its array index here (op 0x1b)
    std::string str;
    float f = 0;
    int32_t i = 0;  // a literal int, or the variable / string number of a reference

    static Value ofInt(int32_t v) {
        Value x;
        x.i = v;
        return x;
    }
    static Value ofFloat(float v) {
        Value x;
        x.type = 4;
        x.f = v;
        return x;
    }
    static Value ofString(std::string v) {
        Value x;
        x.type = 5;
        x.str = std::move(v);
        return x;
    }
};

// A global variable (Global.vrb: type, name, initial value; the table at WA2.exe 0xbc1634): 1 byte,
// 2 short, 3 int, 4 float, 5 string.
struct GlobalVar {
    int type = 3;
    std::string name;
    int32_t i = 0;  // byte, short and int values
    float f = 0;
    std::string str;
};

// The arguments and results of one native call (the engine's context at script object + 0x8b44).
struct CallContext {
    int function = -1;     // the script function's id
    bool waiting = false;  // word 0: the call runs again next frame
    int phase = 0;         // word 1: 0 on the first run; natives set it (the first-call pattern)
    std::array<Value, 16> args;
    int32_t returnInt = 0;  // +0x158
    float returnFloat = 0;  // +0x154
    std::string returnString;  // +0x150 (empty: the engine pushes "")
    int32_t user = 0;  // +0x160: what the script was started with (FUN_00450cf0's second argument)

    int32_t argInt(int i) const { return args[i].i; }
    float argFloat(int i) const { return args[i].f; }
    const std::string& argString(int i) const { return args[i].str; }
};

class ScriptVm;

// What the VM needs from the engine: script files, the function table, the global variables, natives
// and a log.
class ScriptHost {
public:
    virtual ~ScriptHost() = default;
    // The .bnr and .txt of a script in a group ("script"); nullopt if missing.
    virtual std::optional<Bytes> readScriptFile(const std::string& group, const std::string& name) = 0;
    virtual const FunctionTable& functions() const = 0;
    virtual std::vector<GlobalVar>& globals() = 0;
    // Runs a native (id >= 3; 0, 1, 2 are the VM's own SLoad, SCall, call). Returns the engine's
    // yield flag: non-zero ends the VM's work for this frame.
    virtual int callNative(ScriptVm& vm, int id, CallContext& ctx) = 0;
    virtual void log(const std::string& message) = 0;
};

// One script object of the engine (WA2.exe: 0xd190 bytes; run 0x453f10, load 0x453100, start at a
// label 0x450cf0, opcodes 0x4532f0 / 0x451800 / 0x453ad0). Calls (`SCall`, `call`) run in a child
// object with its own locals; `SLoad` replaces the root's script.
class ScriptVm {
public:
    enum State : int { Idle = 0, Ended = 1, Running = 2 };

    explicit ScriptVm(ScriptHost& host, std::string group = "script", ScriptVm* parent = nullptr);
    ~ScriptVm();

    // Loads a script (0x453100): its code, labels and string table; the state becomes Ended until a
    // label starts it. False (logged) if the file is missing or not LSCR.
    bool load(const std::string& name);
    // Starts at a label (0x450cf0): clears the stacks and the locals. False if there is no such label.
    bool start(int label, int32_t user = 0);
    // One frame of the engine's run (0x453f10); returns the state afterwards.
    int run();

    State state() const { return state_; }
    const std::string& name() const { return name_; }
    // The innermost running script (the engine's 0x417390 reports its name).
    const ScriptVm& innermost() const { return child_ ? child_->innermost() : *this; }
    ScriptVm* child() { return child_.get(); }
    uint32_t pc() const { return pc_; }
    int stackDepth() const { return sp_; }

    // The object's state for a save, children included (the engine saves its script objects raw and
    // resumes them where they were, a waiting call included: 0x424310 / 0x454170). The code and the
    // strings are loaded again by name.
    void save(Writer& w) const;
    bool restore(Reader& r);
    const std::vector<std::string>& strings() const { return strings_; }

private:
    // A block record (0x214 bytes at +0xadb4). Pushing one leaves the slot's other fields as an
    // earlier block left them, as the engine does.
    struct Block {
        int type = 0;
        int count = 0;                     // +0x08: switch cases
        uint32_t end = 0;                  // +0x0c
        std::array<uint32_t, 64> a{};      // +0x10: if's else; for's three targets; switch targets
        std::array<int32_t, 64> values{};  // +0x110: switch case values
        int32_t cond = 0;                  // +0x210: the last test's value
    };

    uint32_t read();
    float readFloat();
    void push(Value v);
    Value pop();
    int32_t popInt();
    void storeInt(int32_t v, bool repush);
    void storeFloat(float v, bool repush);
    void storeString(const std::string& v, bool repush);
    int control();
    int op();
    int call();
    int runNative(int id);
    Block& top() { return blocks_[blockTop_]; }
    void pushBlock(int type);
    void doLoad(const std::string& name, int label);
    void deleteChild();
    void error(const std::string& m);
    ScriptVm& root();
    int32_t& localWord(int index);

    ScriptHost& host_;
    ScriptVm* parent_;
    std::unique_ptr<ScriptVm> child_;
    std::string group_;
    std::string name_;
    Script script_;
    std::vector<std::string> strings_;
    State state_ = Idle;
    uint32_t pc_ = 0;
    // The value stack: entries 1..255 as in the engine (sp 0 is empty).
    std::array<Value, 256> stack_;
    int sp_ = 0;
    std::array<Block, 16> blocks_;
    int blockTop_ = 0;
    // Locals: 26 int arrays then 26 float arrays of 16 (one block of words, as the engine's).
    std::array<int32_t, 832> locals_{};
    // The pending instruction (a call that waits runs again without reading).
    bool pending_ = false;
    uint32_t curOp_ = 0;
    int callId_ = 0;
    CallContext ctx_;
    int32_t user_ = 0;
    // SLoad on the root, applied after the frame's instructions.
    bool loadPending_ = false;
    std::string loadName_;
    int loadLabel_ = 0;
};

}  // namespace nyan::wa2
