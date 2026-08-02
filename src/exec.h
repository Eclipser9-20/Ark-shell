#pragma once
#include "ast.h"
#include "shell_state.h"
#include <string>

int execNode(Node* node, ShellState& state);

// Runs `cmd` for command substitution ($(...)): forks, lexes/parses/execs it
// through ark's OWN grammar in the child (never an external shell -- ark is
// meant to be fully independent, not a wrapper around bash/zsh), with the
// child's stdout captured via a pipe and returned as a string. Forking gives
// real subshell semantics for free: variable/cd changes made inside $(...)
// happen in the child's copy of ShellState and never leak back to the
// caller, matching bash/zsh. Wired into expand.cpp's CaptureHook once at
// startup (see main.cpp) -- both interactive and non-interactive modes need
// it, since $(...) can appear in scripts too.
std::string captureCommandOutput(const std::string& cmd, ShellState& state);

// The ONE sanctioned way for a builtin to run an external program. Applies the
// same terminal discipline as the normal command path so a builtin-launched
// program is never a second-class citizen:
//   interactive=false -> line output captured into the scrollback ring (via
//                        runForeground), so it is wheel-scrollable afterwards.
//   interactive=true  -> full-screen TUI handoff: the pinned band is released
//                        and ark's mouse reporting silenced, then the program
//                        runs on the real tty. The next prompt reasserts chrome.
// A builtin that ran an external via a raw fork/exec (like the old `ls`) silently
// bypassed scrollback capture -- the "can't scroll ls" bug. Raw fork/exec in
// builtins.cpp is now banned by tests/builtin_spawn_guard.sh; use this instead.
// Returns the child's exit status (127 if it could not be launched).
int runProgramForBuiltin(const std::vector<std::string>& argv, ShellState& state, bool interactive);

// The `let` builtin: evaluates each argument as an arithmetic expression (with
// side effects -- assignment, `++`, etc.), like `(( ))` but as a normal command.
// Exit status is 0 iff the LAST expression evaluates to nonzero (bash semantics).
int builtinLet(const std::vector<std::string>& argv, ShellState& state);
