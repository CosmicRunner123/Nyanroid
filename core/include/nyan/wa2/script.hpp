#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

// A script function's signature, from LFScriptFunc.fnc (ids 0..) and LFScriptFuncEx.fnc (ids 0x80..)
// (WA2.exe 0x450360). The argument "names" in the file are default strings: a string argument given
// as -1 takes its default (0x453ad0).
struct ScriptFunction {
    int id = -1;
    std::string name;
    int flag = 0;        // byte 0x21 of the record: set on functions that may wait
    int returnType = 0;  // 3 int, 4 float, 5 string
    std::vector<int> argTypes;  // 1, 2, 3, 7 integers (floats are cut to int), 4 float, 5 string, 6 as given
    std::vector<std::string> defaults;
};

class FunctionTable {
public:
    // Adds the records of one .fnc file (its first word is the first id).
    void load(ByteSpan fnc);
    const ScriptFunction* find(int id) const;
    const ScriptFunction* find(std::string_view name) const;
    const std::map<int, ScriptFunction>& all() const { return functions_; }

private:
    std::map<int, ScriptFunction> functions_;
};

// A compiled script (.bnr, `LSCR`; loader WA2.exe 0x453100): the code as 32-bit words and its
// labels (id -> byte offset in the file).
struct Script {
    Bytes data;
    std::map<int, uint32_t> labels;
    uint32_t codeStart = 0;  // the first byte after the label table

    static Script parse(Bytes data);
    uint32_t word(uint32_t offset) const;  // throws past the end
};

// The script's strings (.txt beside it; 0x452f20): fields separated by commas, a leading comma or two
// in a row giving empty fields (the tokenizer 0x44d950 is Shift-JIS aware), and an empty string after
// the last.
std::vector<std::string> splitStrings(std::string_view text);

// Opcodes (the dispatcher 0x453f10): 0 control (with a sub-op, 0x4532f0), 1 global variable, 2 local
// array variable, 3 string of the script's table, 4 call, 5 literal (type, value), 6 operator
// (0x451800).
enum class Op : uint32_t { Control = 0, Global = 1, Local = 2, String = 3, Call = 4, Push = 5, Operator = 6 };

// One instruction as stored.
struct Instruction {
    uint32_t offset = 0;
    Op op = Op::Control;
    std::vector<uint32_t> operands;  // after the opcode word (for Control the sub-op first)
    uint32_t size() const { return 4 * (1 + static_cast<uint32_t>(operands.size())); }
};

// Reads the instruction at `offset`; throws on an unknown opcode or one running past the end.
Instruction decodeInstruction(const Script& script, uint32_t offset);

// A readable listing: control flow as stored, expressions and calls rebuilt from the stack.
std::string disassemble(const Script& script, const std::vector<std::string>& strings, const FunctionTable& functions);

}  // namespace nyan::wa2
