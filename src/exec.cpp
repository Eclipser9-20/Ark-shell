#include "exec.h"
#include "builtins.h"
#include "expand.h"
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

static int runCommand(Node* cmd, ShellState& state) {
    auto argv = expandWords(cmd->words, state);
    if (argv.empty()) return 0;

    auto& reg = builtinRegistry();
    auto it = reg.find(argv[0]);
    if (it != reg.end()) {
        return it->second(argv, state);
    }

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), environ);
    if (rc != 0) {
        std::cerr << argv[0] << ": command not found\n";
        return 127;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int runList(Node* list, ShellState& state) {
    int status = 0;
    for (size_t i = 0; i < list->children.size(); i++) {
        Node* stmt = list->children[i].get();
        JoinOp prevJoin = i > 0 ? list->children[i - 1]->joinOp : JoinOp::None;
        if (prevJoin == JoinOp::And && status != 0) continue;
        if (prevJoin == JoinOp::Or && status == 0) continue;
        status = execNode(stmt, state);
        state.lastStatus = status;
    }
    return status;
}

int execNode(Node* node, ShellState& state) {
    switch (node->kind) {
        case NodeKind::List:
            return runList(node, state);
        case NodeKind::Command:
            return runCommand(node, state);
        default:
            // Pipeline/If/While/For/Case/FunctionDef land here starting
            // Tasks 13/15/16 — until then, executing one is a plan bug,
            // not a runtime input a phase-1 test script should produce.
            std::cerr << "ark: internal error: unimplemented node kind\n";
            return 1;
    }
}
