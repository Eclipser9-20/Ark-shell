#include "expand.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fnmatch.h>
#include <glob.h>
#include <sstream>

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
            while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            long v = std::strtol(s.substr(pos, j - pos).c_str(), nullptr, 10);
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

    long parseUnary() {
        skipSpace();
        if (eat("!")) return !parseUnary();
        if (eat("~")) return ~parseUnary();
        if (pos < s.size() && s[pos] == '-') { pos++; return -parseUnary(); }
        if (pos < s.size() && s[pos] == '+') { pos++; return parseUnary(); }
        return parsePrimary();
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
            if (eat("<<")) v = v << parseAdd();
            else if (eat(">>")) v = v >> parseAdd();
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

    long parseExpr() { return parseLOr(); }
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
        long offset = std::strtol(body.substr(0, colon2).c_str(), nullptr, 10);
        long length = colon2 == std::string::npos ? -1 : std::strtol(body.substr(colon2 + 1).c_str(), nullptr, 10);
        long n = (long)val.size();
        if (offset < 0) offset = n + offset; // negative offset counts from end
        if (offset < 0) offset = 0;
        if (offset > n) return "";
        if (length < 0) return val.substr(offset); // to end
        long end = offset + length;
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
        return evalArithmetic(expr, state);
    }
    if (i < src.size() && src[i] == '(') {
        int depth = 1;
        size_t j = i + 1;
        while (j < src.size() && depth > 0) {
            if (src[j] == '(') depth++;
            else if (src[j] == ')') depth--;
            if (depth > 0) j++;
        }
        std::string cmd = src.substr(i + 1, j - i - 1);
        i = j + 1;
        std::string output = g_captureHook ? g_captureHook(cmd) : "";
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return output;
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

static std::vector<std::string> splitOnWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
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
    std::string hi = body.substr(dots + 2);
    if (hi.find("..") != std::string::npos) return {}; // {a..b..c} step form unsupported
    if (lo.empty() || hi.empty()) return {};

    auto allDigits = [](const std::string& s) {
        size_t k = (s[0] == '-') ? 1 : 0;
        if (k == s.size()) return false;
        for (; k < s.size(); k++) if (!std::isdigit((unsigned char)s[k])) return false;
        return true;
    };
    std::vector<std::string> out;
    if (allDigits(lo) && allDigits(hi)) {
        long a = std::strtol(lo.c_str(), nullptr, 10);
        long b = std::strtol(hi.c_str(), nullptr, 10);
        if (a <= b) for (long v = a; v <= b; v++) out.push_back(std::to_string(v));
        else for (long v = a; v >= b; v--) out.push_back(std::to_string(v));
        return out;
    }
    if (lo.size() == 1 && hi.size() == 1 && std::isalpha((unsigned char)lo[0]) && std::isalpha((unsigned char)hi[0])) {
        char a = lo[0], b = hi[0];
        if (a <= b) for (char c = a; c <= b; c++) out.push_back(std::string(1, c));
        else for (char c = a; c >= b; c--) out.push_back(std::string(1, c));
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

// Expands filesystem glob characters (* ?) via POSIX glob(3). A pattern with
// no glob chars is returned as-is; a pattern that matches nothing is left
// literal (bash's default behavior without nullglob), not silently dropped.
static std::vector<std::string> globExpand(const std::string& pattern) {
    if (pattern.find_first_of("*?") == std::string::npos) return {pattern};
    glob_t gl;
    int rc = ::glob(pattern.c_str(), 0, nullptr, &gl);
    std::vector<std::string> out;
    if (rc == 0) {
        for (size_t i = 0; i < gl.gl_pathc; i++) out.push_back(gl.gl_pathv[i]);
    } else {
        out.push_back(pattern);
    }
    globfree(&gl);
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
        auto pieces = splitOnWhitespace(expanded);
        for (auto& p : pieces) {
            auto globbed = globExpand(p);
            for (auto& g : globbed) result.push_back(std::move(g));
        }
        } // brace-expanded word loop
    }
    return result;
}
