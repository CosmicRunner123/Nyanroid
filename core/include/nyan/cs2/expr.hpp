#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "nyan/cs2/vars.hpp"

namespace nyan::cs2 {

// Evaluates CatSystem2 integer expressions, e.g. `#(950+#300)`, `512-256`,
// `(#100==3) && (#4!=0)`, `0x10`. Variables are written with a sigil
// (`#` in scene scripts, `$` in FE scripts) followed by a number or a
// parenthesised expression. `@` evaluates to `current` (used by move-style
// commands to mean "keep the previous value").
class ExprEvaluator {
public:
    // Resolves bare identifiers (FE script macros, `_EXITCODE_`, object names).
    using IdentResolver = std::function<std::optional<int32_t>(std::string_view)>;

    ExprEvaluator(VarStore& vars, char sigil = '#') : vars_(vars), sigil_(sigil) {}

    // FE scripts also have locals (`\N`) and named identifiers.
    void setLocals(VarStore* locals, char sigil = '\\') {
        locals_ = locals;
        localSigil_ = sigil;
    }
    void setIdentResolver(IdentResolver resolver) { idents_ = std::move(resolver); }
    // The FE scripts' evaluator (cs2.exe 0x5f2260, Amakano 1) returns what its tree walk 0x5f3210
    // returns, and that walk's unary-plus case (0x5f40dc) only stores the operand's value in the
    // result block (which a parent node reads, 0x5f4350): the register it returns (EBX, 0 at the
    // start) is left as is. An expression whose root is a unary plus (`+900`, `(+5)`) is therefore
    // 0; one under an operator is its value. On: `eval` returns 0 for such an expression.
    void setPlusRootIsZero(bool on) { plusRootZero_ = on; }

    // Evaluates a whole expression; throws nyan::Error on syntax errors.
    int32_t eval(std::string_view expr, int32_t current = 0) const;

    // Executes an assignment statement such as `#300=0`, `#(950+#300)+=4`,
    // `#5++`. Returns false if `stmt` is not an assignment.
    // `at`: what `@` means (FE script event handlers: the element index); by default the
    // variable's current value.
    bool assign(std::string_view stmt, std::optional<int32_t> at = std::nullopt) const;

    // Parses the variable reference at the start of `text` (e.g. `#(70+0)`)
    // and returns its index. Returns nullopt if `text` is not a variable.
    std::optional<int32_t> variableIndex(std::string_view text) const;

    char sigil() const { return sigil_; }

    struct Context {
        VarStore* vars;
        char sigil;
        VarStore* locals;
        char localSigil;
        const IdentResolver* idents;
    };

private:
    Context context() const { return {&vars_, sigil_, locals_, localSigil_, idents_ ? &idents_ : nullptr}; }

    VarStore& vars_;
    char sigil_;
    VarStore* locals_ = nullptr;
    char localSigil_ = '\\';
    IdentResolver idents_;
    bool plusRootZero_ = false;
};

// The part of an expression line the engine runs. Its calculator (cs2.exe: tokens 0x878180 /
// 0x877a20, unary + / - and postfix ++ / -- 0x729460, each operator's operand count 0x729330, the
// operator-precedence pass 0x728f90 over the table at 0x8f31a0; YukikoiMelt.exe the same pass
// 0x686210, the same table 0x90f6a8) joins tokens into trees, filling a missing operand with
// nothing, and runs the last tree it made (the top of its operand stack). A well-formed line is one
// tree, the whole line. Empty when no tree is left (or the pass would fail).
std::string_view lastExpressionTree(std::string_view text);

// The calculator itself, as the engine runs a scene line it hands to native 213 (amakanoPE.exe
// 0x5f2260: tokens 0x5f25f0, the pass 0x5f2730, the tree's evaluation 0x5f3210; REBELLIONS.exe
// 0x501ea0 / 0x501bf0 / 0x502fa0 the same): the value of the last tree the pass makes, with its
// assignments done. A name is 0 (0x5f4390: no name table), `@` is `at`, `#n` / `$n` the game's
// variables, `\n` / `$$n` the calculator's own (`priv`, 512 of them). An operator missing an operand,
// and anything the evaluation has no case for (a `(` left open, `,`, `?`), is 0 with nothing under
// it run; `&&` / `||` run both sides; division by 0 gives 0.
int32_t engineCalc(std::string_view text, VarStore& vars, std::array<int32_t, 512>& priv, int32_t at = 0);

}  // namespace nyan::cs2
