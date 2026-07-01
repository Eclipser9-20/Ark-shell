#include "edit.h"
#include "exec.h"
#include "expand.h"
#include "history.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "shell_state.h"
#include <climits>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

// TokyoNight Night palette, matching the zsh/Ghostty/vim theming from
// earlier this session -- blue for the shell name, cyan for cwd, comment-gray
// for the continuation prompt, green/red arrow tracking the last exit status.
namespace tn {
constexpr const char* R = "\x1b[0m";
constexpr const char* BLUE = "\x1b[38;2;122;162;247m";
constexpr const char* CYAN = "\x1b[38;2;125;207;255m";
constexpr const char* GREEN = "\x1b[38;2;158;206;106m";
constexpr const char* RED = "\x1b[38;2;247;118;142m";
constexpr const char* COMMENT = "\x1b[38;2;86;95;137m";
} // namespace tn

static std::string shortCwd(const std::string& cwd, const std::string& home) {
    if (!home.empty() && cwd.rfind(home, 0) == 0) return "~" + cwd.substr(home.size());
    return cwd;
}

static std::string buildPrompt(const ShellState& state, const std::string& home) {
    std::string arrowColor = state.lastStatus == 0 ? tn::GREEN : tn::RED;
    return std::string(tn::BLUE) + "ark " + tn::CYAN + shortCwd(state.cwd, home) + " " +
           arrowColor + "\xe2\x9d\xaf" + tn::R + " "; // "\xe2\x9d\xaf" = UTF-8 for ❯
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

    std::vector<std::unique_ptr<Node>> astRoots; // keeps FunctionDef bodies alive
    std::string pending;
    bool continuing = false;

    for (;;) {
        jobTable.drainSignalQueue();
        std::string prompt = continuing ? continuationPrompt() : buildPrompt(state, home);
        auto got = readLine(prompt, history);
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
            execNode(ast.get(), state);
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
