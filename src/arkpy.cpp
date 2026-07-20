#include "arkpy.h"
#include "arkpy_intel.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
constexpr const char* C_ERRGUT  = "\x1b[38;2;255;85;85m";                       // red gutter for error lines
constexpr const char* C_ERRTXT  = "\x1b[48;2;45;63;118m\x1b[38;2;255;138;138m"; // red-on-statusbar error text

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
    std::string compileCmdOverride; // ^E: session native-compile template, wins over env

    pyi::Analysis analysis;     // live diagnostics + symbols (ark's own engine)
    bool showHover = false;     // one-shot: hover text is in `status`

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
            bool lineHasErr = false;
            for (const auto& d : e.analysis.diags) if (d.line == y) { lineHasErr = true; break; }
            char num[24];
            if (lineHasErr) {
                // ● marker replaces the trailing gutter space on error lines
                snprintf(num, sizeof(num), "%*d\xE2\x97\x8f", gw - 1, y + 1);
                buf += C_ERRGUT; buf += num; buf += C_RESET;
            } else {
                snprintf(num, sizeof(num), "%*d ", gw - 1, y + 1);
                buf += (y == e.cy) ? C_GUTCUR : C_GUTTER;
                buf += num; buf += C_RESET;
            }
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
    std::string right;
    const char* rightColor = nullptr;   // optional SGR override for the right segment
    if (!e.status.empty()) {
        right = e.status + " ";
    } else if (!e.analysis.diags.empty()) {
        // First diagnostic wins the status bar -- ark's own "problems" line.
        const auto& d = e.analysis.diags[0];
        right = "\xE2\x9c\x98 " + std::to_string(e.analysis.diags.size()) + "  " +
                std::to_string(d.line + 1) + ":" + std::to_string(d.col + 1) + "  " + d.msg + " ";
        rightColor = C_ERRTXT;
    } else {
        right = "Py " + std::to_string(e.cy + 1) + ":" + std::to_string(e.cx + 1) +
                "  ^R run  ^B build  ^E flags  ^K hover  ^] def  ^S save  ^Q quit ";
    }
    int pad = e.cols - (int)left.size() - (int)right.size();
    if (pad < 1) { pad = 1; if ((int)left.size() > e.cols - (int)right.size() - 1)
                       left = left.substr(0, e.cols - (int)right.size() - 2); }
    buf += e.dirty ? C_STATDIRTY : C_STATUS;
    buf += left;
    for (int i = 0; i < pad; i++) buf += ' ';
    if (rightColor) buf += rightColor;
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
// Candidates come from ark's own Python engine (pyi::complete): scope-visible
// symbols, builtins, keywords, and member completion after `.`.

// Draw a small completion popup near the cursor; returns chosen string or "".
std::string completionMenu(Editor& e, const std::string& prefix,
                           const std::vector<std::string>& cands);

// ── File I/O ─────────────────────────────────────────────────────────────────
// Expand a leading `~` / `~/` to $HOME so ark-py accepts paths the way the shell
// does. Everything else (absolute, relative, dotted) is left untouched.
std::string expandUserPath(const std::string& p) {
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        if (const char* home = getenv("HOME")) return std::string(home) + p.substr(1);
    }
    return p;
}
// mkdir -p the parent directory of `path`, so `ark-py -o build/out/app.py` (or a
// save-as into a not-yet-existing folder) creates the folders instead of failing.
void ensureParentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return; // no dir part, or root
    std::string dir = path.substr(0, slash);
    std::string cur;
    size_t i = 0;
    if (!dir.empty() && dir[0] == '/') { cur = "/"; i = 1; }
    while (i <= dir.size()) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty() && cur != "/") mkdir(cur.c_str(), 0755); // ignore EEXIST
            if (i < dir.size()) cur += '/';
        } else {
            cur += dir[i];
        }
        i++;
    }
}

void loadFile(Editor& e, const std::string& path) {
    e.filename = expandUserPath(path);
    std::ifstream f(e.filename);
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
    e.filename = expandUserPath(e.filename);   // normalize once, so it persists
    ensureParentDir(e.filename);               // create missing folders
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
std::string statusPrompt(Editor& e, const std::string& label, const std::string& initial = "") {
    std::string input = initial;
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

// Guarantee the source handed to a compiler ends in `.py`, so a native compiler
// (or py_compile) detects Python by extension even when the edited file has no
// extension (e.g. a file literally named "ArkPy"). If the on-disk source isn't
// already `.py`, write the current buffer to a temp `.py` and return that.
std::string ensurePySource(Editor& e, bool& isTemp) {
    std::string base = ensureSourceOnDisk(e, isTemp);
    if (base.empty()) return base;
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, ".py") == 0) return base;
    char tmpl[] = "/tmp/arkpy-XXXXXX.py";
    int fd = mkstemps(tmpl, 3);
    if (fd < 0) return base; // fall back to the non-.py path
    std::string body;
    for (size_t i = 0; i < e.lines.size(); i++) { body += e.lines[i]; body += "\n"; }
    (void)write(fd, body.data(), body.size());
    close(fd);
    if (isTemp) unlink(base.c_str()); // drop the non-.py temp we just made
    isTemp = true;
    return tmpl;
}

// Run argv in a full-screen pane over the editor, wait, show the exit code, then
// wait for a keypress and return to the editor. Returns the child's exit code.
int runProgramPane(Editor& e, const std::vector<std::string>& argv, const std::string& header) {
    leaveRaw(e);
    // Stay INSIDE the alt-screen and just clear it for the output pane -- do NOT
    // switch back to the main screen. That way the build/run output lives only in
    // the alt-screen buffer and vanishes when ark-py exits, instead of being left
    // behind in the user's shell scrollback.
    fputs("\x1b[2J\x1b[H", stdout);
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
    // Stay in the alt-screen; the caller's next render() repaints the editor over
    // the pane. Nothing was written to the main screen, so quitting leaves a clean
    // shell.
    return code;
}

// Split a command string into argv, honoring "double"/'single' quotes (so a flag
// value with spaces stays one token). Quotes are stripped.
std::vector<std::string> tokenizeCmd(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur; bool inTok = false; char q = 0;
    for (char c : cmd) {
        if (q) { if (c == q) q = 0; else cur += c; inTok = true; continue; }
        if (c == '"' || c == '\'') { q = c; inTok = true; continue; }
        if (c == ' ' || c == '\t') { if (inTok) { out.push_back(cur); cur.clear(); inTok = false; } continue; }
        cur += c; inTok = true;
    }
    if (inTok) out.push_back(cur);
    return out;
}
void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
}

// Build the argv for a native compile. A native Python compiler can be advanced
// and want many flags, so the whole command line is configurable:
//   1) ARK_PY_NATIVE_CMD — a full template, whitespace-tokenized (quotes
//      respected), with placeholders substituted anywhere they appear:
//        {src}  source file      {out}  output path
//        {dir}  output's folder   {stem} output basename without extension
//      e.g.  ARK_PY_NATIVE_CMD="mycc {src} -o {out} -O2 -t macos --static --strip"
//   2) ARK_PY_NATIVE_CC [+ ARK_PY_NATIVE_FLAGS] — <cc> <flags…> <src> -o <out>
// Returns {} and sets `err` if neither is configured.
std::vector<std::string> nativeCompileArgv(const std::string& src, const std::string& out,
                                           const std::string& overrideTmpl, std::string& err) {
    std::string dir = out; { size_t s = dir.find_last_of('/'); dir = (s == std::string::npos) ? "." : dir.substr(0, s); }
    std::string stem = out; { size_t s = stem.find_last_of('/'); if (s != std::string::npos) stem = stem.substr(s + 1);
                              size_t d = stem.find_last_of('.'); if (d != std::string::npos) stem = stem.substr(0, d); }
    auto subst = [&](std::string t) {
        replaceAll(t, "{src}", src); replaceAll(t, "{out}", out);
        replaceAll(t, "{dir}", dir); replaceAll(t, "{stem}", stem);
        return t;
    };
    // The in-editor ^E override wins; otherwise ARK_PY_NATIVE_CMD from the env.
    std::string tmpl = overrideTmpl;
    if (tmpl.empty()) { if (const char* env = getenv("ARK_PY_NATIVE_CMD")) tmpl = env; }
    if (!tmpl.empty()) {
        std::vector<std::string> argv;
        for (auto& t : tokenizeCmd(tmpl)) argv.push_back(subst(t));
        // If the template names no {src}/{out}, append them so a bare command still works.
        if (tmpl.find("{src}") == std::string::npos) argv.push_back(src);
        if (tmpl.find("{out}") == std::string::npos) { argv.push_back("-o"); argv.push_back(out); }
        if (argv.empty()) { err = "compile command is empty"; return {}; }
        return argv;
    }
    const char* cc = getenv("ARK_PY_NATIVE_CC");
    if (!cc || !*cc) { err = "native build needs ARK_PY_NATIVE_CC or ARK_PY_NATIVE_CMD (set in ark.config)"; return {}; }
    std::vector<std::string> argv = { cc };
    if (const char* fl = getenv("ARK_PY_NATIVE_FLAGS"); fl && *fl)
        for (auto& t : tokenizeCmd(fl)) argv.push_back(subst(t));
    argv.push_back(src); argv.push_back("-o"); argv.push_back(out);
    return argv;
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
    if (e.compileMode == Editor::Compile::Native && e.compileCmdOverride.empty() &&
        !getenv("ARK_PY_NATIVE_CMD") && !getenv("ARK_PY_NATIVE_CC")) {
        e.status = "native build needs ^E, ARK_PY_NATIVE_CMD, or ARK_PY_NATIVE_CC";
        return;
    }
    // Refuse to compile ONTO the source file -- the output would overwrite your code.
    if (!e.filename.empty() && e.compileTarget == e.filename) {
        e.status = "output path equals the source — choose a different -ocb/-oc target";
        return;
    }
    bool isTemp;
    std::string src = ensurePySource(e, isTemp);
    if (src.empty()) return;
    ensureParentDir(e.compileTarget);
    int code;
    if (e.compileMode == Editor::Compile::Native) {
        std::string nerr;
        auto nargv = nativeCompileArgv(src, e.compileTarget, e.compileCmdOverride, nerr);
        if (nargv.empty()) { if (isTemp) unlink(src.c_str()); e.status = nerr; return; }
        code = runProgramPane(e, nargv, std::string("ark-py: native compile → ") + e.compileTarget);
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
int batchCompile(Editor& e, const std::string& srcIn) {
    // Ensure a .py-named source so a native compiler detects Python by extension.
    std::string src = srcIn;
    bool tempSrc = false;
    if (!(src.size() >= 3 && src.compare(src.size() - 3, 3, ".py") == 0)) {
        char tmpl[] = "/tmp/arkpy-XXXXXX.py";
        int fd = mkstemps(tmpl, 3);
        if (fd >= 0) {
            std::ifstream in(srcIn, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            (void)write(fd, body.data(), body.size());
            close(fd);
            src = tmpl; tempSrc = true;
        }
    }
    ensureParentDir(e.compileTarget);
    std::vector<std::string> argv;
    std::string prog;
    if (e.compileMode == Editor::Compile::Native) {
        std::string nerr;
        argv = nativeCompileArgv(src, e.compileTarget, e.compileCmdOverride, nerr);
        if (argv.empty()) { fprintf(stderr, "ark-py: %s\n", nerr.c_str()); return 2; }
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
    if (tempSrc) unlink(src.c_str());
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
// IDE-style auto-pairing. Returns true if it fully handled the key (so the caller
// skips the plain insert): auto-closes ( [ { " ' < , types over a closer already
// under the cursor, and refuses to auto-close when it would run into an
// identifier (so `func(here)` doesn't get a stray `)`).
bool smartPair(Editor& e, char c) {
    std::string& l = e.lines[e.cy];
    char next = (e.cx < (int)l.size()) ? l[e.cx] : '\0';
    char prev = (e.cx > 0) ? l[e.cx - 1] : '\0';

    // type-over: a closer/quote already sitting under the cursor -> step past it
    if ((c == ')' || c == ']' || c == '}' || c == '>' || c == '"' || c == '\'') && next == c) {
        e.cx++; return true;
    }
    // only auto-close when the cursor isn't butted up against more identifier text
    bool canClose = (next == '\0' || next == ' ' || next == '\t' || next == ')' ||
                     next == ']' || next == '}' || next == ',' || next == ':' || next == '>');
    auto pair = [&](char open, char close) {
        l.insert(l.begin() + e.cx, open);
        l.insert(l.begin() + e.cx + 1, close);
        e.cx++; e.dirty = true;
    };
    if (!canClose) return false;
    switch (c) {
        case '(': pair('(', ')'); return true;
        case '[': pair('[', ']'); return true;
        case '{': pair('{', '}'); return true;
        case '<': pair('<', '>'); return true;
        case '"': pair('"', '"'); return true;
        case '\'':
            // don't pair an apostrophe inside a word (don't -> don''t)
            if (isWordChar(prev)) return false;
            pair('\'', '\''); return true;
        default: return false;
    }
}
void insertNewline(Editor& e) {
    std::string& cur = e.lines[e.cy];
    std::string indent = leadingWS(cur);
    char prev = (e.cx > 0) ? cur[e.cx - 1] : '\0';
    char next = (e.cx < (int)cur.size()) ? cur[e.cx] : '\0';
    // Smart block expansion: Enter with the cursor between a bracket pair opens
    // the closer onto its own line and drops the cursor onto an indented middle
    // line -- `foo(|)` -> `foo(\n    |\n)`.
    if ((prev == '(' && next == ')') || (prev == '[' && next == ']') || (prev == '{' && next == '}')) {
        std::string head = cur.substr(0, e.cx);
        std::string tail = cur.substr(e.cx);
        cur = head;
        e.lines.insert(e.lines.begin() + e.cy + 1, indent + "    ");
        e.lines.insert(e.lines.begin() + e.cy + 2, indent + tail);
        e.cy++; e.cx = (int)indent.size() + 4; e.dirty = true;
        return;
    }
    std::string tail = cur.substr(e.cx);
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
        std::string& cur = e.lines[e.cy];
        // empty auto-pair: deleting the opener with its closer right after removes both
        char prev = cur[e.cx - 1];
        char next = (e.cx < (int)cur.size()) ? cur[e.cx] : '\0';
        if ((prev == '(' && next == ')') || (prev == '[' && next == ']') ||
            (prev == '{' && next == '}') || (prev == '"' && next == '"') ||
            (prev == '\'' && next == '\'') || (prev == '<' && next == '>')) {
            cur.erase(e.cx - 1, 2); e.cx--; e.dirty = true; return;
        }
        // if only whitespace before cursor and it's a multiple of 4, delete a level
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
        if (a == "-o" && i + 1 < argv.size()) inputFile = expandUserPath(argv[++i]);
        else if (a == "-oc" && i + 1 < argv.size()) { e.compileMode = Editor::Compile::Bytecode; e.compileTarget = expandUserPath(argv[++i]); }
        else if (a == "-ocb" && i + 1 < argv.size()) { e.compileMode = Editor::Compile::Native; e.compileTarget = expandUserPath(argv[++i]); }
        else if (!a.empty() && a[0] != '-') inputFile = expandUserPath(a);
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
        // Re-run ark's own Python analyzer for live diagnostics + completion.
        // Cheap enough (a linear pass) to do every loop iteration.
        e.analysis = pyi::analyze(e.lines);
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
        if (uc == 11) {                                     // Ctrl-K  hover (signature/doc)
            std::string h = pyi::hover(e.lines, e.cy, e.cx, e.analysis);
            e.status = h.empty() ? "(no info for symbol under cursor)" : h;
            continue;
        }
        if (uc == 29) {                                     // Ctrl-]  go to definition
            auto def = pyi::definition(e.lines, e.cy, e.cx, e.analysis);
            if (def.first >= 0) { e.cy = def.first; e.cx = def.second; e.status = "→ definition"; }
            else e.status = "definition not found in this buffer";
            continue;
        }
        if (uc == 5) {                                      // Ctrl-E  edit native compile command
            std::string cur = e.compileCmdOverride;
            if (cur.empty()) { if (const char* c = getenv("ARK_PY_NATIVE_CMD")) cur = c; }
            if (cur.empty()) {
                const char* cc = getenv("ARK_PY_NATIVE_CC");
                const char* fl = getenv("ARK_PY_NATIVE_FLAGS");
                cur = std::string(cc ? cc : "cc") + " " + (fl ? std::string(fl) + " " : "") + "{src} -o {out}";
            }
            std::string edited = statusPrompt(e, "compile cmd ({src}{out}{dir}{stem}): ", cur);
            if (!edited.empty()) { e.compileCmdOverride = edited; e.status = "compile command set — ^B to build"; }
            else e.status = "compile command unchanged";
            continue;
        }
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
            bool afterDot = (e.cx > 0 && e.lines[e.cy][e.cx - 1] == '.');
            if (pfx.empty() && !afterDot) { for (int i = 0; i < 4; i++) insertChar(e, ' '); continue; }
            auto comps = pyi::complete(e.lines, e.cy, e.cx, e.analysis);
            std::vector<std::string> cands;
            for (const auto& c : comps) cands.push_back(c.text);
            if (cands.empty()) { if (!afterDot) for (int i = 0; i < 4; i++) insertChar(e, ' '); continue; }
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
        if (uc >= 32) {                                     // printable
            if (!smartPair(e, (char)c)) insertChar(e, (char)c);
            continue;
        }
        // other control chars: ignore
    }

    altScreen(false);
    leaveRaw(e);
    fflush(stdout);
    return 0;
}
