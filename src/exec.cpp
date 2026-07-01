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

static int runPipelineStage(Node* cmd, ShellState& state, int inFd, int outFd, pid_t& pidOut) {
    auto argv = expandWords(cmd->words, state);
    if (argv.empty()) { pidOut = -1; return 0; }

    auto& reg = builtinRegistry();
    auto it = reg.find(argv[0]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (inFd != -1) {
        posix_spawn_file_actions_adddup2(&actions, inFd, STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, inFd);
    }
    if (outFd != -1) {
        posix_spawn_file_actions_adddup2(&actions, outFd, STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, outFd);
    }

    if (it != reg.end()) {
        // Builtins run in-process normally, but inside a pipeline stage they
        // need their own fd redirection, so fork a child specifically for this.
        pid_t pid = fork();
        if (pid == 0) {
            if (inFd != -1) { dup2(inFd, STDIN_FILENO); close(inFd); }
            if (outFd != -1) { dup2(outFd, STDOUT_FILENO); close(outFd); }
            int rc = it->second(argv, state);
            std::cout.flush(); // _exit() below skips iostream buffer flushing
                                // entirely -- without this, a builtin's output
                                // is silently lost whenever stdout is a pipe
                                // (fully buffered) rather than a terminal
                                // (line-buffered), since the buffer never gets
                                // written to the fd before the process dies.
            posix_spawn_file_actions_destroy(&actions);
            _exit(rc);
        }
        posix_spawn_file_actions_destroy(&actions);
        pidOut = pid;
        return 0;
    }

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        std::cerr << argv[0] << ": command not found\n";
        pidOut = -1;
        return 127;
    }
    pidOut = pid;
    return 0;
}

static int runPipeline(Node* pipeline, ShellState& state) {
    size_t n = pipeline->children.size();
    std::vector<pid_t> pids(n, -1);
    int prevReadFd = -1;

    for (size_t i = 0; i < n; i++) {
        int pipeFds[2] = {-1, -1};
        bool hasNext = i + 1 < n;
        if (hasNext) pipe(pipeFds);

        int inFd = prevReadFd;
        int outFd = hasNext ? pipeFds[1] : -1;

        runPipelineStage(pipeline->children[i].get(), state, inFd, outFd, pids[i]);

        if (prevReadFd != -1) close(prevReadFd);
        if (hasNext) { close(pipeFds[1]); prevReadFd = pipeFds[0]; }
    }

    int status = 0;
    for (pid_t pid : pids) {
        if (pid == -1) continue;
        int st = 0;
        waitpid(pid, &st, 0);
        status = WIFEXITED(st) ? WEXITSTATUS(st) : 1; // pipeline status = last stage's
    }
    return status;
}

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
        case NodeKind::Pipeline:
            return runPipeline(node, state);
        default:
            // Pipeline/If/While/For/Case/FunctionDef land here starting
            // Tasks 13/15/16 — until then, executing one is a plan bug,
            // not a runtime input a phase-1 test script should produce.
            std::cerr << "ark: internal error: unimplemented node kind\n";
            return 1;
    }
}
