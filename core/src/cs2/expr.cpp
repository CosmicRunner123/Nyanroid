#include "nyan/cs2/expr.hpp"

#include <algorithm>
#include <string>
#include <cstdlib>
#include <functional>
#include <cctype>
#include <optional>
#include <vector>

#include "nyan/common/error.hpp"

namespace nyan::cs2 {
namespace {

struct VarRef {
    VarStore* store;
    int32_t index;
};

class Parser {
public:
    Parser(std::string_view text, const ExprEvaluator::Context& ctx, int32_t current)
        : s_(text), ctx_(ctx), current_(current) {}

    int32_t parseAll() {
        int32_t v = logicalOr();
        skipWs();
        if (pos_ != s_.size()) error("unexpected '" + std::string(1, s_[pos_]) + "'");
        return v;
    }
    // One unary term (a unary operator and its operand), leaving the rest.
    int32_t parseUnary() { return unary(); }

    // Parses a variable reference (`#N`, `#(expr)`, or a local `\\N`) at the current position.
    std::optional<VarRef> variable() {
        skipWs();
        if (pos_ >= s_.size()) return std::nullopt;
        VarStore* store = nullptr;
        if (s_[pos_] == ctx_.sigil) store = ctx_.vars;
        else if (ctx_.locals && s_[pos_] == ctx_.localSigil) store = ctx_.locals;
        if (!store) return std::nullopt;
        // `$str50` is a string variable, not an integer one.
        if (store == ctx_.vars && s_.substr(pos_ + 1, 3) == "str") return std::nullopt;
        ++pos_;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '(') {
            ++pos_;
            int32_t idx = logicalOr();
            expect(')');
            return VarRef{store, idx};
        }
        // `$NAME`: a macro naming the variable number (FE scripts).
        if (ctx_.idents && pos_ < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) {
            size_t start = pos_;
            while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
            if (auto v = (*ctx_.idents)(s_.substr(start, pos_ - start))) return VarRef{store, *v};
            error("unknown variable name '" + std::string(s_.substr(start, pos_ - start)) + "'");
        }
        if (pos_ >= s_.size() || !(std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-')) {
            error("expected variable number");
        }
        return VarRef{store, number()};
    }

    size_t pos() const { return pos_; }
    std::string_view rest() const { return s_.substr(pos_); }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
    }

private:
    [[noreturn]] void error(const std::string& what) const {
        fail("Expression error in '" + std::string(s_) + "': " + what);
    }

    bool accept(std::string_view op) {
        skipWs();
        if (s_.substr(pos_, op.size()) == op) {
            pos_ += op.size();
            return true;
        }
        return false;
    }
    // Accepts `op` only if it is not the prefix of a longer operator (e.g. `<` vs `<<`).
    bool acceptExact(std::string_view op, std::string_view notFollowedBy) {
        skipWs();
        if (s_.substr(pos_, op.size()) != op) return false;
        size_t next = pos_ + op.size();
        if (next < s_.size() && notFollowedBy.find(s_[next]) != std::string_view::npos) return false;
        pos_ = next;
        return true;
    }
    void expect(char c) {
        skipWs();
        if (pos_ >= s_.size() || s_[pos_] != c) error(std::string("expected '") + c + "'");
        ++pos_;
    }

    int32_t number() {
        skipWs();
        bool neg = false;
        if (pos_ < s_.size() && s_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        int64_t v = 0;
        if (s_.substr(pos_, 2) == "0x" || s_.substr(pos_, 2) == "0X") {
            pos_ += 2;
            size_t start = pos_;
            while (pos_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[pos_]))) {
                v = v * 16 + (std::isdigit(static_cast<unsigned char>(s_[pos_])) ? s_[pos_] - '0'
                                                                                  : (std::tolower(s_[pos_]) - 'a' + 10));
                v &= 0xFFFFFFFF;
                ++pos_;
            }
            if (start == pos_) error("bad hex number");
        } else {
            size_t start = pos_;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                v = v * 10 + (s_[pos_] - '0');
                v &= 0xFFFFFFFF;
                ++pos_;
            }
            if (start == pos_) error("expected number");
        }
        auto r = static_cast<int32_t>(static_cast<uint32_t>(v));
        return neg ? -r : r;
    }

    int32_t primary() {
        skipWs();
        if (pos_ >= s_.size()) error("unexpected end");
        char c = s_[pos_];
        if (c == '(') {
            ++pos_;
            int32_t v = logicalOr();
            expect(')');
            return v;
        }
        if (auto v = variable()) return v->store->get(v->index);
        if (ctx_.idents && (std::isalpha(static_cast<unsigned char>(c)) || c == '_')) {
            size_t start = pos_;
            while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
            // Allow array element names such as `btn[3]`.
            if (pos_ < s_.size() && s_[pos_] == '[') {
                size_t close = s_.find(']', pos_);
                if (close != std::string_view::npos) pos_ = close + 1;
            }
            if (auto v = (*ctx_.idents)(s_.substr(start, pos_ - start))) return *v;
            error("unknown identifier '" + std::string(s_.substr(start, pos_ - start)) + "'");
        }
        if (c == '@') {
            ++pos_;
            return current_;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) return number();
        error("unexpected '" + std::string(1, c) + "'");
    }

    int32_t unary() {
        if (accept("-")) return -unary();
        if (accept("+")) return unary();
        if (acceptExact("!", "=")) return !unary();
        if (accept("~")) return ~unary();
        return primary();
    }
    int32_t mul() {
        int32_t v = unary();
        for (;;) {
            if (acceptExact("*", "=")) v *= unary();
            else if (acceptExact("/", "=")) {
                int32_t d = unary();
                v = d ? v / d : 0;
            } else if (acceptExact("%", "=")) {
                int32_t d = unary();
                v = d ? v % d : 0;
            } else return v;
        }
    }
    int32_t add() {
        int32_t v = mul();
        for (;;) {
            if (acceptExact("+", "+=")) v = static_cast<int32_t>(static_cast<uint32_t>(v) + static_cast<uint32_t>(mul()));
            else if (acceptExact("-", "-=")) v = static_cast<int32_t>(static_cast<uint32_t>(v) - static_cast<uint32_t>(mul()));
            else return v;
        }
    }
    int32_t shift() {
        int32_t v = add();
        for (;;) {
            if (acceptExact("<<", "=")) v = static_cast<int32_t>(static_cast<uint32_t>(v) << (add() & 31));
            else if (acceptExact(">>", "=")) v >>= (add() & 31);
            else return v;
        }
    }
    int32_t relational() {
        int32_t v = shift();
        for (;;) {
            if (accept("<=")) v = v <= shift();
            else if (accept(">=")) v = v >= shift();
            else if (acceptExact("<", "<")) v = v < shift();
            else if (acceptExact(">", ">")) v = v > shift();
            else return v;
        }
    }
    int32_t equality() {
        int32_t v = relational();
        for (;;) {
            if (accept("==")) v = v == relational();
            else if (accept("!=")) v = v != relational();
            else return v;
        }
    }
    int32_t bitAnd() {
        int32_t v = equality();
        while (acceptExact("&", "&=")) v &= equality();
        return v;
    }
    int32_t bitXor() {
        int32_t v = bitAnd();
        while (acceptExact("^", "=")) v ^= bitAnd();
        return v;
    }
    int32_t bitOr() {
        int32_t v = bitXor();
        while (acceptExact("|", "|=")) v |= bitXor();
        return v;
    }
    int32_t logicalAnd() {
        int32_t v = bitOr();
        while (accept("&&")) {
            int32_t r = bitOr();
            v = v && r;
        }
        return v;
    }
    int32_t logicalOr() {
        int32_t v = logicalAnd();
        while (accept("||")) {
            int32_t r = logicalAnd();
            v = v || r;
        }
        return v;
    }

    std::string_view s_;
    const ExprEvaluator::Context& ctx_;
    int32_t current_;
    size_t pos_ = 0;
};

}  // namespace

namespace {

// Whether the engine's tree for `expr` has a unary plus at its root. Brackets make no node of their
// own (0x5f2730), so `(+5)` counts; the plus's operand is one unary term (a unary operator binds
// tighter than any binary one, table 0x700da0), so `+5*2` does not. `++` and `+=` are other tokens
// (0x6abc70: 0x89, 0x9c).
bool rootIsUnaryPlus(std::string_view t, const ExprEvaluator::Context& ctx, int32_t current) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    };
    t = trim(t);
    for (;;) {
        if (t.size() < 2 || t.front() != '(' || t.back() != ')') break;
        int depth = 0;
        size_t close = std::string_view::npos;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '(') ++depth;
            else if (t[i] == ')' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close != t.size() - 1) break;  // the first bracket closes before the end
        t = trim(t.substr(1, t.size() - 2));
    }
    if (t.size() < 2 || t[0] != '+' || t[1] == '+' || t[1] == '=') return false;
    try {
        Parser p(t.substr(1), ctx, current);
        p.parseUnary();
        p.skipWs();
        return p.rest().empty();
    } catch (const Error&) {
        return false;
    }
}

}  // namespace

int32_t ExprEvaluator::eval(std::string_view expr, int32_t current) const {
    Context ctx = context();
    int32_t v = Parser(expr, ctx, current).parseAll();
    if (plusRootZero_ && rootIsUnaryPlus(expr, ctx, current)) return 0;
    return v;
}

std::optional<int32_t> ExprEvaluator::variableIndex(std::string_view text) const {
    Context ctx = context();
    Parser p(text, ctx, 0);
    auto ref = p.variable();
    p.skipWs();
    if (!ref || !p.rest().empty()) return std::nullopt;
    return ref->index;
}

bool ExprEvaluator::assign(std::string_view stmt, std::optional<int32_t> at) const {
    Context ctx = context();
    Parser p(stmt, ctx, at.value_or(0));
    auto ref = p.variable();
    if (!ref) return false;
    VarStore& store = *ref->store;
    const int32_t* idx = &ref->index;
    p.skipWs();
    std::string_view rest = p.rest();

    static constexpr std::string_view kOps[] = {"<<=", ">>=", "+=", "-=", "*=", "/=", "%=",
                                                 "&=",  "|=",  "^=", "++", "--", "="};
    for (std::string_view op : kOps) {
        if (!rest.starts_with(op)) continue;
        if (op == "=" && rest.starts_with("==")) return false;
        std::string_view rhs = rest.substr(op.size());
        int32_t cur = store.get(*idx);
        int32_t v = 0;
        if (op == "=" && assign(rhs, at)) {
            // Chained assignment `a = b = 0`: the inner one runs first; its target's new value flows out.
            Parser q(rhs, ctx, 0);
            auto inner = q.variable();
            v = inner ? inner->store->get(inner->index) : 0;
        } else if (op != "++" && op != "--") {
            v = eval(rhs, at.value_or(cur));
        }
        auto u = [](int32_t x) { return static_cast<uint32_t>(x); };
        int32_t out;
        if (op == "=") out = v;
        else if (op == "+=") out = static_cast<int32_t>(u(cur) + u(v));
        else if (op == "-=") out = static_cast<int32_t>(u(cur) - u(v));
        else if (op == "*=") out = cur * v;
        else if (op == "/=") out = v ? cur / v : 0;
        else if (op == "%=") out = v ? cur % v : 0;
        else if (op == "&=") out = cur & v;
        else if (op == "|=") out = cur | v;
        else if (op == "^=") out = cur ^ v;
        else if (op == "<<=") out = static_cast<int32_t>(u(cur) << (v & 31));
        else if (op == ">>=") out = cur >> (v & 31);
        else if (op == "++") out = cur + 1;
        else out = cur - 1;
        store.set(*idx, out);
        return true;
    }
    return false;
}

}  // namespace nyan::cs2

namespace nyan::cs2 {
namespace {

// cs2.exe 0x8f31a0 (YukikoiMelt.exe 0x90f6a8): an operator's precedence on the stack and coming in.
struct Precedence {
    int32_t type, onStack, incoming;
};
constexpr Precedence kPrecedence[] = {
    {0x00, 0, 0},    {0x01, 30, 31},  {0x02, 30, 31},  {0x04, 30, 31},  {0x03, 30, 31},  {0x80, 28, 29},
    {0x81, 19, 18},  {0x82, 27, 26},  {0x83, 17, 16},  {0x84, 11, 10},  {0x85, 0, 33},   {0x86, 32, 0},
    {0x87, 27, 26},  {0x88, 25, 24},  {0x89, 29, 28},  {0x8a, 28, 29},  {0x8c, 25, 24},  {0x8d, 29, 28},
    {0x8e, 28, 29},  {0x92, 27, 26},  {0x94, 1, 0},    {0x95, 21, 20},  {0x96, 23, 22},  {0x97, 21, 20},
    {0x98, 4, 5},    {0x99, 4, 5},    {0x9a, 4, 5},    {0x9b, 4, 5},    {0x9c, 4, 5},    {0x9d, 4, 5},
    {0x9e, 4, 5},    {0x9f, 4, 5},    {0xa0, 19, 18},  {0xa1, 4, 5},    {0xa2, 4, 5},    {0xa3, 4, 5},
    {0xa4, 21, 20},  {0xa5, 21, 20},  {0xa6, 23, 22},  {0xa8, 0, 33},   {0xa9, 32, 0},   {0xaa, 15, 14},
    {0xac, 13, 12},  {0xad, 9, 8},    {0xaf, 0, 29},   {0xb0, 28, 29},  {0xb1, 28, 29},  {0xbe, -1, 0},
    {0x12, 0, 1},
};

int32_t precedence(int32_t type, bool incoming) {
    for (const auto& p : kPrecedence) {
        if (p.type == type) return incoming ? p.incoming : p.onStack;
    }
    return 0;
}

// cs2.exe 0x877a20: the operators, longest first.
struct OperatorText {
    const char* text;
    int32_t type;
};
constexpr OperatorText kOperators[] = {
    {"<<=", 0x9f}, {">>=", 0xa1}, {"...", 0x91}, {"!=", 0x81}, {"$$", 0x03}, {"%=", 0x99}, {"&&", 0x84},
    {"&=", 0x9a},  {"*=", 0x9b},  {"++", 0x89},  {"+=", 0x9c}, {"--", 0x8d}, {"->", 0x8f}, {"-=", 0x9d},
    {"/=", 0x9e},  {"<<", 0x96},  {"<=", 0x97},  {"==", 0xa0}, {">=", 0xa5}, {">>", 0xa6}, {"^=", 0xa2},
    {"|=", 0xa3},  {"||", 0xad},  {"!", 0x80},   {"#", 0x01},  {"$", 0x02},  {"%", 0x82},  {"&", 0x83},
    {"(", 0x85},   {")", 0x86},   {"*", 0x87},   {"+", 0x88},  {",", 0x8b},  {"-", 0x8c},  {".", 0x90},
    {"/", 0x92},   {":", 0x93},   {";", 0x94},   {"<", 0x95},  {"=", 0x98},  {">", 0xa4},  {"?", 0xa7},
    {"[", 0xa8},   {"\\", 0x04},  {"]", 0xa9},   {"^", 0xaa},  {"{", 0xab},  {"|", 0xac},  {"}", 0xae},
    {"~", 0xaf},
};

// The calculator's tokens and their unary / postfix types (shared by lastExpressionTree and
// engineCalc).
struct CalcToken {
    int32_t kind, type;
    size_t begin, end;
};
std::vector<CalcToken> calcTokens(std::string_view s) {
    // Tokens (0x878180): kind 4 a name, 5 a number or string, 6 an operator; blanks and line ends
    // are dropped (0x728e50), anything else is kind 0, which the pass ignores.
    std::vector<CalcToken> tokens;
    auto isName = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            if (c == '0' && j < s.size() && (s[j] == 'x' || s[j] == 'X')) ++j;
            while (j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
            tokens.push_back({5, 0x0d, i, j});
            i = j;
        } else if (c == '"') {
            size_t j = s.find('"', i + 1);
            j = j == std::string_view::npos ? s.size() : j + 1;
            tokens.push_back({5, 0x0f, i, j});
            i = j;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '@') {
            size_t j = i + 1;
            if (c != '@') {
                while (j < s.size() && isName(s[j])) ++j;
            }
            tokens.push_back({4, 0x07, i, j});
            i = j;
        } else {
            const OperatorText* op = nullptr;
            for (const auto& o : kOperators) {
                if (s.substr(i).starts_with(o.text)) {
                    op = &o;
                    break;
                }
            }
            const size_t len = op ? std::string_view(op->text).size() : 1;
            tokens.push_back({op ? 6 : 0, op ? op->type : 0, i, i + len});
            i += len;
        }
    }
    // 0x729460: a + or - with nothing before it, or after an operator other than `)` / `]`, is
    // unary; a ++ or -- after a name or a `)` / `]` is postfix.
    int32_t prevKind = 0, prevType = 0;
    for (auto& t : tokens) {
        const bool closes = prevType == 0x86 || prevType == 0xa9;
        if ((t.type == 0x88 || t.type == 0x8c) && t.kind == 6 && (prevKind == 0 || prevKind == 6 || prevKind == 7) && !closes) {
            t.type = t.type == 0x88 ? 0xb0 : 0xb1;
        } else if ((t.type == 0x89 || t.type == 0x8d) && t.kind == 6 && (prevKind == 4 || closes)) {
            t.type = t.type == 0x89 ? 0x8a : 0x8e;
        }
        prevKind = t.kind, prevType = t.type;
    }
    return tokens;
}

// 0x729330: operands an operator takes (1 the one before it, 2 the one after, 3 both).
int32_t calcOperandCount(int32_t type) {
    switch (type) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x80: case 0x89: case 0x8d: case 0xaf: case 0xb0: case 0xb1:
        return 2;
    case 0x8a: case 0x8e:
        return 1;
    case 0xab: case 0xae:
        return 0;
    default:
        return 3;
    }
}

}  // namespace

std::string_view lastExpressionTree(std::string_view s) {
    using Token = CalcToken;
    const std::vector<Token> tokens = calcTokens(s);
    auto operandCount = calcOperandCount;
    // 0x728f90: trees as the text spans they cover. A missing operand is nothing.
    struct Span {
        size_t lo, hi;
    };
    struct Op {
        int32_t type;
        size_t begin, end, operandsBelow;
    };
    std::vector<std::optional<Span>> operands;
    std::vector<Op> ops{{0xbe, 0, 0, 0}};  // the start mark
    auto widen = [](Span& sp, const std::optional<Span>& with) {
        if (with) sp.lo = std::min(sp.lo, with->lo), sp.hi = std::max(sp.hi, with->hi);
    };
    auto pop = [&]() -> std::optional<Span> {
        if (operands.empty()) return std::nullopt;
        auto v = operands.back();
        operands.pop_back();
        return v;
    };
    for (size_t k = 0; k <= tokens.size(); ++k) {
        const Token t = k < tokens.size() ? tokens[k] : Token{6, 0xbe, s.size(), s.size()};  // the end mark
        if (t.kind == 4 || t.kind == 5) {
            operands.push_back(Span{t.begin, t.end});
        } else if (t.kind == 6 && ops.back().type != 0) {
            while (t.type != 0) {
                if (precedence(ops.back().type, false) < precedence(t.type, true)) {
                    ops.push_back({t.type, t.begin, t.end, operands.size()});
                    break;
                }
                if ((ops.back().type == 0x85 && t.type == 0x86) || (ops.back().type == 0xa8 && t.type == 0xa9)) {
                    // A bracket pair: its contents (or, for `[ ]`, the operand before it) take
                    // the brackets into their text.
                    const Op open = ops.back();
                    ops.pop_back();
                    if (open.type == 0xa8) {
                        auto inner = pop();
                        if (!operands.empty() && operands.back()) widen(*operands.back(), inner), widen(*operands.back(), Span{t.begin, t.end});
                    } else if (operands.size() > open.operandsBelow && operands.back()) {
                        widen(*operands.back(), Span{open.begin, t.end});
                    }
                    break;
                }
                if (ops.size() <= 1) return {};  // the pass would take the start mark: it fails
                const Op top = ops.back();
                ops.pop_back();
                Span node{top.begin, top.end};
                const int32_t n = operandCount(top.type);
                if (n == 1 || n == 2) widen(node, pop());
                else if (n == 3) widen(node, pop()), widen(node, pop());
                operands.push_back(node);
                if (ops.back().type == 0) break;
            }
        }
        if (ops.size() == 2 && ops[0].type == 0xbe && ops[1].type == 0xbe) break;
    }
    if (operands.empty() || !operands.back()) return {};
    return s.substr(operands.back()->lo, operands.back()->hi - operands.back()->lo);
}


int32_t engineCalc(std::string_view s, VarStore& vars, std::array<int32_t, 512>& priv, int32_t at) {
    const std::vector<CalcToken> tokens = calcTokens(s);
    // 0x5f2730: the same pass as lastExpressionTree's, keeping the nodes (+0x50 left, +0x54 right).
    struct Node {
        int32_t kind, type;
        size_t begin, end;
        int left = -1, right = -1;
    };
    std::vector<Node> nodes;
    nodes.reserve(tokens.size() + 2);
    std::vector<int> operands;  // node indices; -1 a missing operand
    struct Op {
        int32_t type;
        int node;
    };
    std::vector<Op> ops{{0xbe, -1}};
    auto pop = [&]() -> int {
        if (operands.empty()) return -1;
        const int v = operands.back();
        operands.pop_back();
        return v;
    };
    for (size_t k = 0; k <= tokens.size(); ++k) {
        const CalcToken t = k < tokens.size() ? tokens[k] : CalcToken{6, 0xbe, s.size(), s.size()};
        if (t.kind == 4 || t.kind == 5) {
            nodes.push_back({t.kind, t.type, t.begin, t.end});
            operands.push_back(static_cast<int>(nodes.size()) - 1);
        } else if (t.kind == 6 && ops.back().type != 0) {
            while (t.type != 0) {
                if (precedence(ops.back().type, false) < precedence(t.type, true)) {
                    nodes.push_back({6, t.type, t.begin, t.end});
                    ops.push_back({t.type, static_cast<int>(nodes.size()) - 1});
                    break;
                }
                if (ops.back().type == 0x85 && t.type == 0x86) {
                    ops.pop_back();
                    break;
                }
                if (ops.back().type == 0xa8 && t.type == 0xa9) {
                    // `a[b]`: b becomes the right operand of the one before.
                    ops.pop_back();
                    const int inner = pop();
                    if (operands.empty()) operands.push_back(-1);
                    else if (operands.back() >= 0) nodes[static_cast<size_t>(operands.back())].right = inner;
                    break;
                }
                if (ops.size() <= 1) return 0;  // the pass would take the start mark
                const Op top = ops.back();
                ops.pop_back();
                Node& n = nodes[static_cast<size_t>(top.node)];
                const int32_t count = calcOperandCount(top.type);
                if (count == 1) n.left = pop();
                else if (count == 2) n.right = pop();
                else if (count == 3) n.right = pop(), n.left = pop();
                operands.push_back(top.node);
                if (ops.back().type == 0) break;
            }
        }
        if (ops.size() == 2 && ops[0].type == 0xbe && ops[1].type == 0xbe) break;
    }
    if (operands.empty() || operands.back() < 0) return 0;

    // 0x5f3210: a result is a plain value, or a game variable (#, $) or a private one (\, $$) by index.
    struct Result {
        int kind = 0;  // 0 value, 1 game variable, 2 private
        int32_t v = 0;
    };
    auto u = [](int32_t x) { return static_cast<uint32_t>(x); };
    auto get = [&](const Result& r) -> int32_t {
        if (r.kind == 1) return r.v >= 0 ? vars.get(r.v) : 0;
        if (r.kind == 2) return r.v >= 0 && r.v < 512 ? priv[static_cast<size_t>(r.v)] : 0;
        return r.v;
    };
    auto put = [&](const Result& r, int32_t value) -> bool {
        if (r.kind == 1 && r.v >= 0) vars.set(r.v, value);
        else if (r.kind == 2 && r.v >= 0 && r.v < 512) priv[static_cast<size_t>(r.v)] = value;
        else return false;
        return true;
    };
    std::function<Result(int)> eval = [&](int i) -> Result {
        if (i < 0) return {};
        const Node n = nodes[static_cast<size_t>(i)];
        if (n.kind == 5) {
            if (n.type != 0x0d) return {};  // a string
            const std::string text(s.substr(n.begin, n.end - n.begin));
            const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
            return {0, static_cast<int32_t>(std::strtoul(hex ? text.c_str() + 2 : text.c_str(), nullptr, hex ? 16 : 10))};
        }
        if (n.kind == 4) {
            if (n.end - n.begin == 1 && s[n.begin] == '@') return {0, at};  // 0x5f25f0: token 0x27
            if (n.right >= 0) eval(n.right);
            return {};  // 0x5f4390: a name without a name table
        }
        const int32_t t = n.type;
        switch (t) {
        case 0x01: case 0x02:
            if (n.right < 0) return {};
            return {1, get(eval(n.right))};
        case 0x03: case 0x04:
            if (n.right < 0) return {};
            return {2, get(eval(n.right))};
        case 0x80:
            if (n.right < 0) return {};
            return {0, get(eval(n.right)) == 0 ? 1 : 0};
        case 0xaf:
            if (n.right < 0) return {};
            return {0, ~get(eval(n.right))};
        case 0xb0:
            if (n.right < 0) return {};
            return {0, get(eval(n.right))};
        case 0xb1:
            if (n.right < 0) return {};
            return {0, static_cast<int32_t>(0u - u(get(eval(n.right))))};
        case 0x89: case 0x8d: case 0x8a: case 0x8e: {
            // ++x / --x give the new value, x++ / x-- the old one.
            const bool prefix = t == 0x89 || t == 0x8d;
            const int child = prefix ? n.right : n.left;
            if (child < 0) return {};
            const Result r = eval(child);
            const int32_t old = get(r);
            const int32_t now = static_cast<int32_t>(t == 0x89 || t == 0x8a ? u(old) + 1 : u(old) - 1);
            if (!put(r, now)) return {};
            return {0, prefix ? now : old};
        }
        default:
            break;
        }
        const bool binary = (t >= 0x81 && t <= 0x84) || t == 0x87 || t == 0x88 || t == 0x8c || t == 0x92 ||
                            (t >= 0x95 && t <= 0xa6) || t == 0xaa || t == 0xac || t == 0xad;
        if (!binary || n.left < 0 || n.right < 0) return {};
        const Result l = eval(n.left);
        const Result r = eval(n.right);
        const int32_t a = get(l), b = get(r);
        const int32_t sh = b & 31;
        auto store = [&](int32_t value) -> Result {
            if (!put(l, value)) return {};
            return {0, get(l)};
        };
        switch (t) {
        case 0x81: return {0, a != b};
        case 0x82: return {0, b ? a % b : 0};
        case 0x83: return {0, a & b};
        case 0x84: return {0, a && b};
        case 0x87: return {0, static_cast<int32_t>(u(a) * u(b))};
        case 0x88: return {0, static_cast<int32_t>(u(a) + u(b))};
        case 0x8c: return {0, static_cast<int32_t>(u(a) - u(b))};
        case 0x92: return {0, b ? a / b : 0};
        case 0x95: return {0, a < b};
        case 0x96: return {0, static_cast<int32_t>(u(a) << sh)};
        case 0x97: return {0, a <= b};
        case 0x98: return store(b);
        case 0x99: return b ? store(a % b) : Result{};
        case 0x9a: return store(a & b);
        case 0x9b: return store(static_cast<int32_t>(u(a) * u(b)));
        case 0x9c: return store(static_cast<int32_t>(u(a) + u(b)));
        case 0x9d: return store(static_cast<int32_t>(u(a) - u(b)));
        case 0x9e: return b ? store(a / b) : Result{};
        case 0x9f: return store(static_cast<int32_t>(u(a) << sh));
        case 0xa0: return {0, a == b};
        case 0xa1: return store(a >> sh);
        case 0xa2: return store(a ^ b);
        case 0xa3: return store(a | b);
        case 0xa4: return {0, a > b};
        case 0xa5: return {0, a >= b};
        case 0xa6: return {0, a >> sh};
        case 0xaa: return {0, a ^ b};
        case 0xac: return {0, a | b};
        case 0xad: return {0, a || b};
        default: return {};
        }
    };
    return get(eval(operands.back()));
}

}  // namespace nyan::cs2
