#include "arkpy.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// ── ark-py: an IDE-style Python editor that lives inside ark ─────────────────
// Self-contained terminal editor. Everything (raw mode, screen management,
// syntax highlighting, autocomplete, run) is handled here so it doesn't perturb
// the shell's own line editor / chrome. Colours follow ark's neon-dark-blue
// theme (no purple/pink): blue keywords, cyan builtins, green strings, amber
// numbers, muted comments.

namespace {

// ── Colours (truecolor SGR) ──────────────────────────────────────────────────
constexpr const char* C_RESET   = "\x1b[0m";
constexpr const char* C_KEYWORD = "\x1b[38;2;77;159;255m";   // neon blue   #4d9fff
constexpr const char* C_BUILTIN = "\x1b[38;2;34;211;238m";   // cyan        #22d3ee
constexpr const char* C_STRING  = "\x1b[38;2;74;222;128m";   // green       #4ade80
constexpr const char* C_NUMBER  = "\x1b[38;2;224;175;104m";  // amber       #e0af68
constexpr const char* C_COMMENT = "\x1b[38;2;86;95;137m";    // muted       #565f89
constexpr const char* C_DECOR   = "\x1b[38;2;255;207;122m";  // gold        #ffcf7a
constexpr const char* C_DEFNAME = "\x1b[38;2;108;182;255m";  // light blue  #6cb6ff
constexpr const char* C_CONST   = "\x1b[38;2;255;107;122m";  // red         #ff6b7a (True/False/None/self)
constexpr const char* C_TEXT    = "\x1b[38;2;192;202;245m";  // fg          #c0caf5
constexpr const char* C_GUTTER  = "\x1b[38;2;65;72;104m";    // dim gutter  #414868
constexpr const char* C_GUTCUR  = "\x1b[38;2;122;162;247m";  // active line no
constexpr const char* C_STATUS  = "\x1b[48;2;45;63;118m\x1b[38;2;192;202;245m"; // status bar bg/fg
constexpr const char* C_STATDIRTY = "\x1b[48;2;45;63;118m\x1b[38;2;255;207;122m";

const std::unordered_set<std::string>& pyKeywords() {
    static const std::unordered_set<std::string> k = {
        "False","None","True","and","as","assert","async","await","break","class",
        "continue","def","del","elif","else","except","finally","for","from","global",
        "if","import","in","is","lambda","nonlocal","not","or","pass","raise","return",
        "try","while","with","yield","match","case",
    };
    return k;
}
const std::unordered_set<std::string>& pyBuiltins() {
    static const std::unordered_set<std::string> b = {
        "abs","aiter","all","anext","any","ascii","bin","bool","breakpoint","bytearray",
        "bytes","callable","chr","classmethod","compile","complex","delattr","dict","dir",
        "divmod","enumerate","eval","exec","filter","float","format","frozenset","getattr",
        "globals","hasattr","hash","help","hex","id","input","int","isinstance","issubclass",
        "iter","len","list","locals","map","max","memoryview","min","next","object","oct",
        "open","ord","pow","print","property","range","repr","reversed","round","set",
        "setattr","slice","sorted","staticmethod","str","sum","super","tuple","type","vars",
        "zip","__import__","self","cls",
    };
    return b;
}
const std::unordered_set<std::string>& pyConsts() {
    static const std::unordered_set<std::string> c = {"True","False","None","self","cls","__name__"};
    return c;
}

bool isWordStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isWordChar(char c)  { return std::isalnum((unsigned char)c) || c == '_'; }

// ── Editor state ─────────────────────────────────────────────────────────────
struct Editor {
    std::vector<std::string> lines{""};
    int cx = 0, cy = 0;        // cursor: column (byte), row (line index)
    int rowOff = 0, colOff = 0; // scroll offsets
    int rows = 24, cols = 80;   // terminal size
    std::string filename;
    bool dirty = false;
    std::string status;         // transient status message
    std::string pythonBin = "python3";
    termios orig{};             // saved cooked-mode termios

    // Output/compile targets from CLI flags (-o / -oc / -ocb). saveTarget is the
    // source file; compileTarget + compileMode drive the Ctrl-B build.
    enum class Compile { None, Bytecode, Native } compileMode = Compile::None;
    std::string compileTarget;  // where the compiled artifact goes

    int textRows() const { return rows - 1; }   // minus status bar
    int gutterW() const {
        int n = (int)lines.size(), w = 1;
        while (n >= 10) { n /= 10; w++; }
        return (w < 3 ? 3 : w) + 1; // at least 3 digits + a space
    }
};

// ── Terminal plumbing ────────────────────────────────────────────────────────
void enterRaw(Editor& e) {
    tcgetattr(STDIN_FILENO, &e.orig);
    termios raw = e.orig;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
void leaveRaw(Editor& e) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &e.orig); }

void queryTermSize(Editor& e) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        e.rows = ws.ws_row;
        e.cols = ws.ws_col;
    }
}
void altScreen(bool on) { fputs(on ? "\x1b[?1049h\x1b[?25h" : "\x1b[?1049l", stdout); fflush(stdout); }

// ── Python syntax highlighter (single line; carries triple-quote state) ──────
// `inTriple` / `tripCh` track an open triple-quoted string spanning lines.
std::string highlightPy(const std::string& s, bool& inTriple, char& tripCh) {
    std::string out;
    out.reserve(s.size() + 32);
    size_t i = 0, n = s.size();

    auto emit = [&](const char* col, const std::string& t) { out += col; out += t; out += C_RESET; };

    while (i < n) {
        // Continue an open triple-quoted string from a previous line.
        if (inTriple) {
            size_t start = i;
            while (i < n) {
                if (s[i] == tripCh && i + 2 < n + 1 && i + 2 <= n &&
                    i + 2 <= n && s.compare(i, 3, std::string(3, tripCh)) == 0) {
                    i += 3; inTriple = false; break;
                }
                i++;
            }
            emit(C_STRING, s.substr(start, i - start));
            continue;
        }
        char c = s[i];

        if (c == '#') { emit(C_COMMENT, s.substr(i)); break; }

        // Triple-quoted string open?
        if ((c == '"' || c == '\'') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            tripCh = c;
            size_t start = i; i += 3;
            inTriple = true;
            while (i < n) {
                if (s.compare(i, 3, std::string(3, tripCh)) == 0) { i += 3; inTriple = false; break; }
                i++;
            }
            emit(C_STRING, s.substr(start, i - start));
            continue;
        }
        // Normal string.
        if (c == '"' || c == '\'') {
            char q = c; size_t start = i; i++;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            emit(C_STRING, s.substr(start, i - start));
            continue;
        }
        // Decorator.
        if (c == '@' && i + 1 < n && isWordStart(s[i + 1])) {
            size_t start = i; i++;
            while (i < n && isWordChar(s[i])) i++;
            emit(C_DECOR, s.substr(start, i - start));
            continue;
        }
        // Number.
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            size_t start = i;
            while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '_' ||
                             ((s[i] == '+' || s[i] == '-') && i > start &&
                              (s[i - 1] == 'e' || s[i - 1] == 'E')))) i++;
            emit(C_NUMBER, s.substr(start, i - start));
            continue;
        }
        // Word: keyword / builtin / const / def-or-class name / identifier.
        if (isWordStart(c)) {
            size_t start = i;
            while (i < n && isWordChar(s[i])) i++;
            std::string w = s.substr(start, i - start);
            // Is this the name right after `def `/`class `? Colour it as a def name.
            size_t ws = out.size(); (void)ws;
            const char* col = C_TEXT;
            if (pyKeywords().count(w)) col = pyConsts().count(w) ? C_CONST : C_KEYWORD;
            else if (pyConsts().count(w)) col = C_CONST;
            else if (pyBuiltins().count(w)) col = C_BUILTIN;
            else {
                // def/class NAME → def name colour. Look back for the previous token.
                std::string prev;
                size_t j = start;
                while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t')) j--;
                size_t pe = j;
                while (j > 0 && isWordChar(s[j - 1])) j--;
                prev = s.substr(j, pe - j);
                if (prev == "def" || prev == "class") col = C_DEFNAME;
                // function call: NAME(  → keep as identifier (text) unless builtin.
            }
            emit(col, w);
            continue;
        }
        // Punctuation / operator / whitespace: pass through as plain text.
        size_t start = i;
        while (i < n && !isWordStart(s[i]) && !std::isdigit((unsigned char)s[i]) &&
               s[i] != '"' && s[i] != '\'' && s[i] != '#' && s[i] != '@') i++;
        if (i == start) i++; // safety
        out += C_TEXT; out += s.substr(start, i - start); out += C_RESET;
    }
    return out;
}

// ── Rendering ────────────────────────────────────────────────────────────────
void scrollToCursor(Editor& e) {
    int th = e.textRows();
    if (e.cy < e.rowOff) e.rowOff = e.cy;
    if (e.cy >= e.rowOff + th) e.rowOff = e.cy - th + 1;
    int avail = e.cols - e.gutterW();
    if (e.cx < e.colOff) e.colOff = e.cx;
    if (e.cx >= e.colOff + avail) e.colOff = e.cx - avail + 1;
    if (e.rowOff < 0) e.rowOff = 0;
    if (e.colOff < 0) e.colOff = 0;
}

void render(Editor& e) {
    scrollToCursor(e);
    std::string buf;
    buf.reserve(e.cols * e.rows + 256);
    buf += "\x1b[?25l";   // hide cursor during repaint
    buf += "\x1b[H";      // home

    int gw = e.gutterW();
    int avail = e.cols - gw;

    // Highlighter triple-quote state must be seeded from the top of the file,
    // not the top of the viewport, or a triple string opened above the fold
    // would render wrong. Cheap: walk from line 0 tracking state, only emitting
    // rows that are on screen.
    bool inTriple = false; char tripCh = '"';
    for (int y = 0; y < (int)e.lines.size() && y < e.rowOff; y++) {
        // advance triple state without emitting
        bool it = inTriple; char tc = tripCh;
        highlightPy(e.lines[y], it, tc);
        inTriple = it; tripCh = tc;
    }

    for (int sy = 0; sy < e.textRows(); sy++) {
        int y = e.rowOff + sy;
        buf += "\x1b[K"; // clear line
        if (y < (int)e.lines.size()) {
            // gutter
            char num[16];
            snprintf(num, sizeof(num), "%*d ", gw - 1, y + 1);
            buf += (y == e.cy) ? C_GUTCUR : C_GUTTER;
            buf += num; buf += C_RESET;
            // highlight the whole line (to keep triple state correct), then
            // slice the visible window out of the *raw* text with matching colour.
            bool it = inTriple; char tc = tripCh;
            std::string full = e.lines[y];
            // Apply horizontal scroll on the raw string, then highlight the slice.
            std::string vis = (e.colOff < (int)full.size())
                                  ? full.substr(e.colOff, avail)
                                  : std::string();
            // For correct multi-line string colour we still need to advance state
            // over the full line; highlight full but only print the slice's colours.
            // Simplest robust approach: highlight the full line, then advance the
            // real state; print a highlighted slice computed independently with the
            // seeded state (accurate when colOff==0, which is the common case).
            std::string colored = highlightPy(vis, it, tc);
            buf += colored;
            // advance the persistent state across the ENTIRE line
            bool it2 = inTriple; char tc2 = tripCh;
            highlightPy(full, it2, tc2);
            inTriple = it2; tripCh = tc2;
        } else {
            buf += C_GUTTER; buf += "~"; buf += C_RESET;
        }
        buf += "\r\n";
    }

    // ── status bar ──
    std::string name = e.filename.empty() ? "[No Name]" : e.filename;
    std::string left = " ark-py  " + name + (e.dirty ? " *" : "");
    std::string right = e.status.empty()
        ? ("Py " + std::to_string(e.cy + 1) + ":" + std::to_string(e.cx + 1) +
           "  ^R run  ^B build  ^S save  ^Q quit ")
        : (e.status + " ");
    int pad = e.cols - (int)left.size() - (int)right.size();
    if (pad < 1) { pad = 1; if ((int)left.size() > e.cols - (int)right.size() - 1)
                       left = left.substr(0, e.cols - (int)right.size() - 2); }
    buf += e.dirty ? C_STATDIRTY : C_STATUS;
    buf += left;
    for (int i = 0; i < pad; i++) buf += ' ';
    buf += right;
    buf += C_RESET;

    // place the real cursor
    int scy = e.cy - e.rowOff + 1;
    int scx = (e.cx - e.colOff) + gw + 1;
    char pos[32];
    snprintf(pos, sizeof(pos), "\x1b[%d;%dH", scy, scx);
    buf += pos;
    buf += "\x1b[?25h";
    fwrite(buf.data(), 1, buf.size(), stdout);
    fflush(stdout);
}

// ── Autocomplete ─────────────────────────────────────────────────────────────
std::vector<std::string> completions(const Editor& e, const std::string& prefix) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    auto consider = [&](const std::string& w) {
        if (w.size() > prefix.size() && w.compare(0, prefix.size(), prefix) == 0 && !seen.count(w)) {
            seen.insert(w); out.push_back(w);
        }
    };
    for (const auto& k : pyKeywords()) consider(k);
    for (const auto& b : pyBuiltins()) consider(b);
    // identifiers already used in the buffer
    for (const auto& line : e.lines) {
        size_t i = 0;
        while (i < line.size()) {
            if (isWordStart(line[i])) {
                size_t s = i;
                while (i < line.size() && isWordChar(line[i])) i++;
                consider(line.substr(s, i - s));
            } else i++;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Draw a small completion popup near the cursor; returns chosen string or "".
std::string completionMenu(Editor& e, const std::string& prefix,
                           const std::vector<std::string>& cands);

// ── File I/O ─────────────────────────────────────────────────────────────────
void loadFile(Editor& e, const std::string& path) {
    std::ifstream f(path);
    e.filename = path;
    if (!f.is_open()) { e.lines = {""}; e.status = "new file"; return; }
    e.lines.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        e.lines.push_back(line);
    }
    if (e.lines.empty()) e.lines.push_back("");
}
bool saveFile(Editor& e) {
    if (e.filename.empty()) return false;
    std::ofstream f(e.filename, std::ios::trunc);
    if (!f.is_open()) { e.status = "save failed: " + e.filename; return false; }
    for (size_t i = 0; i < e.lines.size(); i++) {
        f << e.lines[i];
        if (i + 1 < e.lines.size()) f << "\n";
    }
    f << "\n";
    e.dirty = false;
    e.status = "saved " + e.filename;
    return true;
}

// Prompt for a line of text on the status bar (cooked-ish, handled in raw).
std::string statusPrompt(Editor& e, const std::string& label) {
    std::string input;
    for (;;) {
        e.status = label + input;
        render(e);
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27) { input.clear(); break; }            // Esc cancels
        if (c == 127 || c == 8) { if (!input.empty()) input.pop_back(); continue; }
        if ((unsigned char)c >= 32) input += c;
    }
    e.status.clear();
    return input;
}

// ── Running & compiling ──────────────────────────────────────────────────────
// Ensure the buffer is on disk and return the path. If the editor has a
// filename it saves there (isTemp=false); otherwise it writes a temp .py
// (isTemp=true, caller unlinks).
std::string ensureSourceOnDisk(Editor& e, bool& isTemp) {
    if (!e.filename.empty()) { saveFile(e); isTemp = false; return e.filename; }
    char tmpl[] = "/tmp/arkpy-XXXXXX.py";
    int fd = mkstemps(tmpl, 3);
    if (fd < 0) { e.status = "cannot create temp file"; isTemp = false; return ""; }
    std::string body;
    for (size_t i = 0; i < e.lines.size(); i++) { body += e.lines[i]; body += "\n"; }
    (void)write(fd, body.data(), body.size());
    close(fd);
    isTemp = true;
    return tmpl;
}

// Run argv in a full-screen pane over the editor, wait, show the exit code, then
// wait for a keypress and return to the editor. Returns the child's exit code.
int runProgramPane(Editor& e, const std::vector<std::string>& argv, const std::string& header) {
    leaveRaw(e);
    altScreen(false);
    printf("\x1b[38;2;74;222;128m── %s ──\x1b[0m\n", header.c_str());
    fputs("\x1b[38;2;65;72;104m$", stdout);
    for (const auto& a : argv) printf(" %s", a.c_str());
    fputs("\x1b[0m\n\n", stdout);
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> cargv;
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    fputs(code == 0 ? "\n\x1b[38;2;74;222;128m" : "\n\x1b[38;2;255;107;122m", stdout);
    printf("── exit %d ── press any key to return ──\x1b[0m", code);
    fflush(stdout);

    enterRaw(e);
    char c;
    ssize_t n;
    do { n = read(STDIN_FILENO, &c, 1); } while (n < 0 && errno == EINTR);
    altScreen(true);
    return code;
}

// Ctrl-R: run the buffer through python3.
void runBuffer(Editor& e) {
    bool isTemp;
    std::string path = ensureSourceOnDisk(e, isTemp);
    if (path.empty()) return;
    int code = runProgramPane(e, {e.pythonBin, path}, "ark-py: running " + path);
    if (isTemp) unlink(path.c_str());
    e.status = "exit " + std::to_string(code);
}

// Ctrl-B: build. Saves the source, then compiles to the configured target:
//   -oc  → Python bytecode (.pyc) via py_compile
//   -ocb → native binary via $ARK_PY_NATIVE_CC <src> -o <out>
// With no compile target (-o only, or nothing) it just saves.
void buildBuffer(Editor& e) {
    if (e.compileMode == Editor::Compile::None) {
        if (e.filename.empty()) { e.status = "nothing to build — set -oc/-ocb or ^S to save"; return; }
        saveFile(e);
        e.status = "saved " + e.filename + " (no -oc/-ocb compile target set)";
        return;
    }
    const char* nativeCC = getenv("ARK_PY_NATIVE_CC");
    if (e.compileMode == Editor::Compile::Native && (!nativeCC || !*nativeCC)) {
        e.status = "native build needs ARK_PY_NATIVE_CC=<compiler> (set it in ark.config)";
        return;
    }
    bool isTemp;
    std::string src = ensureSourceOnDisk(e, isTemp);
    if (src.empty()) return;
    int code;
    if (e.compileMode == Editor::Compile::Native) {
        code = runProgramPane(e, {nativeCC, src, "-o", e.compileTarget},
                              std::string("ark-py: native compile → ") + e.compileTarget);
    } else {
        std::string prog = "import py_compile,sys; py_compile.compile(sys.argv[1], sys.argv[2], doraise=True)";
        code = runProgramPane(e, {e.pythonBin, "-c", prog, src, e.compileTarget},
                              "ark-py: bytecode compile → " + e.compileTarget);
    }
    if (isTemp) unlink(src.c_str());
    e.status = code == 0 ? ("built " + e.compileTarget)
                         : ("build failed (exit " + std::to_string(code) + ")");
}

// Non-interactive batch compile: `ark-py SRC -oc OUT` / `-ocb OUT` with no TTY.
// Compiles SRC directly and returns a process exit status (no editor).
int batchCompile(Editor& e, const std::string& src) {
    std::vector<std::string> argv;
    std::string prog;
    if (e.compileMode == Editor::Compile::Native) {
        const char* cc = getenv("ARK_PY_NATIVE_CC");
        if (!cc || !*cc) { fprintf(stderr, "ark-py: native build needs ARK_PY_NATIVE_CC=<compiler>\n"); return 2; }
        argv = {cc, src, "-o", e.compileTarget};
    } else {
        prog = "import py_compile,sys; py_compile.compile(sys.argv[1], sys.argv[2], doraise=True)";
        argv = {e.pythonBin, "-c", prog, src, e.compileTarget};
    }
    fprintf(stderr, "ark-py: compiling %s → %s\n", src.c_str(), e.compileTarget.c_str());
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> cargv;
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// ── Editing primitives ───────────────────────────────────────────────────────
std::string leadingWS(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(0, i);
}
void insertChar(Editor& e, char c) {
    e.lines[e.cy].insert(e.lines[e.cy].begin() + e.cx, c);
    e.cx++; e.dirty = true;
}
void insertNewline(Editor& e) {
    std::string& cur = e.lines[e.cy];
    std::string tail = cur.substr(e.cx);
    std::string indent = leadingWS(cur);
    // auto-indent: extra level after a line ending in ':'
    std::string trimmed = cur.substr(0, e.cx);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
    if (!trimmed.empty() && trimmed.back() == ':') indent += "    ";
    cur = cur.substr(0, e.cx);
    e.lines.insert(e.lines.begin() + e.cy + 1, indent + tail);
    e.cy++; e.cx = (int)indent.size(); e.dirty = true;
}
void backspace(Editor& e) {
    if (e.cx > 0) {
        // if only whitespace before cursor and it's a multiple of 4, delete a level
        std::string& cur = e.lines[e.cy];
        std::string before = cur.substr(0, e.cx);
        bool allWS = before.find_first_not_of(" ") == std::string::npos;
        int del = 1;
        if (allWS && e.cx >= 4 && e.cx % 4 == 0) del = 4;
        cur.erase(e.cx - del, del);
        e.cx -= del; e.dirty = true;
    } else if (e.cy > 0) {
        int prevLen = (int)e.lines[e.cy - 1].size();
        e.lines[e.cy - 1] += e.lines[e.cy];
        e.lines.erase(e.lines.begin() + e.cy);
        e.cy--; e.cx = prevLen; e.dirty = true;
    }
}
void clampCursor(Editor& e) {
    if (e.cy < 0) e.cy = 0;
    if (e.cy >= (int)e.lines.size()) e.cy = (int)e.lines.size() - 1;
    if (e.cx < 0) e.cx = 0;
    if (e.cx > (int)e.lines[e.cy].size()) e.cx = (int)e.lines[e.cy].size();
}

// current word prefix immediately left of the cursor
std::string wordPrefix(const Editor& e) {
    const std::string& l = e.lines[e.cy];
    int s = e.cx;
    while (s > 0 && isWordChar(l[s - 1])) s--;
    return l.substr(s, e.cx - s);
}
void replacePrefixWith(Editor& e, const std::string& prefix, const std::string& full) {
    std::string& l = e.lines[e.cy];
    l.erase(e.cx - (int)prefix.size(), prefix.size());
    l.insert(e.cx - (int)prefix.size(), full);
    e.cx += (int)full.size() - (int)prefix.size();
    e.dirty = true;
}

std::string completionMenu(Editor& e, const std::string& prefix,
                           const std::vector<std::string>& cands) {
    (void)prefix;
    int sel = 0;
    int maxShow = 8;
    for (;;) {
        // draw editor first
        render(e);
        // popup: below cursor line, at cursor column
        int scy = e.cy - e.rowOff + 1;
        int gw = e.gutterW();
        int scx = (e.cx - e.colOff) + gw + 1;
        int show = (int)cands.size() < maxShow ? (int)cands.size() : maxShow;
        int first = 0;
        if (sel >= maxShow) first = sel - maxShow + 1;
        std::string buf;
        for (int i = 0; i < show; i++) {
            int idx = first + i;
            int py = scy + 1 + i;
            if (py > e.textRows()) break;
            char pos[32]; snprintf(pos, sizeof(pos), "\x1b[%d;%dH", py, scx);
            buf += pos;
            if (idx == sel) buf += "\x1b[48;2;77;159;255m\x1b[38;2;27;30;44m";
            else            buf += "\x1b[48;2;45;63;118m\x1b[38;2;192;202;245m";
            std::string label = " " + cands[idx] + " ";
            if ((int)label.size() < 18) label += std::string(18 - label.size(), ' ');
            buf += label; buf += C_RESET;
        }
        fwrite(buf.data(), 1, buf.size(), stdout); fflush(stdout);

        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) return "";
        if (c == '\t') { sel = (sel + 1) % (int)cands.size(); continue; }
        if (c == '\r' || c == '\n') return cands[sel];
        if (c == 27) {
            // could be an arrow sequence
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
                read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[1] == 'A') { sel = (sel - 1 + (int)cands.size()) % (int)cands.size(); continue; }
                if (seq[1] == 'B') { sel = (sel + 1) % (int)cands.size(); continue; }
            }
            return ""; // Esc cancels
        }
        return "\x01" + std::string(1, c); // signal: consumer should re-handle this byte
    }
}

} // namespace

// ── Entry point ──────────────────────────────────────────────────────────────
int arkPyMain(const std::vector<std::string>& argv, ShellState& state) {
    (void)state;
    Editor e;
    if (const char* pv = getenv("ARK_PY_BIN")) e.pythonBin = pv;

    // Flags:
    //   ark-py [FILE]              open FILE in the editor
    //   ark-py -o FILE             set FILE as the source (open + save target)
    //   ark-py -oc OUT             compile to Python bytecode OUT on ^B / batch
    //   ark-py -ocb OUT            compile natively (via $ARK_PY_NATIVE_CC) to OUT
    // -o/-oc/-ocb compose, e.g. `ark-py -o app.py -ocb app`. With no TTY and a
    // compile target, ark-py compiles the source and exits (scriptable).
    std::string inputFile;
    for (size_t i = 1; i < argv.size(); i++) {
        const std::string& a = argv[i];
        if (a == "-o" && i + 1 < argv.size()) inputFile = argv[++i];
        else if (a == "-oc" && i + 1 < argv.size()) { e.compileMode = Editor::Compile::Bytecode; e.compileTarget = argv[++i]; }
        else if (a == "-ocb" && i + 1 < argv.size()) { e.compileMode = Editor::Compile::Native; e.compileTarget = argv[++i]; }
        else if (!a.empty() && a[0] != '-') inputFile = a;
        else { fprintf(stderr, "ark-py: unknown or incomplete option '%s'\n", a.c_str()); return 2; }
    }

    // Non-interactive batch compile: `ark-py SRC -oc OUT` with no TTY -> just
    // compile and exit, no editor.
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        if (e.compileMode != Editor::Compile::None && !inputFile.empty())
            return batchCompile(e, inputFile);
        fprintf(stderr, "ark-py: requires an interactive terminal (or SRC + -oc/-ocb to batch-compile)\n");
        return 1;
    }

    queryTermSize(e);
    if (!inputFile.empty()) loadFile(e, inputFile);
    else {
        std::string hint = "new buffer — ^R run  ^B build  ^S save  ^Q quit";
        if (e.compileMode == Editor::Compile::Native)   hint += "   [→ native " + e.compileTarget + "]";
        else if (e.compileMode == Editor::Compile::Bytecode) hint += "   [→ .pyc " + e.compileTarget + "]";
        e.status = hint;
    }

    enterRaw(e);
    altScreen(true);

    bool running = true;
    while (running) {
        queryTermSize(e);
        clampCursor(e);
        render(e);

        char c;
        ssize_t nr = read(STDIN_FILENO, &c, 1);
        // ark arms a 1s SIGALRM idle-ticker in interactive mode; a read()
        // interrupted by it returns EINTR, which must NOT be treated as EOF --
        // otherwise the editor "closes almost instantly" on the first tick.
        if (nr < 0 && errno == EINTR) continue;
        if (nr != 1) break;

    rehandle:
        e.status.clear();
        unsigned char uc = (unsigned char)c;

        if (uc == 17) {            // Ctrl-Q  quit
            if (e.dirty) {
                std::string ans = statusPrompt(e, "unsaved changes — quit? (y/N/s=save): ");
                if (ans == "s" || ans == "S") { if (!saveFile(e)) continue; running = false; }
                else if (ans == "y" || ans == "Y") running = false;
            } else running = false;
            continue;
        }
        if (uc == 18) { runBuffer(e); continue; }           // Ctrl-R  run
        if (uc == 2)  { buildBuffer(e); continue; }         // Ctrl-B  build/compile
        if (uc == 19) {                                     // Ctrl-S  save
            if (e.filename.empty()) {
                std::string name = statusPrompt(e, "save as: ");
                if (name.empty()) { e.status = "save cancelled"; continue; }
                e.filename = name;
            }
            saveFile(e);
            continue;
        }
        if (uc == '\t') {                                   // Tab  complete / indent
            std::string pfx = wordPrefix(e);
            if (pfx.empty()) { for (int i = 0; i < 4; i++) insertChar(e, ' '); continue; }
            auto cands = completions(e, pfx);
            if (cands.empty()) { for (int i = 0; i < 4; i++) insertChar(e, ' '); continue; }
            if (cands.size() == 1) { replacePrefixWith(e, pfx, cands[0]); continue; }
            std::string pick = completionMenu(e, pfx, cands);
            if (pick.empty()) continue;
            if (pick.size() >= 1 && pick[0] == '\x01') {    // menu handed a byte back
                replacePrefixWith(e, pfx, pfx); // no-op; fallthrough handles the byte
                c = pick[1];
                goto rehandle;
            }
            replacePrefixWith(e, pfx, pick);
            continue;
        }
        if (uc == '\r' || uc == '\n') { insertNewline(e); continue; }
        if (uc == 127 || uc == 8) { backspace(e); continue; }
        if (uc == 27) {                                     // escape sequence
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': e.cy--; clampCursor(e); break;               // up
                    case 'B': e.cy++; clampCursor(e); break;               // down
                    case 'C': if (e.cx < (int)e.lines[e.cy].size()) e.cx++; // right
                              else if (e.cy + 1 < (int)e.lines.size()) { e.cy++; e.cx = 0; } break;
                    case 'D': if (e.cx > 0) e.cx--;                          // left
                              else if (e.cy > 0) { e.cy--; e.cx = (int)e.lines[e.cy].size(); } break;
                    case 'H': e.cx = 0; break;                              // home
                    case 'F': e.cx = (int)e.lines[e.cy].size(); break;      // end
                    case '3': { char t; read(STDIN_FILENO, &t, 1);          // delete
                                if (e.cx < (int)e.lines[e.cy].size()) e.lines[e.cy].erase(e.cx, 1);
                                else if (e.cy + 1 < (int)e.lines.size()) {
                                    e.lines[e.cy] += e.lines[e.cy + 1];
                                    e.lines.erase(e.lines.begin() + e.cy + 1);
                                } e.dirty = true; break; }
                    case '5': { char t; read(STDIN_FILENO, &t, 1);          // pgup
                                e.cy -= e.textRows(); clampCursor(e); break; }
                    case '6': { char t; read(STDIN_FILENO, &t, 1);          // pgdn
                                e.cy += e.textRows(); clampCursor(e); break; }
                    default: break;
                }
            }
            continue;
        }
        if (uc >= 32) { insertChar(e, (char)c); continue; } // printable
        // other control chars: ignore
    }

    altScreen(false);
    leaveRaw(e);
    fflush(stdout);
    return 0;
}
