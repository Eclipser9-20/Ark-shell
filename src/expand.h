#pragma once
#include "shell_state.h"
#include <functional>
#include <string>
#include <vector>

std::string expandWord(const std::string& word, const ShellState& state);
std::vector<std::string> expandWords(const std::vector<std::string>& words, ShellState& state);

// The executor (Task 12) provides the real implementation: run `cmd` in a
// subshell and return its captured stdout. Set once at startup; expand.cpp
// calls through this hook so expand.h has no dependency on exec.h.
using CaptureHook = std::function<std::string(const std::string&)>;
void setCaptureHook(CaptureHook hook);
