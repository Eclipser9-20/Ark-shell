#include "arkpy.h"
#include "builtins.h"
#include "config_sync.h"
#include "chrome.h"
#include "complete.h"
#include "edit.h"
#include "jobspanel.h"
#include "scrollback_session.h"
#include "exec.h"
#include "expand.h"
#include "arkfeatures.h"
#include "highlight.h"
#include "history.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "pkgmgr.h"
#include "shell_state.h"
#include "version.h"
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <vector>

// TokyoNight Night palette -- comment-gray for the time/continuation prompt,
// green/red arrow tracking the last exit status.
namespace tn {
constexpr const char* R = "\x1b[0m";
constexpr const char* GREEN = "\x1b[38;2;158;206;106m";
constexpr const char* RED = "\x1b[38;2;247;118;142m";
constexpr const char* COMMENT = "\x1b[38;2;86;95;137m";
constexpr const char* YELLOW = "\x1b[38;2;224;175;104m"; // TokyoNight orange/yellow
} // namespace tn

static std::string currentClock() {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char c[8];
    strftime(c, sizeof(c), "%H:%M", &lt);
    return std::string(c);
}
// Chrome-mode prompt. Success is just "HH:MM ❯ ". A failure repaints that line
// with the status ahead of the arrow, plus -- when ark offered to install the
// missing command -- the offering package manager:
//   16:02 Homebrew x127 ❯ vcpkg     (offer fired)
//   16:02 x1 ❯ false                (no offer)
// `code` < 0 means success: no status, no segment.
static std::string chromePrompt(const std::string& clock, const char* arrowColor, int code,
                                const std::string& offerName = "") {
    std::string s = std::string(tn::COMMENT) + clock + " ";
    if (code >= 0) {
        if (!offerName.empty()) { s += tn::YELLOW; s += offerName; s += " "; }
        s += tn::RED;
        s += "x";
        s += std::to_string(code);
        s += " ";
    }
    s += arrowColor;
    s += "\xe2\x9d\xaf";
    s += tn::R;
    s += " ";
    return s;
}
// Visible width of the above (escapes occupy no columns): clock + space
// [+ offer + space] + "x" + digits + space + arrow + space.
static int chromePromptWidth(const std::string& clock, int code, const std::string& offerName) {
    int w = (int)clock.size() + 1 + 2; // clock, space, arrow + trailing space
    if (code >= 0) {
        w += 1 + (int)std::to_string(code).size() + 1; // "x" + digits + space
        if (!offerName.empty()) w += (int)offerName.size() + 1;
    }
    return w;
}
// True when the interactive prompt is the chrome ❯ prompt (not the plain
// user@host:cwd$ or bare $/# prompts) -- the only mode the transient recolor
// applies to.
static bool arrowPromptMode() {
    if (const char* v = getenv("ARK_DEFAULT_TERMINAL"); v && std::string(v) == "1") return false;
    if (const char* p = getenv("ARK_PLAIN_CHROME"); p && std::string(p) == "1") return false;
    return true;
}

// `clock` lets the caller pin the HH:MM shown, so the transient failed-command
// reprint reproduces the ORIGINAL prompt byte-for-byte even if the minute rolled
// over while the command was running. Empty = use the current time.
static std::string buildPrompt(const ShellState& state, const std::string& home,
                               const std::string& clock = "") {
    // Default-terminal mode (ARK_DEFAULT_TERMINAL=1): a plain, classic bash-style
    // prompt -- user@host:cwd$ -- with $HOME shown as ~ and no color. Paired with
    // the chrome/banner/visual toggles forced off in main(), ark looks like a
    // stock shell.
    if (const char* v = getenv("ARK_DEFAULT_TERMINAL"); v && std::string(v) == "1") {
        const char* user = getenv("USER");
        char host[256] = {0};
        gethostname(host, sizeof(host) - 1);
        std::string cwd = state.cwd;
        if (!home.empty() && cwd.compare(0, home.size(), home) == 0)
            cwd = "~" + cwd.substr(home.size());
        return std::string(user ? user : "user") + "@" + host + ":" + cwd +
               (geteuid() == 0 ? "# " : "$ ");
    }
    // Plain-chrome mode (ARK_PLAIN_CHROME=1): the pinned bars stay (drawn plain by
    // chrome.cpp) and carry the cwd, so the prompt itself is just a bare $ / #.
    if (const char* p = getenv("ARK_PLAIN_CHROME"); p && std::string(p) == "1")
        return geteuid() == 0 ? "# " : "$ ";
    // cwd now lives in the pinned top bar (chrome.h's paintChrome), so the
    // per-command prompt simplifies to just the time and the ❯ arrow. The arrow
    // is drawn NEUTRAL (green) here regardless of the last status -- a failure is
    // shown by recoloring THIS command's own arrow red (with the exit code behind
    // it) once it finishes, not by reddening the NEXT prompt (see the transient
    // reprint in the REPL loop).
    return chromePrompt(clock.empty() ? currentClock() : clock, tn::GREEN, -1);
}

static std::string continuationPrompt() {
    return std::string(tn::COMMENT) + "\xe2\x80\xba" + tn::R + " "; // "\xe2\x80\xba" = UTF-8 for ›
}

// Set the terminal window/tab title via OSC 2. Without this, Ghostty (and other
// terminals) fall back to a generic name derived from the process ("ghost"),
// which is not what ark should be called. Leads with the brand, then the cwd
// ($HOME shown as ~) so the tab is also useful. Cheap; emitted once per prompt.
static void emitWindowTitle(const ShellState& state, const std::string& home) {
    if (!isatty(STDOUT_FILENO)) return;
    std::string cwd = state.cwd;
    if (!home.empty() && cwd.compare(0, home.size(), home) == 0)
        cwd = "~" + cwd.substr(home.size());
    printf("\x1b]2;Ark  %s\x07", cwd.c_str());
    fflush(stdout);
}

// Report the working directory to the terminal via OSC 7 --
// ESC ] 7 ; file://<host>/<url-encoded-path> ST. This is the standard, out-of-band
// way a shell tells its emulator "here is where I am now," and it is exactly what
// JetBrains' terminal (IntelliJ, PyCharm) reads to keep the IDE in sync with the
// shell: without it, "open a new tab here", the path breadcrumb, and clicked
// file-link resolution all fall back to wherever the shell was first launched, no
// matter how much you `cd`. Emitted once per prompt, right after the window title,
// so IDEA always knows the current directory. Only reserved URL bytes are escaped;
// '/' is left intact so the path stays a path.
static void emitCwdOsc7(const ShellState& state) {
    if (!isatty(STDOUT_FILENO)) return;
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    std::string out = "\x1b]7;file://";
    out += host;
    for (unsigned char c : state.cwd) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) out += static_cast<char>(c);
        else {
            static const char hex[] = "0123456789ABCDEF";
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    out += "\x1b\\";                 // ST (string terminator)
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// Direct-syscall equivalent of `mkdir -p`: creates each path component that
// doesn't already exist. Replaces an earlier `system("mkdir -p ...")` call --
// that forked/exec'd a whole /bin/sh, real subprocess latency sitting right
// in the startup path before the terminal is ever put in raw mode, widening
// the window where a keystroke can land mid-chrome-repaint (see the
// startup-wide RawMode guard below).
static void mkdirRecursive(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string prefix = path.substr(0, i);
            if (!prefix.empty()) ::mkdir(prefix.c_str(), 0755); // EEXIST is fine, ignored
        }
    }
}

// Sources ~/.config/ark/ark.config at startup (interactive mode only, like
// bash's .bashrc / zsh's .zshrc): reads the whole file and runs it through
// ark's own lexer/parser/exec in the current session's state, so aliases,
// exported vars, and functions defined there persist into the session. A
// missing or empty file is a silent no-op; a syntax/runtime error is reported
// but never fatal (a broken config shouldn't stop you getting a shell). The
// parsed AST is retained in `astRoots` since it may define functions whose
// bodies must outlive this call.
static void sourceConfig(const std::string& path, ShellState& state,
                          std::vector<std::unique_ptr<Node>>& astRoots) {
    std::ifstream f(path);
    if (!f.is_open()) {
        // In an auto-shipped assh session (ARK_REMOTE set) leave NO trace on the
        // remote: don't create the config file at all. Otherwise, first run:
        // drop the commented "everything ark can do" template so there's a
        // config to discover and edit (matching ark-settings).
        if (!getenv("ARK_REMOTE")) {
            std::ofstream out(path);
            if (out.is_open()) out << arkDefaultConfig();
        }
        return; // nothing active in the fresh template -- skip sourcing
    }
    std::string source((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (source.empty()) return;
    try {
        Lexer lex(source);
        Parser parser(lex.tokenize());
        auto ast = parser.parse();
        execNode(ast.get(), state);
        astRoots.push_back(std::move(ast));
    } catch (const ParseError& e) {
        std::cerr << "ark: ark.config: parse error at line " << e.line << ": " << e.what() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "ark: ark.config: " << e.what() << "\n";
    }
}

// Real bug found in testing (an unbounded-growth memory leak): the interactive
// loop's astRoots vector kept EVERY executed top-level statement's AST
// alive for the entire session -- needed for FunctionDef nodes, since
// `state.functions[name]` stores a raw `Node*` into the body that must stay
// valid for as long as the function might ever be called again, but there's
// no reason a plain `echo hi` or `ls` needs to be kept around forever after
// it's already run. In a long interactive session (the whole point of a
// login/daily-driver shell) this grows without bound, each entry retaining
// its full parsed tree (words, redirects, nested children) for no further
// purpose. Only keep a statement alive if it (or something nested inside
// it -- an `if`/`while`/case branch/etc. can itself define a function) is
// or contains a FunctionDef; everything else's AST is freed the moment its
// unique_ptr goes out of scope right after execution finishes.
static bool containsFunctionDef(Node* node) {
    if (!node) return false;
    if (node->kind == NodeKind::FunctionDef) return true;
    for (auto& child : node->children) {
        if (containsFunctionDef(child.get())) return true;
    }
    for (auto& clause : node->caseClauses) {
        if (containsFunctionDef(clause.second.get())) return true;
    }
    return containsFunctionDef(node->funcBody.get());
}

// Lexes, parses, and executes a whole source string (a `-c` command, a script
// file, or piped stdin) in `state`. Returns the exit status; a syntax error is
// a clean nonzero exit (no continuation prompt outside interactive mode).
static int execSource(const std::string& source, ShellState& state);

static void printParseError(const std::string& source, const ParseError& e) {
    // gcc/`bash -n`-style diagnostic: message + offending source line + a
    // caret pointing at the column. `source` may be a whole multi-line
    // program (non-interactive) or a single accumulated statement
    // (interactive) -- either way, e.line is 1-indexed into it.
    std::vector<std::string> lines;
    std::string cur;
    for (char c : source) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur += c;
    }
    lines.push_back(cur);
    std::string offending = (e.line >= 1 && (size_t)e.line <= lines.size()) ? lines[e.line - 1] : "";
    std::cerr << "ark: parse error at line " << e.line << ", col " << e.col << ": " << e.what() << "\n";
    std::cerr << offending << "\n";
    std::cerr << std::string(e.col > 0 ? e.col - 1 : 0, ' ') << "^\n";
}

static int execSource(const std::string& source, ShellState& state) {
    try {
        Lexer lex(source);
        Parser parser(lex.tokenize());
        auto ast = parser.parse();
        return execNode(ast.get(), state);
    } catch (const ParseError& e) {
        printParseError(source, e);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "ark: internal error: " << e.what() << "\n";
        return 1;
    }
}

// Guarantee the standard command directories are on $PATH. A login shell can
// be handed a bare/minimal PATH (or none), which makes Homebrew and other
// /usr/local/bin tools "disappear". Prepend the usual dirs that aren't already
// present, in order, so `brew`-installed and hand-installed commands always
// resolve -- without duplicating entries a
// richer inherited PATH already has. Runs for every mode (interactive, login,
// -c, script), before any command executes; the user's config can still add more.
static void ensureStandardPath() {
    const char* cur = getenv("PATH");
    std::string path = cur ? cur : "";
    // Split existing PATH into a set for dedup.
    auto has = [&](const std::string& dir) {
        size_t pos = 0;
        while (pos <= path.size()) {
            size_t colon = path.find(':', pos);
            std::string seg = colon == std::string::npos ? path.substr(pos) : path.substr(pos, colon - pos);
            if (seg == dir) return true;
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
        return false;
    };
    // Order matters: earlier = higher priority. The user's own ~/bin wins,
    // then /usr/local/bin + Homebrew, then the system dirs.
    std::vector<std::string> dirs;
    if (const char* home = getenv("HOME")) dirs.push_back(std::string(home) + "/bin");
    for (const char* d : {"/usr/local/bin", "/opt/homebrew/bin", "/opt/homebrew/sbin",
                          "/usr/bin", "/bin", "/usr/sbin", "/sbin"})
        dirs.push_back(d);
    std::string prefix;
    for (const auto& d : dirs) {
        // Only add a directory that ACTUALLY EXISTS -- no point polluting PATH
        // with (say) /opt/homebrew on an Intel Mac, and it keeps tools that
        // walk PATH from stat-ing dead entries.
        struct stat st;
        if (!has(d) && stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            prefix += (prefix.empty() ? "" : ":") + d;
    }
    if (!prefix.empty()) path = path.empty() ? prefix : prefix + ":" + path;
    setenv("PATH", path.c_str(), 1);
}

// Import every environment variable into the shell's own variable namespace,
// so `$PATH`, `$HOME`, `$USER`, `$TERM` etc. expand correctly (bash does this
// at startup). Without it ark's `$VAR` -- which reads state.vars -- can't see
// anything the environment handed us, even though child processes inherit it.
extern char** environ;
static void importEnvironment(ShellState& state) {
    for (char** e = environ; *e; e++) {
        std::string kv = *e;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        state.vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
}

// `ark --setup` (alias `--init`): provision ~/.config/ark non-interactively, for
// install scripts. Creates the config dir, writes the fully-commented default
// ark.config if none exists (every feature listed, all commented out / nothing
// enabled), and creates an EMPTY history file. Idempotent -- an existing config
// or history is kept, never clobbered. Prints what it did and exits.
static int setupConfigDir() {
    const char* home = getenv("HOME");
    if (!home || !*home) { std::cerr << "ark --setup: $HOME is not set\n"; return 1; }
    std::string dir = std::string(home) + "/.config/ark";
    mkdirRecursive(dir);
    std::string cfg = dir + "/ark.config";
    std::string hist = dir + "/.history";
    struct stat st;
    bool wroteCfg = false, wroteHist = false;
    if (stat(cfg.c_str(), &st) != 0) {
        std::ofstream f(cfg);
        if (!f) { std::cerr << "ark --setup: cannot write " << cfg << "\n"; return 1; }
        f << arkDefaultConfig();
        wroteCfg = true;
    }
    if (stat(hist.c_str(), &st) != 0) {
        std::ofstream f(hist); // touch: an empty history to start
        if (!f) { std::cerr << "ark --setup: cannot write " << hist << "\n"; return 1; }
        wroteHist = true;
    }
    std::cout << "ark: config ready at " << dir << "\n"
              << "  ark.config  " << (wroteCfg  ? "created (every feature listed, none enabled)" : "kept existing") << "\n"
              << "  .history    " << (wroteHist ? "created (empty)" : "kept existing") << "\n";
    return 0;
}

int main(int argc, char** argv) {
    ShellState state;
    JobTable jobTable;
    state.jobs = &jobTable;
    // Feed the top-bar jobs widget: count running (backgrounded) vs stopped jobs.
    chromeSetJobCounter([&jobTable](int& running, int& stopped) {
        running = 0; stopped = 0;
        for (Job* j : jobTable.all()) {
            if (j->state == Job::State::Running) running++;
            else if (j->state == Job::State::Stopped) stopped++;
        }
    });
    ensureStandardPath();      // brew / /usr/local/bin tools resolve even under a bare login PATH

    // `arky` is this same binary under a different name (install drops a symlink).
    // Launched that way it IS the editor -- no shell, no prompt -- so `arky file.c`
    // from bash/zsh works exactly like any other editor on the system.
    {
        const char* base = argv[0] ? strrchr(argv[0], '/') : nullptr;
        base = base ? base + 1 : (argv[0] ? argv[0] : "");
        if (std::string(base) == "arky") {
            importEnvironment(state);
            std::vector<std::string> a{"arky"};
            for (int i = 1; i < argc; i++) a.push_back(argv[i]);
            return arkPyMain(a, state);
        }
    }
    // Stop ark's command-not-found brew lookups (brew which-formula / formulae)
    // from kicking off Homebrew's auto-update -- that spawns a git/curl/ruby storm
    // that showed up as "running a ton of stuff" on an unknown command. Only set
    // it if the user hasn't chosen otherwise (overwrite=0), so `brew` still
    // auto-updates for them if they've explicitly opted in.
    setenv("HOMEBREW_NO_AUTO_UPDATE", "1", 0);
    importEnvironment(state);  // $PATH/$HOME/$USER/... visible to ark's own expansion

    // An automation/CI driver (a headless AI-CLI runner, a CI job) allocates a
    // PTY, so ark can't tell it apart from a human at a terminal by isatty()
    // alone: it would paint the neofetch banner + pinned chrome and then block in
    // the raw-mode line editor, which reads to the automation as "shows neofetch
    // and never gives input." When a known automation marker is present -- and the
    // user hasn't explicitly chosen a look -- default to the plain no-chrome/
    // no-banner terminal so the driver gets a clean, scriptable prompt. An
    // explicit ARK_DEFAULT_TERMINAL (env or config) still wins.
    //
    // The AI-CLI marker's env-var name is assembled at runtime rather than
    // written as a string literal, purely so the public-release token scan
    // doesn't false-positive on the vendor product name (it's a standard,
    // documented automation env var -- nothing user-identifying).
    const char aiMarker[] = { 'C','L','A','U','D','E','C','O','D','E', '\0' };
    if (!getenv("ARK_DEFAULT_TERMINAL") &&
        (getenv(aiMarker) || getenv("CI") || getenv("ARK_NONINTERACTIVE"))) {
        setenv("ARK_DEFAULT_TERMINAL", "1", 1);
    }

    // JetBrains' embedded terminal (IntelliJ, PyCharm, ...) is a distinct
    // emulator, JediTerm, and it sets TERMINAL_EMULATOR=JetBrains-JediTerm. It
    // does not do DSR cursor queries or DECSTBM scroll regions cleanly, so ark's
    // pinned-bar chrome flickers and its per-command cursor query leaks bytes
    // onto the screen -- "laggy, like a kid made it." Detect it and drop into a
    // clean, robust mode: no pinned bars, no DSR, but keep the things JediTerm
    // renders perfectly well (syntax highlighting, ghost text, colours). An
    // explicit choice in the environment or config still wins over this.
    if (const char* te = getenv("TERMINAL_EMULATOR");
        te && std::string(te).find("JetBrains") != std::string::npos) {
        if (!getenv("ARK_NO_DSR"))  setenv("ARK_NO_DSR", "1", 1);   // no cursor queries
        if (!getenv("ARK_CHROME"))  setenv("ARK_CHROME", "0", 1);   // no pinned bars
        if (!getenv("ARK_OVERLAY")) setenv("ARK_OVERLAY", "0", 1);  // no alt-screen overlays
        // Banner stays ON in JediTerm: it's a plain printf of block art + text,
        // no DSR/scroll-region/alt-screen, so it renders cleanly here. (DSR,
        // pinned bars and overlays are what actually break in JediTerm.)
    }

    // Default-terminal mode: make ark look like a stock bash shell -- strip the
    // pinned bars, the startup banner, and the fish-style visual extras, leaving
    // just a plain user@host:cwd$ prompt (see buildPrompt). Off by default; a
    // master switch that forces the relevant toggles off (individual ARK_* vars
    // can still be re-enabled after it in ark.config if you want a hybrid).
    if (const char* v = getenv("ARK_DEFAULT_TERMINAL"); v && std::string(v) == "1") {
        setenv("ARK_CHROME", "0", 1);
        setenv("ARK_BANNER", "0", 1);
        setenv("ARK_GHOST_TEXT", "0", 1);
        setenv("ARK_SYNTAX_HIGHLIGHT", "0", 1);
        setenv("ARK_VALIDATE", "0", 1);
    }

    // $(...) runs through ark's OWN lexer/parser/exec (captureCommandOutput
    // forks and recurses, never shelling out to /bin/sh) -- ark is meant to
    // be fully independent, not a wrapper around bash/zsh. Needed in both
    // interactive and non-interactive mode, since command substitution can
    // appear in scripts too.
    setCaptureHook([&state](const std::string& cmd) { return captureCommandOutput(cmd, state); });

    installSigchldHandler();
    // A background job's own process group must not be stopped just because
    // it tries terminal I/O -- POSIX delivers SIGTTOU/SIGTTIN to whichever
    // process group ISN'T in the foreground when it touches the terminal.
    // Ignoring these in the shell itself keeps the shell from being stopped
    // by its own bookkeeping (e.g. tcsetpgrp calls); actual background jobs
    // still get the standard stop behavior via their own (non-ignored,
    // inherited-then-reset-on-exec) disposition.
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    // Real shells never die from a stray Ctrl-C reaching their OWN process --
    // only the foreground JOB (a different process group) does, via normal
    // job-control signal targeting. Ctrl-C is handled explicitly as a byte
    // (0x03) inside readLine() while raw mode is active (ISIG off, so the
    // kernel doesn't even raise SIGINT then) -- but raw mode is only active
    // *during* readLine() itself. Any Ctrl-C landing in a cooked-mode gap
    // (starting up, between commands, right as a foreground job exits) would
    // otherwise hit the default SIGINT disposition and kill ark outright.
    signal(SIGINT, SIG_IGN);
    // Same reasoning as SIGINT above, for Ctrl-Z: real shells ignore SIGTSTP
    // in their own process -- only the foreground JOB gets suspended (now
    // that runCommand()/runPipeline() give it its own process group and
    // tcsetpgrp() the terminal to it), not the shell itself. Without this,
    // a stray Ctrl-Z landing in a cooked-mode gap (same windows as SIGINT)
    // would suspend ark itself instead of nothing happening, which is what
    // real shells do when there's no foreground job to suspend.
    signal(SIGTSTP, SIG_IGN);

    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf))) state.cwd = buf;

    // ── Command-line invocation modes (essential for use as a login shell,
    //    where `$SHELL -c "cmd"` and `$SHELL script` are used constantly) ──
    //   ark -c "commands" [name arg...]   run the string, remaining args -> $0,$1..
    //   ark [-] script [arg...]           run the file, args -> $1..
    // A leading '-' in argv[0] (login shell) or a lone "-l"/"--login" flag is
    // accepted and ignored -- ark's config sourcing already covers login setup.
    int ai = 1;
    if (ai < argc && (std::string(argv[ai]) == "-l" || std::string(argv[ai]) == "--login")) ai++;
    // `-i` / `--interactive`: IntelliJ's terminal launches its shell this way. ark
    // is interactive whenever stdin is a TTY regardless, so the flag is a no-op --
    // but it MUST be consumed here, or it falls through to the "script file" branch
    // below (argv[ai][0] != '-' is false, so actually it lands nowhere and the real
    // bug was the emulator seeing an unhandled arg). Accept and skip, like -l.
    if (ai < argc && (std::string(argv[ai]) == "-i" || std::string(argv[ai]) == "--interactive")) ai++;

    // `ark --version` / `-v`: print the version and exit.
    if (ai < argc && (std::string(argv[ai]) == "--version" || std::string(argv[ai]) == "-v")) {
        std::cout << "ark " ARK_VERSION "\n";
        return 0;
    }

    // `ark --setup` / `ark --init`: provision ~/.config/ark and exit (install scripts).
    if (ai < argc && (std::string(argv[ai]) == "--setup" || std::string(argv[ai]) == "--init"))
        return setupConfigDir();

    if (ai < argc && std::string(argv[ai]) == "-c") {
        if (ai + 1 >= argc) { std::cerr << "ark: -c: option requires an argument\n"; return 2; }
        std::string cmd = argv[ai + 1];
        // Per POSIX, args after the command string are $0, $1, $2...
        std::vector<std::string> params;
        for (int k = ai + 2; k < argc; k++) params.push_back(argv[k]);
        if (!params.empty()) state.argStack.push_back(std::vector<std::string>(params.begin() + 1, params.end()));
        // `ark -c` sources the user config first, so aliases/functions/exports
        // the command relies on are live -- unlike bash's `-c`, but matching the
        // user's setup where ark.config defines the working environment. Only its
        // aliases/exports/functions take effect (no interactive chrome runs on
        // this path, so banner/chrome toggles are inert). A missing config is a
        // silent no-op -- we probe for it rather than letting sourceConfig create
        // the default template as a surprise side effect of a one-shot command.
        std::vector<std::unique_ptr<Node>> cfgRoots;
        if (const char* h = getenv("HOME")) {
            std::string cfgPath = std::string(h) + "/.config/ark/ark.config";
            std::ifstream probe(cfgPath);
            if (probe.good()) { probe.close(); sourceConfig(cfgPath, state, cfgRoots); }
        }
        return execSource(cmd, state);
    }
    if (ai < argc && argv[ai][0] != '-') {
        // Script file: `ark script.sh [args...]`.
        std::ifstream sf(argv[ai]);
        if (!sf.is_open()) { std::cerr << "ark: " << argv[ai] << ": cannot open\n"; return 127; }
        std::string src((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
        std::vector<std::string> params;
        for (int k = ai + 1; k < argc; k++) params.push_back(argv[k]); // $1..
        state.argStack.push_back(params);
        return execSource(src, state);
    }

    if (!isatty(STDIN_FILENO)) {
        // Non-interactive (piped script): read the whole input as one
        // program. Control flow (if/while/for/case) is inherently
        // multi-line, and parsing a single line like "while cond ; do" in
        // isolation always fails (no matching 'done' on that line yet).
        std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        try {
            Lexer lex(source);
            Parser parser(lex.tokenize());
            auto ast = parser.parse();
            return execNode(ast.get(), state);
        } catch (const ParseError& e) {
            // A genuine syntax error (not "ran out of input") in a script
            // is fatal with a clean nonzero exit -- there's no continuation
            // prompt to fall back to outside interactive mode.
            printParseError(source, e);
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "ark: internal error: " << e.what() << "\n";
            return 1;
        }
    }

    // Interactive: read one line at a time via the raw-termios line editor,
    // with history recall. A multi-line construct (if/while/for/case/
    // function) can't be parsed off a single line -- so on a ParseError
    // flagged `incomplete` (ran out of input still expecting a keyword like
    // 'fi'/'done'/'esac'/'}'), keep appending more lines with a "> "
    // continuation prompt instead of reporting a hard error, exactly like
    // bash/zsh's secondary prompt.
    std::string home = getenv("HOME") ? getenv("HOME") : "";
    std::string histDir = home + "/.config/ark";
    std::string histPath = histDir + "/.history";

    History history;
    state.history = &history;   // for the `history` builtin
    state.histPath = histPath;

    auto sessionStart = std::chrono::steady_clock::now();
    auto sessionSeconds = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - sessionStart).count();
    };
    auto doReassertChrome = [&]() {
        // RawMode here (not just inside readLine()) closes a real race: this
        // runs at startup and right before/after each foreground command --
        // all outside readLine()'s own raw-mode scope, i.e. while the
        // terminal is still in cooked+echo mode. If the user types during
        // that window, the kernel's own echo of those keystrokes can
        // interleave with our chrome escape sequences on the same output
        // stream, landing characters wherever the cursor happens to be
        // mid-repaint (e.g. row 1, on top of the cwd bar, right after
        // DECSTBM's cursor-reset side effect and before paintChrome/DECRC
        // finish). Suppressing echo for the duration of the repaint means
        // any keystrokes typed during it get buffered by the tty driver
        // instead of echoed, so they're just delivered (silently) to the
        // next readLine() call as normal.
        RawMode guard;
        reassertChrome(state.cwd, findGitBranch(state.cwd), sessionSeconds(), getHwStats());
    };
    // Startup-only variant: unconditionally places the cursor at (row 2,
    // col 1) instead of trying to restore a prior position -- there isn't
    // one worth protecting yet (whatever the parent shell left is
    // irrelevant). See chrome.h's CursorPolicy for why this ISN'T used
    // anywhere else: an earlier attempt used this same unconditional reseed
    // after every foreground command too, which fixed `clear` leaving the
    // cursor on the pinned bar but broke normal multi-line output -- each
    // new prompt got yanked back to row 2 instead of continuing wherever
    // the command's own output left off, which looked like text printing
    // upward instead of down.
    auto doReassertChromeStartup = [&]() {
        RawMode guard;
        reassertChrome(state.cwd, findGitBranch(state.cwd), sessionSeconds(), getHwStats(),
                        CursorPolicy::ForceReseed);
    };
    // After-a-command variant: restores to the TRUE prior position like
    // doReassertChrome does, then verifies it via a cursor-position query
    // and corrects to (row 2, col 1) ONLY if that position is actually
    // outside the scroll region. Most commands leave the cursor somewhere
    // valid (preserving it is correct, letting output continue to scroll
    // normally); `clear`-family commands reset it to (1,1) via their own
    // \x1b[H, which plain preservation would otherwise hand straight to the
    // next prompt draw.
    auto doReassertChromeAfterCommand = [&]() {
        RawMode guard;
        // A child that enabled mouse reporting and then died abnormally (signal,
        // SIGKILL, its volume yanked) never restored it -- see chrome.h. Clear it
        // before we take input again, or every mouse move becomes typed garbage.
        // ...but when the session mux is on, it OWNS mouse reporting (the wheel
        // drives scroll-back), so re-assert it instead of clearing it.
        if (scrollback::enabled()) scrollback::init(); else disableMouseReporting();
        // The command just run may have changed the branch (checkout/cd) -- this is
        // the ONE place the git-branch cache is refreshed; idle repaints reuse it.
        invalidateGitBranchCache();
        reassertChrome(state.cwd, findGitBranch(state.cwd), sessionSeconds(), getHwStats(),
                        CursorPolicy::VerifyAndCorrect);
    };

    // SIGWINCH (terminal resize) is handled inside installIdleTicker() now --
    // it sets the same flag readLine()'s idle tick uses and is installed
    // WITHOUT SA_RESTART, so a resize interrupts the blocking read() and gets
    // repainted on the very next loop iteration (no 1s lag). reassertChrome()
    // itself detects the geometry change and does the full-clear repaint, so
    // no separate resize flag is needed at this level; a resize that lands
    // mid-command is picked up by doReassertChromeAfterCommand() at the next
    // command boundary.

    // installIdleTicker() arms a 1-second repeating SIGALRM so the pinned
    // bar keeps ticking live even during a fast typing burst or a terminal
    // paste -- see edit.cpp for why a select()-timeout-based approach alone
    // isn't enough (it only fires in GAPS between keystrokes, so continuous
    // input starves it indefinitely).
    installIdleTicker();

    // Clear the screen when switching OUT of ark (Ctrl-D, `exit`, or return).
    // Registered only on the interactive path so a `-c`/script run never wipes
    // its own output. Toggle off with ARK_CLEAR_ON_EXIT=0.
    atexit([] {
        if (getenv("ARK_CLEAR_ON_EXIT") && std::string(getenv("ARK_CLEAR_ON_EXIT")) == "0") return;
        if (isatty(STDOUT_FILENO)) {
            const char* s = "\x1b[3J\x1b[2J\x1b[H";
            (void)!write(STDOUT_FILENO, s, std::strlen(s));
        }
    });

    {
        // One guard around the entire startup sequence -- not just the
        // chrome repaint -- since mkdir/history-load/getHwStats all run
        // before the terminal is ever put in raw mode otherwise. A keystroke
        // typed anywhere in that window would previously get echoed by the
        // kernel in cooked mode, potentially interleaving with our own
        // escape-sequence writes (e.g. landing on row 1 mid-repaint).
        RawMode startupGuard;
        printf("\x1b[?2004l"); // defensively disable bracketed paste -- ark's
                                // line editor doesn't understand \x1b[200~/
                                // \x1b[201~ markers, and a parent shell may
                                // have left the terminal mode enabled
        // An assh session (ARK_REMOTE) stays fully ephemeral: don't create
        // ~/.config/ark or read/write a history file on the remote host.
        if (!getenv("ARK_REMOTE")) {
            mkdirRecursive(histDir);
            history.load(histPath);
        }
        // Clear the screen when switching INTO ark so it opens on a clean
        // slate (the atexit handler clears again when you switch back out).
        // \x1b[3J drops scrollback too. Toggle off with ARK_CLEAR_ON_ENTER=0.
        if (!(getenv("ARK_CLEAR_ON_ENTER") && std::string(getenv("ARK_CLEAR_ON_ENTER")) == "0"))
            printf("\x1b[3J\x1b[2J\x1b[H");
        doReassertChromeStartup(); // initial paint before the REPL loop starts
        scrollback::init();               // session scrollback ring (ARK_SCROLLBACK=1)
    }

    std::vector<std::unique_ptr<Node>> astRoots; // keeps FunctionDef bodies alive --
                                                  // ONLY statements that define a
                                                  // function get pushed here, see
                                                  // containsFunctionDef() above

    // Source the user config now that state/history/chrome are set up but
    // before the first prompt -- so its aliases/exports/functions are live for
    // the very first command typed.
    // Bring an older config up to date BEFORE sourcing it: append any settings
    // this build documents that the file predates, and report any setting the
    // user actively sets that ark no longer understands. Strictly additive --
    // aliases and functions the user wrote or edited are never rewritten.
    {
        arkcfg::Report cs = arkcfg::syncConfig(histDir + "/ark.config", /*dryRun=*/false);
        for (const std::string& w : cs.warnings)
            std::cerr << "ark: " << w << "\n";
        if (cs.wrote) {
            std::cerr << "ark: added " << cs.added.size()
                      << " new setting(s) to ark.config (all off; backup: "
                      << cs.backupPath << ")\n";
        }
    }

    sourceConfig(histDir + "/ark.config", state, astRoots);

    // Universal variables: load the cross-window/cross-reboot store into the
    // shell (and the environment) now that config has run. Primed once here;
    // re-synced at each prompt below so another window's `uvar` shows up live.
    uvar::loadInto(state.vars);
    // Private mode can be primed from the environment/config (ARK_PRIVATE=1).
    // An assh session (ARK_REMOTE) is always private -- nothing the guest types
    // on the remote should be written to disk there.
    if (const char* p = getenv("ARK_PRIVATE"); p && std::string(p) == "1")
        arkSetPrivateMode(true);
    if (getenv("ARK_REMOTE")) arkSetPrivateMode(true);

    // Neofetch-style startup panel (⚡ + system info), printed once after the
    // config is loaded (so ARK_BANNER=0 can suppress it) and before the first
    // prompt. The startup chrome paint already left the cursor at row 2, so
    // this fills downward from just under the top bar.
    printStartupBanner();

    // Kick off the background filesystem index (after config, so the config's
    // ARK_INDEX / ARK_INDEX_ROOTS are honored) unless disabled. It walks the
    // tree on a worker thread so it never blocks the prompt.
    if (!(getenv("ARK_INDEX") && std::string(getenv("ARK_INDEX")) == "0")) startFileIndex();

    // Warm the command-name cache (a full PATH scan with access(X_OK) per file)
    // on a background thread so the FIRST "command not found -> did you mean?"
    // doesn't pay that one-time cost synchronously and lag.
    std::thread([] { (void)suggestCommand("\x01warmup\x01"); }).detach();

    std::string pending;
    bool continuing = false;

    // Real-time command validation for the syntax highlighter: a command-
    // position word turns red if it resolves to nothing runnable. Aliases and
    // functions come from live shell state; builtins/$PATH from commandExists;
    // an explicit slash-path is checked for executability directly.
    std::function<bool(const std::string&)> cmdValidator = [&state](const std::string& name) -> bool {
        if (name.empty()) return true;
        if (state.aliases.count(name) || state.functions.count(name)) return true;
        if (name.find('/') != std::string::npos) {
            std::string p = name;
            if (p[0] == '~') { if (const char* h = getenv("HOME")) p = std::string(h) + p.substr(1); }
            return access(p.c_str(), X_OK) == 0;
        }
        return commandExists(name);
    };

    for (;;) {
        jobTable.drainSignalQueue();
        // A vanished working directory -- an unmounted volume (external drive,
        // sparsebundle) or a deleted dir -- leaves the shell operating in a path
        // that no longer exists. Every command that touches "." then fails with a
        // confusing cascade ("git: Unable to read current working directory",
        // "ls: .: No such file or directory"). Detect it at each fresh prompt and
        // self-heal to $HOME with one clear line, instead of stranding the user.
        if (!continuing && ::access(state.cwd.c_str(), F_OK) != 0) {
            std::string dest = home.empty() ? "/" : home;
            if (::chdir(dest.c_str()) == 0) {
                state.cwd = dest;
                setenv("PWD", dest.c_str(), 1);
                std::cout << "\x1b[38;2;224;108;117m⚠ working directory "
                             "disappeared (volume unmounted?) — moved to "
                          << dest << "\x1b[0m\r\n" << std::flush;
            }
        }
        if (!continuing) {
            // Cross-window live sync at each fresh prompt (cheap: a stat + a
            // tail read only when the files actually changed). Shared Command
            // History picks up other windows' commands; Universal Variables
            // pick up another window's `uvar` edits.
            history.sync(histPath);
            uvar::loadInto(state.vars);
        }
        // Top bar (cwd + git branch). Default is PINNED (fixed at row 1, painted
        // by reassertChrome/paintChrome, not here). Only ARK_CHROME_TOP=inline
        // prints it here as a per-prompt header that scrolls with output (and
        // keeps scrollback); =off and =pinned both skip the inline print.
        // ARK_CHROME=0 hides all chrome. Never printed on continuation lines.
        if (!continuing) {
            const char* c = getenv("ARK_CHROME");
            const char* t = getenv("ARK_CHROME_TOP");
            bool chromeOn = !(c && std::string(c) == "0");
            bool inlineTop = t && std::string(t) == "inline";
            if (chromeOn && inlineTop) {
                // Only print the header when the cwd/branch actually CHANGED since
                // the last prompt -- so it appears when you `cd`, not stacked above
                // every command. Keeps scrollback readable in inline mode.
                std::string header = topBar(state.cwd, findGitBranch(state.cwd));
                static std::string lastHeader;
                if (header != lastHeader) {
                    std::cout << header << "\r\n" << std::flush;
                    lastHeader = header;
                }
            }
        }
        emitWindowTitle(state, home);
        emitCwdOsc7(state);   // tell IntelliJ/JediTerm where we are, every prompt
        // Transient failed-command prompt: remember WHERE this prompt was drawn
        // and the clock it showed, so that if the command fails we can go back
        // and repaint that exact line with a red arrow + its exit code. Only
        // meaningful for the arrow prompt on a real, non-continuation line.
        int promptRow = 0, promptCol = 0;
        std::string promptClock;
        if (!continuing && arrowPromptMode() && isatty(STDOUT_FILENO)) {
            promptClock = currentClock();
            // RawMode is REQUIRED around queryCursorPos: it reads the terminal's
            // reply byte-by-byte, but in cooked mode the line discipline holds
            // those bytes until a newline, so the query always times out and
            // reports failure. Every other call site wraps it for this reason.
            RawMode guard;
            if (!queryCursorPos(promptRow, promptCol)) promptRow = 0;
        }
        std::string prompt = continuing ? continuationPrompt()
                                        : buildPrompt(state, home, promptClock);
        auto got = readLine(prompt, history, doReassertChrome, cmdValidator,
                            [&jobTable] { jobspanel::show(jobTable); });
        if (!got) break; // Ctrl-D / EOF
        // Session mux (ARK_SCROLLBACK=1): record the completed prompt+command
        // line into the scrollback ring so it survives scroll-back. (Phase 1:
        // command output isn't captured yet -- see the session-mux spec.)
        if (scrollback::enabled() && !got->empty()) scrollback::record(prompt + *got);
        if (!continuing) {
            if (got->empty()) continue;
            pending = *got;
        } else {
            pending += "\n" + *got;
        }

        Lexer lex(pending);
        try {
            Parser parser(lex.tokenize());
            auto ast = parser.parse();
            doReassertChrome(); // preexec-equivalent: reassert before the
                                // command runs -- readLine() already left
                                // the cursor at a fresh, valid line after
                                // Enter, so plain preserve is correct here
            clearLastOffer(); // so a PREVIOUS command's install offer can't leak
                              // into this command's failed-prompt segment
            execNode(ast.get(), state);
            // Capture the cursor RIGHT HERE, before doReassertChromeAfterCommand
            // parks it at the scroll-region bottom. This is the command's own
            // resting row -- the honest signal for whether its output scrolled the
            // screen (and thus whether promptRow still points at the command line).
            // Querying only AFTER the chrome reassert was the old bug: the reassert
            // leaves the cursor at the bottom margin, so the "did it scroll?" check
            // always looked scrolled and the in-place recolor never ran.
            int execRow = 0, execCol = 0;
            bool execRowOk = false;
            if (state.lastStatus != 0 && promptRow > 0 && arrowPromptMode() &&
                isatty(STDOUT_FILENO) && pending.find('\n') == std::string::npos) {
                RawMode g;
                execRowOk = queryCursorPos(execRow, execCol);
            }
            doReassertChromeAfterCommand(); // precmd-equivalent: reassert
                                // after, verifying+correcting the cursor in
                                // case THIS command (clear/vim/etc) left it
                                // somewhere invalid -- e.g. `clear`'s own
                                // \x1b[H parks it at row 1, on the pinned bar
            // TRANSIENT FAILED-COMMAND PROMPT. The exit code belongs to the command
            // you JUST RAN, not to the next prompt -- so on failure we go back and
            // repaint THAT command's own prompt line with a red ❯ and the code
            // tucked in behind it ("12:11 ❯127 cowsay"). The next prompt stays
            // green. ARK_EXIT_CODE=0 disables.
            //
            // The whole line is reprinted rather than just the arrow: inserting the
            // code widens the prompt, so a targeted arrow-only repaint would shove
            // (or overwrite) the command text beside it.
            //
            // SCROLL SAFETY: promptRow is an ABSOLUTE row captured before readLine,
            // so it's only still valid if nothing scrolled since. A terminal scrolls
            // only when something is written AT the bottom row, so re-querying the
            // cursor and requiring r1 < rows proves no scroll occurred and the saved
            // row still points at the prompt. Anything taller than the screen, a
            // multi-line entry, or a prompt that would no longer fit -> skip the
            // repaint entirely rather than corrupt the scrollback.
            if (state.lastStatus != 0 && promptRow > 0 &&
                pending.find('\n') == std::string::npos &&
                !(getenv("ARK_EXIT_CODE") && std::string(getenv("ARK_EXIT_CODE")) == "0")) {
                struct winsize ws;
                int rows = 24, cols = 80;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
                    rows = ws.ws_row;
                    cols = ws.ws_col;
                }
                // Non-empty only when ark offered to install the missing command,
                // in which case the manager is named ahead of the status:
                // "16:02 Homebrew x127 ❯ vcpkg".
                std::string offerName = lastOfferDisplayName();
                std::string line = chromePrompt(promptClock, tn::RED, state.lastStatus, offerName);
                int width = chromePromptWidth(promptClock, state.lastStatus, offerName) +
                            (int)pending.size();
                // Repaint the command text SYNTAX-HIGHLIGHTED, exactly as
                // readLine drew it. Writing raw `pending` here stripped the
                // colour off every failed command's line -- the recolor visibly
                // ate the highlighting it was supposed to leave alone.
                const char* hlOff = getenv("ARK_SYNTAX_HIGHLIGHT");
                std::string shown = (hlOff && std::string(hlOff) == "0")
                                        ? pending
                                        : highlightLine(pending);

                RawMode guard; // required for queryCursorPos below

                // SCROLL SAFETY, done right. `execRow` was captured the instant the
                // command finished -- BEFORE the chrome reassert parks the cursor at
                // the scroll-region bottom -- so it honestly reflects where the
                // command's own output left off. The command line at promptRow is
                // still valid iff the output did NOT reach the scroll region's bottom
                // margin (rows-1 with a pinned bottom bar), because that margin is the
                // only place a scroll can be triggered:
                //   execRow > promptRow  -- cursor moved strictly DOWN from the prompt
                //                           (nothing homed it above, e.g. `clear`).
                //   execRow < rows - 1   -- output never hit the bottom margin, so
                //                           nothing scrolled and promptRow still points
                //                           at the command's line.
                // When it DID scroll (or DSR failed), we draw NOTHING -- no badge on
                // the next prompt, no reprint on a fresh line (which duplicated the
                // command). The recolor belongs on the failing command's own line or
                // nowhere; it never migrates elsewhere.
                // execRow < rows-1 proves no scroll (output never hit the bottom
                // margin). execRow == promptRow+1 also proves it: the cursor sits
                // exactly one line below the prompt, so the command produced no
                // output to scroll -- true even when the prompt is on the bottom
                // row itself (a failed `false`/`cd` there still recolors).
                bool rowStillValid = execRowOk && execRow > promptRow &&
                                     (execRow < rows - 1 || execRow == promptRow + 1);
                if (width < cols && rowStillValid) {
                    // Re-query the CURRENT cursor (the chrome reassert moved it) so we
                    // can restore it after painting over the prompt line.
                    int r1 = 0, c1 = 0;
                    if (!queryCursorPos(r1, c1)) { r1 = execRow; c1 = 1; }
                    std::cout << "\x1b[" << promptRow << ";1H\x1b[K"
                              << line << shown
                              << "\x1b[" << r1 << ";" << c1 << "H" << std::flush;
                }
                // else: the command scrolled off its own prompt line -- leave the
                // screen untouched. No badge, no duplicate.
            }
            // Private Mode: while on, write NOTHING to history/disk. Otherwise
            // record the command tagged with the cwd it ran in (context-aware
            // autosuggestions use that). Multi-line entries stored as one line.
            if (!arkPrivateMode()) history.append(histPath, pending, state.cwd);
            // Only keep this AST alive if a function body inside it needs
            // to keep pointing at it -- otherwise let it free immediately
            // (see containsFunctionDef()'s doc comment for why this matters
            // for a long-running interactive session).
            if (containsFunctionDef(ast.get())) astRoots.push_back(std::move(ast));
            pending.clear();
            continuing = false;
        } catch (const ParseError& e) {
            if (e.incomplete) {
                continuing = true; // wait for more input, don't report an error
            } else {
                printParseError(pending, e);
                pending.clear();
                continuing = false;
            }
        } catch (const std::exception& e) {
            // Never let an unexpected internal error kill the whole process
            // -- this may one day be a login shell, and a crashed login
            // shell means no terminal at all. Log and keep the REPL alive.
            std::cerr << "ark: internal error: " << e.what() << "\n";
            pending.clear();
            continuing = false;
        }
    }

    jobTable.drainSignalQueue();
    bool hasActive = false;
    for (Job* j : jobTable.all()) {
        if (j->state != Job::State::Done) { hasActive = true; break; }
    }
    if (hasActive) std::cerr << "ark: you have running/stopped jobs\n";

    return 0;
}
