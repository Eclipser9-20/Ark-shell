#pragma once
#include "jobs.h"
#include "shell_state.h"
#include <string>
#include <unordered_map>
#include <vector>

using BuiltinFn = int (*)(const std::vector<std::string>& argv, ShellState& state);

const std::unordered_map<std::string, BuiltinFn>& builtinRegistry();

// The default ~/.config/ark/ark.config contents: a commented catalogue of
// everything ark can do, ready to uncomment. Written on first run (by both
// startup and the ark-settings builtin) if no config exists.
const char* arkDefaultConfig();
