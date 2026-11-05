#include "nyan/wa2/script_vm.hpp"

#include <bit>
#include <climits>
#include <cmath>
#include <cstring>

namespace nyan::wa2 {

namespace {

constexpr int kStackMax = 0xfe;  // the engine refuses a push past entry 0xfe (0x450e40)
constexpr int kBlocks = 16;
constexpr int kLocalArrays = 26;
constexpr int kLocalSize = 16;
constexpr int kFloatLocals = kLocalArrays * kLocalSize;  // where the float arrays start

// The C runtime's float-to-int conversion (_ftol): truncation; out of range gives INT_MIN.
int32_t ftol(float f) {
    if (std::isnan(f) || f >= 2147483648.0f || f < -2147483648.0f) return INT32_MIN;
    return static_cast<int32_t>(f);
}

}  // namespace

ScriptVm::ScriptVm(ScriptHost& host, std::string group, ScriptVm* parent)
    : host_(host), parent_(parent), group_(std::move(group)) {}

ScriptVm::~ScriptVm() = default;

void ScriptVm::error(const std::string& m) {
    host_.log("script " + name_ + " at " + std::to_string(pc_) + ": " + m);
}

ScriptVm& ScriptVm::root() {
    ScriptVm* v = this;
    while (v->parent_) v = v->parent_;
    return *v;
}

int32_t& ScriptVm::localWord(int index) {
    static int32_t sink = 0;
    if (index < 0 || index >= static_cast<int>(locals_.size())) {
        error("local index " + std::to_string(index) + " outside the locals");
        sink = 0;
        return sink;
    }
    return locals_[static_cast<size_t>(index)];
}

uint32_t ScriptVm::read() {
    uint32_t w = script_.word(pc_);
    pc_ += 4;
    return w;
}

float ScriptVm::readFloat() {
    return std::bit_cast<float>(read());
}

bool ScriptVm::load(const std::string& name) {
    auto code = host_.readScriptFile(group_, name + ".bnr");
    if (!code) {
        host_.log("script " + name + ".bnr: not found");
        return false;
    }
    try {
        script_ = Script::parse(std::move(*code));
    } catch (const std::exception& e) {
        host_.log("script " + name + ".bnr: " + e.what());
        return false;
    }
    name_ = name;
    sp_ = 0;
    for (auto& v : stack_) v = Value{};
    blocks_ = {};
    blockTop_ = 0;
    loadPending_ = false;
    pc_ = script_.codeStart;
    state_ = Ended;
    strings_.clear();
    if (auto text = host_.readScriptFile(group_, name + ".txt")) {
        strings_ = splitStrings(std::string(text->begin(), text->end()));
    } else {
        host_.log("script " + name + ".txt: not found");
    }
    return true;
}

bool ScriptVm::start(int label, int32_t user) {
    user_ = user;
    auto it = script_.labels.find(label);
    if (it == script_.labels.end()) return false;
    pc_ = it->second;
    state_ = Running;
    pending_ = false;
    sp_ = 0;
    for (auto& v : stack_) v = Value{};
    locals_ = {};
    blocks_ = {};
    blockTop_ = 0;
    return true;
}

void ScriptVm::push(Value v) {
    if (sp_ > kStackMax) {
        error("stack full");
        return;
    }
    stack_[++sp_] = std::move(v);
}

Value ScriptVm::pop() {
    Value out;
    if (sp_ < 1) {
        sp_ = 0;
        return out;
    }
    Value& e = stack_[sp_];
    switch (e.kind) {
    case Value::GlobalRef: {
        auto& g = host_.globals();
        if (e.i < 0 || e.i >= static_cast<int>(g.size())) {
            error("global " + std::to_string(e.i) + " does not exist");
            break;
        }
        const GlobalVar& v = g[static_cast<size_t>(e.i)];
        switch (v.type) {
        case 1: out.i = static_cast<uint8_t>(v.i); break;
        case 2: out.i = static_cast<int16_t>(v.i); break;
        case 3: out.i = v.i; break;
        case 4: out.type = 4, out.f = v.f; break;
        case 5: out.type = 5, out.str = v.str; break;
        default: error("global " + v.name + " has type " + std::to_string(v.type)); break;
        }
        break;
    }
    case Value::LocalRef: {
        // The index op (0x1b) wrote the element into the reference's type field.
        if (e.type < 0) break;
        int slot = (e.i % kLocalArrays) * kLocalSize + e.type;
        if (e.i / kLocalArrays != 0) {
            out.type = 4;
            out.f = std::bit_cast<float>(localWord(kFloatLocals + slot));
        } else {
            out.i = localWord(slot);
        }
        break;
    }
    case Value::StringRef:
        out.type = 5;
        if (e.i >= 0 && e.i < static_cast<int>(strings_.size())) out.str = strings_[static_cast<size_t>(e.i)];
        else error("string " + std::to_string(e.i) + " past the table");
        break;
    case Value::Literal:
        out.type = e.type;
        out.str = std::move(e.str);
        out.f = e.f;
        out.i = e.i;
        e = Value{};
        e.kind = 0;  // the engine clears the entry
        break;
    default: error("stack entry of kind " + std::to_string(e.kind)); break;
    }
    // References stay in their slot: an assignment pushes the slot again to store into it.
    --sp_;
    return out;
}

int32_t ScriptVm::popInt() {
    Value v = pop();
    return v.type == 3 ? v.i : ftol(v.f);
}

void ScriptVm::storeInt(int32_t v, bool repush) {
    if (repush) {
        if (sp_ > kStackMax) return error("stack full");
        ++sp_;
    }
    Value& e = stack_[sp_];
    if (e.kind == Value::LocalRef) {
        if (e.type < 0) return;
        int slot = (e.i % kLocalArrays) * kLocalSize + e.type;
        if (e.i / kLocalArrays != 0) localWord(kFloatLocals + slot) = std::bit_cast<int32_t>(static_cast<float>(v));
        else localWord(slot) = v;
        return;
    }
    if (e.kind != Value::GlobalRef) return error("store into a stack entry of kind " + std::to_string(e.kind));
    auto& g = host_.globals();
    if (e.i < 0 || e.i >= static_cast<int>(g.size())) return error("global " + std::to_string(e.i) + " does not exist");
    GlobalVar& gv = g[static_cast<size_t>(e.i)];
    switch (gv.type) {
    case 1: gv.i = static_cast<uint8_t>(v); break;
    case 2: gv.i = static_cast<int16_t>(v); break;
    case 3: gv.i = v; break;
    case 4: gv.f = static_cast<float>(v); break;
    default: error("int stored into global " + gv.name); break;
    }
}

void ScriptVm::storeFloat(float v, bool repush) {
    if (repush) {
        if (sp_ > kStackMax) return error("stack full");
        ++sp_;
    }
    Value& e = stack_[sp_];
    if (e.kind == Value::LocalRef) {
        if (e.type < 0) return;
        int slot = (e.i % kLocalArrays) * kLocalSize + e.type;
        if (e.i / kLocalArrays != 0) localWord(kFloatLocals + slot) = std::bit_cast<int32_t>(v);
        else localWord(slot) = ftol(v);
        return;
    }
    if (e.kind != Value::GlobalRef) return error("store into a stack entry of kind " + std::to_string(e.kind));
    auto& g = host_.globals();
    if (e.i < 0 || e.i >= static_cast<int>(g.size())) return error("global " + std::to_string(e.i) + " does not exist");
    GlobalVar& gv = g[static_cast<size_t>(e.i)];
    switch (gv.type) {
    case 1: gv.i = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(v))); break;  // x87 rounding
    case 2: gv.i = static_cast<int16_t>(ftol(v)); break;
    case 3: gv.i = ftol(v); break;
    case 4: gv.f = v; break;
    default: error("float stored into global " + gv.name); break;
    }
}

void ScriptVm::storeString(const std::string& v, bool repush) {
    if (repush) {
        if (sp_ > kStackMax) return error("stack full");
        ++sp_;
    }
    Value& e = stack_[sp_];
    if (e.kind == Value::LocalRef) return error("string stored into a local");
    if (e.kind != Value::GlobalRef) return error("store into a stack entry of kind " + std::to_string(e.kind));
    auto& g = host_.globals();
    if (e.i < 0 || e.i >= static_cast<int>(g.size())) return error("global " + std::to_string(e.i) + " does not exist");
    GlobalVar& gv = g[static_cast<size_t>(e.i)];
    if (gv.type != 5) return error("string stored into global " + gv.name);
    gv.str = v;
}

void ScriptVm::pushBlock(int type) {
    if (blockTop_ < kBlocks - 1) ++blockTop_;
    else error("blocks nested too deep");
    top().type = type;
}

int ScriptVm::control() {
    uint32_t sub = read();
    switch (sub) {
    case 0: break;
    case 1:
        state_ = Ended;
        return 1;
    case 2:  // if: else target, end
        pushBlock(2);
        top().a[0] = read();
        top().end = read();
        break;
    case 3:  // else: when the condition held, jump to the end
        if (top().cond == 0) {
            top().type = 3;
            top().a[0] = read();
        } else {
            pc_ = top().end;
        }
        break;
    case 4:
        if (top().cond != 0) pc_ = top().end;
        break;
    case 5:  // for: end, then three targets
        pushBlock(5);
        top().end = read();
        top().a[0] = read();
        top().a[1] = read();
        top().a[2] = read();
        break;
    case 6:
        pushBlock(6);
        top().end = read();
        break;
    case 7: {  // switch: case targets and values, end
        pushBlock(7);
        top().count = static_cast<int>(read());
        for (int i = 0; i < top().count; ++i) {
            uint32_t target = read();
            uint32_t value = read();
            if (i < 64) top().a[static_cast<size_t>(i)] = target, top().values[static_cast<size_t>(i)] = static_cast<int32_t>(value);
        }
        top().end = read();
        break;
    }
    case 8:
    case 9: break;  // debugger output
    case 10: {  // break: the innermost loop or switch
        int i = blockTop_;
        for (; i >= 0; --i) {
            int t = blocks_[static_cast<size_t>(i)].type;
            if (t == 5 || t == 6 || t == 7) {
                blockTop_ = i;
                pc_ = blocks_[static_cast<size_t>(i)].end;
                break;
            }
        }
        if (i < 0) error("break outside a loop");
        break;
    }
    case 11: {  // continue
        int i = blockTop_;
        for (; i >= 0; --i) {
            const Block& b = blocks_[static_cast<size_t>(i)];
            if (b.type == 5) {
                blockTop_ = i;
                pc_ = b.a[2];
                break;
            }
            if (b.type == 6) {
                blockTop_ = i;
                pc_ = b.a[0];
                break;
            }
        }
        if (i < 0) error("continue outside a loop");
        break;
    }
    case 12: pc_ = read(); break;
    case 13: {  // if's test
        top().cond = popInt();
        if (top().cond != 0) break;
        pc_ = top().a[0] == 0 ? top().end : top().a[0];
        break;
    }
    case 14:  // for's test
        top().cond = popInt();
        pc_ = top().cond != 0 ? top().a[2] : top().end;
        break;
    case 15:  // while's test
        top().cond = popInt();
        if (top().cond == 0) pc_ = top().end;
        break;
    case 16: {  // switch's test
        top().cond = popInt();
        Block& b = top();
        if (b.type != 7) break;
        uint32_t fallback = 0;
        int i = 0;
        for (; i < b.count && i < 64; ++i) {
            if (b.values[static_cast<size_t>(i)] == b.cond) {
                pc_ = b.a[static_cast<size_t>(i)];
                break;
            }
            if (b.values[static_cast<size_t>(i)] == INT32_MIN) fallback = b.a[static_cast<size_t>(i)];
        }
        if (i == b.count) pc_ = fallback == 0 ? b.end : fallback;
        break;
    }
    default: error("control sub-op " + std::to_string(sub)); break;
    }
    return 0;
}

int ScriptVm::op() {
    uint32_t code = read();
    if (code == 0) {  // assignment into the reference below
        Value r = pop();
        if (r.type == 3) storeInt(r.i, false);
        else if (r.type == 4) storeFloat(r.f, false);
        else if (r.type == 5) storeString(r.str, false);
        return 0;
    }
    if (code >= 1 && code <= 0x16) {
        Value r = pop();  // the right operand is on top
        Value l = pop();
        bool str = r.type == 5 || l.type == 5;
        bool flt = !str && (r.type == 4 || l.type == 4);
        auto asFloat = [](const Value& v) { return v.type == 3 ? static_cast<float>(v.i) : v.type == 4 ? v.f : 0.0f; };
        float fr = asFloat(r), fl = asFloat(l);
        if (code <= 7) {  // compound assignment: stored back into the left reference
            if (str) return error("string in an arithmetic assignment"), 0;
            if (flt) {
                switch (code) {
                case 1: storeFloat(fr + fl, true); break;
                case 2: storeFloat(fl - fr, true); break;
                case 3: storeFloat(fr * fl, true); break;
                case 4: storeFloat(fl / fr, true); break;
                default: error("float in a %=, &= or |="); break;
                }
                return 0;
            }
            int32_t a = l.i, b = r.i, v = 0;
            switch (code) {
            case 1: v = a + b; break;
            case 2: v = a - b; break;
            case 3: v = a * b; break;
            case 4: v = b ? a / b : 0; break;
            case 5: v = b ? a % b : 0; break;
            case 6: v = a & b; break;
            case 7: v = a | b; break;
            }
            storeInt(v, true);
            return 0;
        }
        if (str) {
            if (code == 8) push(Value::ofInt(r.str == l.str ? 1 : 0));
            else if (code == 0xf) push(Value::ofInt(std::strcmp(r.str.c_str(), l.str.c_str())));  // _mbscmp's result as is
            else if (code == 0x10) push(Value::ofString(l.str + r.str));
            else error("string operator " + std::to_string(code));
            return 0;
        }
        if (flt) {
            switch (code) {
            case 9: push(Value::ofInt(fl < fr)); break;
            case 10: push(Value::ofInt(fl > fr)); break;
            case 0xd: push(Value::ofInt(fl != 0 && fr != 0)); break;
            case 0xe: push(Value::ofInt(fl != 0 || fr != 0)); break;
            case 0x10: push(Value::ofFloat(fr + fl)); break;
            case 0x11: push(Value::ofFloat(fl - fr)); break;
            case 0x12: push(Value::ofFloat(fr * fl)); break;
            case 0x13: push(Value::ofFloat(fl / fr)); break;
            default: error("float operator " + std::to_string(code)); break;  // nothing is pushed
            }
            return 0;
        }
        int32_t a = l.i, b = r.i;
        switch (code) {
        case 8: push(Value::ofInt(a == b)); break;
        case 9: push(Value::ofInt(a < b)); break;
        case 10: push(Value::ofInt(a > b)); break;
        case 0xb: push(Value::ofInt(a <= b)); break;
        case 0xc: push(Value::ofInt(a >= b)); break;
        case 0xd: push(Value::ofInt(a != 0 && b != 0)); break;
        case 0xe: push(Value::ofInt(a != 0 || b != 0)); break;
        case 0xf: push(Value::ofInt(a != b)); break;
        case 0x10: push(Value::ofInt(a + b)); break;
        case 0x11: push(Value::ofInt(a - b)); break;
        case 0x12: push(Value::ofInt(a * b)); break;
        case 0x13: push(Value::ofInt(b ? a / b : 0)); break;
        case 0x14: push(Value::ofInt(b ? a % b : 0)); break;
        case 0x15: push(Value::ofInt(a & b)); break;
        case 0x16: push(Value::ofInt(a | b)); break;
        }
        return 0;
    }
    switch (code) {
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1a: {
        Value v = pop();
        if (v.type == 5) return error("string in a unary operator"), 0;
        bool flt = v.type == 4;
        if (code == 0x17) push(flt ? Value::ofFloat(-v.f) : Value::ofInt(-v.i));
        else if (code == 0x18) push(Value::ofInt(flt ? v.f == 0 : v.i == 0));
        else if (flt) storeFloat(v.f + (code == 0x19 ? 1.0f : -1.0f), true);
        else storeInt(v.i + (code == 0x19 ? 1 : -1), true);
        return 0;
    }
    case 0x1b: {  // array index: written into the reference now on top
        Value v = pop();
        if (v.type != 3) return error("array index not an int"), 0;
        if (sp_ >= 1) stack_[sp_].type = v.i;
        return 0;
    }
    case 0x1e: pop(); return 0;
    default: return 0;
    }
}

int ScriptVm::call() {
    const FunctionTable& table = host_.functions();
    if (!pending_) {
        callId_ = static_cast<int>(read());
        ctx_ = CallContext{};
        ctx_.function = callId_;
        ctx_.user = user_;
        const ScriptFunction* f = table.find(callId_);
        size_t argc = f ? f->argTypes.size() : 0;
        for (size_t k = 0; k < argc; ++k) {
            size_t idx = argc - k - 1;
            Value v = pop();
            switch (f->argTypes[idx]) {
            case 1:
            case 2:
            case 3:
            case 7:
                if (v.type == 4) v.type = 3, v.i = ftol(v.f);
                if (v.type == 5) error(f->name + ": a string for int argument " + std::to_string(idx));
                break;
            case 4:
                if (v.type == 3) v.type = 4, v.f = static_cast<float>(v.i);
                if (v.type == 5) error(f->name + ": a string for float argument " + std::to_string(idx));
                break;
            case 5:
                if (v.type == 5) {
                    if (v.i == -1 && idx < f->defaults.size()) v.str = f->defaults[idx];
                } else {
                    error(f->name + ": no string for argument " + std::to_string(idx));
                }
                break;
            default: break;
            }
            if (idx < ctx_.args.size()) ctx_.args[idx] = std::move(v);
        }
    }
    int yield = runNative(callId_);
    pending_ = ctx_.waiting;
    if (!ctx_.waiting) {
        const ScriptFunction* f = table.find(callId_);
        int rt = f ? f->returnType : 0;
        if (rt == 3) push(Value::ofInt(ctx_.returnInt));
        else if (rt == 4) push(Value::ofFloat(ctx_.returnFloat));
        else if (rt == 5) push(Value::ofString(ctx_.returnString));
        ctx_.phase = 0;
    }
    return yield;
}

int ScriptVm::runNative(int id) {
    switch (id) {
    case 0: {  // SLoad (0x452230): the root loads it after this frame's instructions
        ScriptVm& r = root();
        r.loadPending_ = true;
        r.loadName_ = ctx_.argString(0);
        r.loadLabel_ = ctx_.argInt(1);
        return 1;
    }
    case 1:  // SCall (0x4539b0): another script in a child
    case 2: {  // call (0x453a40): this script at a label, in a child
        child_ = std::make_unique<ScriptVm>(host_, group_, this);
        std::string script = id == 1 ? ctx_.argString(0) : name_;
        int label = id == 1 ? ctx_.argInt(1) : ctx_.argInt(0);
        if (child_->load(script) && !child_->start(label)) error("no label " + std::to_string(label) + " in " + script);
        return 1;
    }
    default: return host_.callNative(*this, id, ctx_);
    }
}

void ScriptVm::deleteChild() {
    child_.reset();
}

void ScriptVm::doLoad(const std::string& name, int label) {
    loadPending_ = false;
    if (parent_) return error("SLoad applied below the root");
    deleteChild();
    if (load(name) && !start(label)) error("no label " + std::to_string(label) + " in " + name);
}

int ScriptVm::run() {
    if (child_) {
        child_->run();
        if (child_->state_ == Ended) deleteChild();
        if (loadPending_) doLoad(loadName_, loadLabel_);
        return state_;
    }
    if (state_ == Running) {
        int yield = 0;
        do {
            if (!pending_) curOp_ = read();
            switch (curOp_) {
            case 0: yield = control(); break;
            case 1:
            case 2:
            case 3: {
                Value v;
                v.kind = static_cast<int32_t>(curOp_);
                v.i = static_cast<int32_t>(read());
                v.type = -1;
                push(std::move(v));
                break;
            }
            case 4: yield = call(); break;
            case 5: {
                uint32_t type = read();
                if (type == 4) {
                    push(Value::ofFloat(readFloat()));
                } else {
                    Value v;
                    v.type = static_cast<int32_t>(type);
                    v.i = static_cast<int32_t>(read());
                    push(std::move(v));
                }
                break;
            }
            case 6: yield = op(); break;
            default: error("opcode " + std::to_string(curOp_)); break;
            }
            // Blocks whose end has been reached close (if, else, loops, switch: types 2..7).
            if (blockTop_ != 0 && top().type >= 2 && top().type <= 7) {
                while (blockTop_ != 0 && top().end == pc_) --blockTop_;
            }
        } while (yield == 0 && state_ == Running);
    }
    if (loadPending_) doLoad(loadName_, loadLabel_);
    if (child_) {
        // A call made this frame runs at once.
        child_->run();
        if (child_->state_ == Ended) deleteChild();
        if (loadPending_) doLoad(loadName_, loadLabel_);
    }
    return state_;
}

namespace {

void saveValue(Writer& w, const Value& v) {
    w.i32(v.kind);
    w.i32(v.type);
    w.str(v.str);
    w.f32(v.f);
    w.i32(v.i);
}

Value loadValue(Reader& r) {
    Value v;
    v.kind = r.i32();
    v.type = r.i32();
    v.str = r.str();
    v.f = r.f32();
    v.i = r.i32();
    return v;
}

}  // namespace

void ScriptVm::save(Writer& w) const {
    w.str(name_);
    w.str(group_);
    w.i32(state_);
    w.u32(pc_);
    w.i32(sp_);
    for (int i = 1; i <= sp_; ++i) saveValue(w, stack_[static_cast<size_t>(i)]);
    w.i32(blockTop_);
    for (const Block& b : blocks_) {
        w.i32(b.type);
        w.i32(b.count);
        w.u32(b.end);
        w.i32(b.cond);
        for (uint32_t a : b.a) w.u32(a);
        for (int32_t v : b.values) w.i32(v);
    }
    for (int32_t l : locals_) w.i32(l);
    w.u8(pending_);
    w.u32(curOp_);
    w.i32(callId_);
    w.i32(ctx_.function);
    w.u8(ctx_.waiting);
    w.i32(ctx_.phase);
    for (const Value& a : ctx_.args) saveValue(w, a);
    w.i32(ctx_.returnInt);
    w.f32(ctx_.returnFloat);
    w.str(ctx_.returnString);
    w.i32(ctx_.user);
    w.i32(user_);
    w.u8(loadPending_);
    w.str(loadName_);
    w.i32(loadLabel_);
    w.u8(child_ != nullptr);
    if (child_) child_->save(w);
}

bool ScriptVm::restore(Reader& r) {
    std::string name = r.str();
    group_ = r.str();
    if (!load(name)) return false;
    state_ = static_cast<State>(r.i32());
    pc_ = r.u32();
    sp_ = r.i32();
    if (sp_ < 0 || sp_ > kStackMax + 1) return false;
    for (int i = 1; i <= sp_; ++i) stack_[static_cast<size_t>(i)] = loadValue(r);
    blockTop_ = r.i32();
    if (blockTop_ < 0 || blockTop_ >= kBlocks) return false;
    for (Block& b : blocks_) {
        b.type = r.i32();
        b.count = r.i32();
        b.end = r.u32();
        b.cond = r.i32();
        for (uint32_t& a : b.a) a = r.u32();
        for (int32_t& v : b.values) v = r.i32();
    }
    for (int32_t& l : locals_) l = r.i32();
    pending_ = r.u8() != 0;
    curOp_ = r.u32();
    callId_ = r.i32();
    ctx_.function = r.i32();
    ctx_.waiting = r.u8() != 0;
    ctx_.phase = r.i32();
    for (Value& a : ctx_.args) a = loadValue(r);
    ctx_.returnInt = r.i32();
    ctx_.returnFloat = r.f32();
    ctx_.returnString = r.str();
    ctx_.user = r.i32();
    user_ = r.i32();
    loadPending_ = r.u8() != 0;
    loadName_ = r.str();
    loadLabel_ = r.i32();
    child_.reset();
    if (r.u8()) {
        child_ = std::make_unique<ScriptVm>(host_, group_, this);
        if (!child_->restore(r)) return false;
    }
    return true;
}

}  // namespace nyan::wa2
