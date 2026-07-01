#pragma once
#include "history.h"
#include <optional>
#include <string>

// Reads one line interactively using raw termios input, with history recall
// (Up/Down) and basic cursor movement (Left/Right/Backspace). Returns
// nullopt on EOF (Ctrl-D on an empty line).
std::optional<std::string> readLine(const std::string& prompt, History& history);
