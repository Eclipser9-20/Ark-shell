#pragma once
#include "ast.h"
#include "shell_state.h"

int execNode(Node* node, ShellState& state);
