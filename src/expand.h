#pragma once
#include "shell_state.h"
#include <functional>
#include <string>
#include <vector>

std::string expandWord(const std::string& word, const ShellState& state);
std::vector<std::string> expandWords(const std::vector<std::string>& words, ShellState& state);

// Expands ONE word to a single string with full expansion (parameters,
// command substitution, tilde, arithmetic) and quote handling, but WITHOUT
// word-splitting or globbing. This is exactly the semantics an assignment's
// right-hand side needs (`x=$foo bar` never splits the value on the space --
// though the lexer already split that into two words anyway; the real case
// is `x="$foo bar"` or `x=$foo` keeping the value intact). Also the right
// primitive for any other single-value context added later.
std::string expandNoSplit(const std::string& word, ShellState& state);

// The executor (Task 12) provides the real implementation: run `cmd` in a
// subshell and return its captured stdout. Set once at startup; expand.cpp
// calls through this hook so expand.h has no dependency on exec.h.
using CaptureHook = std::function<std::string(const std::string&)>;
void setCaptureHook(CaptureHook hook);
