#include "exec.h"
#include "builtins.h"
#include "expand.h"
#include "jobs.h"
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

extern char** environ;

static void applyRedirectsFileActions(const std::vector<Redirect>& redirects, posix_spawn_file_actions_t& actions) {
    for (const auto& r : redirects) {
        switch (r.kind) {
            case Redirect::Kind::In:
                posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, r.target.c_str(), O_RDONLY, 0);
                break;
            case Redirect::Kind::Out:
                posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                break;
            case Redirect::Kind::Append:
                posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, r.target.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                break;
            case Redirect::Kind::ErrOut:
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                break;
        }
    }
}

static void applyRedirectsInChild(const std::vector<Redirect>& redirects) {
    for (const auto& r : redirects) {
        int fd = -1, target = -1;
        switch (r.kind) {
            case Redirect::Kind::In: fd = open(r.target.c_str(), O_RDONLY); target = STDIN_FILENO; break;
            case Redirect::Kind::Out: fd = open(r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); target = STDOUT_FILENO; break;
            case Redirect::Kind::Append: fd = open(r.target.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); target = STDOUT_FILENO; break;
            case Redirect::Kind::ErrOut: fd = open(r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); target = STDERR_FILENO; break;
        }
        if (fd != -1) { dup2(fd, target); close(fd); }
    }
}

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
    // Explicit redirects on this stage are added AFTER the pipe-wiring
    // dup2/close actions above, so if a stage has both an incoming/outgoing
    // pipe AND its own explicit redirect targeting the same fd, the redirect
    // wins (matches real shell behavior) -- though in practice the only
    // stages where both apply are a first stage with `<` (no previous pipe)
    // or a last stage with `>` (no next pipe), so there's no real conflict.
    applyRedirectsFileActions(cmd->redirects, actions);

    if (it != reg.end()) {
        // Builtins run in-process normally, but inside a pipeline stage they
        // need their own fd redirection, so fork a child specifically for this.
        pid_t pid = fork();
        if (pid == 0) {
            if (inFd != -1) { dup2(inFd, STDIN_FILENO); close(inFd); }
            if (outFd != -1) { dup2(outFd, STDOUT_FILENO); close(outFd); }
            applyRedirectsInChild(cmd->redirects);
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

        // All stages share one process group (the first stage's pid is the
        // group leader). Both parent and child call setpgid on the same
        // target -- the standard double-call pattern that closes the race
        // where the parent might hand the terminal to the group before the
        // child has actually joined it.
        if (pids[i] > 0) {
            pid_t pgid = pids[0];
            setpgid(pids[i], pgid);
        }

        if (prevReadFd != -1) close(prevReadFd);
        if (hasNext) { close(pipeFds[1]); prevReadFd = pipeFds[0]; }
    }

    pid_t jobPgid = pids.empty() ? -1 : pids[0];

    pid_t shellPgid = getpgrp();
    if (jobPgid > 0) tcsetpgrp(STDIN_FILENO, jobPgid);

    int status = 0;
    for (pid_t pid : pids) {
        if (pid == -1) continue;
        int st = 0;
        waitpid(pid, &st, 0);
        status = WIFEXITED(st) ? WEXITSTATUS(st) : 1; // pipeline status = last stage's
    }

    tcsetpgrp(STDIN_FILENO, shellPgid);
    return status;
}

static int runFunctionDef(Node* fn, ShellState& state) {
    state.functions[fn->funcName] = fn->funcBody.get();
    return 0;
}

static int callFunction(Node* body, const std::vector<std::string>& argv, ShellState& state) {
    std::vector<std::string> params(argv.begin() + 1, argv.end()); // argv[0] is the function name
    state.argStack.push_back(params);
    int status = execNode(body, state);
    state.argStack.pop_back();
    return status;
}

static int runCommand(Node* cmd, ShellState& state) {
    auto argv = expandWords(cmd->words, state);
    if (argv.empty()) return 0;

    auto fnIt = state.functions.find(argv[0]);
    if (fnIt != state.functions.end()) {
        return callFunction(fnIt->second, argv, state);
    }

    auto& reg = builtinRegistry();
    auto it = reg.find(argv[0]);
    if (it != reg.end() && cmd->redirects.empty()) {
        return it->second(argv, state);
    }
    if (it != reg.end()) {
        // Builtin with redirects: fork so the redirect doesn't leak into the
        // interactive shell's own stdout/stdin.
        pid_t pid = fork();
        if (pid == 0) {
            applyRedirectsInChild(cmd->redirects);
            int rc = it->second(argv, state);
            std::cout.flush(); // same _exit()-skips-iostream-flush issue as
                                // the pipeline builtin-fork path (Task 13)
            _exit(rc);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    applyRedirectsFileActions(cmd->redirects, actions);

    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        std::cerr << argv[0] << ": command not found\n";
        return 127;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static bool globMatch(const std::string& pattern, const std::string& text) {
    // Minimal glob: supports '*' and literal chars only (no '?'/'[...]' in phase 1).
    size_t p = 0, t = 0, star = std::string::npos, match = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == text[t])) { p++; t++; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; match = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++match; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

static int runIf(Node* ifn, ShellState& state) {
    int cond = execNode(ifn->children[0].get(), state);
    if (cond == 0) return execNode(ifn->children[1].get(), state);
    if (ifn->children.size() > 2) return execNode(ifn->children[2].get(), state);
    return 0;
}

static int runWhile(Node* wn, ShellState& state) {
    int status = 0;
    while (execNode(wn->children[0].get(), state) == 0) {
        status = execNode(wn->children[1].get(), state);
    }
    return status;
}

static int runFor(Node* fn, ShellState& state) {
    int status = 0;
    for (const auto& raw : fn->forWords) {
        state.vars[fn->forVar] = expandWord(raw, state);
        status = execNode(fn->children[0].get(), state);
    }
    return status;
}

static int runCase(Node* cn, ShellState& state) {
    std::string word = expandWord(cn->caseWord, state);
    for (auto& clause : cn->caseClauses) {
        if (globMatch(clause.first, word)) {
            return execNode(clause.second.get(), state);
        }
    }
    return 0;
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
        case NodeKind::If:
            return runIf(node, state);
        case NodeKind::While:
            return runWhile(node, state);
        case NodeKind::For:
            return runFor(node, state);
        case NodeKind::Case:
            return runCase(node, state);
        case NodeKind::FunctionDef:
            return runFunctionDef(node, state);
        default:
            std::cerr << "ark: internal error: unimplemented node kind\n";
            return 1;
    }
}
