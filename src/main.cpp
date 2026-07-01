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
    system(("mkdir -p " + histDir).c_str()); // simplest possible directory-ensure for phase 1

    History history;
    history.load(histPath);

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

    doReassertChrome(); // initial paint before the REPL loop starts
    // Seed the cursor inside the scroll region (row 2) for the very first
    // prompt: reassertChrome()'s save/restore preserves whatever the cursor's
    // TRUE position was, but on a fresh terminal that's row 1 -- outside the
    // scroll region -- since there's no earlier "real" position to restore
    // to yet. Every later reassertChrome() call correctly preserves the
    // actual in-progress cursor position instead; this is a one-time seed.
    printf("\x1b[2;1H");
    fflush(stdout);

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
                                // command runs, in case a PRIOR command
                                // left the terminal in a bad state
            execNode(ast.get(), state);
            doReassertChrome(); // precmd-equivalent: reassert after, in
                                // case THIS command (clear/vim/etc) reset
                                // the scroll region during its own run
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
