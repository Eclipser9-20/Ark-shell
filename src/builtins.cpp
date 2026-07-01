#include "builtins.h"
#include <climits>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int b_cd(const std::vector<std::string>& argv, ShellState& state) {
    std::string target = argv.size() > 1 ? argv[1] : (getenv("HOME") ? getenv("HOME") : "/");
    if (::chdir(target.c_str()) != 0) {
        std::cerr << "cd: " << target << ": No such file or directory\n";
        return 1;
    }
    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf))) state.cwd = buf;
    return 0;
}

static int b_exit(const std::vector<std::string>& argv, ShellState& state) {
    int code = argv.size() > 1 ? std::atoi(argv[1].c_str()) : state.lastStatus;
    std::exit(code);
}

static int b_pwd(const std::vector<std::string>&, ShellState& state) {
    std::cout << state.cwd << "\n";
    return 0;
}

static int b_echo(const std::vector<std::string>& argv, ShellState&) {
    for (size_t i = 1; i < argv.size(); i++) {
        std::cout << argv[i];
        if (i + 1 < argv.size()) std::cout << " ";
    }
    std::cout << "\n";
    return 0;
}

static int b_export(const std::vector<std::string>& argv, ShellState& state) {
    for (size_t i = 1; i < argv.size(); i++) {
        auto eq = argv[i].find('=');
        if (eq != std::string::npos) {
            std::string name = argv[i].substr(0, eq);
            std::string val = argv[i].substr(eq + 1);
            state.vars[name] = val;
            ::setenv(name.c_str(), val.c_str(), 1);
        }
    }
    return 0;
}

static int b_unset(const std::vector<std::string>& argv, ShellState& state) {
    for (size_t i = 1; i < argv.size(); i++) {
        state.vars.erase(argv[i]);
        ::unsetenv(argv[i].c_str());
    }
    return 0;
}

static int b_type(const std::vector<std::string>& argv, ShellState&) {
    if (argv.size() < 2) return 1;
    auto& reg = builtinRegistry();
    if (reg.find(argv[1]) != reg.end()) {
        std::cout << argv[1] << " is a shell builtin\n";
        return 0;
    }
    std::cout << argv[1] << " not found\n";
    return 1;
}

static int b_read(const std::vector<std::string>& argv, ShellState& state) {
    std::string line;
    if (!std::getline(std::cin, line)) return 1;
    if (argv.size() > 1) state.vars[argv[1]] = line;
    return 0;
}

static int b_jobs(const std::vector<std::string>&, ShellState& state) {
    if (!state.jobs) return 0;
    state.jobs->drainSignalQueue();
    for (Job* j : state.jobs->all()) {
        const char* stateName = j->state == Job::State::Running ? "Running"
                               : j->state == Job::State::Stopped ? "Stopped" : "Done";
        std::cout << "[" << j->id << "] " << stateName << "  " << j->cmdline << "\n";
    }
    return 0;
}

static int b_fg(const std::vector<std::string>& argv, ShellState& state) {
    if (!state.jobs || argv.size() < 2) return 1;
    Job* j = state.jobs->find(std::atoi(argv[1].c_str()));
    if (!j) { std::cerr << "fg: no such job\n"; return 1; }
    BlockSigchld guard; // same reap-race guard as exec.cpp's foreground waits
    pid_t shellPgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, j->pgid);
    kill(-j->pgid, SIGCONT);
    int status = 0;
    for (pid_t pid : j->pids) waitpidRetry(pid, &status, 0);
    tcsetpgrp(STDIN_FILENO, shellPgid);
    state.jobs->remove(j->id);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int b_bg(const std::vector<std::string>& argv, ShellState& state) {
    if (!state.jobs || argv.size() < 2) return 1;
    Job* j = state.jobs->find(std::atoi(argv[1].c_str()));
    if (!j) { std::cerr << "bg: no such job\n"; return 1; }
    kill(-j->pgid, SIGCONT);
    j->state = Job::State::Running;
    return 0;
}

const std::unordered_map<std::string, BuiltinFn>& builtinRegistry() {
    static const std::unordered_map<std::string, BuiltinFn> reg = {
        {"cd", b_cd}, {"exit", b_exit}, {"pwd", b_pwd}, {"echo", b_echo},
        {"export", b_export}, {"unset", b_unset}, {"type", b_type}, {"read", b_read},
        {"jobs", b_jobs}, {"fg", b_fg}, {"bg", b_bg},
    };
    return reg;
}
