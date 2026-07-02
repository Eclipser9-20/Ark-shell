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
