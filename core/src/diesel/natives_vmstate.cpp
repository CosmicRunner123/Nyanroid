// The save natives: CreateReferenceTable, ClearReferenceTable, SerializeRootTable and
// DeserializeRootTable (muramasa_en.exe 0x4896e0 -> 0x43e4c0, 0x489750 -> 0x43f0f0, 0x48f750,
// 0x48fb20). The framework saves a game as the Squirrel heap reachable from the root table
// (the framework's SaveVMState and LoadVMState commands), script
// threads with their stacks and call frames included, into the SQFile it opens under the name
// '::SAVEFILE::' (or '::LOADFILE::'); engine objects go through script callbacks that write their
// state into the same file.
//
// The reference table (CreateReferenceTable(order, ignore)): every object (reference-counted value)
// reachable from the root table, walked depth first (forEachHeld below), less those in `ignore` when
// it is an array; then sorted by creation order: strings -2, prototypes -1, an instance what `order`
// returns for it (an integer; else 0x7ffffffd), weak references 0x7ffffffe, the root table
// 0x7fffffff, the rest 0x7ffffffd. And the names: each root-table slot holding a native function or a
// class, by its value (the first slot wins).
//
// The file (all little-endian; a record is a 5-byte tag (4 letters, NUL), the 4-byte file position
// of its end, then its fields):
//   "ROOT" count instances "INSC" {type, creation record} x count
//   "REFT" {type, content record} x count  "INSR" {index, INRE record} for each instance
// A value is: 0 type [value] for null (no value), integers, floats and booleans (4 bytes) and user
// pointers (no value); 1 type index into the reference table; 2 type index into the thread's own
// list (below); -1 (nothing more) for an object in neither.
// Creation records: INSI (the recreate callback writes what it needs, called with the instance),
// STRI (length, bytes), FNCI (the prototype's eight sizes in SQFunctionProto::Create's order), NTCI
// and CLSI (the name's type, then a string name: length, bytes), CLOI (the prototype), THRI (stack
// size), WEKI (0 null | 1/2 type index | -1), TABI (1 if the root table); arrays, user data and
// generators have none. Content records: TABL (count, key-value pairs, the delegate), ARRY (count,
// items), CLOS (environment, outer values, default parameters), FUNC (source and function names,
// the instructions with their SHA-256, stack size, two 4-byte reads at the generator and varargs
// flags, local variables, line infos, the instructions again, literals, parameters, functions, outer
// variables, default parameters), INST (members: name, value), THRD (below). INRE: the reset
// callback writes the instance's engine state.
// A thread: the objects only its stack holds (walked as the reference table is) as a list of its
// own, with their creation and content records and their instances' INRE records; then the stack
// size, _top, _stackbase, the suspend state, _nnativecalls, temp_reg, the varargs stack, the call
// frames (counts, then each: traps, previous stack base and top, target, calls, root flag, varargs,
// the instruction's index, the closure) and the stack's values.
//
// phantom.exe (x86-64, its writers 0x1400a4580 values, 0x1400a22f0 FNCI, 0x1400a0000 NTCI / CLSI,
// 0x1400a0790 INST, 0x1400a1780 FUNC, 0x1400a2e50 THRD) writes the VM's SQInteger and SQBool fields
// 8 bytes wide: integer and boolean values, FNCI's eight sizes, the name lengths of NTCI / CLSI and of
// INST's members, FUNC's stack size, local variables (start, end, position), line infos and default
// parameters, THRD's top, stack base, the four suspend fields, the native calls, the two call stack
// sizes and each frame's root flag; and FUNC's generator and varargs flags one byte each. Every other
// field (tags, record ends, counts, indexes, STRI's length, a frame's traps, bases, target and calls,
// the instruction index) is 4 bytes as in the 32-bit exes.
//
// Loading reads it back in the same order: objects are made in the reference table's order (an
// instance is what the recreate callback returns; native functions and classes are looked up by
// name in the root table; a table marked root is the root table), filled, then each instance's
// reset callback reads its state. Table slots whose key or value did not load are skipped, and the
// root table keeps its classes.
//
// The reference table's order among equal priorities follows the walk here (the exe's follows the
// objects' addresses, which vary from run to run). A thread's list puts its strings and prototypes
// first (Nyanroid: the exe keeps the walk's order, in which a closure can come before the prototype
// its record needs).

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "natives.hpp"
#include "nyan/diesel/crypto.hpp"

// Squirrel's internals (the bundled 2.2.4, as sqvm.cpp includes them): the file is the VM's objects.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wdeprecated-copy"
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#undef sq_type  // squirrel.h's; sqobject.h defines the same
#include "vendor/squirrel/squirrel/sqpcheader.h"
#include "vendor/squirrel/squirrel/sqopcodes.h"
#include "vendor/squirrel/squirrel/sqfuncproto.h"
#include "vendor/squirrel/squirrel/sqvm.h"
#include "vendor/squirrel/squirrel/sqclosure.h"
#include "vendor/squirrel/squirrel/sqstring.h"
#include "vendor/squirrel/squirrel/sqtable.h"
#include "vendor/squirrel/squirrel/squserdata.h"
#include "vendor/squirrel/squirrel/sqarray.h"
#include "vendor/squirrel/squirrel/sqclass.h"
#pragma GCC diagnostic pop

namespace nyan::diesel {

struct ScriptHost::Impl::ReferenceTable {
    struct Entry {
        int32_t order = 0;  // saving: the creation order; loading: -1 if the object did not load
        SQObjectPtr obj;
    };
    std::vector<Entry> refs;                           // +0x20
    std::unordered_map<uintptr_t, int32_t> index;      // +0x2c: an object's place in refs
    std::unordered_map<uintptr_t, SQObjectPtr> names;  // root-table natives and classes: their keys
    struct Local {
        std::vector<SQObjectPtr> list;
        std::unordered_map<uintptr_t, int32_t> index;
    };
    std::vector<Local> locals;  // +0x64: the threads' own lists while their records are written / read

    void clear() {
        locals.clear();
        refs.clear();
        index.clear();
        names.clear();
    }
};

namespace {

using RefTable = ScriptHost::Impl::ReferenceTable;

RefTable& referenceTable(ScriptHost::Impl& h) {
    if (!h.referenceTable) {
        h.referenceTable = std::unique_ptr<RefTable, void (*)(RefTable*)>(new RefTable, [](RefTable* t) { delete t; });
    }
    return *h.referenceTable;
}

uintptr_t objectKey(const SQObject& o) { return static_cast<uintptr_t>(_rawval(o)); }
bool isObject(const SQObject& o) { return ISREFCOUNTED(sq_type(o)) != 0; }

SQObjectPtr stackArg(HSQUIRRELVM v, SQInteger idx) {
    HSQOBJECT o;
    sq_resetobject(&o);
    if (idx <= sq_gettop(v)) sq_getstackobj(v, idx, &o);
    return SQObjectPtr(o);
}

// The objects an object holds, as the exe walks them (0x43d5c0, and 0x4e78a0 for a thread's own): a
// table's, instance's or user data's delegate; an array's items; a table's keys and values (weak
// references as they are); a class's members and an instance's (their values now), but not those
// whose value is a class; a closure's default parameters, outer values and prototype; a prototype's
// source and function names, local variable names, literals, parameters, functions and outer
// variables' names and sources. Nothing else: not a closure's environment, a class's base, a thread's
// stack or a weak reference's object.
template <typename Visit>
void forEachHeld(const SQObjectPtr& o, Visit&& visit) {
    SQTable* delegate = nullptr;
    switch (sq_type(o)) {
        case OT_TABLE: delegate = _table(o)->_delegate; break;
        case OT_INSTANCE: delegate = _instance(o)->_delegate; break;
        case OT_USERDATA: delegate = _userdata(o)->_delegate; break;
        default: break;
    }
    if (delegate) visit(SQObjectPtr(delegate));
    switch (sq_type(o)) {
        case OT_ARRAY: {
            SQArray* a = _array(o);
            for (SQUnsignedInteger i = 0; i < a->_values.size(); ++i) visit(SQObjectPtr(a->_values[i]));
            break;
        }
        case OT_TABLE: {
            SQObjectPtr pos(static_cast<SQInteger>(0)), key, val;
            SQInteger next;
            while ((next = _table(o)->Next(true, pos, key, val)) != -1) {
                visit(key);
                visit(val);
                pos = next;
            }
            break;
        }
        case OT_INSTANCE: {
            SQInstance* in = _instance(o);
            SQObjectPtr pos(static_cast<SQInteger>(0)), key, member;
            SQInteger next;
            while ((next = in->_class->Next(pos, key, member)) != -1) {
                SQObjectPtr val;
                in->Get(key, val);
                if (sq_type(val) != OT_CLASS) {
                    visit(key);
                    visit(val);
                }
                pos = next;
            }
            break;
        }
        case OT_CLASS: {
            SQObjectPtr pos(static_cast<SQInteger>(0)), key, val;
            SQInteger next;
            while ((next = _class(o)->Next(pos, key, val)) != -1) {
                if (sq_type(val) != OT_CLASS) {
                    visit(key);
                    visit(val);
                }
                pos = next;
            }
            break;
        }
        case OT_CLOSURE: {
            SQClosure* c = _closure(o);
            for (SQUnsignedInteger i = 0; i < c->_defaultparams.size(); ++i) visit(SQObjectPtr(c->_defaultparams[i]));
            for (SQUnsignedInteger i = 0; i < c->_outervalues.size(); ++i) visit(SQObjectPtr(c->_outervalues[i]));
            visit(c->_function);
            break;
        }
        case OT_FUNCPROTO: {
            SQFunctionProto* f = _funcproto(o);
            visit(f->_sourcename);
            visit(f->_name);
            for (SQInteger i = 0; i < f->_nlocalvarinfos; ++i) visit(f->_localvarinfos[i]._name);
            for (SQInteger i = 0; i < f->_nliterals; ++i) visit(f->_literals[i]);
            for (SQInteger i = 0; i < f->_nparameters; ++i) visit(f->_parameters[i]);
            for (SQInteger i = 0; i < f->_nfunctions; ++i) visit(f->_functions[i]);
            for (SQInteger i = 0; i < f->_noutervalues; ++i) {
                visit(f->_outervalues[i]._name);
                visit(f->_outervalues[i]._src);
            }
            break;
        }
        default: break;
    }
}

// A thread's own list in the order its records are written: strings, prototypes, then the rest, each
// in the walk's order.
void orderLocal(RefTable::Local& l) {
    auto rank = [](const SQObjectPtr& o) { return sq_type(o) == OT_STRING ? 0 : sq_type(o) == OT_FUNCPROTO ? 1 : 2; };
    std::stable_sort(l.list.begin(), l.list.end(), [&](const SQObjectPtr& a, const SQObjectPtr& b) { return rank(a) < rank(b); });
    l.index.clear();
    for (size_t i = 0; i < l.list.size(); ++i) l.index[objectKey(l.list[i])] = static_cast<int32_t>(i);
}

// Adds an object and what it holds to the thread's list (0x4e78a0 / 0x4e80c0): those the reference
// table and the list do not have.
void addLocal(RefTable& t, const SQObjectPtr& start) {
    RefTable::Local& l = t.locals.back();
    std::vector<SQObjectPtr> stack;
    auto visit = [&](const SQObjectPtr& x) {
        if (!isObject(x)) return;
        uintptr_t k = objectKey(x);
        if (t.index.count(k) || l.index.count(k)) return;
        l.index[k] = static_cast<int32_t>(l.list.size());
        l.list.push_back(x);
        stack.push_back(x);
    };
    visit(start);
    while (!stack.empty()) {
        SQObjectPtr o = stack.back();
        stack.pop_back();
        forEachHeld(o, visit);
    }
}

// The file's bytes: records and values.
class Stream {
public:
    explicit Stream(FileStream& f) : f_(f) {}
    void u32(uint32_t v) { f_.writeBytes(&v, 4); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u16(uint16_t v) { f_.writeBytes(&v, 2); }
    void bytes(const void* p, size_t n) {
        if (n) f_.writeBytes(p, n);
    }
    void tag(const char* t) { f_.writeBytes(t, 5); }
    // A record: its tag, then the position of its end (end() fills it in).
    size_t begin(const char* t) {
        tag(t);
        size_t at = f_.tell();
        u32(0);
        return at;
    }
    void end(size_t at) {
        size_t e = f_.tell();
        f_.seekTo(at);
        u32(static_cast<uint32_t>(e));
        f_.seekTo(e);
    }
    uint32_t r32() {
        uint32_t v = 0;
        f_.readBytes(&v, 4);
        return v;
    }
    int32_t ri32() { return static_cast<int32_t>(r32()); }
    uint16_t r16() {
        uint16_t v = 0;
        f_.readBytes(&v, 2);
        return v;
    }
    void read(void* p, size_t n) {
        if (n && f_.readBytes(p, n) < n) std::memset(static_cast<uint8_t*>(p), 0, n);
    }
    void skip(size_t n) { f_.seekTo(f_.tell() + n); }
    // The VM's SQInteger and SQBool fields: 8 bytes in phantom.exe (x86-64), 4 in the 32-bit exes.
    // Nyanroid's integers are 32-bit: a wide one is written sign-extended and read back truncated.
    void word(int64_t v) {
        if (wide) {
            f_.writeBytes(&v, 8);
        } else {
            i32(static_cast<int32_t>(v));
        }
    }
    int64_t rword() {
        if (!wide) return ri32();
        int64_t v = 0;
        read(&v, 8);
        return v;
    }
    // The first `tag` record from here (within `limit` bytes): its position and the end it gives.
    // The stream stays where it was.
    bool peekRecord(const char* tag, size_t& at, uint32_t& end, size_t limit) {
        const size_t start = f_.tell();
        constexpr size_t kChunk = size_t{1} << 16;
        std::vector<uint8_t> buf;
        size_t base = start;  // the position of buf[0]
        bool found = false;
        while (!found && base - start < limit) {
            size_t have = buf.size();
            buf.resize(have + kChunk);
            size_t got = f_.readBytes(buf.data() + have, kChunk);
            buf.resize(have + got);
            for (size_t i = 0; i + 9 <= buf.size(); ++i) {
                if (std::memcmp(buf.data() + i, tag, 5) != 0) continue;
                at = base + i;
                std::memcpy(&end, buf.data() + i + 5, 4);
                found = true;
                break;
            }
            if (got == 0) break;
            if (!found && buf.size() > 8) {  // a record cut by the chunk's end
                base += buf.size() - 8;
                buf.erase(buf.begin(), buf.end() - 8);
            }
        }
        f_.seekTo(start);
        return found;
    }
    bool wide = false;
    // The tag compared as the exe does (a C string).
    bool expect(const char* t) {
        char b[6] = {};
        read(b, 5);
        return std::strcmp(b, t) == 0;
    }
    size_t tell() const { return f_.tell(); }
    void seek(size_t p) { f_.seekTo(p); }

private:
    FileStream& f_;
};

// Script calls with the root table as `this` (SqPlus's SquirrelFunction). A script error stops the
// native, which raises it again (the exe's SqPlus call throws).
class Callbacks {
public:
    explicit Callbacks(HSQUIRRELVM v) : v_(v) {}
    bool call(const SQObjectPtr& fn, const SQObjectPtr* arg, SQObjectPtr* result) {
        SQInteger top = sq_gettop(v_);
        sq_pushobject(v_, fn);
        sq_pushroottable(v_);
        if (arg) sq_pushobject(v_, *arg);
        if (SQ_FAILED(sq_call(v_, arg ? 2 : 1, result ? SQTrue : SQFalse, SQTrue))) {
            sq_settop(v_, top);
            failed = true;
            return false;
        }
        if (result) *result = stackArg(v_, -1);
        sq_settop(v_, top);
        return true;
    }
    bool failed = false;

protected:
    HSQUIRRELVM v_;
};

class Saver : Callbacks {
public:
    Saver(HSQUIRRELVM v, RefTable& t, FileStream& f, SQObjectPtr recreate, SQObjectPtr reset)
        : Callbacks(v), t_(t), s_(f), recreate_(std::move(recreate)), reset_(std::move(reset)) {
        s_.wide = host(v).game.exe.is64;
    }

    // 0, or -1 if a record could not be written.
    int run() {
        s_.tag("ROOT");
        s_.u32(static_cast<uint32_t>(t_.refs.size()));
        uint32_t instances = 0;
        for (const auto& e : t_.refs) instances += sq_type(e.obj) == OT_INSTANCE;
        s_.u32(instances);
        s_.tag("INSC");
        for (const auto& e : t_.refs) {
            if (!create(e.obj)) return -1;
        }
        s_.tag("REFT");
        for (const auto& e : t_.refs) {
            if (!content(e.obj)) return -1;
        }
        s_.tag("INSR");
        for (size_t i = 0; i < t_.refs.size(); ++i) {
            if (sq_type(t_.refs[i].obj) != OT_INSTANCE) continue;
            s_.u32(static_cast<uint32_t>(i));
            if (!instanceReset(t_.refs[i].obj)) return -1;
        }
        return 0;
    }
    using Callbacks::failed;

private:
    // A value (0x48ea70).
    void value(const SQObject& o) {
        SQObjectType type = sq_type(o);
        if (!ISREFCOUNTED(type)) {
            s_.u32(0);
            switch (type) {
                case OT_NULL:
                case OT_USERPOINTER: s_.u32(type); break;
                case OT_INTEGER:
                case OT_BOOL:
                    s_.u32(type);
                    s_.word(_integer(o));
                    break;
                case OT_FLOAT: {
                    s_.u32(type);
                    float f = _float(o);
                    s_.bytes(&f, 4);
                    break;
                }
                default: s_.u32(OT_NULL); break;
            }
            return;
        }
        if (!t_.locals.empty()) {
            const auto& l = t_.locals.back();
            auto it = l.index.find(objectKey(o));
            if (it != l.index.end()) {
                s_.u32(2);
                s_.u32(type);
                s_.i32(it->second);
                return;
            }
        }
        auto it = t_.index.find(objectKey(o));
        if (it == t_.index.end()) {
            s_.i32(-1);
            return;
        }
        s_.u32(1);
        s_.u32(type);
        s_.i32(it->second);
    }

    // An instance's INSI (0x489760, the recreate callback) or INRE (0x489b40, the reset callback).
    bool instanceRecord(const char* tag, const SQObjectPtr& fn, const SQObjectPtr& o, bool checkFirst) {
        if (sq_type(o) != OT_INSTANCE) return false;
        if (checkFirst && sq_type(fn) != OT_CLOSURE) return false;
        size_t at = s_.begin(tag);
        if (sq_type(fn) != OT_CLOSURE) return false;
        if (!call(fn, &o, nullptr)) return false;
        s_.end(at);
        return true;
    }
    bool instanceReset(const SQObjectPtr& o) { return instanceRecord("INRE", reset_, o, true); }

    void name(const SQObjectPtr& o) {
        auto it = t_.names.find(objectKey(o));
        SQObjectPtr key = it == t_.names.end() ? SQObjectPtr() : it->second;
        s_.u32(sq_type(key));
        if (sq_type(key) == OT_STRING) {
            s_.word(_string(key)->_len);
            s_.bytes(_string(key)->_val, static_cast<size_t>(_string(key)->_len));
        }
    }

    // The type and creation record (0x48f3b0).
    bool create(const SQObjectPtr& o) {
        SQObjectType type = sq_type(o);
        s_.u32(type);
        if (!ISREFCOUNTED(type)) return true;
        switch (type) {
            case OT_INSTANCE: return instanceRecord("INSI", recreate_, o, false);
            case OT_STRING: {
                size_t at = s_.begin("STRI");
                s_.u32(static_cast<uint32_t>(_string(o)->_len));
                s_.bytes(_string(o)->_val, static_cast<size_t>(_string(o)->_len));
                s_.end(at);
                return true;
            }
            case OT_FUNCPROTO: {
                SQFunctionProto* f = _funcproto(o);
                size_t at = s_.begin("FNCI");
                for (SQInteger n : {f->_ninstructions, f->_nliterals, f->_nparameters, f->_nfunctions, f->_noutervalues,
                                    f->_nlineinfos, f->_nlocalvarinfos, f->_ndefaultparams}) {
                    s_.word(n);
                }
                s_.end(at);
                return true;
            }
            case OT_NATIVECLOSURE:
            case OT_CLASS: {
                size_t at = s_.begin(type == OT_CLASS ? "CLSI" : "NTCI");
                name(o);
                s_.end(at);
                return true;
            }
            case OT_CLOSURE: {
                size_t at = s_.begin("CLOI");
                value(_closure(o)->_function);
                s_.end(at);
                return true;
            }
            case OT_USERDATA:
            case OT_GENERATOR:
            case OT_ARRAY: return true;
            case OT_THREAD: {
                size_t at = s_.begin("THRI");
                s_.u32(static_cast<uint32_t>(_thread(o)->_stack.size()));
                s_.end(at);
                return true;
            }
            case OT_WEAKREF: {
                size_t at = s_.begin("WEKI");
                const SQObject& target = _weakref(o)->_obj;
                bool done = false;
                if (sq_type(target) == OT_NULL) {
                    s_.u32(0);
                    s_.u32(OT_NULL);
                    done = true;
                } else if (!t_.locals.empty() && isObject(target)) {
                    const auto& l = t_.locals.back();
                    auto it = l.index.find(objectKey(target));
                    if (it != l.index.end()) {
                        s_.u32(2);
                        s_.u32(sq_type(target));
                        s_.i32(it->second);
                        done = true;
                    }
                }
                if (!done) {
                    auto it = t_.index.find(objectKey(target));
                    if (it == t_.index.end()) {
                        s_.i32(-1);
                    } else {
                        s_.u32(1);
                        s_.u32(sq_type(target));
                        s_.i32(it->second);
                    }
                }
                s_.end(at);
                return true;
            }
            case OT_TABLE: {
                size_t at = s_.begin("TABI");
                s_.u32(_table(o) == _table(v_->_roottable) ? 1 : 0);
                s_.end(at);
                return true;
            }
            default: return false;
        }
    }

    // The type and content record (0x48ece0).
    bool content(const SQObjectPtr& o) {
        SQObjectType type = sq_type(o);
        s_.u32(type);
        switch (type) {
            case OT_TABLE: {
                SQTable* tb = _table(o);
                size_t at = s_.begin("TABL");
                s_.i32(tb->CountUsed());
                SQObjectPtr pos(static_cast<SQInteger>(0)), key, val;
                SQInteger next;
                while ((next = tb->Next(true, pos, key, val)) != -1) {
                    value(key);
                    value(val);
                    pos = next;
                }
                value(tb->_delegate ? SQObjectPtr(tb->_delegate) : SQObjectPtr());
                s_.end(at);
                return true;
            }
            case OT_ARRAY: {
                SQArray* a = _array(o);
                size_t at = s_.begin("ARRY");
                s_.u32(static_cast<uint32_t>(a->_values.size()));
                for (SQUnsignedInteger i = 0; i < a->_values.size(); ++i) value(a->_values[i]);
                s_.end(at);
                return true;
            }
            case OT_CLOSURE: {
                SQClosure* c = _closure(o);
                size_t at = s_.begin("CLOS");
                value(c->_env);
                s_.u32(static_cast<uint32_t>(c->_outervalues.size()));
                for (SQUnsignedInteger i = 0; i < c->_outervalues.size(); ++i) value(c->_outervalues[i]);
                s_.u32(static_cast<uint32_t>(c->_defaultparams.size()));
                for (SQUnsignedInteger i = 0; i < c->_defaultparams.size(); ++i) value(c->_defaultparams[i]);
                s_.end(at);
                return true;
            }
            case OT_THREAD: return thread(o);
            case OT_FUNCPROTO: return prototype(o);
            case OT_INSTANCE: {
                SQInstance* in = _instance(o);
                size_t at = s_.begin("INST");
                std::vector<std::pair<SQObjectPtr, SQObjectPtr>> members;
                SQObjectPtr pos(static_cast<SQInteger>(0)), key, member;
                SQInteger next;
                while ((next = in->_class->Next(pos, key, member)) != -1) {
                    SQObjectPtr val;
                    in->Get(key, val);
                    members.emplace_back(key, val);
                    pos = next;
                }
                s_.u32(static_cast<uint32_t>(members.size()));
                for (const auto& [k, val] : members) {
                    uint32_t len = sq_type(k) == OT_STRING ? static_cast<uint32_t>(_string(k)->_len) : 0;
                    s_.word(len);
                    if (len) s_.bytes(_string(k)->_val, len);
                    value(val);
                }
                s_.end(at);
                return true;
            }
            case OT_STRING:
            case OT_USERDATA:
            case OT_NATIVECLOSURE:
            case OT_GENERATOR:
            case OT_CLASS:
            case OT_WEAKREF:
            case OT_NULL:
            case OT_USERPOINTER: return true;
            case OT_INTEGER:
            case OT_BOOL: s_.word(_integer(o)); return true;
            case OT_FLOAT: {
                float f = _float(o);
                s_.bytes(&f, 4);
                return true;
            }
            default: return false;
        }
    }

    // FUNC (0x48c310).
    bool prototype(const SQObjectPtr& o) {
        SQFunctionProto* f = _funcproto(o);
        size_t at = s_.begin("FUNC");
        value(f->_sourcename);
        value(f->_name);
        size_t blob = static_cast<size_t>(f->_ninstructions) * sizeof(SQInstruction);
        s_.u32(static_cast<uint32_t>(blob));
        s_.bytes(f->_instructions, blob);
        auto digest = sha256(ByteSpan(reinterpret_cast<const uint8_t*>(f->_instructions), blob));
        s_.bytes(digest.data(), digest.size());
        s_.word(f->_stacksize);
        if (s_.wide) {
            // phantom.exe: one byte each (0x1400a19bf, 0x1400a19e4).
            uint8_t flags[2] = {static_cast<uint8_t>(f->_bgenerator), static_cast<uint8_t>(f->_varparams)};
            s_.bytes(flags, 2);
        } else {
            // The 32-bit exes write 4 bytes from the generator flag and 4 from the varargs flag (the
            // padding, then the low byte of the local variables' count).
            uint8_t a[4] = {static_cast<uint8_t>(f->_bgenerator), static_cast<uint8_t>(f->_varparams), 0, 0};
            uint8_t b[4] = {static_cast<uint8_t>(f->_varparams), 0, 0, static_cast<uint8_t>(f->_nlocalvarinfos & 0xff)};
            s_.bytes(a, 4);
            s_.bytes(b, 4);
        }
        for (SQInteger i = 0; i < f->_nlocalvarinfos; ++i) {
            const SQLocalVarInfo& lv = f->_localvarinfos[i];
            value(lv._name);
            s_.word(lv._start_op);
            s_.word(lv._end_op);
            s_.word(lv._pos);
        }
        for (SQInteger i = 0; i < f->_nlineinfos; ++i) {
            s_.word(f->_lineinfos[i]._line);
            s_.word(f->_lineinfos[i]._op);
        }
        s_.bytes(f->_instructions, blob);
        for (SQInteger i = 0; i < f->_nliterals; ++i) value(f->_literals[i]);
        for (SQInteger i = 0; i < f->_nparameters; ++i) value(f->_parameters[i]);
        for (SQInteger i = 0; i < f->_nfunctions; ++i) value(f->_functions[i]);
        for (SQInteger i = 0; i < f->_noutervalues; ++i) {
            const SQOuterVar& ov = f->_outervalues[i];
            s_.u32(static_cast<uint32_t>(ov._type));
            value(ov._name);
            value(ov._src);
        }
        for (SQInteger i = 0; i < f->_ndefaultparams; ++i) s_.word(f->_defaultparams[i]);
        s_.end(at);
        return true;
    }

    // THRD (0x48d620).
    bool thread(const SQObjectPtr& o) {
        SQVM* th = _thread(o);
        size_t at = s_.begin("THRD");
        t_.locals.emplace_back();
        struct Pop {
            RefTable& t;
            ~Pop() { t.locals.pop_back(); }
        } pop{t_};
        for (SQUnsignedInteger i = 0; i < th->_stack.size(); ++i) {
            if (isObject(th->_stack[i])) addLocal(t_, th->_stack[i]);
        }
        addLocal(t_, th->temp_reg);
        for (SQUnsignedInteger i = 0; i < th->_vargsstack.size(); ++i) addLocal(t_, th->_vargsstack[i]);
        for (SQInteger i = 0; i < th->_callsstacksize; ++i) addLocal(t_, th->_callsstack[i]._closure);
        orderLocal(t_.locals.back());
        std::vector<SQObjectPtr> local = t_.locals.back().list;  // records may push lists of their own
        uint32_t instances = 0;
        for (const auto& x : local) instances += sq_type(x) == OT_INSTANCE;
        s_.u32(static_cast<uint32_t>(local.size()));
        s_.u32(instances);
        for (const auto& x : local) {
            if (!create(x)) return false;
        }
        for (const auto& x : local) {
            if (!content(x)) return false;
        }
        for (size_t i = 0; i < local.size(); ++i) {
            if (sq_type(local[i]) != OT_INSTANCE) continue;
            s_.u32(static_cast<uint32_t>(i));
            if (!instanceReset(local[i])) return false;
        }
        s_.u32(static_cast<uint32_t>(th->_stack.size()));
        s_.word(th->_top);
        s_.word(th->_stackbase);
        s_.word(th->_suspended);
        s_.word(th->_suspended_root);
        s_.word(th->_suspended_target);
        s_.word(th->_suspended_traps);
        s_.u16(th->_suspend_varargs.size);
        s_.u16(th->_suspend_varargs.base);
        s_.word(th->_nnativecalls);
        value(th->temp_reg);
        s_.u32(static_cast<uint32_t>(th->_vargsstack.size()));
        for (SQUnsignedInteger i = 0; i < th->_vargsstack.size(); ++i) value(th->_vargsstack[i]);
        s_.word(th->_callsstacksize);
        s_.word(th->_alloccallsstacksize);
        s_.u32(static_cast<uint32_t>(th->_callstackdata.size()));
        for (SQInteger i = 0; i < th->_callsstacksize; ++i) {
            const SQVM::CallInfo& ci = th->_callsstack[i];
            s_.i32(ci._etraps);
            s_.i32(ci._prevstkbase);
            s_.i32(ci._prevtop);
            s_.i32(ci._target);
            s_.i32(ci._ncalls);
            s_.word(ci._root);
            s_.u16(ci._vargs.size);
            s_.u16(ci._vargs.base);
            uint32_t ip = 0;
            if (sq_type(ci._closure) == OT_CLOSURE && ci._ip) {
                ip = static_cast<uint32_t>(ci._ip - _funcproto(_closure(ci._closure)->_function)->_instructions);
            }
            s_.u32(ip);
            value(ci._closure);
        }
        for (SQUnsignedInteger i = 0; i < th->_stack.size(); ++i) value(th->_stack[i]);
        s_.end(at);
        return true;
    }

    RefTable& t_;
    Stream s_;
    SQObjectPtr recreate_, reset_;
};

class Loader : Callbacks {
public:
    Loader(HSQUIRRELVM v, RefTable& t, FileStream& f, SQObjectPtr recreate, SQObjectPtr reset, SQObjectPtr error)
        : Callbacks(v), t_(t), s_(f), recreate_(std::move(recreate)), reset_(std::move(reset)), error_(std::move(error)) {
        s_.wide = host(v).game.exe.is64;
    }

    // 0 (or the lowest the error callback returned), -1 if an object could not be made, -2 if the
    // file is not a save.
    int run() {
        // Phantom saves written by Nyanroid before 2026-11-03 have the 32-bit widths: the first
        // prototype's creation record tells (its eight sizes in 32 bytes, or 64).
        size_t fnci = 0;
        uint32_t fnciEnd = 0;
        if (s_.wide && s_.peekRecord("FNCI", fnci, fnciEnd, size_t{1} << 27) && fnciEnd == fnci + 9 + 32) s_.wide = false;
        if (!s_.expect("ROOT")) return -2;
        uint32_t count = s_.r32();
        t_.clear();
        t_.refs.resize(count);
        uint32_t instances = s_.r32();
        if (!s_.expect("INSC")) return -2;
        for (uint32_t i = 0; i < count; ++i) {
            SQObjectPtr o;
            if (!create(o)) return -1;
            if (sq_type(o) == OT_NULL || isObject(o)) {
                t_.refs[i].obj = o;
            } else {
                t_.refs[i].order = -1;
            }
        }
        if (!s_.expect("REFT")) return -2;
        for (uint32_t i = 0; i < count; ++i) {
            if (!content(t_.refs[i].obj)) return -2;
        }
        if (!s_.expect("INSR")) return -2;
        int result = 0;
        for (uint32_t k = 0; k < instances; ++k) {
            uint32_t idx = s_.r32();
            if (idx >= count) return -2;
            if (instanceReset(t_.refs[idx].obj)) continue;
            if (failed) return -2;
            if (sq_type(error_) != OT_CLOSURE) return 0;
            SQObjectPtr ret;
            SQObjectPtr obj = t_.refs[idx].obj;
            if (!call(error_, &obj, &ret)) return -2;
            if (sq_type(ret) == OT_INTEGER && _integer(ret) <= result) result = _integer(ret);
        }
        return result;
    }
    using Callbacks::failed;

private:
    // A value (0x48ee70); `invalid` is set for one that did not load.
    bool value(SQObjectPtr& out, bool* invalid) {
        if (invalid) *invalid = false;
        int32_t kind = s_.ri32();
        if (kind < 0) {
            if (invalid) *invalid = true;
            return true;
        }
        uint32_t type = s_.r32();
        if (kind == 0) {
            switch (type) {
                case OT_NULL: out.Null(); return true;
                case OT_INTEGER: out = static_cast<SQInteger>(s_.rword()); return true;
                case OT_FLOAT: {
                    float f = 0;
                    s_.read(&f, 4);
                    out = static_cast<SQFloat>(f);
                    return true;
                }
                case OT_BOOL: out = SQObjectPtr(s_.rword() != 0); return true;
                case OT_USERPOINTER: out = static_cast<SQUserPointer>(nullptr); return true;
                default: return false;
            }
        }
        uint32_t idx = s_.r32();
        if (kind == 1) {
            if (idx >= t_.refs.size()) return false;
            out = t_.refs[idx].obj;
            if (t_.refs[idx].order < 0 && invalid) *invalid = true;
            return true;
        }
        if (t_.locals.empty() || idx >= t_.locals.back().list.size()) return false;
        out = t_.locals.back().list[idx];
        return true;
    }

    SQObjectPtr rootGet(const SQObjectPtr& key) {
        SQObjectPtr val;
        _table(v_->_roottable)->Get(key, val);
        return val;
    }
    // A name record's name (NTCI / CLSI): a string, or null.
    SQObjectPtr name() {
        uint32_t type = s_.r32();
        if (type != OT_STRING) return SQObjectPtr();
        int64_t len = s_.rword();
        if (len < 0 || len > (1 << 20)) return SQObjectPtr();
        std::string text(len, '\0');
        s_.read(text.data(), len);
        return SQObjectPtr(SQString::Create(_ss(v_), text.c_str(), static_cast<SQInteger>(text.size())));
    }
    // What did not load: the exe's marker is an integer.
    static void unloaded(SQObjectPtr& out) { out = static_cast<SQInteger>(0); }

    // The type and creation record (0x48f510).
    bool create(SQObjectPtr& out) {
        uint32_t type = s_.r32();
        if (!ISREFCOUNTED(type)) return true;
        switch (type) {
            case OT_INSTANCE: {
                if (!s_.expect("INSI")) return false;
                uint32_t end = s_.r32();
                if (sq_type(recreate_) != OT_CLOSURE) return false;
                if (!call(recreate_, nullptr, &out)) return false;
                s_.seek(end);  // Nyanroid: where the record ends, whatever the callback read
                return true;
            }
            case OT_ARRAY: out = SQArray::Create(_ss(v_), 0); return true;
            case OT_TABLE: {
                if (!s_.expect("TABI")) return false;
                s_.r32();
                if (s_.r32() != 0) {
                    out = v_->_roottable;
                } else {
                    out = SQTable::Create(_ss(v_), 0);
                }
                return true;
            }
            case OT_STRING: {
                if (!s_.expect("STRI")) return false;
                s_.r32();
                uint32_t len = s_.r32();
                std::string text(len, '\0');
                s_.read(text.data(), len);
                out = SQString::Create(_ss(v_), text.c_str(), static_cast<SQInteger>(text.size()));
                return true;
            }
            case OT_FUNCPROTO: {
                if (!s_.expect("FNCI")) return false;
                s_.r32();
                SQInteger n[8];
                for (auto& x : n) x = static_cast<SQInteger>(s_.rword());
                for (auto x : n) {
                    if (x < 0 || x > (1 << 24)) return false;
                }
                out = SQFunctionProto::Create(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
                return true;
            }
            case OT_NATIVECLOSURE:
            case OT_CLASS: {
                if (!s_.expect(type == OT_CLASS ? "CLSI" : "NTCI")) return false;
                s_.r32();
                SQObjectPtr key = name();
                SQObjectPtr val = sq_type(key) == OT_STRING ? rootGet(key) : SQObjectPtr();
                if (sq_type(val) == static_cast<SQObjectType>(type)) {
                    out = val;
                } else {
                    unloaded(out);
                }
                return true;
            }
            case OT_CLOSURE: {
                if (!s_.expect("CLOI")) return false;
                s_.r32();
                SQObjectPtr proto;
                if (!value(proto, nullptr) || sq_type(proto) != OT_FUNCPROTO) return false;
                out = SQClosure::Create(_ss(v_), _funcproto(proto));
                return true;
            }
            case OT_USERDATA: unloaded(out); return true;
            case OT_GENERATOR: return true;
            case OT_THREAD: {
                if (!s_.expect("THRI")) return false;
                s_.r32();
                uint32_t size = s_.r32();
                if (size > (1u << 24)) return false;
                if (!sq_newthread(v_, static_cast<SQInteger>(size))) return false;
                out = stackArg(v_, -1);
                sq_pop(v_, 1);
                return true;
            }
            case OT_WEAKREF: {
                if (!s_.expect("WEKI")) return false;
                s_.r32();
                int32_t kind = s_.ri32();
                if (kind < 0) {
                    unloaded(out);
                    return true;
                }
                uint32_t targetType = s_.r32();
                if (targetType == OT_NULL) {
                    out.Null();
                    return true;
                }
                uint32_t idx = s_.r32();
                SQObjectPtr target;
                if (kind == 1 && idx < t_.refs.size()) {
                    target = t_.refs[idx].obj;
                } else if (kind == 2 && !t_.locals.empty() && idx < t_.locals.back().list.size()) {
                    target = t_.locals.back().list[idx];
                } else {
                    return false;
                }
                if (isObject(target)) {
                    out = _refcounted(target)->GetWeakRef(sq_type(target));
                } else {
                    out = target;
                }
                return true;
            }
            default: return false;
        }
    }

    // The type and content record (0x48f120).
    bool content(const SQObjectPtr& o) {
        uint32_t type = s_.r32();
        switch (type) {
            case OT_TABLE: {
                if (!s_.expect("TABL")) return false;
                uint32_t end = s_.r32();
                uint32_t n = s_.r32();
                bool table = sq_type(o) == OT_TABLE;
                bool root = table && _table(o) == _table(v_->_roottable);
                for (uint32_t i = 0; i < n; ++i) {
                    SQObjectPtr key, val;
                    bool badKey = false, badValue = false;
                    if (!value(key, &badKey) || !value(val, &badValue)) return false;
                    if (root && sq_type(val) == OT_CLASS) continue;
                    if (!badKey && !badValue && table && sq_type(key) != OT_NULL) _table(o)->NewSlot(key, val);
                }
                // The delegate, if a table, except the root table's, which keeps its own (SqPlus's
                // _get / _set for the bound variables) (0x48a801).
                if (s_.tell() < end) {
                    SQObjectPtr d;
                    if (!value(d, nullptr)) return false;
                    if (table && !root && sq_type(d) == OT_TABLE) _table(o)->SetDelegate(_table(d));
                }
                return true;
            }
            case OT_ARRAY: {
                if (sq_type(o) != OT_ARRAY) return false;
                if (!s_.expect("ARRY")) return false;
                s_.r32();
                uint32_t n = s_.r32();
                if (n > (1u << 24)) return false;
                _array(o)->Resize(static_cast<SQInteger>(n));
                for (uint32_t i = 0; i < n; ++i) {
                    SQObjectPtr val;
                    if (!value(val, nullptr)) return false;
                    _array(o)->Set(static_cast<SQInteger>(i), val);
                }
                return true;
            }
            case OT_CLOSURE: {
                if (!s_.expect("CLOS")) return false;
                uint32_t end = s_.r32();
                if (sq_type(o) != OT_CLOSURE) {
                    s_.seek(end);
                    return true;
                }
                SQClosure* c = _closure(o);
                if (!value(c->_env, nullptr)) return false;
                for (SQObjectPtrVec* vec : {&c->_outervalues, &c->_defaultparams}) {
                    uint32_t n = s_.r32();
                    if (n > (1u << 20)) return false;
                    vec->resize(n);
                    for (uint32_t i = 0; i < n; ++i) {
                        if (!value((*vec)[i], nullptr)) return false;
                    }
                }
                return true;
            }
            case OT_THREAD: return thread(o);
            case OT_FUNCPROTO: return prototype(o);
            case OT_INSTANCE: {
                if (!s_.expect("INST")) return false;
                uint32_t end = s_.r32();
                if (sq_type(o) == OT_NULL || !isObject(o)) {
                    s_.seek(end);
                    return true;
                }
                if (sq_type(o) != OT_INSTANCE) return false;
                uint32_t n = s_.r32();
                for (uint32_t i = 0; i < n; ++i) {
                    int64_t len = s_.rword();
                    if (len < 0 || len > (1 << 20)) return false;
                    std::string text(static_cast<size_t>(len), '\0');
                    s_.read(text.data(), len);
                    SQObjectPtr key(SQString::Create(_ss(v_), text.c_str(), static_cast<SQInteger>(text.size())));
                    SQObjectPtr val;
                    bool bad = false;
                    if (!value(val, &bad)) return false;
                    if (!bad) _instance(o)->Set(key, val);
                }
                return true;
            }
            case OT_STRING:
            case OT_USERDATA:
            case OT_NATIVECLOSURE:
            case OT_GENERATOR:
            case OT_CLASS:
            case OT_WEAKREF:
            case OT_NULL:
            case OT_USERPOINTER: return true;
            case OT_INTEGER:
            case OT_BOOL: s_.rword(); return true;
            case OT_FLOAT: s_.r32(); return true;
            default: return false;
        }
    }

    // An INRE record (0x489d80).
    bool instanceReset(const SQObjectPtr& o) {
        if (!s_.expect("INRE")) return false;
        uint32_t end = s_.r32();
        if (sq_type(o) == OT_NULL || !isObject(o)) {
            s_.seek(end);
            return true;
        }
        if (sq_type(o) == OT_INSTANCE) {
            if (sq_type(reset_) != OT_CLOSURE) return false;
            if (!call(reset_, &o, nullptr)) return false;
            s_.seek(end);
            return true;
        }
        s_.seek(end);
        return false;
    }

    // FUNC (0x48c7f0): the sizes are the creation record's.
    bool prototype(const SQObjectPtr& o) {
        if (sq_type(o) != OT_FUNCPROTO) return false;
        SQFunctionProto* f = _funcproto(o);
        if (!s_.expect("FUNC")) return false;
        s_.r32();
        if (!value(f->_sourcename, nullptr) || !value(f->_name, nullptr)) return false;
        s_.skip(s_.r32());  // the instructions, read again below
        s_.skip(32);        // their SHA-256 (not checked)
        f->_stacksize = static_cast<SQInteger>(s_.rword());
        if (s_.wide) {
            uint8_t flags[2];
            s_.read(flags, 2);
            f->_bgenerator = flags[0] != 0;
            f->_varparams = flags[1] != 0;
        } else {
            uint8_t a[4], b[4];
            s_.read(a, 4);
            s_.read(b, 4);
            f->_bgenerator = a[0] != 0;
            f->_varparams = b[0] != 0;
        }
        for (SQInteger i = 0; i < f->_nlocalvarinfos; ++i) {
            SQLocalVarInfo& lv = f->_localvarinfos[i];
            if (!value(lv._name, nullptr)) return false;
            lv._start_op = static_cast<SQUnsignedInteger>(s_.rword());
            lv._end_op = static_cast<SQUnsignedInteger>(s_.rword());
            lv._pos = static_cast<SQUnsignedInteger>(s_.rword());
        }
        for (SQInteger i = 0; i < f->_nlineinfos; ++i) {
            f->_lineinfos[i]._line = static_cast<SQInteger>(s_.rword());
            f->_lineinfos[i]._op = static_cast<SQInteger>(s_.rword());
        }
        s_.read(f->_instructions, static_cast<size_t>(f->_ninstructions) * sizeof(SQInstruction));
        for (SQInteger i = 0; i < f->_nliterals; ++i) {
            if (!value(f->_literals[i], nullptr)) return false;
        }
        for (SQInteger i = 0; i < f->_nparameters; ++i) {
            if (!value(f->_parameters[i], nullptr)) return false;
        }
        for (SQInteger i = 0; i < f->_nfunctions; ++i) {
            if (!value(f->_functions[i], nullptr)) return false;
        }
        for (SQInteger i = 0; i < f->_noutervalues; ++i) {
            SQOuterVar& ov = f->_outervalues[i];
            ov._type = static_cast<SQOuterType>(s_.r32());
            if (!value(ov._name, nullptr) || !value(ov._src, nullptr)) return false;
        }
        for (SQInteger i = 0; i < f->_ndefaultparams; ++i) f->_defaultparams[i] = static_cast<SQInteger>(s_.rword());
        return true;
    }

    // THRD (0x48df70).
    bool thread(const SQObjectPtr& o) {
        if (sq_type(o) != OT_THREAD) return false;
        SQVM* th = _thread(o);
        if (!s_.expect("THRD")) return false;
        s_.r32();
        t_.locals.emplace_back();
        struct Pop {
            RefTable& t;
            ~Pop() { t.locals.pop_back(); }
        } pop{t_};
        uint32_t count = s_.r32();
        if (count > (1u << 24)) return false;
        uint32_t instances = s_.r32();
        t_.locals.back().list.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            SQObjectPtr x;
            if (!create(x)) return false;
            t_.locals.back().list[i] = x;
        }
        for (uint32_t i = 0; i < count; ++i) {
            SQObjectPtr x = t_.locals.back().list[i];
            if (!content(x)) return false;
        }
        for (uint32_t k = 0; k < instances; ++k) {
            uint32_t idx = s_.r32();
            if (idx >= count) return false;
            SQObjectPtr x = t_.locals.back().list[idx];
            if (!instanceReset(x)) return false;
        }
        uint32_t stackSize = s_.r32();
        if (stackSize > (1u << 24)) return false;
        th->_top = static_cast<SQInteger>(s_.rword());
        th->_stackbase = static_cast<SQInteger>(s_.rword());
        th->_suspended = static_cast<SQBool>(s_.rword());
        th->_suspended_root = static_cast<SQBool>(s_.rword());
        th->_suspended_target = static_cast<SQInteger>(s_.rword());
        th->_suspended_traps = static_cast<SQInteger>(s_.rword());
        th->_suspend_varargs.size = s_.r16();
        th->_suspend_varargs.base = s_.r16();
        th->_nnativecalls = static_cast<SQInteger>(s_.rword());
        if (!value(th->temp_reg, nullptr)) return false;
        uint32_t vargs = s_.r32();
        if (vargs > (1u << 20)) return false;
        th->_vargsstack.resize(vargs);
        for (uint32_t i = 0; i < vargs; ++i) {
            if (!value(th->_vargsstack[i], nullptr)) return false;
        }
        th->_stack.resize(stackSize);
        SQInteger calls = static_cast<SQInteger>(s_.rword());
        SQInteger alloc = static_cast<SQInteger>(s_.rword());
        s_.r32();
        if (calls < 0 || calls > (1 << 20)) return false;
        alloc = std::max<SQInteger>({alloc, calls, 4});
        th->_callstackdata.resize(static_cast<SQUnsignedInteger>(alloc));
        th->_callsstack = &th->_callstackdata[0];
        th->_alloccallsstacksize = alloc;
        th->_callsstacksize = calls;
        th->ci = calls > 0 ? &th->_callsstack[calls - 1] : nullptr;
        for (SQInteger i = 0; i < calls; ++i) {
            SQVM::CallInfo& ci = th->_callsstack[i];
            ci._etraps = s_.ri32();
            ci._prevstkbase = s_.ri32();
            ci._prevtop = s_.ri32();
            ci._target = s_.ri32();
            ci._ncalls = s_.ri32();
            ci._root = static_cast<SQBool>(s_.rword());
            ci._vargs.size = s_.r16();
            ci._vargs.base = s_.r16();
            ci._generator = nullptr;
            uint32_t ip = s_.r32();
            if (!value(ci._closure, nullptr)) return false;
            ci._ip = nullptr;
            ci._literals = nullptr;
            if (sq_type(ci._closure) == OT_CLOSURE) {
                SQFunctionProto* f = _funcproto(_closure(ci._closure)->_function);
                if (ip > static_cast<uint32_t>(f->_ninstructions)) return false;
                ci._ip = f->_instructions + ip;
                ci._literals = f->_literals;
            }
        }
        for (uint32_t i = 0; i < stackSize; ++i) {
            if (!value(th->_stack[i], nullptr)) return false;
        }
        return true;
    }

    RefTable& t_;
    Stream s_;
    SQObjectPtr recreate_, reset_, error_;
};

FileStream* findFile(ScriptHost::Impl& h, const std::string& name) { return dynamic_cast<FileStream*>(h.findTask(name)); }

// CreateReferenceTable(order: closure, ignore: array) (0x43e4c0).
SQInteger createReferenceTable(HSQUIRRELVM v) {
    auto& h = host(v);
    RefTable& t = referenceTable(h);
    t.clear();
    SQObjectPtr order = stackArg(v, 2), ignore = stackArg(v, 3);
    std::unordered_set<uintptr_t> skip, seen;
    if (sq_type(ignore) == OT_ARRAY) {
        SQArray* a = _array(ignore);
        for (SQUnsignedInteger i = 0; i < a->_values.size(); ++i) {
            if (isObject(a->_values[i])) skip.insert(objectKey(a->_values[i]));
        }
    }
    std::vector<SQObjectPtr> found, stack;
    auto visit = [&](const SQObjectPtr& x) {
        if (!isObject(x)) return;
        uintptr_t k = objectKey(x);
        if (skip.count(k) || !seen.insert(k).second) return;
        found.push_back(x);
        stack.push_back(x);
    };
    visit(v->_roottable);
    while (!stack.empty()) {
        SQObjectPtr o = stack.back();
        stack.pop_back();
        forEachHeld(o, visit);
    }
    Callbacks calls(v);
    t.refs.reserve(found.size());
    for (const auto& o : found) {
        int32_t at = 0x7ffffffd;
        switch (sq_type(o)) {
            case OT_INSTANCE:
                if (sq_type(order) == OT_CLOSURE) {
                    SQObjectPtr r;
                    if (!calls.call(order, &o, &r)) {
                        t.clear();
                        return SQ_ERROR;
                    }
                    if (sq_type(r) == OT_INTEGER) at = _integer(r);
                }
                break;
            case OT_STRING: at = -2; break;
            case OT_FUNCPROTO: at = -1; break;
            case OT_WEAKREF: at = 0x7ffffffe; break;
            case OT_TABLE:
                if (_table(o) == _table(v->_roottable)) at = 0x7fffffff;
                break;
            default: break;
        }
        t.refs.push_back({at, o});
    }
    std::stable_sort(t.refs.begin(), t.refs.end(), [](const RefTable::Entry& a, const RefTable::Entry& b) { return a.order < b.order; });
    for (size_t i = 0; i < t.refs.size(); ++i) t.index[objectKey(t.refs[i].obj)] = static_cast<int32_t>(i);
    // The root table's native functions and classes, by value.
    SQObjectPtr pos(static_cast<SQInteger>(0)), key, val;
    SQInteger next;
    while ((next = _table(v->_roottable)->Next(true, pos, key, val)) != -1) {
        if (sq_type(val) == OT_NATIVECLOSURE || sq_type(val) == OT_CLASS) t.names.emplace(objectKey(val), key);
        pos = next;
    }
    return 0;
}

SQInteger clearReferenceTable(HSQUIRRELVM v) {
    auto& h = host(v);
    if (h.referenceTable) h.referenceTable->clear();
    return 0;
}

// SerializeRootTable(file: string, recreate: closure, reset: closure): 0, or -1 (0x48f750).
SQInteger serializeRootTable(HSQUIRRELVM v) {
    auto& h = host(v);
    FileStream* f = findFile(h, argString(v, 2));
    if (!f) {
        sq_pushinteger(v, -1);
        return 1;
    }
    Saver saver(v, referenceTable(h), *f, stackArg(v, 3), stackArg(v, 4));
    int r = saver.run();
    if (saver.failed) return SQ_ERROR;  // the callback's error, raised again
    sq_pushinteger(v, r);
    return 1;
}

// DeserializeRootTable(file: string, recreate: closure, reset: closure, error: closure): 0 or the
// lowest the error callback returned; -1 or -2 if the save does not load (0x48fb20).
SQInteger deserializeRootTable(HSQUIRRELVM v) {
    auto& h = host(v);
    FileStream* f = findFile(h, argString(v, 2));
    if (!f) {
        sq_pushinteger(v, -2);
        return 1;
    }
    Loader loader(v, referenceTable(h), *f, stackArg(v, 3), stackArg(v, 4), stackArg(v, 5));
    int r = loader.run();
    if (loader.failed) return SQ_ERROR;
    sq_pushinteger(v, r);
    return 1;
}

// The framing of the engine objects' records (serialize / deserialize): a record begins with the
// position of its end, and each field follows the position it is written at, which the reader
// checks.
class Fields {
public:
    explicit Fields(FileStream& f) : f_(f) {}
    size_t open() {
        size_t at = f_.tell();
        u32(static_cast<uint32_t>(at));
        return at;
    }
    void close(size_t at) {
        size_t e = f_.tell();
        f_.seekTo(at);
        u32(static_cast<uint32_t>(e));
        f_.seekTo(e);
    }
    void marker() { u32(static_cast<uint32_t>(f_.tell())); }
    void raw(uint32_t v) { u32(v); }
    void field(uint32_t v) {
        marker();
        u32(v);
    }
    void field(float v) {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        field(b);
    }
    // Reading: a record's end, a field (false if its position does not match), a value read as is.
    uint32_t end() { return r32(); }
    uint32_t value() { return r32(); }
    float valueFloat() {
        uint32_t b = r32();
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }
    bool marked() {
        uint32_t at = static_cast<uint32_t>(f_.tell());
        return r32() == at;
    }
    bool readField(uint32_t& v) {
        if (!marked()) return false;
        v = r32();
        return true;
    }
    void seek(size_t p) { f_.seekTo(p); }

private:
    void u32(uint32_t v) { f_.writeBytes(&v, 4); }
    uint32_t r32() {
        uint32_t v = 0;
        f_.readBytes(&v, 4);
        return v;
    }
    FileStream& f_;
};

// A tween's record (0x4a9570, 0x4dfe30, 0x4a8ba0 for one, two, three components): its end, then
// each field after its position: actionTime, timer, src, dst, now, nowNoPower (n floats each),
// tempo, active, power, timeScale (not the last step's stamp).
void writeTween(Fields& w, const Tween& t) {
    size_t at = w.open();
    w.field(t.actionTime);
    w.field(t.timer);
    for (const auto* v : {&t.src, &t.dst, &t.now, &t.nowNoPower}) {
        w.marker();
        for (int i = 0; i < t.n; ++i) {
            uint32_t b;
            std::memcpy(&b, &(*v)[static_cast<size_t>(i)], 4);
            w.raw(b);
        }
    }
    w.field(static_cast<uint32_t>(t.tempo));
    w.field(static_cast<uint32_t>(t.active));
    w.field(t.power);
    w.field(t.timeScale);
    w.close(at);
}
bool readTween(Fields& r, Tween& t) {
    uint32_t end = r.end();
    Tween x = t;
    uint32_t v = 0;
    bool ok = r.readField(v);
    std::memcpy(&x.actionTime, &v, 4);
    ok = ok && r.readField(v);
    std::memcpy(&x.timer, &v, 4);
    for (auto* a : {&x.src, &x.dst, &x.now, &x.nowNoPower}) {
        ok = ok && r.marked();
        for (int i = 0; ok && i < x.n; ++i) (*a)[static_cast<size_t>(i)] = r.valueFloat();
    }
    ok = ok && r.readField(v);
    x.tempo = static_cast<int32_t>(v);
    ok = ok && r.readField(v);
    x.active = static_cast<int32_t>(v);
    ok = ok && r.readField(v);
    std::memcpy(&x.power, &v, 4);
    ok = ok && r.readField(v);
    std::memcpy(&x.timeScale, &v, 4);
    if (ok) t = x;
    r.seek(end);
    return ok;
}

void writeShake(Fields& w, const Shake* s);
std::unique_ptr<Shake> readShake(Fields& r, uint32_t kind);

// A text character's record (0x4d9ed0; CText's 0x3d4-byte entry, ruby characters the same): +0x4
// (16 bytes: +0xc / +0x10 the cell; +0x4 / +0x8 the atlas place, not kept by Nyanroid: 0), +0x14 its
// place, +0x1c shown, +0x20 typing start, +0x24 code, +0x4c its effects flag, then each own effect's
// flag and tween (+0x50 / +0x54 fade, +0x80 / +0x84 move, +0xc0 / +0xc4 scale, +0x100 / +0x104
// rotate), +0x150 its own colours and the four corner colours' tweens (+0x154 + 0x4c i), each
// followed by its packed colour (+0x284 + 4 i: the bytes of r, g, b and 255, as the update packs
// them); then its shake as a sprite's (the kind, then its record).
void writeTextChar(Fields& w, const TextChar& c) {
    size_t at = w.open();
    w.marker();
    w.raw(0u), w.raw(0u), w.raw(static_cast<uint32_t>(c.cellWidth)), w.raw(static_cast<uint32_t>(c.cellHeight));
    w.marker();
    uint32_t b;
    std::memcpy(&b, &c.x, 4), w.raw(b);
    std::memcpy(&b, &c.y, 4), w.raw(b);
    w.field(c.shown);
    w.field(c.typingStart);
    w.field(c.code);
    w.field(static_cast<uint32_t>(c.effects));
    w.field(static_cast<uint32_t>(c.fadeSet));
    w.marker(), writeTween(w, c.fade);
    w.field(static_cast<uint32_t>(c.moveSet));
    w.marker(), writeTween(w, c.move);
    w.field(static_cast<uint32_t>(c.scaleSet));
    w.marker(), writeTween(w, c.scale);
    w.field(static_cast<uint32_t>(c.rotateSet));
    w.marker(), writeTween(w, c.rotate);
    w.field(static_cast<uint32_t>(c.colorSet));
    for (const auto& t : c.color) {
        w.marker(), writeTween(w, t);
        uint32_t packed = 0xff000000u;
        for (int k = 0; k < 3; ++k) {
            auto byte = static_cast<uint32_t>(static_cast<int32_t>(std::clamp(t.now[static_cast<size_t>(k)], 0.0f, 1.0f) * 255.0f)) & 0xffu;
            packed |= byte << (8 * k);
        }
        w.field(packed);
    }
    writeShake(w, c.shake.get());
    w.close(at);
}
// Reads one into `c` (null: skipped); its place and code stay the layout's.
void readTextChar(Fields& r, TextChar* c) {
    uint32_t end = r.end();
    TextChar x = c ? *c : TextChar{};
    uint32_t v = 0;
    bool ok = r.marked();
    for (int i = 0; ok && i < 4; ++i) r.value();
    ok = ok && r.marked();
    if (ok) r.value(), r.value();
    ok = ok && r.readField(v);
    std::memcpy(&x.shown, &v, 4);
    ok = ok && r.readField(v);
    std::memcpy(&x.typingStart, &v, 4);
    ok = ok && r.readField(v) && r.readField(v);  // the code, +0x4c
    x.effects = v != 0;
    auto effect = [&](bool& set, Tween& t) {
        ok = ok && r.readField(v);
        set = v != 0;
        ok = ok && r.marked() && readTween(r, t);
    };
    effect(x.fadeSet, x.fade);
    effect(x.moveSet, x.move);
    effect(x.scaleSet, x.scale);
    effect(x.rotateSet, x.rotate);
    ok = ok && r.readField(v);
    x.colorSet = v != 0;
    for (auto& t : x.color) ok = ok && r.marked() && readTween(r, t) && r.readField(v);
    // Its shake: the old one dropped, the saved one made (0x4da560, as a sprite's).
    uint32_t kind = 0;
    if (ok && r.readField(kind)) {
        x.shake.reset();
        if (kind != 0 && kind <= 2) x.shake = std::shared_ptr<Shake>(readShake(r, kind));
    }
    if (ok && c) *c = std::move(x);
    r.seek(end);
}

// A sprite's shake in its record (CSprite 0x45ca30 / 0x45cbf0): its kind (+0x20: 0 none, 1
// CEffectShake, 2 CEffectRandomShake), then, for a shake, its position and record (0x4a7dc0: the
// three tweens and the six fields; the random one's own record follows, 0x4aa270: its end, its
// position, the generator's index, size and words, 0x55b7f0).
void writeShake(Fields& w, const Shake* s) {
    uint32_t kind = s ? (s->random() ? 2u : 1u) : 0u;
    w.field(kind);
    if (!s) return;
    w.marker();
    Shake::Saved v = s->saved();
    size_t at = w.open();
    w.marker(), writeTween(w, v.master);
    w.marker(), writeTween(w, v.amplitude);
    w.marker(), writeTween(w, v.swing);
    w.field(static_cast<uint32_t>(v.interpolate));
    w.field(static_cast<uint32_t>(v.beatTempo));
    w.field(v.sign);
    w.field(static_cast<uint32_t>(v.final));
    w.field(v.frequency);
    w.field(v.edge);
    w.close(at);
    if (kind == 2) {
        MwareRandom& g = const_cast<Shake*>(s)->generator();
        size_t r = w.open();
        w.marker();
        w.raw(static_cast<uint32_t>(g.index()));
        w.raw(static_cast<uint32_t>(MwareRandom::kN * 4));
        for (int32_t word : g.state()) w.raw(static_cast<uint32_t>(word));
        w.close(r);
    }
}
// Reads one (after its kind was read as `kind`, 1 or 2): the shake, or null if it does not read.
std::unique_ptr<Shake> readShake(Fields& r, uint32_t kind) {
    if (!r.marked()) return nullptr;
    auto s = std::make_unique<Shake>(kind == 2);
    Shake::Saved v;
    uint32_t end = r.end(), x = 0;
    bool ok = r.marked() && readTween(r, v.master) && r.marked() && readTween(r, v.amplitude) && r.marked() &&
              readTween(r, v.swing);
    ok = ok && r.readField(x), v.interpolate = static_cast<int32_t>(x);
    ok = ok && r.readField(x), v.beatTempo = static_cast<int32_t>(x);
    ok = ok && r.readField(x), std::memcpy(&v.sign, &x, 4);
    ok = ok && r.readField(x), v.final = static_cast<int32_t>(x);
    ok = ok && r.readField(x), v.frequency = x;
    ok = ok && r.readField(x), v.edge = x;
    r.seek(end);
    if (!ok) return nullptr;
    s->restore(v);
    if (kind == 2) {
        uint32_t rend = r.end();
        std::array<int32_t, MwareRandom::kN> words{};
        if (!r.marked()) return nullptr;
        uint32_t index = r.value(), size = r.value();
        if (size != MwareRandom::kN * 4) return nullptr;
        for (auto& word : words) word = static_cast<int32_t>(r.value());
        s->generator().restore(words, static_cast<int>(index));
        r.seek(rend);
    }
    return s;
}

// A blur trail's record (0x4a4cf0, CSprite +0x65c): levels, interval, power, order, fade, the
// history (+0x2c its count; each frame a list of 0x70-byte entries, 0x4a4b60: four corners' x, y, z,
// w, four colours as bytes r, g, b, 255 and four texture coordinates) and a list of integers
// (0x4a6170). Nyanroid's frames are one quad each, in its own screen pixels with w 1 and texture
// coordinates 0 (Fitted: the exe's corners are the vertex build's).
void writeBlur(Fields& w, const SpriteImage::Blur* b) {
    size_t at = w.open();
    SpriteImage::Blur none;
    if (!b) b = &none;
    w.field(static_cast<uint32_t>(b->levels));
    w.field(b->interval);
    w.field(b->power);
    w.field(static_cast<uint32_t>(b->order));
    w.field(static_cast<uint32_t>(b->fade));
    w.field(static_cast<uint32_t>(b->history.size()));
    for (const auto& frame : b->history) {
        w.marker();
        w.raw(1u);
        w.marker();
        for (const auto& c : frame.quad.corner) {
            float xyzw[4] = {c[0], c[1], c[2], 1.0f};
            for (float v : xyzw) {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                w.raw(bits);
            }
        }
        for (const auto& rgb : frame.rgb) {
            uint32_t packed = 0xff000000u;
            for (int k = 0; k < 3; ++k) {
                auto byte = static_cast<uint32_t>(static_cast<int32_t>(std::clamp(rgb[static_cast<size_t>(k)], 0.0f, 1.0f) * 255.0f)) & 0xffu;
                packed |= byte << (8 * k);
            }
            w.raw(packed);
        }
        for (int k = 0; k < 8; ++k) w.raw(0u);
    }
    w.marker();  // the integer list: its count, its items
    w.raw(0u);
    w.close(at);
}
// A text batch's trail (CBlur's record, 0x4a4cf0): the five settings, the frames (each its quads'
// count, then each quad's 0x70 bytes: the corners, the packed colours with the alpha byte before the
// fade, the atlas UVs (Nyanroid's atlas is its own: 0)), the picks list (empty).
void writeTextBlur(Fields& w, const SpriteImage::Blur* b, const std::deque<std::vector<TextGlyphDraw>>* frames) {
    size_t at = w.open();
    SpriteImage::Blur none;
    if (!b) b = &none;
    w.field(static_cast<uint32_t>(b->levels));
    w.field(b->interval);
    w.field(b->power);
    w.field(static_cast<uint32_t>(b->order));
    w.field(static_cast<uint32_t>(b->fade));
    w.field(static_cast<uint32_t>(frames ? frames->size() : 0));
    if (frames) {
        for (const auto& frame : *frames) {
            w.marker();
            w.raw(static_cast<uint32_t>(frame.size()));
            for (const auto& g : frame) {
                w.marker();
                for (const auto& c : g.item.quad.corner) {
                    float xyzw[4] = {c[0], c[1], c[2], 1.0f};
                    for (float v : xyzw) {
                        uint32_t bits;
                        std::memcpy(&bits, &v, 4);
                        w.raw(bits);
                    }
                }
                for (const auto& col : g.item.color) {
                    uint32_t packed = static_cast<uint32_t>(static_cast<int32_t>(g.alphaByte)) << 24;
                    for (int k = 0; k < 3; ++k) {
                        auto byte = static_cast<uint32_t>(static_cast<int32_t>(std::clamp(col[static_cast<size_t>(k)], 0.0f, 1.0f) * 255.0f)) & 0xffu;
                        packed |= byte << (8 * k);
                    }
                    w.raw(packed);
                }
                for (int k = 0; k < 8; ++k) w.raw(0u);
            }
        }
    }
    w.marker();
    w.raw(0u);
    w.close(at);
}

// Reads the history after the five settings (into `b`, frames of one entry only).
void readBlurHistory(Fields& r, SpriteImage::Blur& b) {
    b.history.clear();
    uint32_t frames = 0;
    if (!r.readField(frames)) return;
    for (uint32_t i = 0; i < frames; ++i) {
        if (!r.marked()) return;
        uint32_t entries = r.value();
        for (uint32_t e = 0; e < entries; ++e) {
            if (!r.marked()) return;
            SpriteImage::BlurFrame f;
            for (auto& c : f.quad.corner) {
                c[0] = r.valueFloat(), c[1] = r.valueFloat(), c[2] = r.valueFloat();
                r.value();
            }
            for (auto& rgb : f.rgb) {
                uint32_t packed = r.value();
                for (int k = 0; k < 3; ++k) rgb[static_cast<size_t>(k)] = static_cast<float>((packed >> (8 * k)) & 0xffu) / 255.0f;
            }
            for (int k = 0; k < 8; ++k) r.value();
            if (entries == 1) b.history.push_back(f);
        }
    }
}

}  // namespace

// serialize / deserialize(file) of SQSprite, SQSpriteMask, SQSpriteRenderTarget and SQText
// (0x500648 / 0x500620: the object's vtable +0x88 / +0x8c; SQSprite 0x510580 / 0x510950,
// SQSpriteMask 0x517800, SQSpriteRenderTarget 0x51cc70, SQText 0x523330 -> CText 0x4dace0): the
// object's engine state in the save file the scripts opened under that name, after the task
// object's empty record (0x565c70). A sprite's (CSprite 0x45ca30): its shake, then its blur trail;
// the SQSprite and render-target wrappers then add +0xcc and the pixels of a picture they keep. A
// text's: its shake, its characters' records (two lists), two blur trails and its kerning pairs.
// Kept here: the framing, a sprite's shake (its tweens and fields, a random shake's generator), the
// blur settings and the trail's history, a frozen capture's picture, the text's characters (typing
// state, own effects); not kept: a render target's picture, a text's or its characters' shakes and
// trails, kerning pairs (the scripts restore the rest by replaying the objects' last actions at
// their timers, before reading these records).
bool writeEngineRecord(ScriptHost::Impl& h, const std::string& file, EngineRecord kind, const EngineState& st) {
    FileStream* f = dynamic_cast<FileStream*>(h.findTask(file));
    if (!f) return false;
    Fields w(*f);
    if (kind == EngineRecord::Camera) {
        // SQCamera 0x52cc10: its shake (the kind, then its record, as a sprite's).
        size_t at = w.open();
        writeShake(w, st.shake ? st.shake->get() : nullptr);
        w.close(at);
        return true;
    }
    if (kind == EngineRecord::Particle) {
        // SQParticle 0x538440: the record's end, its own position, then the system's record
        // (0x479ea0): its end and fields, each after its position. Nyanroid writes the system's own
        // fields and stops there (not its generator or particles).
        size_t at = w.open();
        w.marker();
        if (st.particles) {
            ParticleSystem::Saved s = st.particles->saved();
            size_t sys = w.open();
            w.field(static_cast<uint32_t>(s.visible));
            w.field(static_cast<uint32_t>(s.begun));
            w.field(static_cast<uint32_t>(s.subActive));
            w.field(s.fade);
            w.field(s.totalTime);
            w.field(s.timeScale);
            w.field(s.seekDelay);
            w.close(sys);
        }
        w.close(at);
        return true;
    }
    w.close(w.open());  // the task object's record (0x565c70): empty
    if (kind == EngineRecord::Text) {
        if (!st.hasImage) return false;
        size_t at = w.open();
        // CText's record (0x4dace0): its shake (as a sprite's: the kind, then its record), the
        // characters, the ruby characters (each its own record), its two trails (+0x500 / +0x53c),
        // the position-fix rules (each 12 bytes: the two codes and the
        // value).
        writeShake(w, st.shake ? st.shake->get() : nullptr);
        for (const std::vector<TextChar>* list : {st.chars, st.rubies}) {
            w.field(static_cast<uint32_t>(list ? list->size() : 0));
            if (list) {
                for (const auto& c : *list) writeTextChar(w, c);
            }
        }
        for (size_t k = 0; k < 2; ++k) writeTextBlur(w, st.textBlur, st.textBlurFrames ? &(*st.textBlurFrames)[k] : nullptr);
        w.field(static_cast<uint32_t>(st.fixRules ? st.fixRules->size() : 0));
        if (st.fixRules) {
            for (const auto& rule : *st.fixRules) {
                w.marker();
                w.raw(rule.first), w.raw(rule.second), w.raw(static_cast<uint32_t>(rule.value));
            }
        }
        w.close(at);
        return true;
    }
    size_t outer = w.open();
    w.marker();
    bool ok = st.hasImage || kind == EngineRecord::Mask;
    if (st.hasImage) {
        size_t inner = w.open();  // CSprite (0x45ca30)
        writeShake(w, st.shake ? st.shake->get() : nullptr);
        writeBlur(w, st.blur);
        w.close(inner);
    }
    if (st.hasImage && kind != EngineRecord::Mask) {
        int32_t capture = st.capture ? *st.capture : 0;
        w.field(static_cast<uint32_t>(capture));  // +0xcc
        // A frozen screen capture keeps its picture (the texture's rows: RGBA here); a render
        // target's kept picture is not saved yet.
        std::vector<uint8_t> rgba;
        int sw = h.screenWidth(), sh = h.screenHeight();
        if (kind == EngineRecord::Sprite && st.screen && capture == 0 && h.screenReader &&
            h.screenReader(st.screen, sw, sh, rgba) && rgba.size() == static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4) {
            w.field(static_cast<uint32_t>(rgba.size()));
            f->writeBytes(rgba.data(), rgba.size());
        } else {
            w.field(0u);
        }
    }
    w.close(outer);
    return ok;
}

bool readEngineRecord(ScriptHost::Impl& h, const std::string& file, EngineRecord kind, const EngineState& st) {
    FileStream* f = dynamic_cast<FileStream*>(h.findTask(file));
    if (!f) return false;
    Fields r(*f);
    if (kind == EngineRecord::Camera) {
        // The camera's record (0x52ce70): the old shake is dropped, the saved one made.
        uint32_t end = r.end();
        uint32_t shake = 0;
        if (st.shake) st.shake->reset();
        if (r.readField(shake) && shake != 0 && shake <= 2 && st.shake) *st.shake = readShake(r, shake);
        r.seek(end);
        return true;
    }
    if (kind == EngineRecord::Particle) {
        // The particle system's record (0x538610 -> 0x47a480): its fields, when they are there.
        uint32_t end = r.end();
        if (st.particles && r.marked()) {
            uint32_t sysEnd = r.end();
            uint32_t vis = 0, begun = 0, sub = 0, fade = 0, total = 0, scale = 0, seek = 0;
            if (r.readField(vis) && r.readField(begun) && r.readField(sub) && r.readField(fade) && r.readField(total) &&
                r.readField(scale) && r.readField(seek)) {
                ParticleSystem::Saved s;
                s.visible = static_cast<int32_t>(vis), s.begun = static_cast<int32_t>(begun), s.subActive = static_cast<int32_t>(sub);
                std::memcpy(&s.fade, &fade, 4), std::memcpy(&s.totalTime, &total, 4), std::memcpy(&s.timeScale, &scale, 4),
                    std::memcpy(&s.seekDelay, &seek, 4);
                st.particles->restore(s);
            }
            r.seek(sysEnd);
        }
        r.seek(end);
        return true;
    }
    r.seek(r.end());  // the task object's record
    uint32_t outer = r.end();
    if (kind == EngineRecord::Text) {
        // CText's record (0x4db170): the characters and the ruby characters come back when the text
        // laid out again has as many (the framework makes it again from its last calls before
        // reading this; other counts fail the read), then the fix rules replace the text's.
        // Its shake first: the old one is dropped and the saved one made (0x4db170, as 0x45cbf0).
        uint32_t shake = 0, count = 0;
        bool ok = st.chars && r.readField(shake);
        if (ok && shake != 0) {
            std::unique_ptr<Shake> s = shake <= 2 ? readShake(r, shake) : nullptr;
            ok = s != nullptr;
            if (st.shake) *st.shake = std::move(s);
        } else if (ok && st.shake) {
            st.shake->reset();
        }
        for (std::vector<TextChar>* list : {st.chars, st.rubies}) {
            ok = ok && list && r.readField(count) && (count == 0 || count == list->size());
            for (uint32_t i = 0; ok && i < count; ++i) readTextChar(r, &(*list)[i]);
        }
        // The two trails: their settings (the frames hold the exe's atlas places: the trail starts
        // afresh).
        for (int k = 0; ok && k < 2; ++k) {
            uint32_t end = r.end();
            uint32_t levels = 0, interval = 0, power = 0, order = 0, fade = 0;
            if (st.textBlur && r.readField(levels) && r.readField(interval) && r.readField(power) && r.readField(order) &&
                r.readField(fade)) {
                st.textBlur->levels = static_cast<int32_t>(levels);
                st.textBlur->interval = interval;
                std::memcpy(&st.textBlur->power, &power, 4);
                st.textBlur->order = static_cast<int32_t>(order);
                st.textBlur->fade = static_cast<int32_t>(fade);
            }
            if (st.textBlurFrames) (*st.textBlurFrames)[static_cast<size_t>(k)].clear();
            r.seek(end);
        }
        if (ok && st.fixRules && r.readField(count)) {
            std::vector<PositionFixRule> rules;
            for (uint32_t i = 0; i < count && r.marked(); ++i) {
                PositionFixRule rule;
                rule.first = r.value(), rule.second = r.value(), rule.value = static_cast<int32_t>(r.value());
                rules.push_back(rule);
            }
            *st.fixRules = std::move(rules);
        }
        r.seek(outer);
        return ok;
    }
    if (kind != EngineRecord::Text && st.hasImage && r.marked()) {
        uint32_t inner = r.end();
        uint32_t shake = 0, blurEnd = 0;
        // The shake: the old one is dropped and the saved one made (0x45cbf0).
        bool shakeRead = r.readField(shake);
        if (shakeRead && shake != 0) {
            std::unique_ptr<Shake> s = shake <= 2 ? readShake(r, shake) : nullptr;
            shakeRead = s != nullptr;
            if (st.shake) *st.shake = std::move(s);
        } else if (shakeRead && st.shake) {
            st.shake->reset();
        }
        if (st.blur && shakeRead) {
            blurEnd = r.end();
            uint32_t levels = 0, interval = 0, power = 0, order = 0, fade = 0;
            if (r.readField(levels) && r.readField(interval) && r.readField(power) && r.readField(order) &&
                r.readField(fade)) {
                st.blur->levels = static_cast<int32_t>(levels);
                st.blur->interval = interval;
                std::memcpy(&st.blur->power, &power, 4);
                st.blur->order = static_cast<int32_t>(order);
                st.blur->fade = static_cast<int32_t>(fade);
                readBlurHistory(r, *st.blur);
            }
            r.seek(blurEnd);
        }
        r.seek(inner);
        uint32_t capture = 0, size = 0;
        if (kind != EngineRecord::Mask && r.readField(capture) && r.readField(size)) {
            if (st.capture) *st.capture = static_cast<int32_t>(capture);
            int sw = h.screenWidth(), sh = h.screenHeight();
            if (st.picture && size == static_cast<uint32_t>(sw) * static_cast<uint32_t>(sh) * 4) {
                auto pic = std::make_shared<ScreenPicture>();
                pic->width = sw, pic->height = sh;
                pic->rgba.resize(size);
                if (f->readBytes(pic->rgba.data(), size) == size) *st.picture = std::move(pic);
            }
        }
    }
    r.seek(outer);
    return true;
}

std::string suspendedThreadStacks(HSQUIRRELVM v) {
    std::string out;
    for (SQCollectable* c = _ss(v)->_gc_chain; c; c = c->_next) {
        auto* t = dynamic_cast<SQVM*>(c);
        if (!t || !t->_suspended || t == v) continue;
        out += "thread:\n";
        SQStackInfos si;
        for (SQInteger level = 0; SQ_SUCCEEDED(sq_stackinfos(t, level, &si)); ++level) {
            out += "  at ";
            out += si.funcname ? si.funcname : "?";
            out += " (";
            out += si.source ? si.source : "?";
            out += ":" + std::to_string(si.line) + ")\n";
        }
    }
    return out;
}

std::vector<NativeImpl> vmStateGlobals() {
    return {{"CreateReferenceTable", createReferenceTable},
            {"ClearReferenceTable", clearReferenceTable},
            {"SerializeRootTable", serializeRootTable},
            {"DeserializeRootTable", deserializeRootTable}};
}

}  // namespace nyan::diesel
