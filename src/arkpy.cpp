#include "arkpy.h"
#include "arkpy_intel.h"
#include "arkpy_clangd.h"
#include "arkpy_libindex.h"
#include "arkpy_model.h"
#include "jobs.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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
// Build/run pane exit line.
constexpr const char* C_PANEOK  = "\x1b[38;2;74;222;128m";   // green exit 0
constexpr const char* C_PANEERR = "\x1b[38;2;255;107;122m";  // red non-zero
constexpr const char* C_ERRGUT  = "\x1b[38;2;255;85;85m";                       // red gutter for error lines
constexpr const char* C_ERRTXT  = "\x1b[48;2;45;63;118m\x1b[38;2;255;138;138m"; // red-on-statusbar error text
// Inline "error lens" text trailing the offending line, right of the code.
// The trailing diagnostic bar: a filled block, not tinted text, so it reads as a
// distinct piece of chrome sitting after the code rather than as more code.
constexpr const char* C_LENSERRBAR = "\x1b[48;2;74;34;42m\x1b[38;2;255;138;138m";  // red bar
constexpr const char* C_LENSWRNBAR = "\x1b[48;2;66;54;32m\x1b[38;2;235;198;140m";  // amber bar
// Centered modal prompt (`dialog = on`). The page behind it is repainted in a
// single flat grey so the box is unmistakably the only thing with focus.
constexpr const char* C_DIMTEXT = "\x1b[38;2;62;68;96m";
constexpr const char* C_DLGBG   = "\x1b[48;2;36;40;59m\x1b[38;2;192;202;245m";
constexpr const char* C_DLGEDGE = "\x1b[48;2;36;40;59m\x1b[38;2;122;162;247m";
constexpr const char* C_DLGIN   = "\x1b[48;2;26;27;38m\x1b[38;2;220;228;255m";
constexpr const char* C_SELBG   = "\x1b[48;2;54;74;138m\x1b[38;2;209;218;255m"; // selection
// Indent guides: a hairline down each indentation level. Dimmer than the
// comment colour on purpose -- it should read as structure you can feel rather
// than as content competing with the code.
constexpr const char* C_GUIDE   = "\x1b[38;2;52;58;84m";
constexpr const char* C_GHOST   = "\x1b[38;2;74;82;120m";    // inline suggestion #4a5278
// Completion popup.
constexpr const char* C_POPSEL  = "\x1b[48;2;77;159;255m\x1b[38;2;27;30;44m";  // selected row
constexpr const char* C_POPBG   = "\x1b[48;2;45;63;118m\x1b[38;2;192;202;245m";// unselected row
constexpr const char* C_POPDET  = "\x1b[48;2;45;63;118m\x1b[38;2;122;134;180m";// signature column
constexpr const char* C_POPDETS = "\x1b[48;2;77;159;255m\x1b[38;2;40;54;92m";  // signature, selected

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

    // Output/compile targets from CLI flags (-o / -oc / -ocb). saveTarget is the
    // source file; compileTarget + compileMode drive the Ctrl-B build.
    enum class Compile { None, Bytecode, Native } compileMode = Compile::None;
    std::string compileTarget;  // where the compiled artifact goes
    std::string compileCmdOverride; // ^E: session native-compile template, wins over env

    // Tunables from ~/.config/ark/arkpy.config (see loadArkPyConfig).
    bool ghostOn = true;
    int  ghostMinPrefix = 2;
    std::string modelPort, modelHost;
    std::string runArgs;    // ^G: arguments handed to the program on ^R (sys.argv[1:])
    // C/C++ toolchain. Empty = fall back to $ARK_CC/$ARK_CXX, then to cc/c++.
    // Named, not hardcoded, so any compiler can be dropped in from the config.
    std::string ccBin;      // arkpy.config: cc =
    std::string cxxBin;     // arkpy.config: cxx =
    std::string cflags;     // arkpy.config: cflags =   (also used for C++)
    bool useCMake = true;   // arkpy.config: cmake = off  disables project builds
    bool clangdOn = true;   // arkpy.config: clangd = off disables compile_commands
    // arky chrome, from arkpy.config. -1 = "not set, use the arky default", so a
    // config that says nothing still gets the full IDE and `ark-py` still gets
    // the bare editor -- only an explicit setting overrides either.
    int cfgMenu = -1, cfgTabs = -1, cfgExplorer = -1, cfgMouse = -1;
    int cfgSidebarW = -1;
    bool dialogOn = false;  // arkpy.config: dialog = on -- centered modal prompts
    bool guidesOn = true;   // arkpy.config: indent_guides = off
    int  indentW  = 4;      // arkpy.config: indent_width

    pyi::Analysis analysis;     // live diagnostics + symbols (ark's own engine)
    bool showHover = false;     // one-shot: hover text is in `status`

    // ── Selection ──
    // A selection runs between the anchor (ay,ax) and the live cursor (cy,cx);
    // either end may come first. ay < 0 means "no selection". Set by ^A (all)
    // and shift+arrows; any UNshifted cursor move drops it.
    int ax = 0, ay = -1;
    // Inline suggestion ("ghost text"): the tail of the best completion for the
    // word being typed, drawn dim after the cursor. Right-arrow / ^F accepts.
    // Recomputed once per keystroke in the main loop, not inside render(),
    // so the completion popup's repaints don't fight it.
    std::string ghost;      // the tail shown after the cursor
    std::string ghostFull;  // the whole word it would insert (incl. any "()")
    bool ghostCall = false; // true when accepting should land inside the parens
    std::string ghostAsked;  // prefix the model was last asked about (de-dupe)
    std::string modelPrefix, modelSuffix;  // cached model reply, kept until the prefix moves
    // Live completion popup (VS Code / IntelliJ style): the candidate list that
    // floats under the cursor as you type, filtering live. Coexists with the
    // ghost -- the popup shows every match, the ghost previews the top one inline.
    // Recomputed once per keystroke in the main loop, alongside the ghost, and
    // drawn as the last overlay so it sits on top of everything.
    bool complOn = true;                    // config: popup on/off
    bool complActive = false;               // popup currently showing
    std::vector<pyi::Completion> complCands; // filtered candidates for complPrefix
    std::string complPrefix;                // word prefix the candidates match
    int  complSel = 0;                      // highlighted row
    bool complDismissed = false;            // Esc hid it until the word changes
    std::string complDismissAt;             // prefix at which it was dismissed
    bool hasSel() const { return ay >= 0 && !(ay == cy && ax == cx); }
    // Normalized selection bounds: {startY,startX,endY,endX} with start <= end.
    void selRange(int& sy, int& sx, int& ey, int& ex) const {
        if (ay < cy || (ay == cy && ax <= cx)) { sy = ay; sx = ax; ey = cy; ex = cx; }
        else                                   { sy = cy; sx = cx; ey = ay; ex = ax; }
    }
    void clearSel() { ay = -1; }
    // Begin (or keep) a selection anchored where the cursor is right now --
    // called before a shift+move actually moves the cursor.
    void anchorSel() { if (ay < 0) { ay = cy; ax = cx; } }

    // ── Viewport ──
    // Where this buffer draws, 1-based, in screen coordinates. arky puts a menu
    // bar and tab strip above, a status bar below, and an explorer beside it, so
    // the editor can no longer assume it owns the screen. Defaults are set by
    // layout() each frame.
    int viewX = 1, viewY = 1, viewW = 80, viewH = 23;
    const char* appName = "ark-py";   // "arky" when running as the standalone IDE
    bool showMenu = false, showTabs = false;   // arky's chrome; off for the builtin
    int  sidebarW = 0;                          // explorer width, 0 = hidden

    int textRows() const { return viewH; }
    int gutterW() const {
        int n = (int)lines.size(), w = 1;
        while (n >= 10) { n /= 10; w++; }
        return (w < 3 ? 3 : w) + 1; // at least 3 digits + a space
    }
};

// ── Terminal plumbing ────────────────────────────────────────────────────────
// Terminal state is a property of the TERMINAL, not of a buffer -- arky has one
// Editor per tab, so keeping it on the struct would mean N copies of a single
// global fact and a "restore" that depends on which tab you happened to be in.
termios g_origTermios{};
bool    g_origSaved = false;

void enterRaw(Editor&) {
    // Capture the ORIGINAL terminal state exactly once. runProgramPane() also
    // calls enterRaw() after a child exits, and re-reading here overwrote
    // e.orig with the cooked state we had set up for the child -- so quitting
    // ark-py handed the SHELL a cooked terminal instead of the raw one ark's
    // line editor needs. That is why the main terminal was left double-echoing
    // after running a program that used input().
    if (!g_origSaved) { tcgetattr(STDIN_FILENO, &g_origTermios); g_origSaved = true; }
    termios raw = g_origTermios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
void leaveRaw(Editor&) { if (g_origSaved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_origTermios); }

// Mouse reporting is a property of the TERMINAL, not of any one editor, and it
// has to be switchable from code that runs before enableMouse() is defined (the
// run pane). g_mouseWanted is the intent; enableMouse() is the mechanism.
void enableMouse(bool on);
void enableBracketedPaste(bool on);
bool g_mouseWanted = false;

// Put the terminal into a KNOWN-GOOD cooked mode for a child process.
//
// leaveRaw() is not enough: e.orig is whatever the terminal was in when ark-py
// started, and ark-py is a shell builtin -- so ark's own line editor had the
// terminal in RAW mode at that moment. "Restoring" it handed the child a raw
// terminal: no echo, no line buffering, no signals. That is what made a program
// calling input() need every letter typed twice and finish without ever
// reading a line.
void cookedForChild() {
    termios t{};
    if (tcgetattr(STDIN_FILENO, &t) != 0) return;
    t.c_lflag |= (ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN);
    t.c_iflag |= (ICRNL | BRKINT);
    t.c_iflag &= ~(INLCR | IGNCR);
    t.c_oflag |= (OPOST | ONLCR);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
}

// Work out where the text region sits, given whatever chrome is turned on.
// ark-py (the in-shell builtin) turns all of it off and gets the full screen,
// exactly as before; arky lights up the menu bar, tab strip and explorer.
void layout(Editor& e) {
    e.viewY = 1;
    if (e.showMenu) e.viewY++;
    if (e.showTabs) e.viewY++;
    e.viewX = 1;
    e.viewH = e.rows - e.viewY;          // last row belongs to the status bar
    if (e.viewH < 1) e.viewH = 1;
    // "Editor wins" used to mean handing the editor the FULL width while the
    // sidebar was still painted on top of it. The two then disagreed about where
    // the text ended: the cursor was free to walk right, straight in under the
    // explorer. If there isn't room to split, shrink the sidebar -- and drop it
    // entirely rather than ever let the text region overlap it.
    e.viewW = e.cols - e.sidebarW;
    if (e.viewW < 20) {
        e.sidebarW = (e.cols > 20) ? e.cols - 20 : 0;
        e.viewW = e.cols - e.sidebarW;
    }
    if (e.viewW < 20) { e.sidebarW = 0; e.viewW = e.cols; }
}

void queryTermSize(Editor& e) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        e.rows = ws.ws_row;
        e.cols = ws.ws_col;
    }
}
void altScreen(bool on) {
    if (on) {
        fputs("\x1b[?1049h\x1b[?25h", stdout);
        // DROP ark's DECSTBM scroll region for the duration of the editor.
        //
        // ark-py is a BUILTIN, so unlike a spawned child nothing calls
        // releaseScrollRegionForChild() for it -- ark's pinned-bar region
        // (rows 2..N-1) was still in force while the editor painted the WHOLE
        // screen from row 1. render() writes one \r\n per text row, so the row
        // landing on the region's bottom margin SCROLLED the region instead of
        // moving down, silently eating a line: typing aaa/bbb/ccc rendered as
        // "1 aaa" then "3 ccc", with line 2 overwritten. The editor owns the
        // full screen while it's up; ark reasserts its own region afterwards.
        fputs("\x1b[r", stdout);
    } else {
        fputs("\x1b[?1049l", stdout);
    }
    fflush(stdout);
}

// ── Python syntax highlighter (single line; carries triple-quote state) ──────
// `inTriple` / `tripCh` track an open triple-quoted string spanning lines.
// True when this buffer should be highlighted as C/C++ rather than Python.
// A tiny local extension test: langOf() lives with the build machinery further
// down, and the highlighter must not depend on it.
bool looksLikeC(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == "c" || ext == "h" || ext == "cc" || ext == "cpp" || ext == "cxx" ||
           ext == "c++" || ext == "hpp" || ext == "hh" || ext == "hxx";
}

// Guess the language of an EXTENSIONLESS buffer from what's actually written in
// it, so `arky myscript` (no .py/.cpp/.c suffix) still highlights and analyzes
// correctly instead of always falling back to Python. Returns 0 = Python, 1 = C,
// 2 = C++. A shebang wins outright; otherwise C/C++ markers are scored against
// Python ones over the first ~120 lines -- enough to classify, cheap per repaint.
int detectLangCode(const std::vector<std::string>& lines) {
    if (!lines.empty() && lines[0].rfind("#!", 0) == 0) {
        // The file declares its own interpreter. arky has no dedicated shell
        // highlighter, and '#' is a line comment in both shell and Python, so a
        // sh/bash script renders least-wrong in Python mode -- and a `#!...python`
        // script is Python outright.
        return 0;
    }
    int cish = 0, cppish = 0, py = 0;
    size_t n = std::min<size_t>(lines.size(), 120);
    for (size_t i = 0; i < n; i++) {
        const std::string& L = lines[i];
        auto has = [&](const char* s) { return L.find(s) != std::string::npos; };
        // C++-specific signals.
        if (has("std::") || has("template<") || has("template <") ||
            has("namespace ") || has("nullptr") || has("cout") || has("cerr") ||
            has("#include <iostream>") || has("::")) cppish++;
        // C / native signals.
        if (has("#include") || has("#define") || has("#pragma") || has("int main") ||
            has("printf(") || has("return 0;") || has("void ") || has("malloc(") ||
            has("typedef ") || has("struct ")) cish++;
        // A line ending in ';' or a bare brace is a strong non-Python tell.
        size_t last = L.find_last_not_of(" \t");
        if (last != std::string::npos && (L[last] == ';' || L[last] == '{' || L[last] == '}'))
            cish++;
        // Python-specific signals.
        if (has("def ") || has("import ") || has("print(") || has("elif ") ||
            has("self.") || has("None") || has("True") || has("False")) py++;
        // A colon-terminated line (def/if/for/class ...:) is very Python.
        if (last != std::string::npos && L[last] == ':') py++;
    }
    int native = cish + cppish;
    if (native > py && native > 0) return cppish > 0 ? 2 : 1;
    return 0;   // default: Python, arky's home language
}

// The buffer's basename has no dot (or the buffer is unsaved).
static bool hasNoExtension(const std::string& filename) {
    std::string base = filename;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    return base.find('.') == std::string::npos;
}

// Content-aware variant: a named file with a recognized extension is trusted by
// its suffix (unchanged behavior); an extensionless buffer is classified by its
// contents, so `arky script` figures out C/C++ vs Python on its own.
bool looksLikeC(const Editor& e) {
    if (!hasNoExtension(e.filename)) return looksLikeC(e.filename);
    return detectLangCode(e.lines) != 0;
}

// C/C++ keyword sets. Split so a type reads differently from control flow --
// that distinction is most of what makes C skimmable.
const std::unordered_set<std::string>& cKeywords() {
    static const std::unordered_set<std::string> k = {
        "alignas","alignof","asm","break","case","catch","class","concept","const",
        "consteval","constexpr","constinit","const_cast","continue","co_await",
        "co_return","co_yield","decltype","default","delete","do","dynamic_cast",
        "else","enum","explicit","export","extern","false","final","for","friend",
        "goto","if","inline","mutable","namespace","new","noexcept","nullptr",
        "operator","override","private","protected","public","register",
        "reinterpret_cast","requires","return","sizeof","static","static_assert",
        "static_cast","struct","switch","template","this","thread_local","throw",
        "true","try","typedef","typeid","typename","union","using","virtual",
        "volatile","while",
    };
    return k;
}
const std::unordered_set<std::string>& cTypes() {
    static const std::unordered_set<std::string> t = {
        "auto","bool","char","char8_t","char16_t","char32_t","double","float","int",
        "long","short","signed","unsigned","void","wchar_t","size_t","ssize_t",
        "ptrdiff_t","intptr_t","uintptr_t","int8_t","int16_t","int32_t","int64_t",
        "uint8_t","uint16_t","uint32_t","uint64_t","FILE","string","vector","map",
    };
    return t;
}

// C/C++ completion. arky's semantic engine (pyi::) is Python-only, so before
// this a C++ buffer got Python keywords or nothing -- `#inclu` completed to
// nothing. This is a fast, lexical completer with no clangd dependency: on a
// preprocessor line it offers directives (`#include`, `#define`, …) and, inside
// `#include <…>`, common header names; elsewhere it offers C/C++ keywords, the
// built-in types, and every identifier already used in the buffer. It powers the
// same ghost text and completion popup the Python path does.
std::vector<pyi::Completion> completeC(const std::vector<std::string>& lines,
                                       int row, int col) {
    std::vector<pyi::Completion> out;
    if (row < 0 || row >= (int)lines.size()) return out;
    const std::string& line = lines[row];
    if (col > (int)line.size()) col = (int)line.size();
    auto idc = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    int s = col;
    while (s > 0 && idc(line[s - 1])) s--;
    std::string prefix = line.substr(s, col - s);
    if (prefix.empty()) return out;          // don't dump the whole language

    std::unordered_set<std::string> seen;
    auto push = [&](const std::string& t, pyi::Symbol::Kind k, const std::string& d) {
        if (t.size() <= prefix.size()) return;
        if (t.compare(0, prefix.size(), prefix) != 0) return;
        if (!seen.insert(t).second) return;
        out.push_back(pyi::Completion{t, k, d});
    };

    size_t hstart = line.find_first_not_of(" \t");
    bool ppLine = (hstart != std::string::npos && line[hstart] == '#');
    if (ppLine) {
        // Inside the angle/quote brackets of an #include: complete header names.
        size_t inc = line.find("include");
        size_t br = line.find_first_of("<\"");
        if (inc != std::string::npos && br != std::string::npos && col > (int)br) {
            int hs = col;
            while (hs > 0 && line[hs - 1] != '<' && line[hs - 1] != '"') hs--;
            std::string hp = line.substr(hs, col - hs);
            static const char* headers[] = {
                "stdio.h", "stdlib.h", "string.h", "math.h", "assert.h", "ctype.h",
                "errno.h", "stdint.h", "stddef.h", "stdbool.h", "limits.h", "time.h",
                "signal.h", "unistd.h", "fcntl.h", "sys/types.h", "sys/stat.h",
                "sys/socket.h", "pthread.h", "iostream", "vector", "string", "map",
                "unordered_map", "set", "unordered_set", "algorithm", "memory",
                "utility", "functional", "optional", "variant", "array", "deque",
                "list", "queue", "stack", "tuple", "thread", "mutex", "atomic",
                "chrono", "fstream", "sstream", "iomanip", "cstdio", "cstdlib",
                "cstring", "cmath", "cassert", "cstdint",
            };
            for (const char* h : headers) {
                std::string hh(h);
                if (hh.size() > hp.size() && hh.compare(0, hp.size(), hp) == 0)
                    out.push_back(pyi::Completion{hh, pyi::Symbol::Module, "header"});
            }
            return out;
        }
        static const char* directives[] = {
            "include", "define", "undef", "ifdef", "ifndef", "endif", "pragma",
            "error", "warning", "elif", "else", "if", "line",
        };
        for (const char* d : directives) push(d, pyi::Symbol::Var, "#" + std::string(d));
        return out;
    }

    for (const std::string& k : cKeywords()) push(k, pyi::Symbol::Var, "keyword");
    for (const std::string& t : cTypes())    push(t, pyi::Symbol::Class, "type");
    // Identifiers already used anywhere in the buffer -- function names, locals,
    // macros -- so completion learns the vocabulary of the file being edited.
    for (const std::string& l : lines) {
        size_t i = 0;
        while (i < l.size()) {
            if (idc(l[i]) && !(l[i] >= '0' && l[i] <= '9')) {
                size_t st = i;
                while (i < l.size() && idc(l[i])) i++;
                push(l.substr(st, i - st), pyi::Symbol::Var, "");
            } else i++;
        }
    }
    return out;
}

// C/C++ highlighter. Same signature as the Python one so render() can pick
// between them by extension without knowing anything else -- `inBlock` carries
// an unterminated /* */ across lines the way inTriple carries a docstring.
// Semantic tokens for the line being drawn, in column order. Empty when clangd
// hasn't answered (or isn't running), in which case highlightC falls back to
// pure lexical rules and nothing looks broken.
struct SemSpan { int col, len; const char* color; };

// Map a clangd token type to a colour. Only the kinds that carry real meaning
// beyond what the lexer already knows are mapped -- everything else is left to
// the lexical pass, which is right about keywords and strings by construction.
const char* semColor(const std::string& kind) {
    if (kind == "type" || kind == "class" || kind == "struct" || kind == "enum" ||
        kind == "interface" || kind == "typeParameter" || kind == "concept") return C_BUILTIN;
    if (kind == "function" || kind == "method") return C_DEFNAME;
    if (kind == "macro")                        return C_DECOR;
    if (kind == "enumMember")                   return C_CONST;
    if (kind == "namespace")                    return C_BUILTIN;
    return nullptr;
}

std::string highlightC(const std::string& s, bool& inBlock, char&,
                       const std::vector<SemSpan>* sem = nullptr) {
    std::string out;
    out.reserve(s.size() + 32);
    size_t i = 0, n = s.size();
    auto emit = [&](const char* col, const std::string& t) { out += col; out += t; out += C_RESET; };

    while (i < n) {
        if (inBlock) {                                   // inside /* ... */
            size_t start = i;
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; inBlock = false; break; }
                i++;
            }
            emit(C_COMMENT, s.substr(start, i - start));
            continue;
        }
        char c = s[i];

        if (c == '/' && i + 1 < n && s[i + 1] == '/') { emit(C_COMMENT, s.substr(i)); break; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t start = i; i += 2; inBlock = true;
            while (i < n) {
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; inBlock = false; break; }
                i++;
            }
            emit(C_COMMENT, s.substr(start, i - start));
            continue;
        }
        // Preprocessor: the whole directive, with an #include's <header> or
        // "header" kept as a string so the target still stands out.
        if (c == '#') {
            size_t start = i; i++;
            while (i < n && isWordChar(s[i])) i++;
            emit(C_DECOR, s.substr(start, i - start));
            size_t rest = i;
            while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
            out += s.substr(rest, i - rest);
            if (i < n && (s[i] == '<' || s[i] == '"')) {
                char close = (s[i] == '<') ? '>' : '"';
                size_t hs = i; i++;
                while (i < n && s[i] != close) i++;
                if (i < n) i++;
                emit(C_STRING, s.substr(hs, i - hs));
            }
            continue;
        }
        if (c == '"' || c == '\'') {                      // string or char literal
            char q = c; size_t start = i; i++;
            while (i < n) {
                if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            emit(C_STRING, s.substr(start, i - start));
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t start = i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.' ||
                             s[i] == '\'' ||
                             ((s[i] == '+' || s[i] == '-') && i > start &&
                              (s[i-1] == 'e' || s[i-1] == 'E')))) i++;
            emit(C_NUMBER, s.substr(start, i - start));
            continue;
        }
        if (isWordStart(c)) {
            size_t start = i;
            while (i < n && isWordChar(s[i])) i++;
            std::string w = s.substr(start, i - start);
            size_t j = i;
            while (j < n && s[j] == ' ') j++;
            // clangd's answer wins over the built-in guess: it actually knows
            // whether `Foo` is a type, a function, or a variable, which is the
            // whole reason for asking.
            const char* semc = nullptr;
            if (sem) {
                for (const SemSpan& sp : *sem)
                    if (sp.col == (int)start) { semc = sp.color; break; }
            }
            if (semc)                        emit(semc, w);
            else if (cKeywords().count(w))   emit(C_KEYWORD, w);
            else if (cTypes().count(w))      emit(C_BUILTIN, w);
            // A name followed by '(' is a call (or a definition). Cheap, and
            // right far more often than it is wrong.
            else if (j < n && s[j] == '(')   emit(C_DEFNAME, w);
            else                             out += w;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

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
                else {
                    // A call reads as a call: NAME( gets the same colour a def
                    // does. The C highlighter already did this; Python leaving
                    // every call plain is what made `pygame.init()` look inert
                    // next to the coloured builtins beside it.
                    size_t k = i;
                    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                    if (k < n && s[k] == '(') col = C_DEFNAME;
                }
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
    // The text viewport, NOT the terminal width. These disagree whenever the
    // explorer is open, and using the wider number meant colOff never advanced
    // for any column inside the sidebar's width: the cursor kept moving right
    // while the view refused to follow, so the tail of a long line simply could
    // not be reached. layout() has already run, so viewW is current.
    int avail = e.viewW - e.gutterW();
    if (avail < 8) avail = 8;
    if (e.cx < e.colOff) e.colOff = e.cx;
    if (e.cx >= e.colOff + avail) e.colOff = e.cx - avail + 1;
    if (e.rowOff < 0) e.rowOff = 0;
    if (e.colOff < 0) e.colOff = 0;
}

// Set while a modal dialog is up: render() paints the buffer flat grey and
// unhighlighted, so the only lit thing on screen is the dialog.
bool g_dimBackdrop = false;

const clangdc::Diag* fixForCursor(Editor& e);   // defined below saveFile

void render(Editor& e) {
    layout(e);          // must precede scrollToCursor: it computes viewW
    scrollToCursor(e);
    std::string buf;
    buf.reserve(e.cols * e.rows + 256);
    buf += "\x1b[?25l";   // hide cursor during repaint

    int gw = e.gutterW();
    int avail = e.viewW - gw;
    if (avail < 8) avail = 8;

    // Selection bounds, resolved once for the whole repaint.
    int ssy = -1, ssx = 0, sey = -1, sex = 0;
    if (e.hasSel()) e.selRange(ssy, ssx, sey, sex);

    // Highlighter triple-quote state must be seeded from the top of the file,
    // not the top of the viewport, or a triple string opened above the fold
    // would render wrong. Cheap: walk from line 0 tracking state, only emitting
    // rows that are on screen.
    bool inTriple = false; char tripCh = '"';
    // Which highlighter this buffer gets. Decided once per repaint, from the
    // filename -- an unsaved buffer stays Python, which is the arky default.
    const bool cSyntax = looksLikeC(e);

    // Effective indent of every line, for the guides. A blank line inherits the
    // indent of the block around it -- otherwise the hairlines break at every
    // empty line inside a function, which is exactly where you most want them to
    // stay continuous.
    std::vector<int> indentOf(e.lines.size(), 0);
    if (e.guidesOn) {
        for (size_t i = 0; i < e.lines.size(); i++) {
            const std::string& L = e.lines[i];
            size_t f = L.find_first_not_of(" \t");
            indentOf[i] = (f == std::string::npos) ? -1 : (int)f;   // -1 = blank
        }
        for (size_t i = 0; i < indentOf.size(); i++) {
            if (indentOf[i] >= 0) continue;
            int before = 0, after = 0;
            for (int j = (int)i - 1; j >= 0; j--) if (indentOf[j] >= 0) { before = indentOf[j]; break; }
            for (size_t j = i + 1; j < indentOf.size(); j++) if (indentOf[j] >= 0) { after = indentOf[j]; break; }
            indentOf[i] = std::min(before, after);
        }
    }
    // clangd's semantic tokens for this buffer, bucketed by line so each row
    // costs a lookup rather than a scan of the whole document.
    static std::map<int, std::vector<SemSpan>> semByLine;
    static std::string semFor;
    if (cSyntax && clangdc::running()) {
        std::vector<clangdc::Token> toks;
        if (clangdc::tokens(e.filename, toks)) {
            semByLine.clear();
            semFor = e.filename;
            for (const clangdc::Token& t : toks) {
                const char* col = semColor(t.kind);
                if (col) semByLine[t.line].push_back(SemSpan{t.col, t.len, col});
            }
        }
    }
    if (!cSyntax || semFor != e.filename) semByLine.clear();
    for (int y = 0; y < (int)e.lines.size() && y < e.rowOff; y++) {
        // advance triple state without emitting
        bool it = inTriple; char tc = tripCh;
        highlightPy(e.lines[y], it, tc);
        inTriple = it; tripCh = tc;
    }

    for (int sy = 0; sy < e.textRows(); sy++) {
        int y = e.rowOff + sy;
        // Position explicitly and pad to the viewport width. The old code used
        // \x1b[K and \r\n, which is only correct when the editor owns the whole
        // screen -- here it would erase the explorer sitting to the right.
        char rowpos[32];
        snprintf(rowpos, sizeof(rowpos), "\x1b[%d;%dH", e.viewY + sy, e.viewX);
        buf += rowpos;
        int used = 0;                       // columns emitted on this row
        if (y < (int)e.lines.size()) {
            // gutter
            const pyi::Diag* lineDiag = nullptr;
            int nDiag = 0;
            for (const auto& d : e.analysis.diags)
                if (d.line == y) { if (!lineDiag) lineDiag = &d; nDiag++; }
            char num[24];
            // The gutter stays a gutter. A marker crammed in beside the line
            // number reads as a defect in the CHROME rather than in the code,
            // and it shifts the eye left when the thing worth looking at is to
            // the right. The whole report is the trailing bar below.
            snprintf(num, sizeof(num), "%*d ", gw - 1, y + 1);
            buf += lineDiag ? C_ERRGUT : (y == e.cy ? C_GUTCUR : C_GUTTER);
            buf += num; buf += C_RESET;
            used += gw;
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
            // Indent guides occupy the line's leading whitespace, which every
            // painter below would otherwise emit as plain spaces. Draw that
            // region here and hand the rest on, so selection, dimming, syntax
            // colour and the ghost all keep working untouched.
            int lead = 0;
            if (e.guidesOn && e.indentW > 0) {
                int ind = indentOf[y];
                // A guide marks the START of each level, column 0 included: a
                // line indented one level sits to the right of a guide, and
                // without the one at column 0 the outermost level is invisible.
                // Blank lines get them too (vis is empty there) -- that is the
                // whole point of inheriting an indent above.
                int upto = std::min(ind, e.colOff + avail);
                if (upto > e.colOff) {
                    std::string g;
                    for (int cx2 = e.colOff; cx2 < upto; cx2++)
                        g += (cx2 % e.indentW == 0) ? "\xE2\x94\x82" : " ";
                    buf += C_GUIDE; buf += g; buf += C_RESET;
                    lead = upto - e.colOff;
                    used += lead;
                    if (lead <= (int)vis.size()) vis.erase(0, lead);
                    else vis.clear();
                }
            }
            bool selHere = (ssy >= 0 && y >= ssy && y <= sey);
            if (selHere) {
                // A selected line renders PLAIN with a selection background.
                // highlightPy emits SGR 0 resets between tokens, which would
                // knock the background out mid-line -- so inside a selection
                // syntax colour steps aside rather than fighting it.
                int a = (y == ssy) ? ssx : 0;
                int b = (y == sey) ? sex : (int)full.size() + 1;  // +1: include the newline
                a -= e.colOff + lead; b -= e.colOff + lead;
                if (a < 0) a = 0;
                if (b > (int)vis.size() + 1) b = (int)vis.size() + 1;
                if (a > (int)vis.size()) a = (int)vis.size();
                buf += C_TEXT;
                buf += vis.substr(0, a);
                if (b > a) {
                    buf += C_SELBG;
                    buf += vis.substr(a, std::min(b, (int)vis.size()) - a);
                    if (b > (int)vis.size()) buf += " ";   // show the eaten newline
                    buf += C_RESET;
                    buf += C_TEXT;
                }
                if (a < (int)vis.size() && b < (int)vis.size())
                    buf += vis.substr(b);
                buf += C_RESET;
                used += (int)vis.size();
            } else if (g_dimBackdrop) {
                buf += C_DIMTEXT; buf += vis; buf += C_RESET;
                used += (int)vis.size();
            } else {
                const std::vector<SemSpan>* sem = nullptr;
                if (cSyntax) {
                    auto sit = semByLine.find(y);
                    if (sit != semByLine.end()) sem = &sit->second;
                }
                // The suggestion belongs AT the cursor, not after the line: with
                // a ')' still to the right, appending it drew `print(my)ghost`.
                // Highlighting the two halves in sequence keeps the tokenizer's
                // carried state (an open triple-quote) correct across the split.
                int gcut = -1;
                if (!e.ghost.empty() && y == e.cy && !selHere) {
                    gcut = e.cx - e.colOff - lead;
                    if (gcut < 0 || gcut > (int)vis.size()) gcut = -1;
                    if (gcut >= 0 && used + (int)(vis.size() + e.ghost.size()) >= e.viewW) gcut = -1;
                }
                if (gcut >= 0) {
                    std::string lhs = vis.substr(0, gcut), rhs = vis.substr(gcut);
                    buf += cSyntax ? highlightC(lhs, it, tc, sem) : highlightPy(lhs, it, tc);
                    buf += C_GHOST; buf += e.ghost; buf += C_RESET;
                    buf += cSyntax ? highlightC(rhs, it, tc, sem) : highlightPy(rhs, it, tc);
                    used += (int)vis.size() + (int)e.ghost.size();
                } else {
                    buf += cSyntax ? highlightC(vis, it, tc, sem) : highlightPy(vis, it, tc);
                    used += (int)vis.size();
                    // No room to inline it (or a selection is up): append, as before.
                    if (!e.ghost.empty() && y == e.cy && !selHere &&
                        e.cx >= (int)e.lines[y].size() &&
                        used + (int)e.ghost.size() < e.viewW) {
                        buf += C_GHOST; buf += e.ghost; buf += C_RESET;
                        used += (int)e.ghost.size();
                    }
                }
            }
            // advance the persistent state across the ENTIRE line
            bool it2 = inTriple; char tc2 = tripCh;
            if (cSyntax) highlightC(full, it2, tc2, nullptr); else highlightPy(full, it2, tc2);
            inTriple = it2; tripCh = tc2;

            // ── Inline diagnostics ("error lens") ──
            // The message trails its own line, right of the code, so you never
            // have to look away from where the mistake is. Only the first
            // diagnostic per line is shown; extras are counted.
            if (lineDiag && !selHere) {
                // A filled bar carrying "✗ <reason>", set off from the code by
                // two spaces. The icon is part of the bar, so the severity reads
                // at a glance without a second colour to decode.
                const char* icon = (lineDiag->sev == pyi::Diag::Warning)
                                       ? "\xE2\x9A\xA0 "    // ⚠
                                       : "\xE2\x9C\x97 ";   // ✗
                int overhead = 2 /*gap*/ + 2 /*icon+space*/ + 2 /*bar padding*/;
                int room = e.viewW - used - overhead;
                if (room >= 8) {
                    std::string m = lineDiag->msg;
                    if (nDiag > 1) m += "  (+" + std::to_string(nDiag - 1) + ")";
                    // If clangd offers a fix for the line the cursor is on, say so
                    // right in the bar -- otherwise the fix is invisible. Checked
                    // only for the cursor line so it costs one lookup per frame.
                    if (y == e.cy) {
                        const clangdc::Diag* fx = fixForCursor(e);
                        if (fx && fx->line == y) m += "   \xE2\x8C\xA5^L fix";  // ⌥-ish hint
                    }
                    if ((int)m.size() > room) m = m.substr(0, room - 1) + "\xE2\x80\xA6";
                    buf += "  ";
                    buf += (lineDiag->sev == pyi::Diag::Warning) ? C_LENSWRNBAR : C_LENSERRBAR;
                    buf += " ";
                    buf += icon;
                    buf += m;
                    buf += " ";
                    buf += C_RESET;
                    used += 2 + 2 + 1 + (int)m.size() + 1;
                }
            }
        } else {
            buf += C_GUTTER; buf += "~"; buf += C_RESET;
            used += 1;
        }
        if (used < e.viewW) buf.append(e.viewW - used, ' ');
    }

    // ── status bar ── (full width, bottom row: arky keeps ark-py's exactly)
    {
        char sp[32];
        snprintf(sp, sizeof(sp), "\x1b[%d;1H", e.rows);
        buf += sp;
    }
    std::string name = e.filename.empty() ? "[No Name]" : e.filename;
    std::string left = std::string(" ") + e.appName + "  " + name + (e.dirty ? " *" : "");
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
                (e.hasSel() ? "  [sel]" : "") +
                "  ^O open  ^S save  ^R run  ^B build  ^A all  ^K hover  ^] def  ^Q quit ";
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

    // place the real cursor (viewport-relative)
    int scy = e.viewY + (e.cy - e.rowOff);
    int scx = e.viewX + gw + (e.cx - e.colOff);
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

// Draw a completion popup near the cursor -- name, kind tag, and signature per
// row; returns the chosen identifier, or "".
std::string completionMenu(Editor& e, const std::string& prefix,
                           const std::vector<pyi::Completion>& cands);
// Draw the live popup (non-modal) from e.complCands/e.complSel/e.complPrefix as
// an overlay on top of the already-painted frame, leaving the cursor in place.
void drawCompletionPopup(Editor& e);

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
    // A file's own directory is the first place Python looks for an import, so
    // it is the first place we look too. Re-set on every open: the local path
    // changes when the editor moves to a file in another folder.
    {
        std::string dir = ".";
        size_t slash = e.filename.find_last_of('/');
        if (slash != std::string::npos) dir = e.filename.substr(0, slash);
        pylib::setLocalPath({dir});
    }
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

// The clangd fix (if any) offered for the diagnostic ON the cursor's line, or
// else the nearest one -- so the error bar can advertise it and ^L can apply it.
// Returns nullptr when there's nothing to fix. Static storage: the returned
// pointer is only read synchronously, before the next diagnostics refresh.
const clangdc::Diag* fixForCursor(Editor& e) {
    if (!looksLikeC(e) || e.filename.empty() || !clangdc::running()) return nullptr;
    static std::vector<clangdc::Diag> diags;
    if (!clangdc::diagnostics(e.filename, diags)) return nullptr;
    const clangdc::Diag* best = nullptr;
    int bestDist = 1 << 30;
    for (const clangdc::Diag& d : diags) {
        if (d.fix.empty()) continue;
        int dist = std::abs(d.line - e.cy);
        if (dist < bestDist) { bestDist = dist; best = &d; }
    }
    return best;
}

// Apply clangd's quick-fix for the cursor's diagnostic: splice each edit into the
// buffer. Edits are applied last-first so an earlier edit never shifts the
// coordinates of a later one. Handles multi-line newText (a fix that inserts a
// whole line, like an #include). No-op with a status note when there's no fix.
bool applyClangdFix(Editor& e) {
    const clangdc::Diag* d = fixForCursor(e);
    if (!d) {
        e.status = looksLikeC(e) ? "no quick-fix here" : "quick-fix is C/C++ only";
        return false;
    }
    std::vector<clangdc::TextEdit> edits = d->fix;
    std::sort(edits.begin(), edits.end(), [](const clangdc::TextEdit& a, const clangdc::TextEdit& b) {
        return a.startLine != b.startLine ? a.startLine > b.startLine : a.startCol > b.startCol;
    });
    for (const clangdc::TextEdit& te : edits) {
        int sl = te.startLine, el = te.endLine;
        if (sl < 0 || sl >= (int)e.lines.size()) continue;
        if (el < 0 || el >= (int)e.lines.size()) el = (int)e.lines.size() - 1;
        int sc = std::min(te.startCol, (int)e.lines[sl].size());
        int ec = std::min(te.endCol,   (int)e.lines[el].size());
        std::string head = e.lines[sl].substr(0, sc);
        std::string tail = e.lines[el].substr(ec);
        std::string merged = head + te.newText + tail;
        // Remove the spanned lines, then splice the merged text back (splitting on
        // any newlines the fix introduced).
        e.lines.erase(e.lines.begin() + sl, e.lines.begin() + el + 1);
        std::vector<std::string> pieces;
        size_t start = 0;
        for (size_t i = 0; i <= merged.size(); i++) {
            if (i == merged.size() || merged[i] == '\n') {
                pieces.push_back(merged.substr(start, i - start));
                start = i + 1;
            }
        }
        e.lines.insert(e.lines.begin() + sl, pieces.begin(), pieces.end());
    }
    e.dirty = true;
    e.cy = std::min(e.cy, (int)e.lines.size() - 1);
    e.cx = std::min(e.cx, (int)e.lines[e.cy].size());
    e.status = "applied fix: " + d->fixTitle;
    return true;
}

// Repaint hook. statusPrompt() and the completion popup need the WHOLE frame
// redrawn -- menu bar, tabs and explorer included -- but they are defined long
// before the App that owns those. This indirection lets arky install its
// chrome-aware repaint without either half knowing about the other.
void render(Editor& e);
void (*g_redraw)(Editor&) = nullptr;
void redraw(Editor& e) { if (g_redraw) g_redraw(e); else render(e); }

// Prompt for a line of text on the status bar (cooked-ish, handled in raw).
// Draw the prompt as a centered modal box over a dimmed page. Same input loop,
// different presentation -- chosen by `dialog = on` in arkpy.config.
void drawDialog(Editor& e, const std::string& label, const std::string& input) {
    g_dimBackdrop = true;
    redraw(e);                       // repaint the page flat grey underneath
    g_dimBackdrop = false;

    int w = e.cols - 20;
    if (w > 72) w = 72;
    if (w < 30) w = e.cols - 4;
    if (w < 12) w = 12;
    int x = (e.cols - w) / 2 + 1;
    int y = e.rows / 2 - 1;
    if (y < 1) y = 1;

    std::string body = label + input;
    if ((int)body.size() > w - 4) body = body.substr(body.size() - (w - 4));

    std::string buf;
    char pos[32];
    auto row = [&](int ry, const char* color, const std::string& text) {
        snprintf(pos, sizeof(pos), "\x1b[%d;%dH", ry, x);
        buf += pos; buf += color; buf += text;
        int pad = w - (int)text.size();
        if (pad > 0) buf.append(pad, ' ');
        buf += C_RESET;
    };
    row(y,     C_DLGEDGE, std::string(" "));
    row(y + 1, C_DLGBG,   std::string("  ") + label);
    row(y + 2, C_DLGIN,   std::string("  ") + input);
    row(y + 3, C_DLGBG,   std::string("  Enter to confirm · Esc to cancel"));
    row(y + 4, C_DLGEDGE, std::string(" "));
    // Park the cursor at the end of what has been typed.
    int cx = x + 2 + (int)input.size();
    if (cx > x + w - 1) cx = x + w - 1;
    snprintf(pos, sizeof(pos), "\x1b[%d;%dH", y + 2, cx);
    buf += pos;
    fwrite(buf.data(), 1, buf.size(), stdout);
    fflush(stdout);
}

std::string statusPrompt(Editor& e, const std::string& label, const std::string& initial = "") {
    std::string input = initial;
    for (;;) {
        if (e.dialogOn) {
            drawDialog(e, label, input);
        } else {
            e.status = label + input;
            redraw(e);
        }
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27) {
            // Esc ALONE cancels. An escape SEQUENCE (an arrow key, a mouse
            // report) must be consumed whole -- reading just the ESC and
            // bailing left "[A" or a mouse report's digits to be typed into
            // the answer, which is where stray characters in prompts came from.
            fd_set rf; FD_ZERO(&rf); FD_SET(STDIN_FILENO, &rf);
            timeval tv{0, 20 * 1000};
            if (select(STDIN_FILENO + 1, &rf, nullptr, nullptr, &tv) <= 0) {
                input.clear(); break;                     // nothing followed: real Esc
            }
            char b;
            if (read(STDIN_FILENO, &b, 1) != 1) { input.clear(); break; }
            if (b != '[' && b != 'O') continue;           // 2-byte Meta form: ignore
            for (int guard = 0; guard < 40; guard++) {    // drain to the final byte
                if (read(STDIN_FILENO, &b, 1) != 1) break;
                if (b >= 0x30 && b <= 0x3f) continue;
                break;
            }
            continue;
        }
        if (c == 127 || c == 8) { if (!input.empty()) input.pop_back(); continue; }
        if ((unsigned char)c >= 32) input += c;
    }
    e.status.clear();
    return input;
}

// ── ark-py's own config ──────────────────────────────────────────────────────
// ~/.config/ark/arkpy.config -- `key = value`, `#` comments. Separate from
// ark.config on purpose: this is editor state, not shell state, and you edit it
// far more often. Written with every key commented out on first run, so the
// file itself is the documentation.
//
// An environment variable ALWAYS wins over the file, so a one-off
// `ARK_PY_MODEL=9999 ark-py x.py` still works without editing anything.
const char* arkPyDefaultConfig() {
    return R"CFG(# ark-py -- the in-shell Python editor.  `key = value`, # comments.
# Every line is commented out and shows the default. Uncomment to change.
# An environment variable of the same name (upper-cased, ARK_PY_ prefixed)
# overrides anything here.

# --- inline suggestions (the dim ghost text) ---------------------------------
# ghost = on            # off to turn suggestions off entirely
# min_prefix = 2        # characters to type before a suggestion appears
#                       # Tab takes the whole suggestion; shift+Tab one word.
# popup = on            # off to hide the live completion list (VS Code style).
#                       # It floats under the cursor as you type; up/down move,
#                       # Tab/Enter accept, Esc dismisses. min_prefix gates it too.

# --- completion server (optional) --------------------------------------------
# ark's own analyzer always answers first. A server is only asked when nothing
# local matches, so the editor never waits on it.
#
# Protocol -- one connection per request, one line each way:
#   ->  {"prefix":"pri","path":"x.py","line":3,"col":4,"before":"...","after":"..."}
#   <-  {"completion":"nt()"}
# A bare line of text is accepted as the completion too, so a server can be a
# few lines of Python. Reply with the text to insert AFTER the cursor.
# model_port = 1234
# model_host = 127.0.0.1
# model_timeout_ms = 150

# --- running ------------------------------------------------------------------
# python = python3      # interpreter for ^R run, and for library indexing
# args = --verbose in.txt   # handed to the program as sys.argv[1:] on ^R
#                       # ^G sets these for the session without editing this file.
#                       # The program gets a real terminal, so input() works.

# ── C / C++ ──────────────────────────────────────────────────────────────────
# arky picks the toolchain from the file extension. .c uses `cc`, .cpp/.cc/.h
# use `c++` -- the two names every unix has. Point these at whatever you
# actually use (gcc-14, clang, zig cc, your own compiler) and everything else
# follows: ^R compiles and runs, ^B builds.
# cc  = cc
# cxx = c++
# cflags = -O2 -Wall -Wextra -std=c++20
#
# cmake = on          # a buffer inside a CMake project builds the PROJECT on ^B
#                     # (cmake -S . -B build, then cmake --build build) instead
#                     # of compiling the one file. Off = always single-file.
# clangd = on         # keep build/compile_commands.json fresh so clangd (and
#                     # any other tool that reads it) sees the real build flags.

# ── arky ─────────────────────────────────────────────────────────────────────
# These apply when the editor is launched as `arky` (the IDE). Launched as
# `ark-py` it is always the bare editor and ignores this whole section, so you
# can turn arky into as much or as little of an IDE as you want without losing
# the plain one.
# menu = on           # the top menu bar
# tabs = on           # the tab strip (^T cycles, ^P > Close Tab)
# explorer = on       # the file sidebar (^E focuses it, ^E again returns)
# explorer_width = 24 # sidebar width in columns
# mouse = on          # click to place the cursor, click tabs and files, wheel
#                     # scrolls. Off if you'd rather keep your terminal's own
#                     # selection and scrollback behaviour.
# indent_guides = on  # hairlines down each level of indentation
# indent_width = 4     # columns per level (the guides' spacing)
# dialog = off        # on = prompts (open, save as, run args, compile command)
#                     # appear as a centered box with the page dimmed behind it,
#                     # instead of as a line on the bottom bar. Same keys either
#                     # way: type, Enter to confirm, Esc to cancel.
)CFG";
}

// Read the config, applying it to `e` and to the environment the model client
// reads. Creates the file (fully commented, nothing enabled) if absent.
// Tri-state parse for the arky toggles: 1 = on, 0 = off.
int onOff(const std::string& v) {
    return (v != "0" && v != "off" && v != "false" && v != "no") ? 1 : 0;
}

void loadArkPyConfig(Editor& e) {
    const char* home = getenv("HOME");
    if (!home || !*home) return;
    std::string dir = std::string(home) + "/.config/ark";
    std::string path = dir + "/arkpy.config";

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        ensureParentDir(path);            // mkdir -p ~/.config/ark
        std::ofstream f(path);
        if (f) f << arkPyDefaultConfig();
        return;                            // nothing enabled in a fresh file
    }

    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line.erase(h);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r");
            size_t b = s.find_last_not_of(" \t\r");
            return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
        };
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k.empty() || v.empty()) continue;

        // setenv(..., 0) = do NOT overwrite: an env var the user exported wins
        // over the file, which is the behaviour every other ark knob has.
        if      (k == "ghost")            e.ghostOn = (v != "0" && v != "off" && v != "false");
        else if (k == "min_prefix")       { int n = atoi(v.c_str()); if (n >= 1 && n <= 12) e.ghostMinPrefix = n; }
        else if (k == "popup")            e.complOn = (v != "0" && v != "off" && v != "false");
        else if (k == "args")             e.runArgs = v;
        else if (k == "python")           setenv("ARK_PY_BIN", v.c_str(), 0);
        else if (k == "model_timeout_ms") setenv("ARK_PY_MODEL_TIMEOUT_MS", v.c_str(), 0);
        else if (k == "model_port") {
            // model_host is applied below once both are known.
            e.modelPort = v;
        } else if (k == "model_host")     e.modelHost = v;
        else if (k == "cc")               e.ccBin = v;
        else if (k == "cxx")              e.cxxBin = v;
        else if (k == "cflags")           e.cflags = v;
        else if (k == "cmake")            e.useCMake = (v != "0" && v != "off" && v != "false");
        else if (k == "clangd")           e.clangdOn = (v != "0" && v != "off" && v != "false");
        // arky chrome. Stored on the Editor because that is what the config
        // loader has; arkPyMain copies them onto the App once it exists.
        else if (k == "menu")             e.cfgMenu = onOff(v);
        else if (k == "tabs")             e.cfgTabs = onOff(v);
        else if (k == "explorer")         e.cfgExplorer = onOff(v);
        else if (k == "mouse")            e.cfgMouse = onOff(v);
        else if (k == "dialog")           e.dialogOn = onOff(v) != 0;
        else if (k == "indent_guides")    e.guidesOn = onOff(v) != 0;
        else if (k == "indent_width")     { int n = atoi(v.c_str()); if (n >= 2 && n <= 8) e.indentW = n; }
        else if (k == "explorer_width")   { int n = atoi(v.c_str()); if (n >= 10 && n <= 80) e.cfgSidebarW = n; }
    }
    if (!e.modelPort.empty()) {
        std::string addr = (e.modelHost.empty() ? std::string("127.0.0.1") : e.modelHost)
                           + ":" + e.modelPort;
        setenv("ARK_PY_MODEL", addr.c_str(), 0);
    }
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
    (void)header;   // the command line below already says what this is
    // Mouse reporting off for the duration of the child. Left on, a scroll of the
    // wheel is delivered to the CHILD's stdin as escape bytes -- an input() read
    // eats them, or sees them as a line, and the program falls straight out with
    // exit code 0.
    const bool hadMouse = g_mouseWanted;
    enableMouse(false);
    enableBracketedPaste(false);      // the child gets a plain terminal
    leaveRaw(e);
    cookedForChild();   // the child needs a real terminal, not ark's raw one
    // Stay INSIDE the alt-screen and just clear it for the output pane -- do NOT
    // switch back to the main screen. That way the build/run output lives only in
    // the alt-screen buffer and vanishes when ark-py exits, instead of being left
    // behind in the user's shell scrollback.
    fputs("\x1b[2J\x1b[H", stdout);

    // Deliberately plain: the command, a rule, then its output. No header (the
    // command line already says what this is) and no box -- just enough
    // structure to see where the tool's output starts and stops.
    queryTermSize(e);
    std::string rule(e.cols > 2 ? e.cols - 1 : 1, '-');
    fputs("\x1b[38;2;65;72;104m$", stdout);
    for (const auto& a : argv) printf(" %s", a.c_str());
    printf("\n%s\x1b[0m\n", rule.c_str());
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> cargv;
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    // MUST retry on EINTR. ark arms a 1-second SIGALRM idle ticker, which
    // interrupts this wait -- waitpid then returns -1 with `st` untouched
    // (zero), which reads as "exited, code 0". The pane printed its footer
    // while the program was still running, and any program that then waited on
    // input() was left orphaned holding the terminal: keystrokes went to the
    // orphan instead of the editor, and the shell got the terminal back in the
    // wrong state. jobs.h documents this same trap for the shell's own waits.
    int st = 0;
    if (pid > 0) {
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { /* retry */ }
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

    // Let late output land BEFORE the footer. waitpid() returns when our direct
    // child exits, but a compiler that re-execs or hands work off to a
    // grandchild can still be writing to the terminal at that moment -- the
    // footer then printed first and the tool's own summary line ran straight
    // into it, on the same row.
    // A short settle costs nothing on a build that already finished.
    usleep(60 * 1000);

    // Always start on a fresh line: the child's last write may have ended
    // mid-line (no trailing newline), which is what glued the two together.
    fputs("\r\n", stdout);
    printf("\x1b[38;2;65;72;104m%s\x1b[0m\n", rule.c_str());
    printf("%sexit %d\x1b[0m \x1b[38;2;65;72;104m· press any key\x1b[0m\n",
           code == 0 ? C_PANEOK : C_PANEERR, code);
    fflush(stdout);

    enterRaw(e);
    enableBracketedPaste(true);
    if (hadMouse) enableMouse(true);
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
// ── Languages ────────────────────────────────────────────────────────────────
// arky is not a Python editor with C bolted on: the buffer's extension picks the
// toolchain, and everything below (run, build, clangd) branches on it.
enum class Lang { Python, C, Cpp };

std::string parentOf(const std::string& path);   // defined with the file explorer

Lang langOf(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return Lang::Python;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == "c") return Lang::C;
    if (ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "c++" ||
        ext == "h"  || ext == "hpp" || ext == "hh"  || ext == "hxx") return Lang::Cpp;
    return Lang::Python;
}

// Content-aware variant, mirroring looksLikeC(const Editor&): an extensionless
// buffer's language comes from its contents, so an extensionless C++ file gets
// C++ toolchain/clangd and a Python one stays Python.
Lang langOf(const Editor& e) {
    if (!hasNoExtension(e.filename)) return langOf(e.filename);
    switch (detectLangCode(e.lines)) {
        case 1:  return Lang::C;
        case 2:  return Lang::Cpp;
        default: return Lang::Python;
    }
}

// The C and C++ compilers, in priority order: the buffer's own override, then
// the config/environment, then the system default. `cc`/`c++` are the fallbacks
// precisely because they are what EVERY unix has -- a machine with a different
// toolchain sets cc=/cxx= in arkpy.config and nothing else has to know its name.
std::string ccFor(const Editor& e, Lang l) {
    if (l == Lang::Cpp) {
        if (!e.cxxBin.empty()) return e.cxxBin;
        if (const char* v = getenv("ARK_CXX")) if (*v) return v;
        return "c++";
    }
    if (!e.ccBin.empty()) return e.ccBin;
    if (const char* v = getenv("ARK_CC")) if (*v) return v;
    return "cc";
}

bool haveTool(const std::string& name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0;
    const char* p = getenv("PATH");
    if (!p) return false;
    std::string path(p), part;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == ':') {
            if (!part.empty() && access((part + "/" + name).c_str(), X_OK) == 0) return true;
            part.clear();
        } else part += path[i];
    }
    return false;
}

// Walk up from the buffer looking for a CMake project. A hit means ^B builds the
// PROJECT, not the one file -- editing one translation unit of a real program and
// having the build compile it in isolation is never what you meant.
std::string cmakeRootFor(const std::string& file) {
    if (file.empty()) return "";
    std::string dir = parentOf(file);
    for (int up = 0; up < 24 && !dir.empty() && dir != "/"; up++) {
        struct stat st{};
        if (stat((dir + "/CMakeLists.txt").c_str(), &st) == 0) return dir;
        dir = parentOf(dir);
    }
    return "";
}

// The directory IntelliJ (or any project tool) treats as the project root, found
// by walking up from the file. IntelliJ marks its root with a `.idea/` directory,
// so that wins outright -- it is exactly "where we are in IDEA". Failing that, any
// common project/VCS marker anchors the explorer at the project, not at whatever
// deep source subdirectory (src/main/java/...) the edited file happens to sit in.
// Returns "" when nothing is found, so the caller can fall back to the file's dir.
std::string projectRootFor(const std::string& file) {
    if (file.empty()) return "";
    static const char* markers[] = {
        "pom.xml", "build.gradle", "build.gradle.kts",
        "settings.gradle", "settings.gradle.kts",
        "CMakeLists.txt", "Cargo.toml", "go.mod",
        "pyproject.toml", "package.json", ".git",
    };
    std::string idea, generic;
    std::string dir = parentOf(file);
    for (int up = 0; up < 32 && !dir.empty() && dir != "/"; up++) {
        struct stat st{};
        if (idea.empty() && stat((dir + "/.idea").c_str(), &st) == 0) idea = dir;
        if (generic.empty())
            for (const char* m : markers)
                if (stat((dir + "/" + m).c_str(), &st) == 0) { generic = dir; break; }
        dir = parentOf(dir);
    }
    // .idea is the definitive IntelliJ root; else the nearest generic project marker.
    return !idea.empty() ? idea : generic;
}

// clangd reads compile_commands.json; CMake emits it. Ask for it always, so the
// editor's own indexing and any external clangd see the same flags the build used.
std::vector<std::string> cmakeConfigureArgv(const std::string& root) {
    return {"cmake", "-S", root, "-B", root + "/build",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
}

// Generate build/compile_commands.json if a CMake project has none yet. clangd
// finds it by walking up from the file, so this is the whole integration: once
// the database exists, clangd (in arky, in your other editor, anywhere) knows
// the real include paths and standard for every file in the project. Runs at
// most once per project per session, silently, in the background -- a configure
// step must never block a keystroke.
// Bring clangd up for this buffer, if it's C/C++ and clangd is installed.
// Rooted at the CMake project when there is one, else the file's own directory.
void ensureClangd(Editor& e) {
    if (!e.clangdOn || e.filename.empty()) return;
    if (langOf(e) == Lang::Python) return;
    if (!haveTool("clangd")) return;
    std::string root = cmakeRootFor(e.filename);
    if (root.empty()) root = parentOf(e.filename);
    if (!clangdc::start(root)) return;
    std::string whole;
    for (size_t i = 0; i < e.lines.size(); i++) {
        whole += e.lines[i];
        if (i + 1 < e.lines.size()) whole += '\n';
    }
    clangdc::open(e.filename, whole);
}

void ensureCompileCommands(Editor& e) {
    static std::set<std::string> done;
    if (!e.clangdOn || !e.useCMake) return;
    if (langOf(e) == Lang::Python) return;
    std::string root = cmakeRootFor(e.filename);
    if (root.empty() || !done.insert(root).second) return;
    struct stat st{};
    if (stat((root + "/build/compile_commands.json").c_str(), &st) == 0) return;
    if (!haveTool("cmake") || !haveTool("clangd")) return;

    pid_t pid = fork();
    if (pid != 0) return;                       // parent: never waits
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
    std::vector<std::string> a = cmakeConfigureArgv(root);
    std::vector<char*> cargv;
    for (std::string& s : a) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);
    execvp(cargv[0], cargv.data());
    _exit(127);
}

void runBuffer(Editor& e) {
    // C/C++: compile to a temporary binary, then run THAT. `cc foo.c && ./a.out`
    // in one keystroke, which is what ^R means for a compiled language.
    Lang lang = langOf(e);
    if (lang != Lang::Python) {
        if (e.filename.empty()) { e.status = "save the buffer first (^S) — the compiler needs a file"; return; }
        if (!saveFile(e)) return;
        std::string out = "/tmp/arky-run-" + std::to_string((long)getpid());
        std::string cc = ccFor(e, lang);
        if (!haveTool(cc)) { e.status = "compiler not found: " + cc + " — set cc=/cxx= in arkpy.config"; return; }
        std::vector<std::string> cargv = {cc};
        for (const std::string& f : tokenizeCmd(e.cflags)) cargv.push_back(f);
        cargv.push_back(e.filename);
        cargv.push_back("-o");
        cargv.push_back(out);
        int code = runProgramPane(e, cargv, "arky: compiling " + e.filename);
        if (code != 0) { e.status = "compile failed (exit " + std::to_string(code) + ")"; return; }
        std::vector<std::string> rargv = {out};
        for (const std::string& a : tokenizeCmd(e.runArgs)) rargv.push_back(a);
        code = runProgramPane(e, rargv, "arky: running " + e.filename);
        unlink(out.c_str());
        e.status = "exit " + std::to_string(code);
        return;
    }
    bool isTemp;
    std::string path = ensureSourceOnDisk(e, isTemp);
    if (path.empty()) return;
    // sys.argv[0] is the script; everything from ^G (or `args` in the config)
    // follows it, quoted the same way a shell would split it.
    std::vector<std::string> argv = {e.pythonBin, path};
    for (const std::string& a : tokenizeCmd(e.runArgs)) argv.push_back(a);
    int code = runProgramPane(e, argv, "ark-py: running " + path);
    if (isTemp) unlink(path.c_str());
    e.status = "exit " + std::to_string(code);
}

// Ctrl-B: build. Saves the source, then compiles to the configured target:
//   -oc  → Python bytecode (.pyc) via py_compile
//   -ocb → native binary via $ARK_PY_NATIVE_CC <src> -o <out>
// With no compile target (-o only, or nothing) it just saves.
void buildBuffer(Editor& e) {
    // C/C++: a CMake project builds as a project; a lone file compiles beside
    // itself (foo.c -> foo), which is the least surprising default.
    Lang lang = langOf(e);
    if (lang != Lang::Python) {
        if (e.filename.empty()) { e.status = "save the buffer first (^S) — the compiler needs a file"; return; }
        if (!saveFile(e)) return;
        std::string root = e.useCMake ? cmakeRootFor(e.filename) : "";
        if (!root.empty() && haveTool("cmake")) {
            int code = runProgramPane(e, cmakeConfigureArgv(root), "arky: cmake configure");
            if (code == 0)
                code = runProgramPane(e, {"cmake", "--build", root + "/build"}, "arky: cmake build");
            e.status = code == 0 ? ("built " + root + "/build")
                                 : ("cmake failed (exit " + std::to_string(code) + ")");
            return;
        }
        std::string cc = ccFor(e, lang);
        if (!haveTool(cc)) { e.status = "compiler not found: " + cc + " — set cc=/cxx= in arkpy.config"; return; }
        std::string out = e.compileTarget;
        if (out.empty()) {
            out = e.filename;
            size_t dot = out.rfind('.');
            if (dot != std::string::npos && dot > out.rfind('/') + 0) out.erase(dot);
        }
        if (out == e.filename) { e.status = "output path equals the source — set -ocb"; return; }
        std::vector<std::string> cargv = {cc};
        for (const std::string& f : tokenizeCmd(e.cflags)) cargv.push_back(f);
        cargv.push_back(e.filename);
        cargv.push_back("-o");
        cargv.push_back(out);
        int code = runProgramPane(e, cargv, "arky: building " + out);
        e.status = code == 0 ? ("built " + out)
                             : ("build failed (exit " + std::to_string(code) + ")");
        return;
    }
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
    // A closing quote counts as "open space" too: inside an auto-paired string
    // (`f"|"`), the char under the cursor is the closing quote, and without this
    // an f-string's `{` -- the single most common brace in Python -- never paired.
    bool canClose = (next == '\0' || next == ' ' || next == '\t' || next == ')' ||
                     next == ']' || next == '}' || next == ',' || next == ':' ||
                     next == '>' || next == '"' || next == '\'');
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
void clampCursor(Editor& e);

// True if byte `col` of `line` is real code -- not inside a string literal and
// not after a `#`. Single-line scan (triple-quoted blocks are handled by the
// caller only insofar as they look like strings on their own line), which is
// all the suggestion path needs: it only ever asks about the cursor's line.
bool inCodeContext(const std::string& line, int col) {
    char quote = 0;
    for (int i = 0; i < col && i < (int)line.size(); i++) {
        char c = line[i];
        if (quote) {
            if (c == '\\') { i++; continue; }
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            return false;
        }
    }
    return quote == 0;
}

// ── System pasteboard ────────────────────────────────────────────────────────
// ark-py's ^X/^C/^V talk to the real OS clipboard so text crosses the editor
// boundary. Falls back to an in-editor buffer when no helper exists (a bare
// Linux tty, say). stderr is discarded so a missing helper can't scribble on
// the alt-screen. Each helper is tried in turn; the first that exits 0 wins.
const char* const kCopyCmds[] = {
    "pbcopy 2>/dev/null",                               // macOS
    "wl-copy 2>/dev/null",                              // wayland
    "xclip -selection clipboard 2>/dev/null",           // X11
    "xsel --clipboard --input 2>/dev/null",
};
const char* const kPasteCmds[] = {
    "pbpaste 2>/dev/null",
    "wl-paste --no-newline 2>/dev/null",
    "xclip -selection clipboard -o 2>/dev/null",
    "xsel --clipboard --output 2>/dev/null",
};
// NOTE the BlockSigchld guards. ark installs a SIGCHLD handler that reaps
// children asynchronously; without blocking it, the handler collects popen's
// child first and pclose()'s waitpid() then fails with ECHILD. The copy really
// happened, but pclose reports -1 -- which is exactly how this first showed up:
// text landed on the pasteboard while ark said "no clipboard helper".
// Base64 for OSC 52. Small, self-contained: pulling in a dependency to encode a
// clipboard payload would be absurd.
std::string b64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (unsigned char)in[i + 2];
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += (i + 1 < in.size()) ? T[(v >> 6) & 63] : '=';
        o += (i + 2 < in.size()) ? T[v & 63] : '=';
    }
    return o;
}

// Hand the selection to the TERMINAL's clipboard via OSC 52. This is the copy
// that matters: it is the same clipboard Cmd+V reads, it needs no helper binary,
// and it works over SSH where pbcopy would set the clipboard on the wrong
// machine. Terminals cap the payload, so very large copies fall back to the
// helper alone.
void osc52Copy(const std::string& text) {
    if (text.size() > 64 * 1024) return;
    std::string seq = "\x1b]52;c;" + b64(text) + "\x07";
    fwrite(seq.data(), 1, seq.size(), stdout);
    fflush(stdout);
}

bool systemCopy(const std::string& text) {
    osc52Copy(text);                 // best-effort; the helpers below still run
    for (const char* cmd : kCopyCmds) {
        BlockSigchld guard;
        FILE* p = popen(cmd, "w");
        if (!p) continue;
        fwrite(text.data(), 1, text.size(), p);
        if (pclose(p) == 0) return true;
    }
    return false;
}
bool systemPaste(std::string& out) {
    for (const char* cmd : kPasteCmds) {
        BlockSigchld guard;
        FILE* p = popen(cmd, "r");
        if (!p) continue;
        std::string got;
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), p)) > 0) got.append(b, n);
        if (pclose(p) == 0 && !got.empty()) { out = got; return true; }
    }
    return false;
}

// Delete the active selection and leave the cursor where it started. Returns
// false (and does nothing) when there is no selection, so callers can write
// `if (deleteSelection(e)) ...` as a "replace" preamble.
bool deleteSelection(Editor& e) {
    if (!e.hasSel()) { e.clearSel(); return false; }
    int sy, sx, ey, ex;
    e.selRange(sy, sx, ey, ex);
    if (sy == ey) {
        e.lines[sy].erase(sx, ex - sx);
    } else {
        e.lines[sy] = e.lines[sy].substr(0, sx) + e.lines[ey].substr(ex);
        e.lines.erase(e.lines.begin() + sy + 1, e.lines.begin() + ey + 1);
    }
    if (e.lines.empty()) e.lines.push_back("");
    e.cy = sy; e.cx = sx;
    e.clearSel();
    e.dirty = true;
    clampCursor(e);
    return true;
}
// Delete from the cursor back to the start of the previous word -- what
// Option+Delete sends (ESC DEL) and what ^W means everywhere else.
void deleteWordBack(Editor& e) {
    if (e.cx == 0) { backspace(e); return; }
    std::string& l = e.lines[e.cy];
    int s = e.cx;
    while (s > 0 && (l[s - 1] == ' ' || l[s - 1] == '\t')) s--;
    while (s > 0 && isWordChar(l[s - 1])) s--;
    if (s == e.cx) s--;              // punctuation run: take at least one char
    l.erase(s, e.cx - s);
    e.cx = s; e.dirty = true;
}
// Delete from the cursor back to the first non-blank (then to column 0) --
// Cmd+Delete's job in a macOS text field, ^U's job in a terminal.
void deleteToLineStart(Editor& e) {
    if (e.cx == 0) { backspace(e); return; }
    std::string& l = e.lines[e.cy];
    int indent = 0;
    while (indent < (int)l.size() && (l[indent] == ' ' || l[indent] == '\t')) indent++;
    int target = (e.cx > indent) ? indent : 0;
    l.erase(target, e.cx - target);
    e.cx = target; e.dirty = true;
}
void clampCursor(Editor& e) {
    if (e.cy < 0) e.cy = 0;
    if (e.cy >= (int)e.lines.size()) e.cy = (int)e.lines.size() - 1;
    if (e.cx < 0) e.cx = 0;
    if (e.cx > (int)e.lines[e.cy].size()) e.cx = (int)e.lines[e.cy].size();
}

// Insert text EXACTLY as given: no auto-indent, no auto-pairing, no completion.
// Running pasted code through the normal keystroke path re-indents every line on
// top of the indentation it already has (the staircase) and doubles every
// bracket the auto-pairer sees. Both paste routes go through here.
void insertLiteral(Editor& e, const std::string& text) {
    if (text.empty()) return;
    std::string tail = e.lines[e.cy].substr(e.cx);
    e.lines[e.cy].erase(e.cx);
    size_t start = 0;
    bool firstSeg = true;
    for (;;) {
        size_t nlp = text.find('\n', start);
        std::string seg = text.substr(start, nlp == std::string::npos ? std::string::npos
                                                                     : nlp - start);
        if (!seg.empty() && seg.back() == '\r') seg.pop_back();
        if (firstSeg) { e.lines[e.cy] += seg; e.cx = (int)e.lines[e.cy].size(); firstSeg = false; }
        else {
            e.cy++;
            e.lines.insert(e.lines.begin() + e.cy, seg);
            e.cx = (int)seg.size();
        }
        if (nlp == std::string::npos) break;
        start = nlp + 1;
    }
    e.lines[e.cy] += tail;
    e.dirty = true;
    clampCursor(e);
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

// One-letter kind tag shown at the left of each popup row, plus its colour.
const char* kindTag(pyi::Symbol::Kind k, const char*& color) {
    switch (k) {
        case pyi::Symbol::Func:   color = C_DEFNAME; return "\xC6\x92";  // ƒ
        case pyi::Symbol::Class:  color = C_DECOR;   return "C";
        case pyi::Symbol::Param:  color = C_CONST;   return "p";
        case pyi::Symbol::Import: color = C_BUILTIN; return "i";
        case pyi::Symbol::Module: color = C_BUILTIN; return "m";
        case pyi::Symbol::Attr:   color = C_STRING;  return ".";
        default:                  color = C_TEXT;    return "v";
    }
}

// Take the inline suggestion: replace the typed prefix with the full word. For
// a callable that means the parens come too, with the cursor parked between
// them ready for arguments -- `pri` + accept -> `print(|)`.
bool acceptGhost(Editor& e) {
    if (e.ghost.empty()) return false;
    std::string pfx = wordPrefix(e);
    replacePrefixWith(e, pfx, e.ghostFull);
    if (e.ghostCall) e.cx--;      // step back inside the ()
    e.ghost.clear(); e.ghostFull.clear();
    return true;
}

// Take only the NEXT WORD of the suggestion -- right-arrow / shift-Tab. For a
// plain identifier that is the whole thing; it earns its keep on multi-word
// suggestions from a model backend, where you often want the first token and
// not the rest of the guess.
bool acceptGhostWord(Editor& e) {
    if (e.ghost.empty()) return false;
    // Take the leading run of word characters; if the suggestion starts with
    // punctuation instead (`()`, `.foo`), take that run.
    size_t n = 0;
    if (isWordChar(e.ghost[0])) { while (n < e.ghost.size() && isWordChar(e.ghost[n])) n++; }
    else { while (n < e.ghost.size() && !isWordChar(e.ghost[n]) && e.ghost[n] != ' ') n++; }
    while (n < e.ghost.size() && e.ghost[n] == ' ') n++;   // trailing space rides along
    if (n == 0) n = 1;
    if (n >= e.ghost.size()) return acceptGhost(e);        // that was all of it

    std::string chunk = e.ghost.substr(0, n);
    e.lines[e.cy].insert(e.cx, chunk);
    e.cx += (int)chunk.size();
    e.dirty = true;
    e.ghost.erase(0, n);          // the rest stays on offer
    return true;
}

// Commit the highlighted row of the live popup, the way Tab/Enter do in a GUI
// IDE: replace the typed prefix with the candidate, add "()" for a callable and
// land the cursor inside, then close the popup (and the ghost, now redundant).
bool acceptCompletion(Editor& e) {
    if (!e.complActive || e.complCands.empty()) return false;
    if (e.complSel < 0 || e.complSel >= (int)e.complCands.size()) e.complSel = 0;
    const pyi::Completion& c = e.complCands[e.complSel];
    bool callable = (c.kind == pyi::Symbol::Func || c.kind == pyi::Symbol::Class);
    replacePrefixWith(e, e.complPrefix, c.text + (callable ? "()" : ""));
    if (callable) e.cx--;                       // step back inside the ()
    e.complActive = false; e.complCands.clear();
    e.ghost.clear(); e.ghostFull.clear();
    return true;
}

std::string completionMenu(Editor& e, const std::string& prefix,
                           const std::vector<pyi::Completion>& cands) {
    (void)prefix;
    int sel = 0;
    const int maxShow = 9;

    // Column widths: the name column fits the longest name (capped), the detail
    // column takes whatever is left of the screen after it.
    size_t nameW = 0;
    bool anyDetail = false;
    for (const auto& c : cands) {
        nameW = std::max(nameW, c.text.size());
        if (!c.detail.empty()) anyDetail = true;
    }
    nameW = std::min(nameW, (size_t)28);
    size_t detW = 0;
    if (anyDetail) for (const auto& c : cands) detW = std::max(detW, c.detail.size());
    detW = std::min(detW, (size_t)40);

    for (;;) {
        redraw(e);   // draw the editor (and arky's chrome) underneath first
        int gw = e.gutterW();
        int scy = e.cy - e.rowOff + 1;
        int scx = (e.cx - e.colOff) + gw + 1;

        int show = std::min((int)cands.size(), maxShow);
        // Width: "▏ tag name  detail ".
        int width = 2 + 2 + (int)nameW + (detW ? 2 + (int)detW : 0) + 1;
        if (width > e.cols - 2) width = e.cols - 2;
        // Anchor the popup under the START of the word being completed, and
        // pull it left if it would hang off the right edge.
        int px = scx - (int)prefix.size();
        if (px + width > e.cols) px = e.cols - width + 1;
        if (px < 1) px = 1;
        // Prefer below the cursor; flip above when there isn't room.
        bool above = (scy + show > e.textRows());
        int py0 = above ? scy - show : scy + 1;
        if (py0 < 1) { py0 = 1; show = std::min(show, e.textRows()); }

        int first = 0;
        if (sel >= show) first = sel - show + 1;

        std::string buf;
        for (int i = 0; i < show; i++) {
            int idx = first + i;
            if (idx >= (int)cands.size()) break;
            int py = py0 + i;
            if (py < 1 || py > e.textRows()) continue;
            char pos[32]; snprintf(pos, sizeof(pos), "\x1b[%d;%dH", py, px);
            buf += pos;

            bool on = (idx == sel);
            buf += on ? C_POPSEL : C_POPBG;

            const char* kc = nullptr;
            const char* tag = kindTag(cands[idx].kind, kc);
            std::string row = " ";
            row += tag;
            row += " ";
            std::string nm = cands[idx].text;
            if (nm.size() > nameW) nm = nm.substr(0, nameW - 1) + "\xE2\x80\xA6";
            row += nm;
            if ((int)nm.size() < (int)nameW) row += std::string(nameW - nm.size(), ' ');
            buf += row;

            if (detW) {
                std::string d = cands[idx].detail;
                if (d.size() > detW) d = d.substr(0, detW - 1) + "\xE2\x80\xA6";
                buf += on ? C_POPDETS : C_POPDET;
                buf += "  ";
                buf += d;
                if (d.size() < detW) buf += std::string(detW - d.size(), ' ');
            }
            buf += on ? C_POPSEL : C_POPBG;
            buf += " ";
            buf += C_RESET;
        }
        // Put the terminal cursor back where the user is typing.
        char home[32]; snprintf(home, sizeof(home), "\x1b[%d;%dH", scy, scx);
        buf += home;
        fwrite(buf.data(), 1, buf.size(), stdout); fflush(stdout);

        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) return "";
        if (c == '\t')                 { sel = (sel + 1) % (int)cands.size(); continue; }
        if (c == '\r' || c == '\n')    return cands[sel].text;
        if (c == 27) {
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

// Non-modal twin of completionMenu's drawing: paints e.complCands under the
// cursor from the current selection, then homes the terminal cursor back to the
// typing position. Same geometry and colours as the modal, so the live popup and
// the Tab-invoked one look identical -- only the input handling differs (the live
// one is driven from the main loop, so typing keeps flowing).
void drawCompletionPopup(Editor& e) {
    const auto& cands = e.complCands;
    if (cands.empty()) return;
    const int maxShow = 9;

    size_t nameW = 0;
    bool anyDetail = false;
    for (const auto& c : cands) {
        nameW = std::max(nameW, c.text.size());
        if (!c.detail.empty()) anyDetail = true;
    }
    nameW = std::min(nameW, (size_t)28);
    size_t detW = 0;
    if (anyDetail) for (const auto& c : cands) detW = std::max(detW, c.detail.size());
    detW = std::min(detW, (size_t)40);

    int gw = e.gutterW();
    int scy = e.cy - e.rowOff + 1;
    int scx = (e.cx - e.colOff) + gw + 1;

    int show = std::min((int)cands.size(), maxShow);
    int width = 2 + 2 + (int)nameW + (detW ? 2 + (int)detW : 0) + 1;
    if (width > e.cols - 2) width = e.cols - 2;
    int px = scx - (int)e.complPrefix.size();
    if (px + width > e.cols) px = e.cols - width + 1;
    if (px < 1) px = 1;
    bool above = (scy + show > e.textRows());
    int py0 = above ? scy - show : scy + 1;
    if (py0 < 1) { py0 = 1; show = std::min(show, e.textRows()); }

    // Scroll the window so the selected row is always visible.
    int first = 0;
    if (e.complSel >= show) first = e.complSel - show + 1;

    std::string buf;
    for (int i = 0; i < show; i++) {
        int idx = first + i;
        if (idx >= (int)cands.size()) break;
        int py = py0 + i;
        if (py < 1 || py > e.textRows()) continue;
        char pos[32]; snprintf(pos, sizeof(pos), "\x1b[%d;%dH", py, px);
        buf += pos;

        bool on = (idx == e.complSel);
        buf += on ? C_POPSEL : C_POPBG;

        const char* kc = nullptr;
        const char* tag = kindTag(cands[idx].kind, kc);
        std::string row = " ";
        row += tag;
        row += " ";
        std::string nm = cands[idx].text;
        if (nm.size() > nameW) nm = nm.substr(0, nameW - 1) + "\xE2\x80\xA6";
        row += nm;
        if ((int)nm.size() < (int)nameW) row += std::string(nameW - nm.size(), ' ');
        buf += row;

        if (detW) {
            std::string d = cands[idx].detail;
            if (d.size() > detW) d = d.substr(0, detW - 1) + "\xE2\x80\xA6";
            buf += on ? C_POPDETS : C_POPDET;
            buf += "  ";
            buf += d;
            if (d.size() < detW) buf += std::string(detW - d.size(), ' ');
        }
        buf += on ? C_POPSEL : C_POPBG;
        buf += " ";
        buf += C_RESET;
    }
    // Cursor back where the user is typing.
    char home[32]; snprintf(home, sizeof(home), "\x1b[%d;%dH", scy, scx);
    buf += home;
    fwrite(buf.data(), 1, buf.size(), stdout); fflush(stdout);
}

// ═════════════════════════════════════════════════════════════════════════════
//  arky -- the standalone IDE
// ═════════════════════════════════════════════════════════════════════════════
// ark-py's engine (analyzer, diagnostics, completion, ghost text, run/build,
// arkpy.config, model port) wearing the shell the vendored editor had: a menu
// bar, tabs, a file explorer, a command palette and mouse support. The editor
// itself is unchanged -- arky adds chrome around it and a buffer list under it,
// so every fix that lands in ark-py lands here too.

constexpr const char* C_MENUBAR = "\x1b[48;2;36;40;59m\x1b[38;2;169;177;214m";
constexpr const char* C_TABBAR  = "\x1b[48;2;26;27;38m\x1b[38;2;86;95;137m";
constexpr const char* C_TABON   = "\x1b[48;2;45;63;118m\x1b[38;2;192;202;245m";
constexpr const char* C_SIDEBG  = "\x1b[48;2;31;35;53m\x1b[38;2;169;177;214m";
constexpr const char* C_SIDEDIR = "\x1b[48;2;31;35;53m\x1b[38;2;122;162;247m";
constexpr const char* C_SIDESEL = "\x1b[48;2;45;63;118m\x1b[38;2;255;255;255m";
constexpr const char* C_SIDEHDR = "\x1b[48;2;31;35;53m\x1b[38;2;86;95;137m";

struct DirEnt {
    std::string name;
    bool isDir = false;
};

struct App {
    std::vector<Editor> tabs;
    int active = 0;
    bool sidebar = true;
    int  sidebarW = 26;
    std::string root;                 // explorer directory
    std::vector<DirEnt> entries;
    int expSel = 0, expOff = 0;
    bool expFocus = false;            // arrow keys drive the explorer, not the text
    bool mouse = true;
    // Resolved chrome for this session (defaults + arkpy.config). Lives on the
    // App, not on an Editor: it describes the WINDOW, and every tab must agree.
    bool menu = true;
    bool tabs_ = true;
    std::string appName = "arky";
    std::string pendingStatus;

    Editor& cur() { return tabs[active]; }
};

// Directory listing for the explorer: directories first, then files, both
// alphabetical, dotfiles hidden. `..` is always offered so you can walk up.
void readDir(App& app) {
    app.entries.clear();
    app.entries.push_back({"..", true});
    DIR* d = opendir(app.root.c_str());
    if (!d) return;
    std::vector<DirEnt> dirs, files;
    while (dirent* de = readdir(d)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        if (!n.empty() && n[0] == '.') continue;
        bool isdir = false;
        if (de->d_type == DT_DIR) isdir = true;
        else if (de->d_type == DT_UNKNOWN) {          // some filesystems don't fill d_type
            struct stat st{};
            if (stat((app.root + "/" + n).c_str(), &st) == 0) isdir = S_ISDIR(st.st_mode);
        }
        (isdir ? dirs : files).push_back({n, isdir});
    }
    closedir(d);
    auto byName = [](const DirEnt& a, const DirEnt& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (auto& e : dirs)  app.entries.push_back(e);
    for (auto& e : files) app.entries.push_back(e);
    if (app.expSel >= (int)app.entries.size()) app.expSel = 0;
}

// Normalize `dir/..` without touching the filesystem.
std::string parentOf(const std::string& p) {
    if (p == "/" || p.empty()) return "/";
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

// Push the window's chrome onto whichever Editor is being drawn. Cheap and
// idempotent by design, so it can run on EVERY paint path -- a tab opened from
// the explorer starts life with sidebarW 0, and any repaint before the next trip
// through the main loop laid its text straight over the sidebar.
void applyChrome(App& app) {
    Editor& e = app.cur();
    e.appName  = app.appName.c_str();
    e.showMenu = app.menu;
    e.showTabs = app.tabs_;
    e.sidebarW = app.sidebar ? app.sidebarW : 0;
    layout(e);
}

void syncEditorChrome(App& app) {
    queryTermSize(app.cur());     // the DSR round trip: once per loop, not per paint
    applyChrome(app);
}

// Open `path` in a tab -- reusing the tab that already has it, if any.
void openInTab(App& app, const std::string& path) {
    std::string full = expandUserPath(path);
    for (size_t i = 0; i < app.tabs.size(); i++) {
        if (app.tabs[i].filename == full) { app.active = (int)i; return; }
    }
    Editor e;
    e.rows = app.cur().rows;
    e.cols = app.cur().cols;
    e.ghostOn = app.cur().ghostOn;
    e.ghostMinPrefix = app.cur().ghostMinPrefix;
    e.pythonBin = app.cur().pythonBin;
    e.runArgs = app.cur().runArgs;
    // Toolchain settings are per-SESSION, not per-buffer: a new tab inherits
    // them so opening a second file doesn't silently fall back to `cc`.
    e.ccBin = app.cur().ccBin;
    e.cxxBin = app.cur().cxxBin;
    e.cflags = app.cur().cflags;
    e.useCMake = app.cur().useCMake;
    e.clangdOn = app.cur().clangdOn;
    loadFile(e, full);
    e.dirty = false;
    ensureCompileCommands(e);
    ensureClangd(e);
    app.tabs.push_back(std::move(e));
    app.active = (int)app.tabs.size() - 1;
}

void closeTab(App& app) {
    if (app.tabs.size() <= 1) { app.cur().status = "last tab -- ^Q to quit"; return; }
    app.tabs.erase(app.tabs.begin() + app.active);
    if (app.active >= (int)app.tabs.size()) app.active = (int)app.tabs.size() - 1;
}

// Screen row the tab strip lives on, and the column span of each tab, so the
// mouse handler and the renderer agree on exactly one layout.
struct TabSpan { int x0, x1; };
std::vector<TabSpan> tabSpans(App& app) {
    std::vector<TabSpan> out;
    int x = 1;
    for (auto& t : app.tabs) {
        std::string base = t.filename.empty() ? "untitled" : t.filename;
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        int w = (int)base.size() + (t.dirty ? 4 : 3);
        out.push_back({x, x + w - 1});
        x += w;
    }
    return out;
}

void drawChrome(App& app) {
    applyChrome(app);             // never paint with another tab's geometry
    Editor& e = app.cur();
    std::string buf;
    buf += "\x1b[?25l";

    // ── menu bar ──
    buf += "\x1b[1;1H";
    buf += C_MENUBAR;
    std::string menu = "  arky   ^P palette   ^R run   ^B build   ^O open   ^E explorer   ^T tab   ^Q quit";
    if ((int)menu.size() > e.cols) menu = menu.substr(0, e.cols);
    buf += menu;
    if ((int)menu.size() < e.cols) buf.append(e.cols - menu.size(), ' ');
    buf += C_RESET;

    // ── tab strip ──
    buf += "\x1b[2;1H";
    buf += C_TABBAR;
    int used = 0;
    auto spans = tabSpans(app);
    for (size_t i = 0; i < app.tabs.size(); i++) {
        std::string base = app.tabs[i].filename.empty() ? "untitled" : app.tabs[i].filename;
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        std::string label = " " + base + (app.tabs[i].dirty ? " \xE2\x97\x8F " : " ");
        if (used + (int)label.size() > e.cols) break;
        buf += ((int)i == app.active) ? C_TABON : C_TABBAR;
        buf += label;
        used += (int)label.size();
    }
    buf += C_TABBAR;
    if (used < e.cols) buf.append(e.cols - used, ' ');
    buf += C_RESET;
    (void)spans;

    // ── explorer ──
    if (app.sidebar && e.sidebarW > 0) {
        // e.sidebarW, not app.sidebarW: layout() may have shrunk it to fit, and
        // painting at the unclamped width is what put the sidebar over the text.
        const int sw = e.sidebarW;
        int x0 = e.cols - sw + 1;
        int top = e.viewY;
        int h = e.viewH;
        // Column width, not byte length: a name with any non-ASCII in it made
        // cell.size() overcount, so the padding below came up short and the row
        // bled past the sidebar into the text area.
        auto dispW = [](const std::string& t) {
            int w = 0;
            for (unsigned char ch : t) if ((ch & 0xC0) != 0x80) w++;
            return w;
        };
        // Truncate on a character boundary, never mid-UTF-8 (a split sequence
        // renders as garbage and can desync the whole line).
        auto clipW = [&](const std::string& t, int maxw) {
            if (dispW(t) <= maxw) return t;
            int w = 0; size_t i = 0;
            while (i < t.size() && w < maxw - 1) {
                i++;
                while (i < t.size() && ((unsigned char)t[i] & 0xC0) == 0x80) i++;
                w++;
            }
            return t.substr(0, i) + "\xE2\x80\xA6";
        };
        std::string hdr = " EXPLORER";
        // Just the folder's own name -- the full path is long, always truncated,
        // and the tail you'd actually want is the part that got cut.
        std::string dirLabel = app.root;
        size_t sl = dirLabel.find_last_of('/');
        if (sl != std::string::npos && sl + 1 < dirLabel.size()) dirLabel = dirLabel.substr(sl + 1);
        dirLabel = clipW(dirLabel, sw - 2);
        for (int i = 0; i < h; i++) {
            char pos[32];
            snprintf(pos, sizeof(pos), "\x1b[%d;%dH", top + i, x0);
            buf += pos;
            std::string cell;
            const char* color = C_SIDEBG;
            if (i == 0) { cell = hdr; color = C_SIDEHDR; }
            else if (i == 1) { cell = " " + dirLabel; color = C_SIDEHDR; }
            else {
                int idx = app.expOff + i - 2;
                if (idx >= 0 && idx < (int)app.entries.size()) {
                    const DirEnt& de = app.entries[idx];
                    // Plain ASCII markers on purpose. The previous glyph was a
                    // Nerd Font private-use codepoint, which renders as a blank
                    // or a tofu box on any terminal without a patched font --
                    // i.e. on most of them.
                    std::string nm = de.name + (de.isDir ? "/" : "");
                    cell = std::string(de.isDir ? " > " : "   ") +
                           clipW(nm, sw - 3);
                    color = (idx == app.expSel && app.expFocus) ? C_SIDESEL
                            : de.isDir ? C_SIDEDIR : C_SIDEBG;
                    if (idx == app.expSel && !app.expFocus) color = C_TABON;
                }
            }
            cell = clipW(cell, sw);
            int cw = dispW(cell);
            buf += color;
            buf += cell;
            if (cw < sw) buf.append(sw - cw, ' ');
            buf += C_RESET;
        }
    }
    fwrite(buf.data(), 1, buf.size(), stdout);
    // render() paints the text region + status bar and parks the cursor.
    render(app.cur());
}

App* g_appPtr = nullptr;
void chromeRedraw(Editor&) { if (g_appPtr) drawChrome(*g_appPtr); }

// ── Command palette ──────────────────────────────────────────────────────────
// Everything reachable by key is reachable here by name, so nothing is hidden
// behind a chord you have to already know.
struct Command { const char* label; const char* hint; };
const std::vector<Command>& paletteCommands() {
    static const std::vector<Command> c = {
        {"File: Open…",              "^O"},
        {"File: Save",               "^S"},
        {"File: Close Tab",          ""},
        {"File: Next Tab",           "^T"},
        {"Run: Run File",            "^R"},
        {"Run: Set Arguments…",      "^G"},
        {"Build: Build File",        "^B"},
        {"Build: Set Compile Command…", ""},
        {"View: Toggle Explorer",    ""},
        {"View: Focus Explorer",     "^E"},
        {"Go: Definition",           "^]"},
        {"Go: Hover Info",           "^K"},
        {"Edit: Select All",         "^A"},
        {"Help: Quit",               "^Q"},
    };
    return c;
}

// Returns the chosen command index, or -1.
int paletteMenu(App& app) {
    std::string filter;
    int sel = 0;
    for (;;) {
        std::vector<int> hits;
        for (size_t i = 0; i < paletteCommands().size(); i++) {
            std::string l = paletteCommands()[i].label;
            std::string lo, fo;
            for (char ch : l) lo += (char)tolower((unsigned char)ch);
            for (char ch : filter) fo += (char)tolower((unsigned char)ch);
            if (fo.empty() || lo.find(fo) != std::string::npos) hits.push_back((int)i);
        }
        if (sel >= (int)hits.size()) sel = hits.empty() ? 0 : (int)hits.size() - 1;

        drawChrome(app);
        Editor& e = app.cur();
        int w = 54;
        if (w > e.cols - 4) w = e.cols - 4;
        int x = (e.cols - w) / 2 + 1;
        int y = e.viewY + 1;
        int rowsShown = std::min((int)hits.size(), 10);

        std::string buf;
        char pos[32];
        snprintf(pos, sizeof(pos), "\x1b[%d;%dH", y, x);
        buf += pos;
        buf += C_POPBG;
        std::string prompt = " > " + filter;
        if ((int)prompt.size() > w) prompt = prompt.substr(0, w);
        buf += prompt;
        if ((int)prompt.size() < w) buf.append(w - prompt.size(), ' ');
        buf += C_RESET;

        for (int i = 0; i < rowsShown; i++) {
            snprintf(pos, sizeof(pos), "\x1b[%d;%dH", y + 1 + i, x);
            buf += pos;
            const Command& c = paletteCommands()[hits[i]];
            buf += (i == sel) ? C_POPSEL : C_POPBG;
            std::string row = std::string("  ") + c.label;
            std::string hint = c.hint;
            int padTo = w - (int)hint.size() - 2;
            if ((int)row.size() > padTo) row = row.substr(0, padTo);
            buf += row;
            if ((int)row.size() < padTo) buf.append(padTo - row.size(), ' ');
            buf += hint;
            buf += "  ";
            buf += C_RESET;
        }
        snprintf(pos, sizeof(pos), "\x1b[%d;%dH", y, x + (int)prompt.size());
        buf += pos;
        fwrite(buf.data(), 1, buf.size(), stdout);
        fflush(stdout);

        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) return -1;
        if (c == '\r' || c == '\n') return hits.empty() ? -1 : hits[sel];
        if (c == 127 || c == 8) { if (!filter.empty()) filter.pop_back(); continue; }
        if (c == 27) {
            char b1, b2;
            if (read(STDIN_FILENO, &b1, 1) == 1 && b1 == '[' &&
                read(STDIN_FILENO, &b2, 1) == 1) {
                if (b2 == 'A' && sel > 0) sel--;
                if (b2 == 'B' && sel + 1 < (int)hits.size()) sel++;
                continue;
            }
            return -1;
        }
        if ((unsigned char)c >= 32) { filter += c; sel = 0; }
    }
}

void runPaletteCommand(App& app, int idx, bool& running) {
    Editor& e = app.cur();
    switch (idx) {
        case 0: {                                            // Open…
            std::string p = statusPrompt(e, "open: ", app.root + "/");
            if (!p.empty()) openInTab(app, p);
            break;
        }
        case 1:                                              // Save
            if (e.filename.empty()) {
                std::string name = statusPrompt(e, "save as: ", "untitled.py");
                if (name.empty()) { e.status = "save cancelled"; break; }
                std::string prev = e.filename;
                e.filename = name;
                if (!saveFile(e)) e.filename = prev;
            } else saveFile(e);
            break;
        case 2: closeTab(app); break;
        case 3: if (app.tabs.size() > 1) app.active = (app.active + 1) % (int)app.tabs.size(); break;
        case 4: runBuffer(e); break;
        case 5: {
            std::string a = statusPrompt(e, "run args (sys.argv[1:]): ", e.runArgs);
            e.runArgs = a;
            e.status = a.empty() ? "run args cleared" : ("run args: " + a);
            break;
        }
        case 6: buildBuffer(e); break;
        case 7: {                                            // compile command
            std::string cur = e.compileCmdOverride;
            if (cur.empty()) { if (const char* c = getenv("ARK_PY_NATIVE_CMD")) cur = c; }
            if (cur.empty()) {
                const char* cc = getenv("ARK_PY_NATIVE_CC");
                const char* fl = getenv("ARK_PY_NATIVE_FLAGS");
                cur = std::string(cc ? cc : "cc") + " " + (fl ? std::string(fl) + " " : "") + "{src} -o {out}";
            }
            std::string edited = statusPrompt(e, "compile cmd ({src}{out}{dir}{stem}): ", cur);
            if (!edited.empty()) { e.compileCmdOverride = edited; e.status = "compile command set — ^B to build"; }
            break;
        }
        case 8: app.sidebar = !app.sidebar; if (!app.sidebar) app.expFocus = false; break;
        case 9: app.sidebar = true; app.expFocus = true; break;
        case 10: {
            auto def = pyi::definition(e.lines, e.cy, e.cx, e.analysis);
            if (def.first >= 0) { e.cy = def.first; e.cx = def.second; e.status = "→ definition"; }
            else e.status = "definition not found in this buffer";
            break;
        }
        case 11: {
            std::string h = pyi::hover(e.lines, e.cy, e.cx, e.analysis);
            e.status = h.empty() ? "(no info for symbol under cursor)" : h;
            break;
        }
        case 12:
            e.ay = 0; e.ax = 0;
            e.cy = (int)e.lines.size() - 1;
            e.cx = (int)e.lines[e.cy].size();
            break;
        case 13: running = false; break;
        default: break;
    }
}

// ── Mouse ────────────────────────────────────────────────────────────────────
// An SGR mouse report has already had its "ESC [ <" consumed by the caller.
// Routes a click to whichever region of the frame it landed in.
void handleMouse(App& app, int btn, int mx, int my, bool press) {
    Editor& e = app.cur();
    // Drag reports arrive as the button code + 32. A drag extends the selection
    // that the press started; the release itself carries no new information.
    bool drag = (btn & 32) != 0 && (btn & 64) == 0;
    if (drag) {
        int gw = e.gutterW();
        if (my < e.viewY || my >= e.viewY + e.viewH || mx < e.viewX + gw) return;
        if (app.sidebar && e.sidebarW > 0 && mx > e.cols - e.sidebarW) return;
        int line = e.rowOff + (my - e.viewY);
        if (line < 0) line = 0;
        if (line >= (int)e.lines.size()) line = (int)e.lines.size() - 1;
        int col = e.colOff + (mx - e.viewX - gw);
        if (col < 0) col = 0;
        if (col > (int)e.lines[line].size()) col = (int)e.lines[line].size();
        if (!e.hasSel()) e.anchorSel();      // anchor at where the press landed
        e.cy = line;
        e.cx = col;
        return;
    }
    if (!press) return;

    // Wheel: buttons 64 (up) / 65 (down) -- scroll the text, three lines a notch.
    if (btn == 64 || btn == 65) {
        e.rowOff += (btn == 65) ? 3 : -3;
        if (e.rowOff < 0) e.rowOff = 0;
        if (e.rowOff > (int)e.lines.size() - 1) e.rowOff = (int)e.lines.size() - 1;
        return;
    }
    if (btn != 0) return;   // left button only

    // Tab strip.
    if (my == 2) {
        auto spans = tabSpans(app);
        for (size_t i = 0; i < spans.size(); i++)
            if (mx >= spans[i].x0 && mx <= spans[i].x1) { app.active = (int)i; app.expFocus = false; return; }
        return;
    }
    // Explorer.
    if (app.sidebar && e.sidebarW > 0 && mx > e.cols - e.sidebarW) {  // the settled width
        int idx = app.expOff + (my - e.viewY) - 2;
        if (idx < 0 || idx >= (int)app.entries.size()) return;
        app.expFocus = true;
        // A click on the already-selected row opens it; the first click selects.
        if (idx == app.expSel) {
            const DirEnt de = app.entries[idx];
            if (de.isDir) {
                app.root = (de.name == "..") ? parentOf(app.root) : app.root + "/" + de.name;
                app.expSel = 0; app.expOff = 0;
                readDir(app);
            } else {
                openInTab(app, app.root + "/" + de.name);
                app.expFocus = false;
            }
        } else app.expSel = idx;
        return;
    }
    // Text area: place the cursor where the click landed.
    if (my >= e.viewY && my < e.viewY + e.viewH && mx >= e.viewX) {
        app.expFocus = false;
        int gw = e.gutterW();
        int line = e.rowOff + (my - e.viewY);
        if (line < 0) line = 0;
        if (line >= (int)e.lines.size()) line = (int)e.lines.size() - 1;
        e.cy = line;
        int col = e.colOff + (mx - e.viewX - gw);
        if (col < 0) col = 0;
        if (col > (int)e.lines[line].size()) col = (int)e.lines[line].size();
        e.cx = col;
        e.clearSel();
    }
}

// Bracketed paste. Without it a Cmd+V is indistinguishable from someone typing
// very fast: every newline runs the auto-indenter and every '(' runs the
// auto-pairer, so pasted code arrives as a staircase full of doubled brackets.
// With it the terminal wraps the payload in ESC[200~ ... ESC[201~ and we can
// insert it verbatim.
void enableBracketedPaste(bool on) {
    fputs(on ? "\x1b[?2004h" : "\x1b[?2004l", stdout);
    fflush(stdout);
}

void enableMouse(bool on) {
    // 1000 = press/release, 1006 = SGR coordinates (works past column 223),
    // 1015 = urxvt fallback for terminals that don't do SGR.
    // 1002 = report DRAG while a button is held. Without it the editor sees a
    // press and a release and nothing in between, so dragging could not select --
    // and since turning mouse reporting on also takes away the terminal's own
    // drag-select, that left no way at all to select with the mouse and copy.
    fputs(on ? "\x1b[?1000h\x1b[?1002h\x1b[?1006h"
             : "\x1b[?1000l\x1b[?1002l\x1b[?1006l\x1b[?1015l", stdout);
    fflush(stdout);
    g_mouseWanted = on;
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
    bool ideaPreset = false;              // `arky -i`: every IDE affordance on
    for (size_t i = 1; i < argv.size(); i++) {
        const std::string& a = argv[i];
        if (a == "-o" && i + 1 < argv.size()) inputFile = expandUserPath(argv[++i]);
        else if (a == "-oc" && i + 1 < argv.size()) { e.compileMode = Editor::Compile::Bytecode; e.compileTarget = expandUserPath(argv[++i]); }
        else if (a == "-ocb" && i + 1 < argv.size()) { e.compileMode = Editor::Compile::Native; e.compileTarget = expandUserPath(argv[++i]); }
        else if (a == "-i" || a == "--idea") ideaPreset = true;   // IntelliJ-style "the works"
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

    // Library indexing resolves imports against the interpreter's real sys.path
    // (discovered lazily, only if the buffer actually imports something) plus
    // the directory of the file being edited, so a local `import helpers` works.
    loadArkPyConfig(e);                                  // may set ARK_PY_* below
    if (const char* pv2 = getenv("ARK_PY_BIN")) e.pythonBin = pv2;
    pylib::setInterpreter(e.pythonBin);
    pymodel::start();   // no-op unless a port is configured

    queryTermSize(e);
    if (!inputFile.empty()) loadFile(e, inputFile);
    else {
        std::string hint = "new buffer — ^O open  ^S save  ^R run  ^B build  ^Q quit";
        if (e.compileMode == Editor::Compile::Native)   hint += "   [→ native " + e.compileTarget + "]";
        else if (e.compileMode == Editor::Compile::Bytecode) hint += "   [→ .pyc " + e.compileTarget + "]";
        e.status = hint;
    }

    // ── arky: the buffer list + chrome wrapped around that editor ──
    // Same code, two faces. Invoked as `ark-py` it stays the bare editor people
    // already have muscle memory for; invoked as `arky` it comes up as the IDE,
    // with the menu bar, tab strip, explorer and mouse on. Everything below is a
    // default -- arkpy.config still has the last word.
    const bool asIde = !argv.empty() && argv[0] == "arky";
    App app;
    if (asIde) {
        app.tabs.reserve(4);
    } else {
        // Plain ark-py: the bare editor, no chrome at all.
        app.appName = "ark-py";
        app.menu    = false;
        app.tabs_   = false;
        app.sidebar = false;
    }
    app.mouse = asIde;
    // An explicit setting in arkpy.config wins over both defaults, in either
    // direction: `explorer = off` strips the sidebar out of arky, and
    // `explorer = on` puts it into plain ark-py.
    if (e.cfgMenu     >= 0) app.menu    = e.cfgMenu != 0;
    if (e.cfgTabs     >= 0) app.tabs_   = e.cfgTabs != 0;
    if (e.cfgExplorer >= 0) app.sidebar = e.cfgExplorer != 0;
    if (e.cfgMouse    >= 0) app.mouse   = e.cfgMouse != 0;
    if (e.cfgSidebarW >  0) app.sidebarW = e.cfgSidebarW;
    // `arky -i` (IntelliJ preset): turn the editor into the fullest IDE it has --
    // menu, tabs, explorer, mouse, centered dialogs, indent guides -- regardless
    // of the per-feature defaults. An explicit config value the user set still
    // shows through where the preset doesn't force it.
    if (ideaPreset) {
        app.menu = true; app.tabs_ = true; app.sidebar = true; app.mouse = true;
        if (app.sidebarW < 28) app.sidebarW = 28;
        for (Editor& t : app.tabs) { t.dialogOn = true; t.guidesOn = true; }
    }
    app.tabs.push_back(std::move(e));
    {
        char cwdbuf[4096];
        app.root = getcwd(cwdbuf, sizeof(cwdbuf)) ? cwdbuf : ".";
        // With a file open, root the explorer at its PROJECT (the .idea/ dir when
        // launched inside IntelliJ, else a git/build marker) rather than the file's
        // own deep source directory -- so the tree shows the whole project, the way
        // an IDE does. Fall back to the file's dir if it isn't inside a project.
        if (!app.tabs[0].filename.empty()) {
            std::string proj = projectRootFor(app.tabs[0].filename);
            app.root = !proj.empty() ? proj : parentOf(app.tabs[0].filename);
        }
        readDir(app);
    }

    ensureCompileCommands(app.tabs[0]);
    ensureClangd(app.tabs[0]);

    g_appPtr = &app;
    g_redraw = chromeRedraw;

    enterRaw(app.tabs[0]);
    altScreen(true);
    enableBracketedPaste(true);
    if (app.mouse) enableMouse(true);

    std::string clip;   // ^X/^C/^V scratch (in-editor, not the system pasteboard)
    bool running = true;
    while (running) {
        Editor& e = app.cur();   // rebound each pass: tabs can be added/closed
        queryTermSize(e);
        syncEditorChrome(app);
        clampCursor(e);
        // Re-run ark's own Python analyzer for live diagnostics + completion.
        // Cheap enough (a linear pass) to do every loop iteration.
        // Only for Python. Running the Python analyzer over a C buffer produced
        // a screenful of confident nonsense -- every line flagged by a parser
        // that was reading a language the file isn't written in.
        if (looksLikeC(e)) {
            e.analysis = pyi::Analysis{};
            // clangd is the analyzer for C/C++. Its diagnostics are poured into
            // the same pyi::Analysis the renderer already draws, so the error
            // bar, the gutter and the counts all work unchanged.
            if (clangdc::running()) {
                std::vector<clangdc::Diag> cds;
                if (clangdc::diagnostics(e.filename, cds)) {
                    for (const clangdc::Diag& d : cds) {
                        pyi::Diag pd;
                        pd.line = d.line;
                        pd.col  = d.col;
                        pd.endCol = d.col + 1;
                        pd.sev  = (d.severity == 1) ? pyi::Diag::Error : pyi::Diag::Warning;
                        pd.msg  = d.msg;
                        e.analysis.diags.push_back(pd);
                    }
                }
            }
            // Keep the server's copy in step with the buffer, on a debounce --
            // a didChange per keystroke would have clangd reparsing the
            // translation unit faster than it can finish.
            static std::string lastSent;
            static int sinceEdit = 0;
            if (clangdc::running() && !e.filename.empty()) {
                std::string whole;
                for (size_t li = 0; li < e.lines.size(); li++) {
                    whole += e.lines[li];
                    if (li + 1 < e.lines.size()) whole += '\n';
                }
                if (whole != lastSent && ++sinceEdit >= 2) {
                    clangdc::change(e.filename, whole);
                    lastSent = whole;
                    sinceEdit = 0;
                }
            }
        }
        else e.analysis = pyi::analyze(e.lines);

        // ── Inline suggestion ──
        // The single best completion for the word being typed, shown as dim
        // text after the cursor. Only when the cursor is at the end of a word
        // (nothing to the right to trample) and the prefix is long enough that
        // the guess is worth trusting. A callable suggests its parens too, so
        // `pri` -> `print()`.
        e.ghost.clear();
        e.ghostFull.clear();
        if (!e.hasSel() && e.ghostOn) {
            const std::string& gl = e.lines[e.cy];
            // Only at the true END of the line. The ghost is drawn after the
            // line's text, so suggesting mid-line put the text in the wrong
            // place entirely -- `print("Ha ha| ha")` drew the suggestion after
            // the closing paren.
            // End of line, OR nothing but closing delimiters to the right --
            // which is exactly the `print(my|)` case. Requiring the true end of
            // the line meant completion went dead the moment the editor had
            // auto-inserted a ')', i.e. inside every call you ever type.
            bool atEnd = (e.cx >= (int)gl.size());
            if (!atEnd) {
                atEnd = true;
                for (int i = e.cx; i < (int)gl.size(); i++) {
                    char rc = gl[i];
                    if (rc == ')' || rc == ']' || rc == '}' || rc == ',' ||
                        rc == ':' || rc == ' ' || rc == '\t') continue;
                    atEnd = false;
                    break;
                }
            }
            std::string pfx = wordPrefix(e);
            bool afterDot = (e.cx > 0 && gl[e.cx - 1] == '.');
            // ...and never inside a string or comment. `"Ha ha ha"` is prose,
            // not code: completing `ha` to `hasattr()` there is pure noise.
            // Local symbol completion needs a partial word (or a `.`); the model,
            // by contrast, predicts the NEXT token from surrounding code, so it
            // can and should fire even with an empty prefix -- right after `<<`,
            // `=`, `(`, `return `, etc., which is exactly where you most want a
            // whole-expression suggestion.
            bool wantLocal = ((int)pfx.size() >= e.ghostMinPrefix) ||
                             (afterDot && !pfx.empty());
            bool wantModel = pymodel::enabled();
            if (atEnd && inCodeContext(gl, e.cx) && (wantLocal || wantModel)) {
                if (wantLocal) {
                    auto cands = looksLikeC(e) ? completeC(e.lines, e.cy, e.cx)
                                                 : pyi::complete(e.lines, e.cy, e.cx, e.analysis);
                    for (const auto& c : cands) {
                        if (c.text.size() <= pfx.size()) continue;
                        if (c.text.compare(0, pfx.size(), pfx) != 0) continue;
                        bool callable = (c.kind == pyi::Symbol::Func || c.kind == pyi::Symbol::Class);
                        e.ghostFull = c.text + (callable ? "()" : "");
                        e.ghost = e.ghostFull.substr(pfx.size());
                        e.ghostCall = callable;
                        break;
                    }
                }

            // Nothing local? Fall back to the model backend, if one is configured.
            // ask() is fire-and-forget on a worker thread; take() collects a reply
            // on some later keystroke.
            if (e.ghost.empty() && wantModel) {
                // Build the bounded context first: its TAIL is also the dedup and
                // reply-matching key. Keying on the context (not just the word)
                // means empty-prefix positions each ask exactly once, and a reply
                // meant for one cursor spot can never surface at a different one.
                std::string before, after;
                for (int y = std::max(0, e.cy - 40); y <= e.cy; y++) {
                    if (y == e.cy) before += e.lines[y].substr(0, e.cx);
                    else { before += e.lines[y]; before += "\n"; }
                }
                if (before.size() > 4096) before = before.substr(before.size() - 4096);
                for (int y = e.cy; y < (int)e.lines.size() && y < e.cy + 10; y++) {
                    if (y == e.cy) after += e.lines[y].substr(e.cx);
                    else { after += "\n"; after += e.lines[y]; }
                }
                if (after.size() > 1024) after.resize(1024);
                std::string key = before.size() > 64 ? before.substr(before.size() - 64)
                                                     : before;

                std::string suffix;
                if (pymodel::take(key, suffix) && !suffix.empty()) {
                    e.modelPrefix = key;
                    e.modelSuffix = suffix;
                }
                if (key == e.modelPrefix && !e.modelSuffix.empty()) {
                    e.ghost = e.modelSuffix;
                    e.ghostFull = pfx + e.modelSuffix;
                    e.ghostCall = false;
                } else if (key != e.ghostAsked) {
                    e.ghostAsked = key;
                    pymodel::ask(key, e.filename, e.cy + 1, e.cx + 1, before, after);
                }
            }
            }
        }

        // ── Live completion popup ──
        // The full candidate list under the cursor, VS Code / IntelliJ style:
        // it appears as you type, filters live, and the arrow keys drive it. The
        // ghost (above) previews the top hit inline; this shows all of them so
        // you can pick a lower-ranked one without pressing Tab first.
        e.complCands.clear();
        if (e.complOn && !e.hasSel()) {
            const std::string& cl = e.lines[e.cy];
            std::string pfx = wordPrefix(e);
            bool afterDot = (e.cx > 0 && cl[e.cx - 1] == '.');
            // Same gate as the ghost -- in real code, at a completable spot --
            // but keyed off the popup's own trigger length so it can show a
            // couple of characters in, exactly like a graphical IDE.
            // Esc latches the popup shut, but only for the word it was showing:
            // the moment the prefix changes (you typed or deleted a char) the
            // latch releases and the popup is free to come back.
            if (e.complDismissed && pfx != e.complDismissAt) e.complDismissed = false;
            bool trigger = ((int)pfx.size() >= e.ghostMinPrefix) || (afterDot);
            if (trigger && !e.complDismissed && inCodeContext(cl, e.cx)) {
                auto cands = looksLikeC(e) ? completeC(e.lines, e.cy, e.cx)
                                           : pyi::complete(e.lines, e.cy, e.cx, e.analysis);
                for (auto& cand : cands) {
                    if (!pfx.empty() && cand.text.compare(0, pfx.size(), pfx) != 0) continue;
                    if (cand.text == pfx) continue;   // already fully typed: nothing to add
                    e.complCands.push_back(cand);
                    if ((int)e.complCands.size() >= 50) break;   // sane ceiling
                }
                e.complPrefix = pfx;
            }
        }
        e.complActive = !e.complCands.empty();
        if (e.complSel >= (int)e.complCands.size()) e.complSel = 0;

        drawChrome(app);
        if (e.complActive) drawCompletionPopup(e);

        char c;
        // While a model request is out, poll rather than block: the reply lands
        // on a worker thread, and a blocked read() would hold the suggestion
        // back until the user happened to press another key.
        if (pymodel::pending()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            timeval tv{0, 30 * 1000};
            int sel = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;    // timeout (or EINTR): recompute + repaint
        }
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
        if (uc == 15) {                                     // Ctrl-O  open a file
            if (e.dirty) {
                std::string ans = statusPrompt(e, "unsaved changes — save first? (y/N/c=cancel): ");
                if (ans == "c" || ans == "C") continue;
                if ((ans == "y" || ans == "Y") && !saveFile(e)) continue;
            }
            std::string path = statusPrompt(e, "open: ", e.filename);
            if (path.empty()) { e.status = "open cancelled"; continue; }
            std::string full = expandUserPath(path);
            // Report rather than silently starting an empty buffer on a typo'd
            // path -- but a genuinely new file is still fine to start.
            struct stat st{};
            bool exists = (stat(full.c_str(), &st) == 0);
            if (exists && S_ISDIR(st.st_mode)) { e.status = "not a file: " + full; continue; }
            if (exists && access(full.c_str(), R_OK) != 0) {
                e.status = "cannot read: " + full; continue;
            }
            loadFile(e, full);
            e.cx = e.cy = e.rowOff = e.colOff = 0;
            e.clearSel();
            e.dirty = false;
            e.status = exists ? ("opened " + full + "  (" + std::to_string(e.lines.size()) + " lines)")
                              : ("new file " + full);
            continue;
        }
        if (uc == 1) {                                      // Ctrl-A  select all
            e.ay = 0; e.ax = 0;
            e.cy = (int)e.lines.size() - 1;
            e.cx = (int)e.lines[e.cy].size();
            e.status = "selected all — Delete to clear, ^C to copy";
            continue;
        }
        if (uc == 24 || uc == 3) {                          // Ctrl-X cut / Ctrl-C copy
            // No selection copies the CURRENT LINE rather than refusing. A
            // refusal here is worse than it looks: the clipboard keeps whatever
            // was in it -- often the terminal's own screen-scrape, gutter numbers
            // and sidebar included -- and it looks like arky copied that.
            bool wholeLine = !e.hasSel();
            if (wholeLine) { e.ay = e.cy; e.ax = 0; e.cx = (int)e.lines[e.cy].size(); }
            int sy, sx, ey, ex; e.selRange(sy, sx, ey, ex);
            clip.clear();
            for (int y = sy; y <= ey; y++) {
                int a = (y == sy) ? sx : 0;
                int b = (y == ey) ? ex : (int)e.lines[y].size();
                clip += e.lines[y].substr(a, b - a);
                if (y != ey) clip += "\n";
            }
            if (wholeLine) clip += "\n";
            bool sys = systemCopy(clip);
            // Count LINES, not separators: the whole-line copy ends with a
            // newline, which would otherwise report one line too many.
            int nl = (int)std::count(clip.begin(), clip.end(), '\n') + (wholeLine ? 0 : 1);
            std::string where = sys ? "clipboard" : "buffer (no clipboard helper)";
            if (uc == 24) { deleteSelection(e); e.status = "cut " + std::to_string(nl) + " line(s) to " + where; }
            else          { e.clearSel(); e.status = "copied " + std::to_string(nl) + " line(s) to " + where; }
            continue;
        }
        if (uc == 22) {                                     // Ctrl-V  paste
            std::string sysClip;
            if (systemPaste(sysClip)) clip = sysClip;
            if (clip.empty()) { e.status = "clipboard empty"; continue; }
            deleteSelection(e);
            insertLiteral(e, clip);
            e.status = "pasted";
            continue;
        }
        if (uc == 21) { if (!deleteSelection(e)) deleteToLineStart(e); continue; }  // ^U
        if (uc == 23) { if (!deleteSelection(e)) deleteWordBack(e); continue; }     // ^W
        if (uc == 6) { if (!acceptGhost(e)) e.status = "no suggestion"; continue; }  // ^F accept
        if (uc == 14) {                                     // ^N  force the popup
            // Tab takes the inline suggestion, so this is how you get the LIST
            // when the top guess isn't the one you wanted.
            e.ghost.clear(); e.ghostFull.clear();
            c = '\t';
            goto rehandle;
        }
        // ── arky chrome keys (handled before the editor sees them) ──
        if (uc == 16) {                                     // ^P  command palette
            int pick = paletteMenu(app);
            if (pick >= 0) runPaletteCommand(app, pick, running);
            continue;
        }
        if (uc == 20) {                                     // ^T  next tab
            if (app.tabs.size() > 1) app.active = (app.active + 1) % (int)app.tabs.size();
            continue;
        }
        if (uc == 25) {                                     // ^Y  previous tab
            int n = (int)app.tabs.size();
            if (n > 1) app.active = (app.active - 1 + n) % n;
            continue;
        }
        if (uc == 5) {                                      // ^E  focus the explorer
            if (!app.sidebar) app.sidebar = true;
            app.expFocus = !app.expFocus;
            app.cur().status = app.expFocus
                               ? "explorer: \u2191\u2193 move  Enter open  \u2190/Bksp up  [ ] resize  ^E back to editor"
                               : "";
            continue;
        }
        if (app.expFocus) {
            // NOTE: escape sequences are deliberately NOT parsed here. This block
            // used to read a fixed ESC [ X, which is right for a bare arrow key and
            // wrong for everything longer -- a mouse report (ESC [ <0;40;12M) had
            // its first two bytes eaten and the rest, "0;40;12M", fell through and
            // was TYPED INTO THE BUFFER. That is the "clicking prints codes" bug.
            // The one CSI parser below handles every sequence and routes arrows
            // back here when the explorer has focus.
            // ^E is the ONLY way back to the editor, on purpose. Tab belongs to
            // completion and Esc belongs to cancelling a run or a build; giving
            // either one a second meaning here would cost more than the
            // convenience is worth.
            // Resize the sidebar in place: [ narrower, ] wider. Bounded so it can
            // never squeeze the text area out of existence.
            if (uc == '[' || uc == ']') {
                int w = app.sidebarW + (uc == ']' ? 2 : -2);
                int maxw = app.cur().cols / 2;
                if (w < 12) w = 12;
                if (maxw > 12 && w > maxw) w = maxw;
                app.sidebarW = w;
                app.cur().status = "explorer width " + std::to_string(w) + "  ([ / ] to resize)";
                continue;
            }
            if (uc == '\r' || uc == '\n') {
                if (app.expSel >= 0 && app.expSel < (int)app.entries.size()) {
                    const DirEnt de = app.entries[app.expSel];
                    if (de.isDir) {
                        app.root = (de.name == "..") ? parentOf(app.root) : app.root + "/" + de.name;
                        app.expSel = 0; app.expOff = 0;
                        readDir(app);
                    } else {
                        openInTab(app, app.root + "/" + de.name);
                        app.expFocus = false;
                    }
                }
                continue;
            }
            // A focused explorer OWNS the keyboard. Letting printable keys fall
            // through meant every keystroke aimed at the file list was silently
            // typed into the buffer hidden behind the sidebar. Here they are
            // type-ahead: press "m" to jump to the next entry starting with m.
            if (uc >= 32 && uc < 127) {
                int n = (int)app.entries.size();
                if (n == 0) continue;
                char want = (char)tolower(uc);
                for (int i = 1; i <= n; i++) {
                    int idx = (app.expSel + i) % n;
                    const std::string& nm = app.entries[idx].name;
                    if (!nm.empty() && tolower((unsigned char)nm[0]) == want) {
                        app.expSel = idx;
                        int visible = app.cur().viewH - 2;
                        if (app.expSel < app.expOff) app.expOff = app.expSel;
                        if (app.expSel >= app.expOff + visible) app.expOff = app.expSel - visible + 1;
                        break;
                    }
                }
                continue;
            }
            if (uc == 127 || uc == 8) {          // Backspace: up a directory
                app.root = parentOf(app.root);
                app.expSel = 0; app.expOff = 0;
                readDir(app);
                continue;
            }
        }
        if (uc == 7) {                                      // Ctrl-G  run arguments
            // Kept for the session (and settable up-front via `args` in
            // arkpy.config), so re-running with the same argv is just ^R.
            std::string a = statusPrompt(e, "run args (sys.argv[1:]): ", e.runArgs);
            e.runArgs = a;
            e.status = a.empty() ? "run args cleared" : ("run args: " + a);
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
        if (uc == 12) {                                     // Ctrl-L  apply clangd quick-fix
            applyClangdFix(e);
            continue;
        }
        if (uc == 19) {                                     // Ctrl-S  save
            if (e.filename.empty()) {
                // Pre-fill with the best name we know, so saving is one Enter
                // instead of retyping a path: the compile target's basename when
                // -oc/-ocb gave us one, else a plain default. Editable as usual.
                std::string suggest = "untitled.py";
                if (!e.compileTarget.empty()) {
                    std::string base = e.compileTarget;
                    size_t slash = base.find_last_of('/');
                    if (slash != std::string::npos) base = base.substr(slash + 1);
                    if (!base.empty()) suggest = base + ".py";
                }
                std::string name = statusPrompt(e, "save as: ", suggest);
                if (name.empty()) { e.status = "save cancelled"; continue; }
                // Only ADOPT the name if the write actually succeeds -- committing
                // it first meant a failed save (bad path, read-only dir) still left
                // the editor believing it owned that file.
                std::string prev = e.filename;
                e.filename = name;
                if (!saveFile(e)) { e.filename = prev; continue; }
                continue;
            }
            saveFile(e);
            continue;
        }
        if (uc == '\t') {                                   // Tab  complete / indent
            if (deleteSelection(e)) continue;
            // The live popup owns Tab while it's up: commit the highlighted row.
            if (acceptCompletion(e)) continue;
            // Tab takes the whole suggestion. Only when there ISN'T one does it
            // fall through to the popup (ambiguous prefix) or an indent.
            if (acceptGhost(e)) continue;
            std::string pfx = wordPrefix(e);
            bool afterDot = (e.cx > 0 && e.lines[e.cy][e.cx - 1] == '.');
            if (pfx.empty() && !afterDot) { for (int i = 0; i < 4; i++) insertChar(e, ' '); continue; }
            auto cands = looksLikeC(e) ? completeC(e.lines, e.cy, e.cx)
                                             : pyi::complete(e.lines, e.cy, e.cx, e.analysis);
            if (cands.empty()) { if (!afterDot) for (int i = 0; i < 4; i++) insertChar(e, ' '); continue; }
            // One candidate: take it outright, parens and all (same insertion the
            // inline suggestion would have made).
            if (cands.size() == 1) { if (!acceptGhost(e)) replacePrefixWith(e, pfx, cands[0].text); continue; }
            e.ghost.clear();   // the popup owns the screen below the cursor now
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
        if (uc == '\r' || uc == '\n') {
            // Enter commits the popup selection (VS Code / IntelliJ), else newline.
            if (acceptCompletion(e)) continue;
            deleteSelection(e); insertNewline(e); continue;
        }
        if (uc == 127 || uc == 8) {                         // Backspace
            if (!deleteSelection(e)) backspace(e);
            continue;
        }
        if (uc == 27) {                                     // escape sequence
            char b;
            // Lone Esc with the popup up = dismiss it (latched until the word
            // changes), matching how Esc closes the suggestion list in an IDE.
            if (read(STDIN_FILENO, &b, 1) != 1) {
                if (e.complActive) {
                    e.complDismissed = true; e.complDismissAt = e.complPrefix;
                    e.complActive = false; e.complCands.clear();
                }
                continue;
            }
            // ESC DEL / ESC BS -- what Option+Delete sends: delete word back.
            if (b == 127 || b == 8) {
                if (!deleteSelection(e)) deleteWordBack(e);
                continue;
            }
            // Meta-prefixed 2-byte forms. macOS terminals send Option+Left/Right
            // as ESC b / ESC f by default; dropping them meant Option+arrow did
            // nothing at all there. Same word motion as the CSI form.
            if (b == 'b' || b == 'f') {
                e.clearSel();
                const std::string& l = e.lines[e.cy];
                if (b == 'f') {
                    while (e.cx < (int)l.size() && !isWordChar(l[e.cx])) e.cx++;
                    while (e.cx < (int)l.size() &&  isWordChar(l[e.cx])) e.cx++;
                } else {
                    while (e.cx > 0 && !isWordChar(l[e.cx - 1])) e.cx--;
                    while (e.cx > 0 &&  isWordChar(l[e.cx - 1])) e.cx--;
                }
                continue;
            }
            if (b != '[' && b != 'O') {                             // bare Esc-ish
                if (e.complActive) {                                // dismiss the popup first
                    e.complDismissed = true; e.complDismissAt = e.complPrefix;
                    e.complActive = false; e.complCands.clear();
                }
                e.clearSel(); continue;                             // then drop any selection
            }
            // Collect a full CSI: parameter bytes 0x30-0x3f, then intermediates,
            // then one final byte. The old two-byte read couldn't see modifiers
            // (shift+arrow is ESC [ 1 ; 2 A), so shift-selection never arrived.
            std::string params;
            char final = 0;
            for (int guard = 0; guard < 40; guard++) {
                if (read(STDIN_FILENO, &b, 1) != 1) break;
                if (b >= 0x30 && b <= 0x3f) { params += b; continue; }
                final = b; break;
            }
            if (!final) continue;

            // Bracketed paste: ESC[200~ <payload> ESC[201~. Read to the closing
            // marker and insert the payload verbatim -- this is the ONLY path a
            // terminal paste takes, so auto-indent and auto-pairing never see it.
            if (final == '~' && params == "200") {
                std::string payload;
                const std::string endMark = "\x1b[201~";
                for (;;) {
                    char b2;
                    ssize_t nb = read(STDIN_FILENO, &b2, 1);
                    if (nb < 0 && errno == EINTR) continue;
                    if (nb != 1) break;
                    payload += b2;
                    if (payload.size() >= endMark.size() &&
                        payload.compare(payload.size() - endMark.size(),
                                        endMark.size(), endMark) == 0) {
                        payload.erase(payload.size() - endMark.size());
                        break;
                    }
                    if (payload.size() > 8u * 1024 * 1024) break;   // runaway guard
                }
                deleteSelection(e);
                insertLiteral(e, payload);
                e.ghost.clear(); e.ghostFull.clear();
                int nl = (int)std::count(payload.begin(), payload.end(), '\n') + 1;
                e.status = "pasted " + std::to_string(nl) + " line(s)";
                continue;
            }

            // SGR mouse report: ESC [ < btn ; col ; row (M press | m release).
            // '<' is a parameter byte, so it has already been collected above.
            if (!params.empty() && params[0] == '<' && (final == 'M' || final == 'm')) {
                int btn = 0, mx = 0, my = 0;
                if (sscanf(params.c_str() + 1, "%d;%d;%d", &btn, &mx, &my) == 3)
                    handleMouse(app, btn, mx, my, final == 'M');
                continue;
            }
            // modifyOtherKeys / CSI-u: many terminals report Ctrl+<key> not as a
            // control byte but as ESC [ 27 ; <mod> ; <codepoint> ~ (Ctrl+T is
            // 27;5;116). Dropping those on the floor is what made a stray letter
            // appear -- the codepoint IS the letter, and it leaked out as text.
            // Decode it back into the byte the editor already knows how to
            // handle and re-dispatch.
            if (final == '~' && params.compare(0, 3, "27;") == 0) {
                int p27 = 0, pmod = 0, pcode = 0;
                if (sscanf(params.c_str(), "%d;%d;%d", &p27, &pmod, &pcode) == 3 && pcode > 0) {
                    bool ctrl = (pmod >= 2) && (((pmod - 1) & 4) != 0);
                    if (ctrl && pcode >= 64 && pcode < 128) c = (char)(pcode & 0x1f);
                    else if (pcode < 128) c = (char)pcode;
                    else continue;
                    goto rehandle;
                }
                continue;
            }

            // Ctrl+PageUp / Ctrl+PageDown cycle tabs, matching every browser and
            // editor. first=="5"/"6" is PageUp/PageDown; the ctrl bit is in mod.
            if (final == '~' && (params.compare(0, 2, "5;") == 0 ||
                                 params.compare(0, 2, "6;") == 0)) {
                int m = atoi(params.c_str() + 2);
                if (m >= 2 && (((m - 1) & 4) != 0)) {      // ctrl held
                    int n = (int)app.tabs.size();
                    if (n > 1) app.active = (params[0] == '6') ? (app.active + 1) % n
                                                              : (app.active - 1 + n) % n;
                    continue;
                }
            }

            // With the explorer focused, arrows drive the FILE LIST. Handled here,
            // inside the one parser that understands full-length sequences, so a
            // modified or unexpected sequence can never leak into the buffer.
            if (app.expFocus) {
                if (final == 'A' && app.expSel > 0) app.expSel--;
                if (final == 'B' && app.expSel + 1 < (int)app.entries.size()) app.expSel++;
                if (final == 'D') {                       // left: up a directory
                    app.root = parentOf(app.root);
                    app.expSel = 0; app.expOff = 0;
                    readDir(app);
                }
                int visible = e.viewH - 2;
                if (app.expSel < app.expOff) app.expOff = app.expSel;
                if (visible > 0 && app.expSel >= app.expOff + visible)
                    app.expOff = app.expSel - visible + 1;
                continue;
            }

            // Modifier is the second ';'-separated param: 2 = shift, 4 = alt,
            // 6 = shift+alt ... (all are "modifier-1" bitmasks + 1).
            int mod = 0;
            size_t semi = params.find(';');
            if (semi != std::string::npos) mod = atoi(params.c_str() + semi + 1);
            bool shift = (mod >= 2) && (((mod - 1) & 1) != 0);
            bool word  = (mod >= 2) && (((mod - 1) & 2) != 0);   // alt/option
            std::string first = params.substr(0, semi == std::string::npos ? params.size() : semi);

            // While the popup is up, Up/Down drive the LIST, not the cursor --
            // exactly like a GUI IDE. Every other key (including left/right) falls
            // through and, by moving the cursor, lets the next recompute close it.
            if (e.complActive && (final == 'A' || final == 'B') && mod == 0) {
                int n = (int)e.complCands.size();
                e.complSel = (final == 'A') ? (e.complSel - 1 + n) % n
                                            : (e.complSel + 1) % n;
                continue;
            }
            // Shift extends a selection; anything unshifted collapses it.
            bool isMove = (final=='A'||final=='B'||final=='C'||final=='D'||
                           final=='H'||final=='F'||first=="5"||first=="6");
            if (isMove) { if (shift) e.anchorSel(); else e.clearSel(); }

            switch (final) {
                case 'A': e.cy--; clampCursor(e); break;               // up
                case 'B': e.cy++; clampCursor(e); break;               // down
                case 'C':                                              // right
                    // Right-arrow with a suggestion showing takes it -- there is
                    // nothing to the right to move onto anyway.
                    // Right-arrow just MOVES. Tab takes the whole suggestion,
                    // shift+Tab takes one word -- the arrow keys stay yours.
                    if (word) { const std::string& l = e.lines[e.cy];
                                while (e.cx < (int)l.size() && !isWordChar(l[e.cx])) e.cx++;
                                while (e.cx < (int)l.size() &&  isWordChar(l[e.cx])) e.cx++; }
                    else if (e.cx < (int)e.lines[e.cy].size()) e.cx++;
                    else if (e.cy + 1 < (int)e.lines.size()) { e.cy++; e.cx = 0; }
                    break;
                case 'D':                                              // left
                    if (word) { const std::string& l = e.lines[e.cy];
                                while (e.cx > 0 && !isWordChar(l[e.cx - 1])) e.cx--;
                                while (e.cx > 0 &&  isWordChar(l[e.cx - 1])) e.cx--; }
                    else if (e.cx > 0) e.cx--;
                    else if (e.cy > 0) { e.cy--; e.cx = (int)e.lines[e.cy].size(); }
                    break;
                case 'H': e.cx = 0; break;                              // home
                case 'F': e.cx = (int)e.lines[e.cy].size(); break;      // end
                case 'Z': acceptGhostWord(e); break;                    // shift+Tab: one word
                case '~':
                    if (first == "3") {                                 // delete (forward)
                        if (deleteSelection(e)) break;
                        if (e.cx < (int)e.lines[e.cy].size()) e.lines[e.cy].erase(e.cx, 1);
                        else if (e.cy + 1 < (int)e.lines.size()) {
                            e.lines[e.cy] += e.lines[e.cy + 1];
                            e.lines.erase(e.lines.begin() + e.cy + 1);
                        }
                        e.dirty = true;
                    } else if (first == "5" || first == "6") {
                        bool down = (first == "6");
                        if (word) {
                            // Option+Up/Down. Several terminals encode these as
                            // PageUp/PageDown carrying the alt modifier -- which
                            // landed on the page-jump below and clamped straight
                            // to the top or bottom of the file ("it flings"). On
                            // macOS these mean PARAGRAPH, so that is what they do.
                            int y = e.cy;
                            auto blank = [&](int i) {
                                if (i < 0 || i >= (int)e.lines.size()) return true;
                                return e.lines[i].find_first_not_of(" \t") == std::string::npos;
                            };
                            y += down ? 1 : -1;
                            while (y > 0 && y < (int)e.lines.size() - 1 && blank(y)) y += down ? 1 : -1;
                            while (y > 0 && y < (int)e.lines.size() - 1 && !blank(y)) y += down ? 1 : -1;
                            e.cy = y;
                        } else {
                            e.cy += down ? e.textRows() : -e.textRows();
                        }
                        clampCursor(e);
                    }
                    else if (first == "1" || first == "7") e.cx = 0;      // home
                    else if (first == "4" || first == "8") e.cx = (int)e.lines[e.cy].size();
                    break;
                default: break;
            }
            continue;
        }
        if (uc >= 32) {                                     // printable
            deleteSelection(e);                             // typing replaces a selection
            if (!smartPair(e, (char)c)) insertChar(e, (char)c);
            continue;
        }
        // other control chars: ignore
    }

    pymodel::stop();
    clangdc::stop();
    // Unconditionally, even if app.mouse is off -- a config flip mid-session must
    // not be able to leave the terminal reporting. A terminal left in mouse mode
    // feeds wheel escapes into whatever runs next, which is how "I scrolled and
    // the command exited with code 0" happens: the report lands on the child's
    // stdin, not on the shell's.
    enableMouse(false);
    enableBracketedPaste(false);
    altScreen(false);
    leaveRaw(e);
    fflush(stdout);
    return 0;
}
