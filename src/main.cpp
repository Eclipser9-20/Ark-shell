#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "shell_state.h"
#include <climits>
#include <csignal>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

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

    // Read the whole input as one program rather than line-by-line: control
    // flow (if/while/for/case) is inherently multi-line, and parsing a single
    // line like "while cond ; do" in isolation always fails (no matching
    // 'done' on that line yet). The interactive line editor (Task 19) handles
    // the interactive case differently (one line at a time with its own
    // continuation-prompt handling); this is the non-interactive/piped path.
    std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());

    Lexer lex(source);
    Parser parser(lex.tokenize());
    auto ast = parser.parse();
    execNode(ast.get(), state);
    return 0;
}
