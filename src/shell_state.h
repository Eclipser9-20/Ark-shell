#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct Node; // forward-declared; ast.h isn't included here to avoid a cycle
class JobTable; // forward-declared; introduced in Task 17

struct ShellState {
    std::unordered_map<std::string, std::string> vars;
    std::unordered_map<std::string, Node*> functions; // non-owning; owner lives in main.cpp's AST-root list
    std::vector<std::vector<std::string>> argStack;    // positional params per call frame
    int lastStatus = 0;
    std::string cwd;
    JobTable* jobs = nullptr; // non-owning; owned by main.cpp
};
