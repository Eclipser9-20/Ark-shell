#include "builtins.h"
#include <climits>
#include <cstdlib>
#include <iostream>
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

const std::unordered_map<std::string, BuiltinFn>& builtinRegistry() {
    static const std::unordered_map<std::string, BuiltinFn> reg = {
        {"cd", b_cd}, {"exit", b_exit}, {"pwd", b_pwd}, {"echo", b_echo},
        {"export", b_export}, {"unset", b_unset}, {"type", b_type}, {"read", b_read},
    };
    return reg;
}
