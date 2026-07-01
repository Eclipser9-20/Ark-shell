#pragma once
#include "shell_state.h"
#include <string>
#include <unordered_map>
#include <vector>

using BuiltinFn = int (*)(const std::vector<std::string>& argv, ShellState& state);

const std::unordered_map<std::string, BuiltinFn>& builtinRegistry();
