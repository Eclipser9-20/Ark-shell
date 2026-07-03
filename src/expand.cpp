#include "expand.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fnmatch.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>

static CaptureHook g_captureHook;
void setCaptureHook(CaptureHook hook) { g_captureHook = std::move(hook); }

static bool isNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static std::string lookupVar(const std::string& name, const ShellState& state) {
    auto it = state.vars.find(name);
    return it != state.vars.end() ? it->second : "";
}

// --- Arithmetic expansion: $(( expr )) --------------------------------------
// A small recursive-descent integer evaluator with C operator precedence.
// Bare identifiers resolve to their variable's value parsed as an integer
// (0 if unset or non-numeric, matching bash). Read-only: no in-expression
// assignment (x+=1) -- the common idiom `i=$((i+1))` does its assignment on
// the OUTER `i=` and works fine with a read-only evaluator. Malformed input
// evaluates to 0 rather than throwing, so a bad expression degrades to "0"
// instead of crashing the shell.
namespace {
struct ArithEval {
    const std::string& s;
    size_t pos = 0;
    const ShellState& state;
    bool ok = true;

    ArithEval(const std::string& str, const ShellState& st) : s(str), state(st) {}

    void skipSpace() { while (pos < s.size() && std::isspace((unsigned char)s[pos])) pos++; }
    bool eof() { skipSpace(); return pos >= s.size(); }
    char peek() { skipSpace(); return pos < s.size() ? s[pos] : '\0'; }

    // Consume a specific multi-char operator if it's next; returns true if so.
    bool eat(const char* op) {
        skipSpace();
        size_t n = std::strlen(op);
        if (s.compare(pos, n, op) == 0) { pos += n; return true; }
        return false;
    }

    long parsePrimary() {
        skipSpace();
        if (pos >= s.size()) { ok = false; return 0; }
        char c = s[pos];
        if (c == '(') {
            pos++;
            long v = parseExpr();
            skipSpace();
            if (pos < s.size() && s[pos] == ')') pos++; else ok = false;
            return v;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t j = pos;
            int base = 10;
            if (s[j] == '0' && j + 1 < s.size() && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
                base = 16; j += 2; while (j < s.size() && std::isxdigit((unsigned char)s[j])) j++;
            } else if (s[j] == '0' && j + 1 < s.size() && s[j + 1] >= '0' && s[j + 1] <= '7') {
                base = 8; while (j < s.size() && s[j] >= '0' && s[j] <= '7') j++;
            } else {
                while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            }
            long v = std::strtol(s.substr(pos, j - pos).c_str(), nullptr, base);
            pos = j;
            return v;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = pos;
            while (j < s.size() && isNameChar(s[j])) j++;
            std::string name = s.substr(pos, j - pos);
            pos = j;
            std::string val = lookupVar(name, state);
            return val.empty() ? 0 : std::strtol(val.c_str(), nullptr, 10);
        }
        ok = false;
        return 0;
    }

    // Exponentiation `a ** b`: binds tighter than * / % but looser than unary
    // minus (so -2**2 == -(2**2) == -4, matching bash/python). Right-associative:
    // 2**3**2 == 2**(3**2) == 512, via parsing the exponent through parseUnary.
    long parsePower() {
        long base = parsePrimary();
        skipSpace();
        if (pos + 1 < s.size() && s[pos] == '*' && s[pos + 1] == '*') {
            pos += 2;
            long e = parseUnary();
            if (e < 0) return 0;
            long r = 1;
            for (long k = 0; k < e; k++) r *= base;
            return r;
        }
        return base;
    }

    long parseUnary() {
        skipSpace();
        if (eat("!")) return !parseUnary();
        if (eat("~")) return ~parseUnary();
        if (pos < s.size() && s[pos] == '-') { pos++; return -parseUnary(); }
        if (pos < s.size() && s[pos] == '+') { pos++; return parseUnary(); }
        return parsePower();
    }

    long parseMul() {
        long v = parseUnary();
        for (;;) {
            skipSpace();
            if (eat("*")) v = v * parseUnary();
            else if (eat("/")) { long d = parseUnary(); v = d != 0 ? v / d : 0; }
            else if (eat("%")) { long d = parseUnary(); v = d != 0 ? v % d : 0; }
            else break;
        }
        return v;
    }

    long parseAdd() {
        long v = parseMul();
        for (;;) {
            skipSpace();
            if (pos < s.size() && s[pos] == '+' && !(pos + 1 < s.size() && s[pos + 1] == '+')) { pos++; v = v + parseMul(); }
            else if (pos < s.size() && s[pos] == '-' && !(pos + 1 < s.size() && s[pos + 1] == '-')) { pos++; v = v - parseMul(); }
            else break;
        }
        return v;
    }

    long parseShift() {
        long v = parseAdd();
        for (;;) {
            // Guard the shift COUNT: a count >= 64 or < 0 is undefined behavior
            // (UBSan-flagged on `$(( 1<<200 ))`); define it as 0 like most shells.
            // Cast to unsigned for << so a negative value doesn't shift-UB either.
            if (eat("<<")) { long s = parseAdd(); v = (s >= 0 && s < 64) ? (long)((unsigned long)v << s) : 0; }
            else if (eat(">>")) { long s = parseAdd(); v = (s >= 0 && s < 64) ? (v >> s) : 0; }
            else break;
        }
        return v;
    }

    long parseRel() {
        long v = parseShift();
        for (;;) {
            // Two-char forms first so "<=" isn't mis-read as "<" then "=".
            if (eat("<=")) v = (v <= parseShift());
            else if (eat(">=")) v = (v >= parseShift());
            else if (eat("<")) v = (v < parseShift());
            else if (eat(">")) v = (v > parseShift());
            else break;
        }
        return v;
    }

    long parseEq() {
        long v = parseRel();
        for (;;) {
            if (eat("==")) v = (v == parseRel());
            else if (eat("!=")) v = (v != parseRel());
            else break;
        }
        return v;
    }

    long parseBAnd() { long v = parseEq(); while (peek() == '&' && !(pos + 1 < s.size() && s[pos + 1] == '&')) { pos++; v = v & parseEq(); } return v; }
    long parseBXor() { long v = parseBAnd(); while (peek() == '^') { pos++; v = v ^ parseBAnd(); } return v; }
    long parseBOr()  { long v = parseBXor(); while (peek() == '|' && !(pos + 1 < s.size() && s[pos + 1] == '|')) { pos++; v = v | parseBXor(); } return v; }
    long parseLAnd() { long v = parseBOr(); while (eat("&&")) { long r = parseBOr(); v = (v && r); } return v; }
    long parseLOr()  { long v = parseLAnd(); while (eat("||")) { long r = parseLAnd(); v = (v || r); } return v; }

    // Ternary `cond ? a : b` -- lowest precedence bar comma, right-associative
    // so `a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`.
    long parseTernary() {
        long c = parseLOr();
        skipSpace();
        if (pos < s.size() && s[pos] == '?') {
            pos++;
            long a = parseTernary();
            skipSpace();
            if (pos < s.size() && s[pos] == ':') pos++; else ok = false;
            long b = parseTernary();
            return c ? a : b;
        }
        return c;
    }

    // Comma operator: lowest precedence, left-to-right, yields the last value.
    long parseComma() {
        long v = parseTernary();
        while (peek() == ',') { pos++; v = parseTernary(); }
        return v;
    }

    long parseExpr() { return parseComma(); }
};
} // namespace

static std::string evalArithmetic(const std::string& expr, const ShellState& state) {
    ArithEval e(expr, state);
    long v = e.parseExpr();
    return std::to_string(v);
}

// Expands a single ${...} or $NAME form starting at src[i] == '$'.
// Expands the INNER text of a ${...} form (everything between the braces).
// Supports the common bash parameter-expansion operators. Read-only (the
// assigning form ${var:=word} behaves like ${var:-word} without persisting,
// since expandOne runs on a const ShellState -- noted, low-frequency).
static std::string expandParam(const std::string& inner, const ShellState& state) {
    if (inner.empty()) return "";

    // ${#name} -- length
    if (inner[0] == '#' && inner.size() > 1) {
        return std::to_string(lookupVar(inner.substr(1), state).size());
    }

    // Operators that use a colon-prefixed form (treat unset AND empty alike)
    // vs the non-colon form (unset only). Scan for the first operator char
    // after the name. Name is the leading run of name-chars.
    size_t np = 0;
    while (np < inner.size() && isNameChar(inner[np])) np++;
    std::string name = inner.substr(0, np);
    std::string rest = inner.substr(np); // operator + operand, or ""
    std::string val = lookupVar(name, state);
    bool isSet = state.vars.find(name) != state.vars.end();

    if (rest.empty()) return val; // plain ${name}

    // ${name:offset:length} and ${name:offset} -- substring. Distinguished
    // from ${name:-word} etc. by the char after ':' being a digit or '-'
    // sign/space that forms a number (here: only when it's NOT one of the
    // :- := :+ :? operators).
    auto isModifierColon = [&](char after) {
        return after == '-' || after == '=' || after == '+' || after == '?';
    };
    if (rest[0] == ':' && rest.size() > 1 && !isModifierColon(rest[1])) {
        // substring: parse offset[:length]
        std::string body = rest.substr(1);
        size_t colon2 = body.find(':');
        bool hasLength = colon2 != std::string::npos; // distinguishes ${x:0} from ${x:0:-1}
        long offset = std::strtol(body.substr(0, colon2).c_str(), nullptr, 10);
        long length = hasLength ? std::strtol(body.substr(colon2 + 1).c_str(), nullptr, 10) : 0;
        long n = (long)val.size();
        if (offset < 0) offset = n + offset; // negative offset counts from the end
        if (offset < 0) offset = 0;
        if (offset > n) return "";
        if (!hasLength) return val.substr(offset); // no length -> to the end
        // A NEGATIVE length is an index from the END (bash): ${x:0:-1} strips the
        // last char. A positive length is a count from `offset`.
        long end = length < 0 ? n + length : offset + length;
        if (end > n) end = n;
        if (end < offset) end = offset;
        return val.substr(offset, end - offset);
    }

    // Colon-modifier forms: :- := :+ :? (colon => also trigger on empty)
    if (rest.size() >= 2 && rest[0] == ':' && isModifierColon(rest[1])) {
        char op = rest[1];
        std::string word = rest.substr(2);
        bool unsetOrEmpty = val.empty();
        if (op == '-') return unsetOrEmpty ? word : val;
        if (op == '=') return unsetOrEmpty ? word : val; // read-only: no persist
        if (op == '+') return unsetOrEmpty ? "" : word;
        if (op == '?') return val; // error form unsupported; just return value
    }
    // Non-colon modifier forms: - = + ? (trigger on UNSET only)
    if (rest[0] == '-' || rest[0] == '=' || rest[0] == '+' || rest[0] == '?') {
        char op = rest[0];
        std::string word = rest.substr(1);
        if (op == '-') return isSet ? val : word;
        if (op == '=') return isSet ? val : word;
        if (op == '+') return isSet ? word : "";
        if (op == '?') return val;
    }

    // ${name#pat} / ${name##pat} -- remove matching prefix (shortest/longest)
    if (rest[0] == '#') {
        bool longest = rest.size() > 1 && rest[1] == '#';
        std::string pat = rest.substr(longest ? 2 : 1);
        if (longest) {
            for (size_t len = val.size(); (long)len >= 0; len--)
                if (fnmatch(pat.c_str(), val.substr(0, len).c_str(), 0) == 0) return val.substr(len);
        } else {
            for (size_t len = 0; len <= val.size(); len++)
                if (fnmatch(pat.c_str(), val.substr(0, len).c_str(), 0) == 0) return val.substr(len);
        }
        return val;
    }
    // ${name%pat} / ${name%%pat} -- remove matching suffix (shortest/longest)
    if (rest[0] == '%') {
        bool longest = rest.size() > 1 && rest[1] == '%';
        std::string pat = rest.substr(longest ? 2 : 1);
        if (longest) {
            for (size_t len = val.size(); (long)len >= 0; len--)
                if (fnmatch(pat.c_str(), val.substr(val.size() - len).c_str(), 0) == 0) return val.substr(0, val.size() - len);
        } else {
            for (size_t len = 0; len <= val.size(); len++)
                if (fnmatch(pat.c_str(), val.substr(val.size() - len).c_str(), 0) == 0) return val.substr(0, val.size() - len);
        }
        return val;
    }
    // ${name/pat/repl} / ${name//pat/repl} -- literal substring replace
    // (first / all). Glob patterns in the search side are treated literally
    // here -- the overwhelmingly common use (${p//\//_}, ${f/.txt/.bak}) is
    // literal, and full glob-substring matching is a much larger feature.
    if (rest[0] == '/') {
        bool all = rest.size() > 1 && rest[1] == '/';
        std::string body = rest.substr(all ? 2 : 1);
        size_t slash = body.find('/');
        std::string pat = slash == std::string::npos ? body : body.substr(0, slash);
        std::string repl = slash == std::string::npos ? "" : body.substr(slash + 1);
        if (pat.empty()) return val;
        std::string out;
        size_t p = 0;
        for (;;) {
            size_t hit = val.find(pat, p);
            if (hit == std::string::npos) { out += val.substr(p); break; }
            out += val.substr(p, hit - p);
            out += repl;
            p = hit + pat.size();
            if (!all) { out += val.substr(p); break; }
        }
        return out;
    }

    // ${name,} ${name,,} -- lowercase (first char / all); ${name^} ${name^^} -- uppercase.
    if (rest[0] == ',' || rest[0] == '^') {
        char op = rest[0];
        bool all = rest.size() > 1 && rest[1] == op;
        std::string r = val;
        if (op == ',') {
            if (all) for (char& ch : r) ch = (char)std::tolower((unsigned char)ch);
            else if (!r.empty()) r[0] = (char)std::tolower((unsigned char)r[0]);
        } else {
            if (all) for (char& ch : r) ch = (char)std::toupper((unsigned char)ch);
            else if (!r.empty()) r[0] = (char)std::toupper((unsigned char)r[0]);
        }
        return r;
    }

    return val; // unrecognized operator: fall back to the plain value
}

// Advances i past the whole expansion and returns the substituted text.
static std::string expandOne(const std::string& src, size_t& i, const ShellState& state) {
    size_t start = i;
    i++; // skip '$'
    if (i < src.size() && src[i] == '{') {
        size_t close = src.find('}', i);
        if (close == std::string::npos) { i = src.size(); return src.substr(start); }
        std::string inner = src.substr(i + 1, close - i - 1);
        i = close + 1;
        return expandParam(inner, state);
    }
    // Arithmetic expansion $(( expr )) -- MUST be checked before the $(...)
    // command-substitution branch below, since both start with "$(". The
    // inner expression can contain balanced parens, e.g. $(( (2+3)*4 )).
    if (i + 1 < src.size() && src[i] == '(' && src[i + 1] == '(') {
        size_t j = i + 2;
        int depth = 0;
        std::string expr;
        bool closed = false;
        while (j < src.size()) {
            if (src[j] == '\'' || src[j] == '"') {
                char q = src[j]; expr += src[j]; j++;
                while (j < src.size() && src[j] != q) { if (q == '"' && src[j] == '\\' && j + 1 < src.size()) { expr += src[j]; j++; } expr += src[j]; j++; }
                if (j < src.size()) { expr += src[j]; j++; }
                continue;
            }
            if (src[j] == '(') { depth++; expr += src[j]; j++; }
            else if (src[j] == ')') {
                if (depth == 0) {
                    // This ')' plus the next one close the arithmetic.
                    if (j + 1 < src.size() && src[j + 1] == ')') { j += 2; closed = true; }
                    else j += 1; // tolerate a single ')' as the close
                    break;
                }
                depth--; expr += src[j]; j++;
            } else { expr += src[j]; j++; }
        }
        i = j;
        (void)closed;
        // Expand the expression first so nested $((...)), $(...) and $var
        // references resolve before evaluation -- e.g. $(( $((2+3)) * 2 ))
        // or $(( $count + 1 )). Bare identifiers (x, count) are left for
        // evalArithmetic to look up directly.
        return evalArithmetic(expandWord(expr, state), state);
    }
    if (i < src.size() && src[i] == '(') {
        int depth = 1;
        size_t j = i + 1;
        while (j < src.size() && depth > 0) {
            char c = src[j];
            // Skip quoted spans so a ')' inside '...' or "..." doesn't
            // prematurely close the command substitution.
            if (c == '\'' || c == '"') {
                char q = c; j++;
                while (j < src.size() && src[j] != q) { if (q == '"' && src[j] == '\\' && j + 1 < src.size()) j++; j++; }
                if (j < src.size()) j++;
                continue;
            }
            if (c == '(') depth++;
            else if (c == ')') { depth--; if (depth == 0) break; }
            j++;
        }
        std::string cmd = src.substr(i + 1, j - i - 1);
        i = j + 1;
        std::string output = g_captureHook ? g_captureHook(cmd) : "";
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return output;
    }
    // Special single-char parameters: $? $$ $# $@ $* $! $0. Checked before
    // the digit/name scans since none of these are name-chars ($0 IS a digit
    // but is special -- the shell/script name, not a positional param).
    if (i < src.size()) {
        char sp = src[i];
        if (sp == '?') { i++; return std::to_string(state.lastStatus); }
        if (sp == '$') { i++; return std::to_string((long)getpid()); }
        if (sp == '!') { i++; return ""; } // last-background-pid: not tracked yet
        if (sp == '#') {
            i++;
            return std::to_string(state.argStack.empty() ? 0 : (long)state.argStack.back().size());
        }
        if (sp == '@' || sp == '*') {
            i++;
            if (state.argStack.empty()) return "";
            std::string out;
            const auto& args = state.argStack.back();
            for (size_t k = 0; k < args.size(); k++) { if (k) out += " "; out += args[k]; }
            return out;
        }
        if (sp == '0') { i++; return "ark"; } // shell/script name
    }
    if (i < src.size() && std::isdigit((unsigned char)src[i])) {
        size_t j = i;
        while (j < src.size() && std::isdigit((unsigned char)src[j])) j++;
        int idx = std::stoi(src.substr(i, j - i));
        i = j;
        if (!state.argStack.empty() && idx >= 1 && (size_t)idx <= state.argStack.back().size()) {
            return state.argStack.back()[idx - 1];
        }
        return "";
    }
    size_t j = i;
    while (j < src.size() && isNameChar(src[j])) j++;
    std::string name = src.substr(i, j - i);
    i = j;
    return lookupVar(name, state);
}

std::string expandWord(const std::string& word, const ShellState& state) {
    std::string out;
    out.reserve(word.size());
    size_t i = 0;
    while (i < word.size()) {
        if (word[i] == '$' && i + 1 < word.size()) {
            out += expandOne(word, i, state);
        } else {
            out += word[i++];
        }
    }
    return out;
}

// Expands a leading ~ to $HOME, only for unquoted words (bash never expands
// ~ inside single or double quotes). `~user` (other users' home dirs) is
// out of scope -- YAGNI, nobody here has multiple local accounts to cd between.
static std::string expandTilde(const std::string& word) {
    if (word.empty() || word[0] != '~') return word;
    if (word.size() > 1 && word[1] != '/') return word;
    const char* home = getenv("HOME");
    if (!home) return word;
    return std::string(home) + word.substr(1);
}

// IFS-style field splitting that is QUOTE-AWARE: whitespace inside a \x01
// (double-quote) or \x02 (single-quote) sentinel span does NOT split the
// field, and the sentinel markers themselves are stripped from the output.
// This is what makes a mixed word like `name=\x02a b\x02` stay a single
// field "name=a b" instead of splitting on the interior space -- the bug
// that broke `alias ll='ls -la'` (the value arrived as one arg but got
// split), and equally `echo a"b c"d` -> should be one word "ab cd".
static std::vector<std::string> splitFields(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool started = false, inDq = false, inSq = false;
    for (char c : s) {
        if (c == '\x01' && !inSq) { inDq = !inDq; started = true; continue; }
        if (c == '\x02' && !inDq) { inSq = !inSq; started = true; continue; }
        if (!inDq && !inSq && std::isspace((unsigned char)c)) {
            if (started) { out.push_back(cur); cur.clear(); started = false; }
            continue; // collapse runs of unquoted whitespace
        }
        cur += c;
        started = true;
    }
    if (started) out.push_back(cur);
    return out;
}

// --- Brace expansion: {a,b,c} and {1..5} ------------------------------------
// A purely textual, quote-agnostic word transform that runs BEFORE variable/
// glob expansion (matching bash's ordering). `echo a{b,c}d` -> "abd acd";
// `echo {1..4}` -> "1 2 3 4"; nesting like {a,b{c,d}} works via recursion.
// Braces inside a quote sentinel span (\x01.../\x02...) are left alone, so
// `echo "{a,b}"` stays literal.

// A {..} range: numeric ({1..5}, {5..1}) or single-char ({a..e}). Returns the
// enumerated items, or empty if `body` isn't a valid range.
static std::vector<std::string> braceRange(const std::string& body) {
    size_t dots = body.find("..");
    if (dots == std::string::npos) return {};
    std::string lo = body.substr(0, dots);
    std::string afterLo = body.substr(dots + 2);
    // Optional step: {lo..hi..step}. A third ".." beyond that is malformed.
    std::string hi, stepStr;
    size_t dots2 = afterLo.find("..");
    if (dots2 == std::string::npos) {
        hi = afterLo;
    } else {
        hi = afterLo.substr(0, dots2);
        stepStr = afterLo.substr(dots2 + 2);
        if (stepStr.find("..") != std::string::npos) return {};
    }
    if (lo.empty() || hi.empty()) return {};

    auto allDigits = [](const std::string& s) {
        size_t k = (s[0] == '-') ? 1 : 0;
        if (k == s.size()) return false;
        for (; k < s.size(); k++) if (!std::isdigit((unsigned char)s[k])) return false;
        return true;
    };
    long step = 1;
    if (!stepStr.empty()) {
        if (!allDigits(stepStr)) return {};
        step = std::labs(std::strtol(stepStr.c_str(), nullptr, 10));
        if (step == 0) step = 1;
    }
    std::vector<std::string> out;
    if (allDigits(lo) && allDigits(hi)) {
        long a = std::strtol(lo.c_str(), nullptr, 10);
        long b = std::strtol(hi.c_str(), nullptr, 10);
        // Cap the element count so a huge/extreme range -- {1..99999999999} (OOM
        // hang) or {1..9223372036854775807} (v+=step overflows -> infinite loop)
        // -- is left literal instead of hanging the shell. Unsigned subtraction
        // is well-defined even across LONG_MIN..LONG_MAX; iterate by a bounded
        // count rather than comparing a possibly-overflowing v.
        constexpr unsigned long kMaxRange = 100000;
        unsigned long span = (a <= b) ? (unsigned long)b - (unsigned long)a
                                      : (unsigned long)a - (unsigned long)b;
        if (span / (unsigned long)step >= kMaxRange) return {}; // too large -> don't expand
        for (unsigned long k = 0; k * (unsigned long)step <= span; k++)
            out.push_back(std::to_string(a <= b ? a + (long)(k * step) : a - (long)(k * step)));
        return out;
    }
    if (lo.size() == 1 && hi.size() == 1 && std::isalpha((unsigned char)lo[0]) && std::isalpha((unsigned char)hi[0])) {
        int a = (unsigned char)lo[0], b = (unsigned char)hi[0];
        if (a <= b) for (int c = a; c <= b; c += step) out.push_back(std::string(1, (char)c));
        else for (int c = a; c >= b; c -= step) out.push_back(std::string(1, (char)c));
        return out;
    }
    return {};
}

// Split `body` on top-level commas (not nested inside inner braces), for the
// {a,b,c} form. Returns fewer than 2 pieces if there's no top-level comma.
static std::vector<std::string> braceSplitCommas(const std::string& body) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : body) {
        if (c == '{') { depth++; cur += c; }
        else if (c == '}') { depth--; cur += c; }
        else if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    return parts;
}

static std::vector<std::string> braceExpand(const std::string& w) {
    // Find the first VALID brace group: a '{' with a matching '}' whose body
    // has either a top-level comma or is a range. Braces inside a sentinel
    // span are skipped. A '{' that isn't a valid group is passed over so a
    // later valid group can still expand (bash: `{x}{a,b}` -> "{x}a {x}b").
    size_t n = w.size();
    for (size_t i = 0; i < n; i++) {
        if (w[i] == '\x01' || w[i] == '\x02') {
            // skip to the matching closing sentinel of the same kind
            char s = w[i];
            size_t j = i + 1;
            while (j < n && w[j] != s) j++;
            i = j;
            continue;
        }
        // Skip `$`-prefixed constructs whole: ${param}, $(cmd), $((arith)) are
        // NOT brace groups. Without this, `${v,,}` is mis-read as brace group
        // `{v,,}` and comma-split, and braces inside $(...) get wrongly expanded.
        if (w[i] == '$' && i + 1 < n && (w[i + 1] == '{' || w[i + 1] == '(')) {
            char open = w[i + 1], close = (open == '{') ? '}' : ')';
            int depth = 0; size_t j = i + 1;
            for (; j < n; j++) {
                if (w[j] == open) depth++;
                else if (w[j] == close) { depth--; if (depth == 0) { j++; break; } }
            }
            i = j - 1;
            continue;
        }
        if (w[i] != '{') continue;
        // find matching '}' at the same depth
        int depth = 1;
        size_t j = i + 1;
        for (; j < n && depth > 0; j++) {
            if (w[j] == '{') depth++;
            else if (w[j] == '}') depth--;
            if (depth == 0) break;
        }
        if (depth != 0) continue; // unbalanced -- not a group
        std::string body = w.substr(i + 1, j - (i + 1));
        std::string pre = w.substr(0, i);
        std::string post = w.substr(j + 1);

        std::vector<std::string> alts = braceSplitCommas(body);
        if (alts.size() < 2) {
            alts = braceRange(body);
            if (alts.empty()) continue; // no comma, no range -> not expandable here
        }
        std::vector<std::string> result;
        for (const auto& alt : alts) {
            for (const auto& sub : braceExpand(pre + alt + post)) {
                result.push_back(sub);
            }
        }
        return result;
    }
    return {w};
}

// Collects `base` and every subdirectory reachable beneath it (recursively)
// into `dirs`. `base` is either "" (meaning the current directory) or a path
// ending in '/'. Hidden directories (dot-prefixed) are skipped, matching
// zsh's default of `**` not descending into or matching dotfiles.
static void collectDirsRecursive(const std::string& base, std::vector<std::string>& dirs) {
    dirs.push_back(base);
    DIR* d = opendir(base.empty() ? "." : base.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;
        std::string full = base + name; // base ends with '/' (or is "")
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            collectDirsRecursive(full + "/", dirs);
        }
    }
    closedir(d);
}

// zsh-style recursive glob: `**` matches across directory levels. E.g.
// `**/*.cpp` finds every .cpp under the current tree, `src/**/*.h` under src.
// Implemented by enumerating the directory tree rooted at the fixed prefix
// before `**`, then running an ordinary glob of `<eachDir>/<suffix>` and
// unioning the results.
static std::vector<std::string> recursiveGlob(const std::string& pattern) {
    size_t stars = pattern.find("**");
    size_t slash = pattern.rfind('/', stars);
    std::string prefix = (slash == std::string::npos) ? "" : pattern.substr(0, slash + 1);
    size_t after = stars + 2;                 // past "**"
    if (after < pattern.size() && pattern[after] == '/') after++; // past the "/"
    std::string suffix = pattern.substr(after);
    if (suffix.empty()) suffix = "*";         // `**` alone -> everything

    std::vector<std::string> dirs;
    collectDirsRecursive(prefix, dirs);

    std::vector<std::string> out;
    for (const auto& dir : dirs) {
        glob_t gl;
        std::string p = dir + suffix;
        if (::glob(p.c_str(), 0, nullptr, &gl) == 0) {
            for (size_t i = 0; i < gl.gl_pathc; i++) out.push_back(gl.gl_pathv[i]);
        }
        globfree(&gl);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) out.push_back(pattern); // no match -> literal
    return out;
}

// ── Advanced Metadata Globbing (zsh-style glob qualifiers) ──────────────────
// A trailing `(...)` on a glob filters matches by metadata, so you can find
// files by TYPE, SIZE, or AGE directly in a command:
//   *(.)        regular files only        *(/)        directories only
//   *(@)        symlinks                  *(x)        executable (also * )
//   *(.L+1000)  regular files > 1000 bytes (Lk/Lm/Lg = KiB/MiB/GiB units)
//   *(.mh-2)    modified < 2 HOURS ago    *(.md+7)    modified > 7 DAYS ago
//                                         (m units: h hour, d day, w week)
// Qualifiers chain: `*.log(.mh-1)` = regular .log files touched in the last
// hour. A '-' age means "more recent than", '+' means "older than".
static bool applyOneQualifier(const std::string& path, const char*& q) {
    struct stat st;
    char c = *q++;
    switch (c) {
        case '.': return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        case '/': return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        case '@': return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
        case 'x': case '*': return access(path.c_str(), X_OK) == 0;
        case 'r': return access(path.c_str(), R_OK) == 0;
        case 'w': return access(path.c_str(), W_OK) == 0;
        case 'L': { // size: L[kmg][+-]N
            long long unit = 1;
            if (*q == 'k') { unit = 1024; q++; }
            else if (*q == 'm') { unit = 1024LL * 1024; q++; }
            else if (*q == 'g') { unit = 1024LL * 1024 * 1024; q++; }
            char sign = '+';
            if (*q == '+' || *q == '-') sign = *q++;
            long long n = 0; bool any = false;
            while (std::isdigit((unsigned char)*q)) { n = n * 10 + (*q++ - '0'); any = true; }
            if (!any || stat(path.c_str(), &st) != 0) return false;
            long long bytes = (long long)st.st_size, threshold = n * unit;
            return sign == '+' ? bytes > threshold : bytes < threshold;
        }
        case 'm': { // mtime age: m[hdw][+-]N
            long long unit = 60; // default minutes if no unit char
            if (*q == 'h') { unit = 3600; q++; }
            else if (*q == 'd') { unit = 86400; q++; }
            else if (*q == 'w') { unit = 604800; q++; }
            else if (*q == 'M') { unit = 60; q++; }
            char sign = '-';
            if (*q == '+' || *q == '-') sign = *q++;
            long long n = 0; bool any = false;
            while (std::isdigit((unsigned char)*q)) { n = n * 10 + (*q++ - '0'); any = true; }
            if (!any || stat(path.c_str(), &st) != 0) return false;
            long long age = (long long)time(nullptr) - (long long)st.st_mtime;
            long long threshold = n * unit;
            return sign == '-' ? age < threshold : age > threshold; // -=recent, +=old
        }
        default: return true; // unknown qualifier char: ignore (permissive)
    }
}
static std::vector<std::string> applyGlobQualifiers(const std::vector<std::string>& in,
                                                    const std::string& qual) {
    std::vector<std::string> out;
    for (const auto& path : in) {
        const char* q = qual.c_str();
        bool ok = true;
        while (*q && ok) ok = applyOneQualifier(path, q);
        if (ok) out.push_back(path);
    }
    return out;
}

// Expands filesystem glob characters (* ?) via POSIX glob(3), with zsh-style
// recursive `**` and trailing metadata qualifiers handled separately. A
// pattern with no glob chars (and no qualifier) is returned as-is; a pattern
// that matches nothing is left literal (bash's default without nullglob).
static std::vector<std::string> globExpand(const std::string& pattern) {
    // Peel a trailing metadata qualifier `(...)` if it's really one (its first
    // char is a known qualifier), so a literal path like `foo(1)` is untouched.
    std::string base = pattern, qual;
    if (base.size() >= 3 && base.back() == ')') {
        size_t open = base.rfind('(');
        if (open != std::string::npos && open + 1 < base.size() - 1) {
            std::string inside = base.substr(open + 1, base.size() - open - 2);
            if (!inside.empty() && std::strchr("./@x*rwLm", inside[0])) {
                qual = inside;
                base = base.substr(0, open);
            }
        }
    }
    if (base.find_first_of("*?") == std::string::npos && qual.empty()) return {pattern};

    std::vector<std::string> out;
    if (base.find("**") != std::string::npos) {
        out = recursiveGlob(base);
        if (out.size() == 1 && out[0] == base) out.clear(); // recursiveGlob's "no match" literal
    } else {
        glob_t gl;
        if (::glob(base.c_str(), 0, nullptr, &gl) == 0)
            for (size_t i = 0; i < gl.gl_pathc; i++) out.push_back(gl.gl_pathv[i]);
        globfree(&gl);
    }
    if (!qual.empty()) out = applyGlobQualifiers(out, qual);
    if (out.empty()) out.push_back(pattern); // no match -> literal
    return out;
}

std::string expandNoSplit(const std::string& w, ShellState& state) {
    bool singleQuoted = w.size() >= 2 && w.front() == '\x02' && w.back() == '\x02';
    bool doubleQuoted = w.size() >= 2 && w.front() == '\x01' && w.back() == '\x01';
    if (singleQuoted) return w.substr(1, w.size() - 2); // fully literal
    std::string inner = doubleQuoted ? w.substr(1, w.size() - 2) : expandTilde(w);
    return expandWord(inner, state); // $/command-sub expansion, but the caller
                                      // (assignment RHS) never splits or globs
}

std::vector<std::string> expandWords(const std::vector<std::string>& words, ShellState& state) {
    std::vector<std::string> result;
    for (const auto& rawWord : words) {
        // Brace expansion runs first (bash ordering) and turns one word into
        // possibly many; each resulting word then goes through the normal
        // quote/variable/split/glob pipeline below.
        for (const auto& w : braceExpand(rawWord)) {
        bool singleQuoted = w.size() >= 2 && w.front() == '\x02' && w.back() == '\x02';
        bool doubleQuoted = w.size() >= 2 && w.front() == '\x01' && w.back() == '\x01';
        if (singleQuoted) {
            // Fully literal: no $ expansion, no splitting.
            result.push_back(w.substr(1, w.size() - 2));
            continue;
        }
        std::string inner = doubleQuoted ? w.substr(1, w.size() - 2) : expandTilde(w);
        std::string expanded = expandWord(inner, state);
        if (doubleQuoted) {
            // $ expansion still applies, but no IFS splitting/globbing.
            result.push_back(expanded);
            continue;
        }
        auto pieces = splitFields(expanded);
        for (auto& p : pieces) {
            auto globbed = globExpand(p);
            for (auto& g : globbed) result.push_back(std::move(g));
        }
        } // brace-expanded word loop
    }
    return result;
}
