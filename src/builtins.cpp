#include "builtins.h"
#include "exec.h"
#include "lexer.h"
#include "parser.h"
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int b_cd(const std::vector<std::string>& argv, ShellState& state) {
    std::string target = argv.size() > 1 ? argv[1] : (getenv("HOME") ? getenv("HOME") : "/");
    // `cd -` returns to the previous directory (OLDPWD) and echoes it, like
    // bash/zsh. OLDPWD is tracked in the shell's own vars (and mirrored to the
    // environment so child processes see it).
    bool printResult = false;
    if (target == "-") {
        auto it = state.vars.find("OLDPWD");
        if (it == state.vars.end() || it->second.empty()) {
            std::cerr << "cd: OLDPWD not set\n";
            return 1;
        }
        target = it->second;
        printResult = true;
    }
    std::string prevCwd = state.cwd;
    if (::chdir(target.c_str()) != 0) {
        std::cerr << "cd: " << target << ": No such file or directory\n";
        return 1;
    }
    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf))) state.cwd = buf;
    state.vars["OLDPWD"] = prevCwd;
    ::setenv("OLDPWD", prevCwd.c_str(), 1);
    ::setenv("PWD", state.cwd.c_str(), 1);
    if (printResult) std::cout << state.cwd << "\n";
    return 0;
}

static int b_exit(const std::vector<std::string>& argv, ShellState& state) {
    int code = argv.size() > 1 ? std::atoi(argv[1].c_str()) : state.lastStatus;
    std::exit(code);
}

// Shared cd primitive for pushd/popd (does the chdir + OLDPWD/PWD bookkeeping
// without the `cd`-specific arg parsing). Returns true on success.
static bool changeDir(const std::string& target, ShellState& state) {
    std::string prev = state.cwd;
    if (::chdir(target.c_str()) != 0) {
        std::cerr << "cd: " << target << ": No such file or directory\n";
        return false;
    }
    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf))) state.cwd = buf;
    state.vars["OLDPWD"] = prev;
    ::setenv("OLDPWD", prev.c_str(), 1);
    ::setenv("PWD", state.cwd.c_str(), 1);
    return true;
}

// Prints the directory stack (current dir first, then the stack top-down),
// space-separated -- like `dirs` in bash/zsh.
static void printDirStack(const ShellState& state) {
    std::cout << state.cwd;
    for (auto it = state.dirStack.rbegin(); it != state.dirStack.rend(); ++it) std::cout << " " << *it;
    std::cout << "\n";
}

static int b_pushd(const std::vector<std::string>& argv, ShellState& state) {
    if (argv.size() < 2) {
        // No arg: swap the current dir with the top of the stack.
        if (state.dirStack.empty()) { std::cerr << "pushd: no other directory\n"; return 1; }
        std::string top = state.dirStack.back();
        std::string here = state.cwd;
        if (!changeDir(top, state)) return 1;
        state.dirStack.back() = here;
        printDirStack(state);
        return 0;
    }
    std::string here = state.cwd;
    if (!changeDir(argv[1], state)) return 1;
    state.dirStack.push_back(here);
    printDirStack(state);
    return 0;
}

static int b_popd(const std::vector<std::string>&, ShellState& state) {
    if (state.dirStack.empty()) { std::cerr << "popd: directory stack empty\n"; return 1; }
    std::string target = state.dirStack.back();
    state.dirStack.pop_back();
    if (!changeDir(target, state)) return 1;
    printDirStack(state);
    return 0;
}

static int b_dirs(const std::vector<std::string>&, ShellState& state) {
    printDirStack(state);
    return 0;
}

// Opens ~/.config/ark/ark.config in an editor. Prefers Pistin (Gideon's
// terminal IDE), then $EDITOR, then vi -- so the config is edited in the
// intended tool but still works on a bare box. Creates the file (with a
// commented template) if it doesn't exist yet, so the editor always opens
// something. Runs the editor as a normal foreground child.
// `source FILE` / `. FILE`: run FILE's commands in the CURRENT shell (so its
// assignments, aliases, functions, and cd's affect this session), unlike
// executing it as a subprocess. The parsed AST is retained in a session-
// lifetime static so any functions it defines keep working after source
// returns.
// `return [n]`: stop the current function (or sourced script) and set its
// status to n (default: last status). Sets a flag that runList and the loop
// executors honor, unwinding back to callFunction which consumes it.
static int b_break(const std::vector<std::string>& argv, ShellState& state) {
    if (state.loopDepth == 0) { std::cerr << "break: only meaningful in a `for' or `while' loop\n"; return 1; }
    state.loopCtl = ShellState::LoopCtl::Break;
    state.loopCtlLevels = argv.size() > 1 ? std::max(1, std::atoi(argv[1].c_str())) : 1;
    return 0;
}

static int b_continue(const std::vector<std::string>& argv, ShellState& state) {
    if (state.loopDepth == 0) { std::cerr << "continue: only meaningful in a `for' or `while' loop\n"; return 1; }
    state.loopCtl = ShellState::LoopCtl::Continue;
    state.loopCtlLevels = argv.size() > 1 ? std::max(1, std::atoi(argv[1].c_str())) : 1;
    return 0;
}

static int b_return(const std::vector<std::string>& argv, ShellState& state) {
    state.returnStatus = argv.size() > 1 ? std::atoi(argv[1].c_str()) : state.lastStatus;
    state.returnFlag = true;
    return state.returnStatus;
}

// `local NAME[=value] ...`: declares function-scoped variables. Each name's
// prior state is saved in the current function's local scope and restored
// when the function returns (see callFunction). Outside a function it behaves
// like a plain assignment (with a warning), matching common shells.
static int b_local(const std::vector<std::string>& argv, ShellState& state) {
    bool inFunction = !state.localScopes.empty();
    if (!inFunction && argv.size() > 1) {
        std::cerr << "local: can only be used in a function\n";
    }
    for (size_t i = 1; i < argv.size(); i++) {
        auto eq = argv[i].find('=');
        std::string name = eq == std::string::npos ? argv[i] : argv[i].substr(0, eq);
        std::string val = eq == std::string::npos ? "" : argv[i].substr(eq + 1);
        if (inFunction) {
            auto& scope = state.localScopes.back();
            if (scope.find(name) == scope.end()) { // first `local` for this name in this call
                auto vit = state.vars.find(name);
                scope[name] = ShellState::SavedVar{vit != state.vars.end(), vit != state.vars.end() ? vit->second : std::string()};
            }
        }
        state.vars[name] = val;
    }
    return 0;
}

static int b_source(const std::vector<std::string>& argv, ShellState& state) {
    if (argv.size() < 2) { std::cerr << "source: filename argument required\n"; return 1; }
    std::ifstream f(argv[1]);
    if (!f.is_open()) { std::cerr << "source: " << argv[1] << ": cannot open\n"; return 1; }
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static std::vector<std::unique_ptr<Node>> sourcedRoots; // keep function bodies alive for the session
    try {
        Lexer lex(src);
        Parser p(lex.tokenize());
        auto ast = p.parse();
        int rc = execNode(ast.get(), state);
        sourcedRoots.push_back(std::move(ast));
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "source: " << argv[1] << ": " << e.what() << "\n";
        return 1;
    }
}

static int b_ark_settings(const std::vector<std::string>&, ShellState&) {
    const char* home = getenv("HOME");
    std::string dir = std::string(home ? home : "") + "/.config/ark";
    std::string cfg = dir + "/ark.config";

    ::mkdir((std::string(home ? home : "") + "/.config").c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    struct stat st;
    if (stat(cfg.c_str(), &st) != 0) {
        std::ofstream f(cfg);
        f << "# ark.config -- sourced at startup (interactive sessions).\n"
             "# Aliases, exports, and functions here persist across sessions.\n"
             "#\n"
             "# Cross-directory completion: list dirs to search from anywhere\n"
             "# (a program in ~/bin then completes even from ~/projects):\n"
             "# export ARK_SEARCH_DIRS=\"$HOME/bin:$HOME/projects\"\n"
             "#\n"
             "# alias ll='ls -la'\n"
             "# export EDITOR=pistin\n";
    }

    std::string editor;
    if (access("/usr/local/bin/pistin", X_OK) == 0) editor = "/usr/local/bin/pistin";
    else {
        std::string p = std::string(home ? home : "") + "/Pistin/pistin";
        if (access(p.c_str(), X_OK) == 0) editor = p;
    }
    if (editor.empty()) { const char* e = getenv("EDITOR"); editor = (e && *e) ? e : "vi"; }

    BlockSigchld guard;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        execlp(editor.c_str(), editor.c_str(), cfg.c_str(), (char*)nullptr);
        std::cerr << "ark-settings: could not launch " << editor << "\n";
        _exit(127);
    }
    int status = 0;
    waitpidRetry(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int b_pwd(const std::vector<std::string>&, ShellState& state) {
    std::cout << state.cwd << "\n";
    return 0;
}

// Interprets the standard backslash escapes echo -e supports.
static std::string echoEscapes(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[++i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case 'a': out += '\a'; break;
                case 'b': out += '\b'; break;
                case '0': out += '\0'; break;
                default: out += '\\'; out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

static int b_echo(const std::vector<std::string>& argv, ShellState&) {
    // Parse leading flags: -n (no trailing newline), -e (interpret escapes),
    // -E (don't). Multiple/combined (-ne) allowed, like bash.
    size_t start = 1;
    bool newline = true, escapes = false;
    while (start < argv.size() && argv[start].size() >= 2 && argv[start][0] == '-') {
        const std::string& f = argv[start];
        bool allFlagChars = true;
        for (size_t k = 1; k < f.size(); k++) if (f[k] != 'n' && f[k] != 'e' && f[k] != 'E') { allFlagChars = false; break; }
        if (!allFlagChars) break;
        for (size_t k = 1; k < f.size(); k++) {
            if (f[k] == 'n') newline = false;
            else if (f[k] == 'e') escapes = true;
            else if (f[k] == 'E') escapes = false;
        }
        start++;
    }
    for (size_t i = start; i < argv.size(); i++) {
        std::cout << (escapes ? echoEscapes(argv[i]) : argv[i]);
        if (i + 1 < argv.size()) std::cout << " ";
    }
    if (newline) std::cout << "\n";
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

static int b_alias(const std::vector<std::string>& argv, ShellState& state) {
    if (argv.size() == 1) {
        // No args: list all aliases, in `alias name='value'` form.
        for (const auto& [name, value] : state.aliases) {
            std::cout << "alias " << name << "='" << value << "'\n";
        }
        return 0;
    }
    int rc = 0;
    for (size_t i = 1; i < argv.size(); i++) {
        auto eq = argv[i].find('=');
        if (eq == std::string::npos) {
            // `alias name` -- show that one alias.
            auto it = state.aliases.find(argv[i]);
            if (it != state.aliases.end()) std::cout << "alias " << it->first << "='" << it->second << "'\n";
            else { std::cerr << "alias: " << argv[i] << ": not found\n"; rc = 1; }
        } else {
            // `alias name=value` -- define. The value arrived already
            // unquoted by the lexer/expander (alias ll='ls -la' gives the
            // arg "ll=ls -la"), so store everything after the first '='.
            state.aliases[argv[i].substr(0, eq)] = argv[i].substr(eq + 1);
        }
    }
    return rc;
}

static int b_unalias(const std::vector<std::string>& argv, ShellState& state) {
    int rc = 0;
    for (size_t i = 1; i < argv.size(); i++) {
        if (state.aliases.erase(argv[i]) == 0) { std::cerr << "unalias: " << argv[i] << ": not found\n"; rc = 1; }
    }
    return rc;
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
    j->state = Job::State::Running;
    kill(-j->pgid, SIGCONT);

    int rc = 0;
    bool stopped = false;
    for (pid_t pid : j->pids) {
        int st = 0;
        // WUNTRACED: a resumed job can get Ctrl-Z'd again -- without this,
        // waitpid() just blocks forever waiting for a real exit that isn't
        // coming, hanging ark instead of returning control to the prompt.
        waitpidRetry(pid, &st, WUNTRACED);
        if (WIFSTOPPED(st)) { stopped = true; rc = 128 + WSTOPSIG(st); break; }
        rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    tcsetpgrp(STDIN_FILENO, shellPgid);

    if (stopped) {
        // Re-stopped, not exited -- it's still a real job, don't drop it
        // from the table (that would make `fg`/`jobs` unable to find it
        // again even though the process is still alive).
        j->state = Job::State::Stopped;
        std::cerr << "\n[" << j->id << "]+  Stopped                 " << j->cmdline << "\n";
    } else {
        state.jobs->remove(j->id);
    }
    return rc;
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
        {"alias", b_alias}, {"unalias", b_unalias},
        {"pushd", b_pushd}, {"popd", b_popd}, {"dirs", b_dirs},
        {"ark-settings", b_ark_settings},
        {"source", b_source}, {".", b_source},
        {"return", b_return}, {"local", b_local},
        {"break", b_break}, {"continue", b_continue},
    };
    return reg;
}
