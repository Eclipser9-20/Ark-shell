#include "arkpy_intel.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace pyi {
namespace {

bool isIdStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdChar(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }

const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> k = {
        "False","None","True","and","as","assert","async","await","break","class",
        "continue","def","del","elif","else","except","finally","for","from","global",
        "if","import","in","is","lambda","nonlocal","not","or","pass","raise","return",
        "try","while","with","yield","match","case",
    };
    return k;
}
const std::unordered_set<std::string>& compoundHeaders() {
    static const std::unordered_set<std::string> h = {
        "if","elif","else","for","while","def","class","try","except","finally","with",
        "async",  // `async def` / `async for` / `async with`
    };
    return h;
}
// Builtins → a short signature/doc for hover + completion detail.
const std::unordered_map<std::string, std::string>& builtinDocs() {
    static const std::unordered_map<std::string, std::string> b = {
        {"print", "print(*values, sep=' ', end='\\n', file=sys.stdout) — write to a stream"},
        {"len", "len(obj) -> int — number of items"},
        {"range", "range(stop) | range(start, stop[, step]) — arithmetic sequence"},
        {"int", "int(x=0, base=10) -> int"},
        {"str", "str(object='') -> str"},
        {"float", "float(x=0.0) -> float"},
        {"bool", "bool(x) -> bool"},
        {"list", "list(iterable=()) -> list"},
        {"dict", "dict(**kwargs) | dict(mapping) -> dict"},
        {"set", "set(iterable=()) -> set"},
        {"tuple", "tuple(iterable=()) -> tuple"},
        {"open", "open(file, mode='r', encoding=None) -> file object"},
        {"input", "input(prompt='') -> str — read a line from stdin"},
        {"enumerate", "enumerate(iterable, start=0) -> (index, item) pairs"},
        {"zip", "zip(*iterables) -> tuples of paired items"},
        {"map", "map(func, *iterables) -> iterator"},
        {"filter", "filter(func, iterable) -> iterator"},
        {"sorted", "sorted(iterable, key=None, reverse=False) -> list"},
        {"sum", "sum(iterable, /, start=0)"},
        {"min", "min(iterable, *, key=None)"},
        {"max", "max(iterable, *, key=None)"},
        {"abs", "abs(x) -> number"},
        {"round", "round(number, ndigits=None)"},
        {"isinstance", "isinstance(obj, classinfo) -> bool"},
        {"type", "type(object) -> the object's type"},
        {"super", "super() -> proxy to the parent class"},
        {"repr", "repr(obj) -> str — printable representation"},
        {"any", "any(iterable) -> bool"},
        {"all", "all(iterable) -> bool"},
        {"reversed", "reversed(seq) -> reverse iterator"},
        {"format", "format(value, format_spec='') -> str"},
        {"getattr", "getattr(obj, name[, default])"},
        {"setattr", "setattr(obj, name, value)"},
        {"hasattr", "hasattr(obj, name) -> bool"},
    };
    return b;
}
// Members exposed by well-known types / modules, for `obj.<prefix>` completion.
const std::unordered_map<std::string, std::vector<std::string>>& memberDB() {
    static const std::unordered_map<std::string, std::vector<std::string>> m = {
        {"str", {"upper","lower","strip","lstrip","rstrip","split","rsplit","join","replace",
                 "find","rfind","index","startswith","endswith","format","encode","title",
                 "capitalize","count","zfill","ljust","rjust","center","isdigit","isalpha",
                 "isalnum","isspace","splitlines","partition","removeprefix","removesuffix"}},
        {"list", {"append","extend","insert","remove","pop","clear","index","count","sort",
                  "reverse","copy"}},
        {"dict", {"get","keys","values","items","update","pop","popitem","setdefault","clear",
                  "copy","fromkeys"}},
        {"set", {"add","remove","discard","pop","clear","union","intersection","difference",
                 "symmetric_difference","issubset","issuperset","update","copy"}},
        {"os", {"getcwd","chdir","listdir","mkdir","makedirs","remove","rename","path","environ",
                "getenv","system","walk","stat","sep","name","urandom"}},
        {"os.path", {"join","exists","isfile","isdir","islink","basename","dirname","abspath",
                     "splitext","split","expanduser","getsize","realpath"}},
        {"sys", {"argv","exit","path","stdin","stdout","stderr","version","platform","modules",
                 "maxsize","getsizeof"}},
        {"math", {"sqrt","floor","ceil","pi","e","sin","cos","tan","log","log2","log10","pow",
                  "exp","fabs","factorial","gcd","inf","nan","isnan","hypot","degrees","radians"}},
        {"json", {"dumps","loads","dump","load"}},
        {"re", {"match","search","findall","finditer","sub","split","compile","escape","fullmatch"}},
        {"random", {"random","randint","randrange","choice","choices","shuffle","sample","uniform",
                    "seed","gauss"}},
        {"time", {"time","sleep","strftime","localtime","gmtime","perf_counter","monotonic"}},
    };
    return m;
}

// A logical line: its physical line index, indentation, and cleaned text (the
// concatenation of continued physical lines, comments removed, quote bodies
// blanked so operators inside strings don't confuse structure detection).
struct LogicalLine {
    int line;            // physical line where it starts
    int indent;          // leading spaces (tabs counted as 1 for ordering)
    std::string text;    // structural text (strings blanked, comment stripped)
    bool endsColon;      // last significant char is ':'
};

int leadingIndent(const std::string& s) {
    int n = 0;
    for (char c : s) { if (c == ' ') n++; else if (c == '\t') n += 1; else break; }
    return n;
}

} // namespace

Analysis analyze(const std::vector<std::string>& lines) {
    Analysis out;
    int bracket = 0;
    bool inTriple = false; char tripCh = '"';
    bool continuation = false; // previous physical line ended with a backslash

    // Structural text of the current logical line (across continuations), plus
    // where it started and its indent.
    std::string logical;
    int logLine = -1, logIndent = 0;
    std::vector<LogicalLine> logicals;

    auto flushLogical = [&]() {
        if (logLine < 0) return;
        // trailing significant char
        std::string t = logical;
        // trim trailing spaces
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        LogicalLine ll{logLine, logIndent, logical, !t.empty() && t.back() == ':'};
        logicals.push_back(ll);
        logical.clear();
        logLine = -1;
    };

    for (int li = 0; li < (int)lines.size(); li++) {
        const std::string& raw = lines[li];
        bool startsLogical = (bracket == 0 && !inTriple && !continuation);
        if (startsLogical) {
            flushLogical();
            // blank / comment-only lines don't start a logical line
            int ind = leadingIndent(raw);
            size_t f = raw.find_first_not_of(" \t");
            if (f != std::string::npos && raw[f] != '#') {
                logLine = li; logIndent = ind;
            }
        }

        // Walk the physical line char by char, tracking strings/brackets and
        // accumulating structural text into `logical` (if we're in one).
        std::string structural;
        size_t i = 0, n = raw.size();
        continuation = false;
        while (i < n) {
            char c = raw[i];
            if (inTriple) {
                if (c == tripCh && raw.compare(i, 3, std::string(3, tripCh)) == 0) { inTriple = false; i += 3; }
                else i++;
                continue;
            }
            if (c == '#') break; // comment to end of line
            if (c == '"' || c == '\'') {
                if (raw.compare(i, 3, std::string(3, c)) == 0) { tripCh = c; inTriple = true; i += 3; continue; }
                // single-line string
                char q = c; size_t start = i; i++;
                bool closed = false;
                while (i < n) {
                    if (raw[i] == '\\' && i + 1 < n) { i += 2; continue; }
                    if (raw[i] == q) { i++; closed = true; break; }
                    i++;
                }
                if (!closed) {
                    // Unterminated single-line string (not a continuation via \).
                    Diag d; d.line = li; d.col = (int)start; d.endCol = (int)n;
                    d.msg = "unterminated string literal"; d.sev = Diag::Error;
                    out.diags.push_back(d);
                }
                structural += ' '; // blank the string body for structure detection
                continue;
            }
            if (c == '(' || c == '[' || c == '{') { bracket++; structural += c; i++; continue; }
            if (c == ')' || c == ']' || c == '}') {
                if (bracket == 0) {
                    Diag d; d.line = li; d.col = (int)i; d.endCol = (int)i + 1;
                    d.msg = std::string("unmatched '") + c + "'"; d.sev = Diag::Error;
                    out.diags.push_back(d);
                } else bracket--;
                structural += c; i++; continue;
            }
            if (c == '\\' && i + 1 == n) { continuation = true; i++; continue; }
            structural += c; i++;
        }
        if (logLine >= 0) {
            if (!logical.empty()) logical += ' ';
            logical += structural;
        }
    }
    flushLogical();

    if (bracket > 0) {
        Diag d; d.line = (int)lines.size() - 1; if (d.line < 0) d.line = 0;
        d.col = 0; d.endCol = 1; d.msg = "unclosed bracket — missing ')', ']' or '}'";
        out.diags.push_back(d);
    }
    if (inTriple) {
        Diag d; d.line = (int)lines.size() - 1; if (d.line < 0) d.line = 0;
        d.col = 0; d.endCol = 1; d.msg = "unterminated triple-quoted string";
        out.diags.push_back(d);
    }

    // ── logical-line level checks: missing ':' on headers, indented blocks ──
    auto firstWord = [](const std::string& s) {
        size_t f = s.find_first_not_of(" \t");
        if (f == std::string::npos) return std::string();
        size_t e = f;
        while (e < s.size() && isIdChar(s[e])) e++;
        return s.substr(f, e - f);
    };
    for (size_t k = 0; k < logicals.size(); k++) {
        const LogicalLine& ll = logicals[k];
        std::string w = firstWord(ll.text);
        bool isHeader = compoundHeaders().count(w) > 0;
        // `else`/`try`/`finally` are headers; `async` only as `async def/for/with`.
        if (isHeader && !ll.endsColon) {
            // find the physical line's real end for the caret
            int col = (int)lines[ll.line].size();
            Diag d; d.line = ll.line; d.col = col > 0 ? col - 1 : 0; d.endCol = col;
            d.msg = "expected ':' after '" + w + "' statement"; d.sev = Diag::Error;
            out.diags.push_back(d);
        }
        if (ll.endsColon) {
            // the next logical line must be more indented
            if (k + 1 >= logicals.size() || logicals[k + 1].indent <= ll.indent) {
                Diag d; d.line = ll.line; d.col = ll.indent; d.endCol = ll.indent + 1;
                d.msg = "expected an indented block after '" + w + "'"; d.sev = Diag::Error;
                out.diags.push_back(d);
            }
        }
    }

    // ── symbol extraction (from logical lines) ──
    auto addSym = [&](const std::string& name, Symbol::Kind kind, int line, int col,
                      const std::string& detail) {
        if (name.empty()) return;
        Symbol s; s.name = name; s.kind = kind; s.line = line; s.col = col; s.detail = detail;
        out.symbols.push_back(s);
    };
    for (const LogicalLine& ll : logicals) {
        const std::string& t = ll.text;
        size_t f = t.find_first_not_of(" \t");
        if (f == std::string::npos) continue;
        std::string w = firstWord(t);
        int lineNo = ll.line;

        auto readName = [&](size_t& p) {
            while (p < t.size() && (t[p] == ' ' || t[p] == '\t')) p++;
            size_t s = p;
            while (p < t.size() && isIdChar(t[p])) p++;
            return t.substr(s, p - s);
        };

        if (w == "def" || (w == "async")) {
            size_t p = f + w.size();
            std::string kw2 = readName(p);
            if (w == "async" && kw2 != "def") { /* async for/with -> not a def */ }
            else {
                if (w == "async") { /* p is after 'def' */ }
                else p = f + 3; // after 'def'
                std::string name = readName(p);
                // signature = the (...) part
                std::string detail = "def " + name;
                size_t lp = t.find('(', p);
                if (lp != std::string::npos) {
                    size_t rp = t.find(')', lp);
                    if (rp != std::string::npos) detail = "def " + name + t.substr(lp, rp - lp + 1);
                    // parameters as Param symbols
                    if (rp != std::string::npos) {
                        std::string params = t.substr(lp + 1, rp - lp - 1);
                        size_t q = 0;
                        while (q < params.size()) {
                            while (q < params.size() && !isIdStart(params[q])) q++;
                            size_t s = q;
                            while (q < params.size() && isIdChar(params[q])) q++;
                            std::string pn = params.substr(s, q - s);
                            if (!pn.empty() && !keywords().count(pn)) addSym(pn, Symbol::Param, lineNo, 0, "");
                            // skip to next comma at depth 0
                            int depth = 0;
                            while (q < params.size() && !(params[q] == ',' && depth == 0)) {
                                if (params[q] == '(' || params[q] == '[' || params[q] == '{') depth++;
                                else if (params[q] == ')' || params[q] == ']' || params[q] == '}') depth--;
                                q++;
                            }
                            if (q < params.size()) q++; // skip comma
                        }
                    }
                }
                addSym(name, Symbol::Func, lineNo, (int)f, detail);
                continue;
            }
        }
        if (w == "class") {
            size_t p = f + 5;
            std::string name = readName(p);
            addSym(name, Symbol::Class, lineNo, (int)f, "class " + name);
            continue;
        }
        if (w == "import") {
            // import a, b.c as d
            size_t p = f + 6;
            while (p < t.size()) {
                std::string mod = readName(p);
                // dotted
                while (p < t.size() && t[p] == '.') { p++; std::string more = readName(p); mod += "." + more; }
                // 'as' alias
                size_t save = p; std::string kw = readName(p);
                std::string bind = mod;
                if (kw == "as") bind = readName(p); else p = save;
                addSym(bind, Symbol::Module, lineNo, (int)f, "import " + mod);
                while (p < t.size() && t[p] != ',') p++;
                if (p < t.size()) p++; else break;
            }
            continue;
        }
        if (w == "from") {
            // from m import a, b as c
            size_t p = t.find(" import", f);
            if (p != std::string::npos) {
                p += 7;
                while (p < t.size()) {
                    std::string nm = readName(p);
                    if (nm.empty()) break;
                    size_t save = p; std::string kw = readName(p);
                    std::string bind = nm;
                    if (kw == "as") bind = readName(p); else p = save;
                    if (bind != "*") addSym(bind, Symbol::Import, lineNo, (int)f, "from … import " + nm);
                    while (p < t.size() && t[p] != ',') p++;
                    if (p < t.size()) p++; else break;
                }
            }
            continue;
        }
        if (w == "for") {
            // for a, b in ...  -> loop targets
            size_t inpos = t.find(" in ", f);
            if (inpos != std::string::npos) {
                std::string targets = t.substr(f + 3, inpos - (f + 3));
                size_t q = 0;
                while (q < targets.size()) {
                    while (q < targets.size() && !isIdStart(targets[q])) q++;
                    size_t s = q; while (q < targets.size() && isIdChar(targets[q])) q++;
                    std::string nm = targets.substr(s, q - s);
                    if (!nm.empty() && !keywords().count(nm)) addSym(nm, Symbol::Var, lineNo, 0, "loop var");
                }
            }
            continue;
        }
        // `with ... as NAME`
        if (w == "with") {
            size_t aspos = 0;
            while ((aspos = t.find(" as ", aspos)) != std::string::npos) {
                size_t p = aspos + 4; std::string nm = readName(p);
                addSym(nm, Symbol::Var, lineNo, 0, "context var");
                aspos = p;
            }
            continue;
        }
        // simple assignment: NAME = ...   or   NAME: type = ...   (statement start)
        if (isIdStart(t[f]) && !keywords().count(w)) {
            size_t p = f; std::string name = readName(p);
            while (p < t.size() && (t[p] == ' ' || t[p] == '\t')) p++;
            // skip a type annotation `: type`
            if (p < t.size() && t[p] == ':') { size_t eq = t.find('=', p); if (eq != std::string::npos) p = eq; }
            if (p < t.size() && t[p] == '=' && (p + 1 >= t.size() || t[p + 1] != '=')) {
                addSym(name, Symbol::Var, lineNo, (int)f, "");
            }
        }
    }

    return out;
}

// ── completion ───────────────────────────────────────────────────────────────
std::vector<Completion> complete(const std::vector<std::string>& lines,
                                 int row, int col, const Analysis& a) {
    std::vector<Completion> out;
    if (row < 0 || row >= (int)lines.size()) return out;
    const std::string& line = lines[row];
    if (col > (int)line.size()) col = (int)line.size();

    // current prefix
    int s = col;
    while (s > 0 && isIdChar(line[s - 1])) s--;
    std::string prefix = line.substr(s, col - s);

    // member access?  look for a '.' immediately before the prefix
    bool member = (s > 0 && line[s - 1] == '.');
    std::unordered_set<std::string> seen;
    auto push = [&](const std::string& text, Symbol::Kind k, const std::string& detail) {
        if (text.size() <= prefix.size()) return;
        if (text.compare(0, prefix.size(), prefix) != 0) return;
        if (seen.count(text)) return;
        seen.insert(text);
        out.push_back(Completion{text, k, detail});
    };

    if (member) {
        // resolve the receiver word before the '.'
        int r = s - 1;                 // at the '.'
        int e = r; r--;                // char before '.'
        while (r >= 0 && isIdChar(line[r])) r--;
        std::string recv = line.substr(r + 1, e - (r + 1));
        (void)e;
        // Map the receiver to a type: an imported module name, or a builtin type.
        std::string typeKey = recv;
        for (const auto& sym : a.symbols)
            if (sym.name == recv && sym.kind == Symbol::Module) { typeKey = sym.detail.substr(sym.detail.find(' ') + 1); break; }
        auto it = memberDB().find(typeKey);
        if (it == memberDB().end() && recv == "self") {
            // members assigned as self.NAME = ... anywhere in the buffer
            for (const auto& l : lines) {
                size_t p = l.find("self.");
                while (p != std::string::npos) {
                    size_t q = p + 5, st = q;
                    while (q < l.size() && isIdChar(l[q])) q++;
                    push(l.substr(st, q - st), Symbol::Attr, "self");
                    p = l.find("self.", q);
                }
            }
        }
        if (it != memberDB().end())
            for (const auto& m : it->second) push(m, Symbol::Attr, typeKey + "." + m);
        std::sort(out.begin(), out.end(), [](const Completion& x, const Completion& y){ return x.text < y.text; });
        return out;
    }

    // plain identifier: buffer symbols, then builtins, then keywords
    for (const auto& sym : a.symbols)
        push(sym.name, sym.kind, sym.detail);
    for (const auto& kv : builtinDocs())
        push(kv.first, Symbol::Func, kv.second);
    for (const auto& k : keywords())
        push(k, Symbol::Var, "keyword");
    std::sort(out.begin(), out.end(), [](const Completion& x, const Completion& y){
        return x.text < y.text;
    });
    return out;
}

// word at (row,col)
static std::string wordAt(const std::vector<std::string>& lines, int row, int col) {
    if (row < 0 || row >= (int)lines.size()) return "";
    const std::string& l = lines[row];
    if (col > (int)l.size()) col = (int)l.size();
    int s = col, e = col;
    while (s > 0 && isIdChar(l[s - 1])) s--;
    while (e < (int)l.size() && isIdChar(l[e])) e++;
    return l.substr(s, e - s);
}

std::string hover(const std::vector<std::string>& lines, int row, int col, const Analysis& a) {
    std::string w = wordAt(lines, row, col);
    if (w.empty()) return "";
    for (const auto& s : a.symbols)
        if (s.name == w && !s.detail.empty()) return s.detail;
    auto it = builtinDocs().find(w);
    if (it != builtinDocs().end()) return it->second;
    if (keywords().count(w)) return w + " — Python keyword";
    return "";
}

std::pair<int,int> definition(const std::vector<std::string>& lines, int row, int col, const Analysis& a) {
    std::string w = wordAt(lines, row, col);
    if (w.empty()) return {-1, -1};
    // prefer a def/class; else first definition
    int best = -1, bestCol = 0;
    for (const auto& s : a.symbols) {
        if (s.name != w) continue;
        if (s.kind == Symbol::Func || s.kind == Symbol::Class) return {s.line, s.col};
        if (best < 0) { best = s.line; bestCol = s.col; }
    }
    return best >= 0 ? std::pair<int,int>{best, bestCol} : std::pair<int,int>{-1, -1};
}

} // namespace pyi
