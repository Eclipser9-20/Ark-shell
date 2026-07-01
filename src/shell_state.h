#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct Node; // forward-declared; ast.h isn't included here to avoid a cycle
class JobTable; // forward-declared; introduced in Task 17

struct ShellState {
    std::unordered_map<std::string, std::string> vars;
    std::unordered_map<std::string, std::string> aliases; // name -> replacement text
    std::unordered_map<std::string, Node*> functions; // non-owning; owner lives in main.cpp's AST-root list
    std::vector<std::vector<std::string>> argStack;    // positional params per call frame
    std::vector<std::string> dirStack;                 // pushd/popd directory stack (top = back())

    // `return` control flow: the return builtin sets returnFlag+returnStatus;
    // runList and the loop/if executors stop when it's set, and callFunction
    // consumes it (converting it back into the call's status).
    bool returnFlag = false;
    int returnStatus = 0;

    // `local` variable scopes -- one per active function call. Each records a
    // name's prior state (whether it existed and its value) so the variable
    // is restored when the function returns.
    struct SavedVar { bool existed; std::string value; };
    std::vector<std::unordered_map<std::string, SavedVar>> localScopes;

    int lastStatus = 0;
    std::string cwd;
    JobTable* jobs = nullptr; // non-owning; owned by main.cpp
};
