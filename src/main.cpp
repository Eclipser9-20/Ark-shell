#include "chrome.h"
#include "edit.h"
#include "exec.h"
#include "expand.h"
#include "history.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "shell_state.h"
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// TokyoNight Night palette, matching the zsh/Ghostty/vim theming from
// earlier this session -- comment-gray for the time/continuation prompt,
// green/red arrow tracking the last exit status.
namespace tn {
constexpr const char* R = "\x1b[0m";
constexpr const char* GREEN = "\x1b[38;2;158;206;106m";
constexpr const char* RED = "\x1b[38;2;247;118;142m";
constexpr const char* COMMENT = "\x1b[38;2;86;95;137m";
} // namespace tn

static std::string buildPrompt(const ShellState& state, const std::string&) {
    // cwd now lives in the pinned top bar (chrome.h's paintChrome), so the
    // per-command prompt simplifies to just the time and the status arrow.
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char clock[8];
    strftime(clock, sizeof(clock), "%H:%M", &local);
    std::string arrowColor = state.lastStatus == 0 ? tn::GREEN : tn::RED;
    return std::string(tn::COMMENT) + clock + " " + arrowColor + "\xe2\x9d\xaf" + tn::R + " ";
}

static std::string continuationPrompt() {
    return std::string(tn::COMMENT) + "\xe2\x80\xba" + tn::R + " "; // "\xe2\x80\xba" = UTF-8 for ›
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

int main() {
    ShellState state;
    JobTable jobTable;
    state.jobs = &jobTable;

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
        reassertChrome(state.cwd, findGitBranch(state.cwd), sessionSeconds(), getHwStats(),
                        CursorPolicy::VerifyAndCorrect);
    };

    // The handler only flips an atomic flag -- printf/getHwStats/file I/O
    // are not async-signal-safe, and calling them directly from a signal
    // handler risks deadlock if SIGWINCH arrives mid-printf elsewhere (same
    // class of bug as the SIGCHLD handler earlier this session). The main
    // loop polls the flag once per iteration and does the real repaint.
    static std::atomic<bool> g_resized{false};
    struct sigaction winch;
    std::memset(&winch, 0, sizeof(winch));
    winch.sa_handler = [](int) { g_resized.store(true, std::memory_order_relaxed); };
    sigemptyset(&winch.sa_mask);
    winch.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &winch, nullptr);

    // installIdleTicker() arms a 1-second repeating SIGALRM so the pinned
    // bar keeps ticking live even during a fast typing burst or a terminal
    // paste -- see edit.cpp for why a select()-timeout-based approach alone
    // isn't enough (it only fires in GAPS between keystrokes, so continuous
    // input starves it indefinitely).
    installIdleTicker();

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
        mkdirRecursive(histDir);
        history.load(histPath);
        doReassertChromeStartup(); // initial paint before the REPL loop starts
    }

    std::vector<std::unique_ptr<Node>> astRoots; // keeps FunctionDef bodies alive
    std::string pending;
    bool continuing = false;

    for (;;) {
        jobTable.drainSignalQueue();
        if (g_resized.exchange(false, std::memory_order_relaxed)) doReassertChrome();
        std::string prompt = continuing ? continuationPrompt() : buildPrompt(state, home);
        auto got = readLine(prompt, history, doReassertChrome);
        if (!got) break; // Ctrl-D / EOF
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
            execNode(ast.get(), state);
            doReassertChromeAfterCommand(); // precmd-equivalent: reassert
                                // after, verifying+correcting the cursor in
                                // case THIS command (clear/vim/etc) left it
                                // somewhere invalid -- e.g. `clear`'s own
                                // \x1b[H parks it at row 1, on the pinned bar
            history.append(histPath, pending); // multi-line entries are stored as one history line
            astRoots.push_back(std::move(ast));
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
