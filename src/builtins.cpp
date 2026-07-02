#include "builtins.h"
#include "complete.h"
#include "exec.h"
#include "arkfeatures.h"
#include "history.h"
#include "lexer.h"
#include "parser.h"
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

const char* arkDefaultConfig() {
    return R"CFG(# ark.config -- sourced at startup (interactive sessions).
# Aliases, exports, and functions here persist across sessions.
# Edit any time with `ark-settings`.

# ─── YOUR SETTINGS ─────────────────────────────────────────────────────────
# alias ll='ls -la'
# export EDITOR=nvim        # your editor for `ark-settings`

# ═══════════════════════════════════════════════════════════════════════════
#  EVERYTHING ARK CAN DO  —  uncomment / edit to taste
# ═══════════════════════════════════════════════════════════════════════════

# ─── TOGGLES (all default ON; set to 0 to disable) ─────────────────────────
# export ARK_GHOST_TEXT=0          # fish-style autosuggestions (dim ghost text)
# export ARK_SYNTAX_HIGHLIGHT=0    # colored command line as you type
# export ARK_VALIDATE=0            # red-underline unknown commands as you type
# export ARK_CHROME=0              # pinned top/bottom status bars
# export ARK_AUTOCD=0              # type a directory name to cd into it
# export ARK_NU_MODE=1             # nushell-style: `ls` shows a bordered table
# export ARK_BANNER=0              # neofetch-style startup panel (logo + system info)
# export ARK_BANNER_LOGO=bolt      # startup logo:  ark (rounded wordmark) | bolt | none
# export ARK_BANNER_ACCENT=purple  # logo color: blue/green/red/purple/pink/cyan/yellow/orange or hex 7aa2f7
# export ARK_BANNER_INFO=os,cpu,mem  # which lines: user,os,kernel,shell,host,cpu,mem,uptime (or 'all')
# export ARK_BANNER_SUBTITLE="heaven"  # the tagline shown under the logo
# export ARK_SPELLCHECK=0          # "did you mean X?" on an unknown command
# export ARK_AUTOCORRECT=1         # auto-fix a typo'd command and run it (gti->git)
# export ARK_BREW_SUGGEST=0        # "brew install X" for an uninstalled command
# export ARK_MANPAGE_COMPLETE=0    # Tab-complete flags from a command's man page
# export ARK_PRIVATE=1             # start in private mode (nothing saved to history)
# export ARK_CTRLC=append          # Ctrl-C shows ^C after the command (default: own line)
# export ARK_FRESHLINE=0           # don't auto-add a newline when output lacks one

# ─── COMPLETION: FIND ANYTHING, ANYWHERE ───────────────────────────────────
# Tab accepts the ghost suggestion if one's showing, else completes the word.
# A background index of your whole home tree (built at startup) lets Tab find
# any file/program by name from anywhere. `ark-reindex` rebuilds it. Flag args
# (-x/--long) tab-complete from the command's man page (ARK_MANPAGE_COMPLETE).
# export ARK_INDEX=0                        # disable the filesystem index
# export ARK_INDEX_ROOTS="$HOME:/opt"       # roots to index (default: $HOME)
# Extra dirs whose entries complete by FULL PATH (lighter than the index):
# export ARK_SEARCH_DIRS="$HOME/bin:$HOME/projects:$HOME/scripts"

# ─── POWER FEATURES (built in; commands, not toggles) ──────────────────────
# private [on|off]   Private Mode: pause writing commands to history/disk.
# uvar NAME VALUE    Universal Variable: persists across ALL windows AND survives
# uvar NAME          reboot (stored in ~/.config/ark/universal, also exported).
# uvar               Set in one window, appears in the others live. `uvar -u NAME`.
# history            Shared across every window/tab live. `history -c` clears it.
# Autosuggestions are CONTEXT-AWARE: a match from the current directory wins.
# Metadata globbing:  *(.)=files *(/)=dirs *(@)=symlinks *(x)=exec
#   *(.L+1000)=files >1000 bytes (Lk/Lm/Lg units)   *(.mh-2)=modified <2h ago
#   (m units h/d/w; '-'=newer than, '+'=older than)  e.g.  ls -la *.log(.mh-1)

# ─── ALIASES ───────────────────────────────────────────────────────────────
# alias la='ls -A'
# alias gs='git status'
# alias ..='cd ..'
# alias ...='cd ../..'

# ─── FUNCTIONS ─────────────────────────────────────────────────────────────
# mkcd() { mkdir -p "$1" && cd "$1"; }
# greet() { echo "hi, ${1:-world}"; }

# ═══════════════════════════════════════════════════════════════════════════
#  REFERENCE  —  the ark language (always-on; a cheat sheet, nothing to enable)
# ═══════════════════════════════════════════════════════════════════════════
#  EXPANSION  $VAR ${VAR:-def} ${VAR:+alt} ${#VAR} ${VAR#pfx} ${VAR%sfx}
#             ${VAR/a/b} ${VAR//a/b} ${VAR:off:len}  $(cmd) `cmd`
#             $(( 2+3*4 ))  {a,b,c} {1..9}  * ? [abc] **/  ~  $? $$ $# $@ $0
#  CONTROL    if/elif/else/fi  for..do..done  while..do..done  case..esac
#             break continue return  cmd && cmd || cmd ; cmd  ! cmd  ( subshell )
#  FUNCTIONS  name() { .. }   function name { .. }   local VAR=val
#  REDIRECT   > >> < 2>  | (pipe)  & (background)  << EOF  <<'EOF'  <<- EOF
#  BUILTINS   cd(cd -/auto_cd) pwd echo(-n -e) export unset type read
#             alias unalias pushd popd dirs source(.) return local break continue
#             jobs fg bg exit ark-settings ark-reindex
#             private  uvar  history(-c)
#  EDITING    Ctrl-A/E Ctrl-K/U Ctrl-W Ctrl-Y  Alt-←/→  Ctrl-R  Tab  →/Ctrl-F  ↑/↓
)CFG";
}

// ── Nushell-flavored `ls` (ARK_NU_MODE=1) ──────────────────────────────────
static std::string nuHumanSize(off_t bytes) {
    const char* units[] = {"B", "K", "M", "G", "T"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof(buf), "%lld B", (long long)bytes);
    else snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

static std::string nuRelTime(time_t t, time_t now) {
    long d = (long)(now - t);
    if (d < 0) d = 0;
    char buf[32];
    if (d < 60) snprintf(buf, sizeof(buf), "%lds ago", d);
    else if (d < 3600) snprintf(buf, sizeof(buf), "%ldm ago", d / 60);
    else if (d < 86400) snprintf(buf, sizeof(buf), "%ldh ago", d / 3600);
    else snprintf(buf, sizeof(buf), "%ldd ago", d / 86400);
    return buf;
}

// Renders a nushell-style bordered table of a directory's entries. Columns:
// index, name, type, size, modified. The rounded box-drawing borders and
// column layout are nushell's signature look.
static int nuLs(const std::string& dir, time_t now) {
    DIR* d = opendir(dir.c_str());
    if (!d) { std::cerr << "ls: cannot open " << dir << "\n"; return 1; }
    struct Row { std::string idx, name, type, size, mod; };
    std::vector<Row> rows;
    struct dirent* e;
    std::vector<std::string> names;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        if (!n.empty() && n[0] == '.') continue; // hide dotfiles, like plain ls
        names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    int i = 0;
    for (const auto& n : names) {
        struct stat st;
        std::string full = dir + "/" + n;
        Row r;
        r.idx = std::to_string(i++);
        r.name = n;
        if (stat(full.c_str(), &st) == 0) {
            r.type = S_ISDIR(st.st_mode) ? "dir" : "file";
            r.size = S_ISDIR(st.st_mode) ? "" : nuHumanSize(st.st_size);
            r.mod = nuRelTime(st.st_mtime, now);
        }
        rows.push_back(std::move(r));
    }
    // Column widths (header vs widest cell).
    const char* hdr[5] = {"#", "name", "type", "size", "modified"};
    size_t w[5] = {1, 4, 4, 4, 8};
    for (auto& r : rows) {
        std::string* c[5] = {&r.idx, &r.name, &r.type, &r.size, &r.mod};
        for (int k = 0; k < 5; k++) w[k] = std::max(w[k], c[k]->size());
    }
    auto pad = [](const std::string& s, size_t width) { return s + std::string(width - s.size(), ' '); };
    auto rule = [&](const char* l, const char* m, const char* r) {
        std::string out = l;
        for (int k = 0; k < 5; k++) { for (size_t j = 0; j < w[k] + 2; j++) out += "\xe2\x94\x80"; out += (k < 4 ? m : r); }
        return out;
    };
    const std::string V = "\xe2\x94\x82"; // │
    std::cout << rule("\xe2\x95\xad", "\xe2\x94\xac", "\xe2\x95\xae") << "\n"; // ╭┬╮
    std::cout << V;
    for (int k = 0; k < 5; k++) std::cout << " " << pad(hdr[k], w[k]) << " " << V;
    std::cout << "\n" << rule("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4") << "\n"; // ├┼┤
    for (auto& r : rows) {
        std::string* c[5] = {&r.idx, &r.name, &r.type, &r.size, &r.mod};
        std::cout << V;
        for (int k = 0; k < 5; k++) std::cout << " " << pad(*c[k], w[k]) << " " << V;
        std::cout << "\n";
    }
    std::cout << rule("\xe2\x95\xb0", "\xe2\x94\xb4", "\xe2\x95\xaf") << "\n"; // ╰┴╯
    return 0;
}

// `ls`: in nushell mode (ARK_NU_MODE=1) render a bordered table; otherwise
// exec the real /bin/ls so default behavior is unchanged.
static int b_ls(const std::vector<std::string>& argv, ShellState& state) {
    const char* nu = getenv("ARK_NU_MODE");
    if (nu && std::string(nu) == "1") {
        std::string dir = argv.size() > 1 ? argv[1] : state.cwd;
        if (dir.empty()) dir = ".";
        return nuLs(dir, time(nullptr));
    }
    // Passthrough to the real ls.
    BlockSigchld guard;
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> av;
        av.push_back(const_cast<char*>("ls"));
        for (size_t i = 1; i < argv.size(); i++) av.push_back(const_cast<char*>(argv[i].c_str()));
        av.push_back(nullptr);
        execvp("ls", av.data());
        _exit(127);
    }
    int status = 0;
    waitpidRetry(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

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

// Opens ~/.config/ark/ark.config in the user's editor ($VISUAL / $EDITOR,
// else nano/vi). Creates the file (with a commented template) if it doesn't
// exist yet, so the editor always opens something. Runs the editor as a
// normal foreground child.
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

// `ark-reindex`: rebuild the whole-filesystem completion index (e.g. after
// creating files this session). Reports the entry count once it settles.
static int b_ark_reindex(const std::vector<std::string>&, ShellState&) {
    rebuildFileIndex();
    std::cout << "ark: reindexing filesystem in the background...\n";
    return 0;
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
        f << arkDefaultConfig();
    }

    // Honor the user's editor choice: $VISUAL, then $EDITOR, then a common
    // fallback that exists on a bare box. (Set `export EDITOR=...` in ark.config
    // to always open the config in your editor of choice.)
    std::string editor;
    if (const char* v = getenv("VISUAL"); v && *v) editor = v;
    else if (const char* e = getenv("EDITOR"); e && *e) editor = e;
    else if (access("/usr/bin/nano", X_OK) == 0 || access("/opt/homebrew/bin/nano", X_OK) == 0) editor = "nano";
    else editor = "vi";

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

// ── Private Mode (Feature) ──────────────────────────────────────────────────
// `private` / `private on` -> stop writing history to disk; `private off` ->
// resume; bare `private` toggles. Reports the new state.
static int b_private(const std::vector<std::string>& argv, ShellState&) {
    bool now;
    if (argv.size() >= 2) {
        std::string a = argv[1];
        now = (a == "on" || a == "1" || a == "true");
        if (!(now || a == "off" || a == "0" || a == "false")) {
            std::cerr << "private: usage: private [on|off]\n";
            return 1;
        }
    } else {
        now = !arkPrivateMode(); // bare toggle
    }
    arkSetPrivateMode(now);
    std::cout << "private mode " << (now ? "ON — commands are not saved to history"
                                         : "OFF — commands are saved again") << "\n";
    return 0;
}

// ── Universal Variables (Feature) ───────────────────────────────────────────
// `uvar` (list all) · `uvar NAME` (show one) · `uvar NAME VALUE` (set,
// persisted across windows + reboots) · `uvar -u NAME` (erase).
static int b_uvar(const std::vector<std::string>& argv, ShellState& state) {
    if (argv.size() == 1) {
        auto all = uvar::all();
        std::vector<std::string> keys;
        for (auto& kv : all) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (auto& k : keys) std::cout << k << "=" << all[k] << "\n";
        return 0;
    }
    if (argv[1] == "-u") {
        if (argv.size() < 3) { std::cerr << "uvar: -u needs a name\n"; return 1; }
        uvar::unset(argv[2]);
        state.vars.erase(argv[2]);
        return 0;
    }
    if (argv.size() == 2) {
        auto all = uvar::all();
        auto it = all.find(argv[1]);
        if (it == all.end()) return 1;
        std::cout << it->second << "\n";
        return 0;
    }
    // uvar NAME VALUE [VALUE...] -> join extra args with spaces
    std::string value = argv[2];
    for (size_t i = 3; i < argv.size(); i++) value += " " + argv[i];
    uvar::set(argv[1], value);
    state.vars[argv[1]] = value;
    return 0;
}

// ── Command History (Feature: shared history + private mode) ─────────────────
// `history` (list, numbered) · `history -c` (clear memory + the shared file).
static int b_history(const std::vector<std::string>& argv, ShellState& state) {
    if (!state.history) return 1;
    if (argv.size() >= 2 && argv[1] == "-c") {
        state.history->clear(state.histPath);
        return 0;
    }
    const auto& lines = state.history->lines();
    int width = 1, n = (int)lines.size();
    while (n >= 10) { n /= 10; width++; }
    for (size_t i = 0; i < lines.size(); i++)
        std::printf("%*zu  %s\n", width, i + 1, lines[i].c_str());
    return 0;
}

const std::unordered_map<std::string, BuiltinFn>& builtinRegistry() {
    static const std::unordered_map<std::string, BuiltinFn> reg = {
        {"cd", b_cd}, {"exit", b_exit}, {"pwd", b_pwd}, {"echo", b_echo},
        {"export", b_export}, {"unset", b_unset}, {"type", b_type}, {"read", b_read},
        {"jobs", b_jobs}, {"fg", b_fg}, {"bg", b_bg},
        {"alias", b_alias}, {"unalias", b_unalias},
        {"pushd", b_pushd}, {"popd", b_popd}, {"dirs", b_dirs},
        {"ark-settings", b_ark_settings}, {"ark-reindex", b_ark_reindex},
        {"source", b_source}, {".", b_source},
        {"return", b_return}, {"local", b_local},
        {"break", b_break}, {"continue", b_continue},
        {"ls", b_ls},
        {"private", b_private}, {"uvar", b_uvar}, {"history", b_history},
    };
    return reg;
}
