#include "exec.h"
#include "builtins.h"
#include "expand.h"
#include "arkfeatures.h"
#include "pkgmgr.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <set>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

// Reports why a command couldn't run. `err` is the errno posix_spawnp returned.
// Only a genuine "not found" (ENOENT) gets the command-not-found treatment
// (spelling / brew-install suggestions); any OTHER failure -- a permission
// problem, an exec-format error, a broken symlink target -- reports the REAL
// reason (strerror) instead of the misleading "command not found". Spelling is
// tried first; the package-manager install offer only when nothing close
// exists (so `gti` -> `git`, not "install gti"). A suggestion identical to what
// was typed is never shown. `allowPrompt` is true only for a plain foreground
// command -- a failed pipeline stage still gets a passive hint but never an
// interactive [y/N] prompt (you don't stop a pipeline to answer a question).
static void reportCommandNotFound(const std::string& name, int err, bool allowPrompt) {
    if (err != ENOENT) { // the file exists but couldn't be executed
        std::cerr << name << ": " << std::strerror(err) << "\n";
        return;
    }
    std::cerr << name << ": command not found\n";
    const char* spellOff = getenv("ARK_SPELLCHECK");
    if (!(spellOff && std::string(spellOff) == "0")) {
        std::string guess = suggestCommand(name);
        if (!guess.empty() && guess != name) {
            std::cerr << "ark: did you mean '" << guess << "'?\n";
            return;
        }
    }
    offerInstall(name, allowPrompt); // "install with brew/apt/…? [y/N]" (or a passive hint)
}

// Autocorrect (ARK_AUTOCORRECT=1): if the command word is an unrunnable typo
// with a HIGH-confidence fix (edit distance 1), silently rewrite it to the
// correction and run THAT -- `gti status` becomes `git status`. Default off:
// running a different command than typed must be near-certain, so this is a
// deliberate opt-in beyond the always-on "did you mean?" suggestion. Only the
// command (argv[0]) is corrected, never arguments; slash-paths are left alone.
static void maybeAutocorrect(std::vector<std::string>& argv) {
    const char* on = getenv("ARK_AUTOCORRECT");
    if (!on || std::string(on) != "1" || argv.empty()) return;
    const std::string cmd = argv[0];
    if (cmd.empty() || cmd.find('/') != std::string::npos) return; // explicit path: leave it
    if (commandExists(cmd)) return;                                // already runnable
    std::string guess = suggestCommand(cmd);
    if (guess.empty() || levenshtein(cmd, guess) > 1) return;      // not confident enough
    std::cerr << "ark: correcting '" << cmd << "' → '" << guess << "'\n";
    argv[0] = guess;
}

// Real bug found live: neither Ctrl-C nor Ctrl-Z worked on ANY foreground
// child (a plain external command, a shell script). Root cause: ark ignores
// SIGINT/SIGTSTP/SIGTTIN/SIGTTOU for ITSELF (see main.cpp) so it can't be
// killed/stopped by a stray signal in a cooked-mode gap -- but SIG_IGN
// dispositions are INHERITED ACROSS exec() (unlike custom handlers, which
// reset to default), so every spawned child was BORN with those same
// signals already ignored. The kernel still delivered Ctrl-C/Ctrl-Z
// correctly to the child's process group; the child just had no reaction
// configured for them at all, inherited from ark. Returns the sigset used
// to reset these back to default (SIG_DFL) via posix_spawnattr_setsigdefault
// + POSIX_SPAWN_SETSIGDEF, so a spawned child gets normal signal behavior
// regardless of what ark itself ignores for its own defensive reasons.
static sigset_t foregroundDefaultSignals() {
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    sigaddset(&s, SIGQUIT);
    sigaddset(&s, SIGTSTP);
    sigaddset(&s, SIGTTIN);
    sigaddset(&s, SIGTTOU);
    return s;
}

// Writes `body` to an anonymous temp file and returns a read-positioned fd.
// The file is unlinked immediately, so it has no name on disk and is reclaimed
// the moment the last fd referencing it closes -- no path bookkeeping or
// cleanup needed. Returns -1 on failure. This is how a here-doc body becomes
// a real readable stdin.
static int heredocTempFd(const std::string& body) {
    char tmpl[] = "/tmp/ark_heredoc_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd == -1) return -1;
    unlink(tmpl); // fd stays valid; the file is now anonymous
    size_t off = 0;
    while (off < body.size()) {
        ssize_t w = write(fd, body.data() + off, body.size() - off);
        if (w < 0) { if (errno == EINTR) continue; break; } // retry on the idle-ticker
                                                            // SIGALRM instead of
                                                            // truncating the heredoc
        if (w == 0) break;
        off += (size_t)w;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static std::string heredocBody(const Redirect& r, ShellState& state) {
    // Expand $vars in the body unless the delimiter was quoted (<<'EOF').
    // expandWord does parameter/command/arith expansion with no word-splitting,
    // which is exactly here-doc semantics (newlines and spacing preserved).
    return r.heredocExpand ? expandWord(r.target, state) : r.target;
}

// posix_spawn path. Any here-doc opens a temp fd HERE in the parent; those fds
// are pushed to `heredocFds` so the caller closes them after the spawn returns.
static void applyRedirectsFileActions(const std::vector<Redirect>& redirects, posix_spawn_file_actions_t& actions,
                                       ShellState& state, std::vector<int>& heredocFds) {
    for (const auto& r : redirects) {
        switch (r.kind) {
            case Redirect::Kind::In:
                posix_spawn_file_actions_addopen(&actions, r.fd >= 0 ? r.fd : STDIN_FILENO, r.target.c_str(), O_RDONLY, 0);
                break;
            case Redirect::Kind::Out:
                posix_spawn_file_actions_addopen(&actions, r.fd >= 0 ? r.fd : STDOUT_FILENO, r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                break;
            case Redirect::Kind::Append:
                posix_spawn_file_actions_addopen(&actions, r.fd >= 0 ? r.fd : STDOUT_FILENO, r.target.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                break;
            case Redirect::Kind::ErrOut:
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                break;
            case Redirect::Kind::DupFd:
                // `N>&M` -> make fd N a copy of fd M; `N>&-` (dupFd<0) closes N.
                if (r.dupFd >= 0) posix_spawn_file_actions_adddup2(&actions, r.dupFd, r.fd);
                else posix_spawn_file_actions_addclose(&actions, r.fd);
                break;
            case Redirect::Kind::HereDoc: {
                int fd = heredocTempFd(heredocBody(r, state));
                if (fd != -1) {
                    posix_spawn_file_actions_adddup2(&actions, fd, STDIN_FILENO);
                    posix_spawn_file_actions_addclose(&actions, fd); // child drops its copy after the dup
                    heredocFds.push_back(fd);                        // parent closes after spawn
                }
                break;
            }
        }
    }
}

static void applyRedirectsInChild(const std::vector<Redirect>& redirects, ShellState& state) {
    for (const auto& r : redirects) {
        if (r.kind == Redirect::Kind::HereDoc) {
            int fd = heredocTempFd(heredocBody(r, state));
            if (fd != -1) { dup2(fd, STDIN_FILENO); close(fd); }
            continue;
        }
        if (r.kind == Redirect::Kind::DupFd) {
            // `N>&M` -> fd N becomes a copy of fd M (current value); `N>&-` closes N.
            if (r.dupFd >= 0) dup2(r.dupFd, r.fd);
            else close(r.fd);
            continue;
        }
        int fd = -1, target = -1;
        switch (r.kind) {
            case Redirect::Kind::In: fd = open(r.target.c_str(), O_RDONLY); target = r.fd >= 0 ? r.fd : STDIN_FILENO; break;
            case Redirect::Kind::Out: fd = open(r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); target = r.fd >= 0 ? r.fd : STDOUT_FILENO; break;
            case Redirect::Kind::Append: fd = open(r.target.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); target = r.fd >= 0 ? r.fd : STDOUT_FILENO; break;
            case Redirect::Kind::ErrOut: fd = open(r.target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); target = STDERR_FILENO; break;
            case Redirect::Kind::DupFd: break;  // handled above
            case Redirect::Kind::HereDoc: break; // handled above
        }
        if (fd != -1) { dup2(fd, target); close(fd); }
    }
}

// Dispatch a node applying background/negate but NOT its own (compound)
// redirects -- used where redirects were already applied by the caller
// (a pipeline stage's forked child).
static int execNodeNoRedir(Node* node, ShellState& state);

// Which fd does a redirect target in the current process? (mirrors the
// target choices in applyRedirectsInChild.)
static int redirectTargetFd(const Redirect& r) {
    switch (r.kind) {
        case Redirect::Kind::In: return r.fd >= 0 ? r.fd : STDIN_FILENO;
        case Redirect::Kind::Out:
        case Redirect::Kind::Append: return r.fd >= 0 ? r.fd : STDOUT_FILENO;
        case Redirect::Kind::ErrOut: return STDERR_FILENO;
        case Redirect::Kind::DupFd: return r.fd;
        case Redirect::Kind::HereDoc: return STDIN_FILENO;
    }
    return STDOUT_FILENO;
}

// Apply redirects to the CURRENT process, first saving the affected fds so they
// can be restored afterward. Used for compound commands (`for..done > f`), which
// run in-shell -- their variable changes must persist, so we can't just fork.
static std::vector<std::pair<int, int>> applyRedirectsSaving(const std::vector<Redirect>& redirects, ShellState& state) {
    std::vector<std::pair<int, int>> saved; // (targetFd, savedCopyFd)
    for (const auto& r : redirects) {
        int target = redirectTargetFd(r);
        int copy = fcntl(target, F_DUPFD_CLOEXEC, 10); // -1 if target wasn't open
        saved.emplace_back(target, copy);
    }
    applyRedirectsInChild(redirects, state); // does the actual opens + dup2s
    return saved;
}

static void restoreRedirects(std::vector<std::pair<int, int>>& saved) {
    // Restore in reverse so overlapping targets unwind correctly.
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        int target = it->first, copy = it->second;
        if (copy >= 0) { dup2(copy, target); close(copy); }
    }
}

static std::string buildCmdline(Node* node) {
    std::string cmdline;
    auto appendWords = [&](Node* cmd) {
        for (auto& w : cmd->words) { cmdline += w; cmdline += " "; }
    };
    if (node->kind == NodeKind::Pipeline) {
        for (auto& child : node->children) appendWords(child.get());
    } else {
        appendWords(node);
    }
    return cmdline;
}

static int runPipelineStage(Node* cmd, ShellState& state, int inFd, int outFd, pid_t& pidOut) {
    // A subshell stage of a pipeline (`(a; b) | c` or `c | (a; b)`): fork,
    // wire the pipe fds, and run the subshell body in the child.
    // A subshell OR a compound command (if/while/for/case) as a pipeline stage:
    // fork, wire the pipe fds + the stage's own redirects, and run it in the
    // child. This is what makes `for ..; done | wc` and `cmd | while read` work.
    if (cmd->kind == NodeKind::Subshell || cmd->kind == NodeKind::If ||
        cmd->kind == NodeKind::While || cmd->kind == NodeKind::For ||
        cmd->kind == NodeKind::Case) {
        std::cout.flush();
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (inFd != -1) { dup2(inFd, STDIN_FILENO); close(inFd); }
            if (outFd != -1) { dup2(outFd, STDOUT_FILENO); close(outFd); }
            applyRedirectsInChild(cmd->redirects, state);
            int rc = (cmd->kind == NodeKind::Subshell)
                         ? execNode(cmd->children[0].get(), state)
                         : execNodeNoRedir(cmd, state);
            std::cout.flush();
            _exit(rc & 0xff);
        }
        pidOut = pid;
        return 0;
    }
    auto argv = expandWords(cmd->words, state);
    if (argv.empty()) { pidOut = -1; return 0; }

    auto& reg = builtinRegistry();
    auto it = reg.find(argv[0]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    std::vector<int> heredocFds; // parent-side here-doc temp fds, closed after spawn
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
    applyRedirectsFileActions(cmd->redirects, actions, state, heredocFds);
    auto closeHeredocFds = [&]() { for (int fd : heredocFds) close(fd); };

    if (it != reg.end()) {
        // Builtins run in-process normally, but inside a pipeline stage they
        // need their own fd redirection, so fork a child specifically for this.
        // Flush BEFORE forking: stdout is fully buffered when piped/non-tty,
        // so any earlier statement's output still sitting unflushed in this
        // process's stdio buffer would otherwise get copied into the child
        // by fork() and re-emitted a second time when the child's own
        // pre-_exit() flush (below) runs -- see captureCommandOutput() for
        // the fuller explanation of this class of bug.
        std::cout.flush();
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (inFd != -1) { dup2(inFd, STDIN_FILENO); close(inFd); }
            if (outFd != -1) { dup2(outFd, STDOUT_FILENO); close(outFd); }
            applyRedirectsInChild(cmd->redirects, state);
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
        closeHeredocFds();
        pidOut = pid;
        return 0;
    }

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    // Process-group assignment for pipeline stages is handled separately by
    // runPipeline() (all stages share the first stage's pgid, via setpgid()
    // after spawn) -- this attr is only for the signal-disposition reset
    // (see foregroundDefaultSignals()), needed here just as much as for a
    // plain single command.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t defaultSigs = foregroundDefaultSignals();
    posix_spawnattr_setsigdefault(&attr, &defaultSigs);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);

    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], &actions, &attr, cargv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    closeHeredocFds();
    if (rc != 0) {
        reportCommandNotFound(argv[0], rc, /*allowPrompt=*/false); // pipeline stage
        pidOut = -1;
        return 127;
    }
    pidOut = pid;
    return 0;
}

static int runPipeline(Node* pipeline, ShellState& state) {
    // Guards the whole spawn-loop + wait-loop below: without this, the
    // global SIGCHLD handler could reap a fast-exiting stage before this
    // function's own waitpid() calls get to it (same race as runCommand).
    BlockSigchld guard;

    size_t n = pipeline->children.size();
    std::vector<pid_t> pids(n, -1);
    int prevReadFd = -1;

    for (size_t i = 0; i < n; i++) {
        int pipeFds[2] = {-1, -1};
        bool hasNext = i + 1 < n;
        if (hasNext && pipe(pipeFds) != 0) {
            // Out of fds/resources: can't wire the rest of the pipeline. Report
            // and stop spawning further stages rather than silently running a
            // stage's output to the shell's own stdout.
            perror("ark: pipe");
            break;
        }

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

    // A backgrounded pipeline is executed inside its own forked wrapper
    // process (see execNode's central background handling) -- that wrapper
    // is not the foreground job, so it must NOT try to grab the controlling
    // terminal for its sub-stages. Only foreground pipelines do the
    // tcsetpgrp handoff; a background one still waits on its own stages
    // (acting as their supervisor) but skips terminal control entirely.
    pid_t shellPgid = getpgrp();
    if (!pipeline->background && jobPgid > 0) tcsetpgrp(STDIN_FILENO, jobPgid);

    int status = 0;
    bool stopped = false;
    for (pid_t pid : pids) {
        if (pid == -1) continue;
        int st = 0;
        // WUNTRACED: notice Ctrl-Z/SIGTSTP (a stop), not just an exit --
        // same job-control gap runCommand() had for a plain single command.
        // A terminal-generated stop signal hits every process in the
        // foreground pgid at roughly the same time, so once one stage
        // reports stopped, don't keep waiting on the rest (they're either
        // also stopped or about to be) -- just record it and register the
        // whole pipeline as one stopped job below.
        waitpidRetry(pid, &st, WUNTRACED);
        if (WIFSTOPPED(st)) {
            stopped = true;
            status = 128 + WSTOPSIG(st);
            break;
        }
        status = WIFEXITED(st) ? WEXITSTATUS(st) : 1; // pipeline status = last stage's
    }

    if (!pipeline->background) tcsetpgrp(STDIN_FILENO, shellPgid);

    if (stopped) {
        int jobId = 0;
        if (state.jobs) {
            std::vector<pid_t> alive;
            for (pid_t pid : pids) if (pid != -1) alive.push_back(pid);
            jobId = state.jobs->add(jobPgid, alive, buildCmdline(pipeline));
            Job* j = state.jobs->find(jobId);
            if (j) j->state = Job::State::Stopped;
        }
        std::cerr << "\n[" << jobId << "]+  Stopped                 " << buildCmdline(pipeline) << "\n";
    }
    return status;
}

static int runFunctionDef(Node* fn, ShellState& state) {
    state.functions[fn->funcName] = fn->funcBody.get();
    return 0;
}

static int callFunction(Node* body, const std::vector<std::string>& argv, ShellState& state) {
    // Recursion guard: unbounded function recursion (`f() { f; }`) otherwise
    // overflows the C++ call stack -> SIGSEGV. argStack depth IS the function
    // nesting depth, so cap it and error out cleanly instead of crashing.
    constexpr size_t kMaxFunctionDepth = 1000;
    if (state.argStack.size() >= kMaxFunctionDepth) {
        std::cerr << argv[0] << ": maximum function recursion depth exceeded\n";
        return 1;
    }
    std::vector<std::string> params(argv.begin() + 1, argv.end()); // argv[0] is the function name
    state.argStack.push_back(params);
    state.localScopes.emplace_back(); // fresh local-variable scope for this call
    int status = execNode(body, state);
    // Restore any variables `local` shadowed in this call.
    for (auto& [name, saved] : state.localScopes.back()) {
        if (saved.existed) state.vars[name] = saved.value;
        else state.vars.erase(name);
    }
    state.localScopes.pop_back();
    state.argStack.pop_back();
    // `return` sets returnFlag; consume it here, turning it into the call's
    // status. (It stays set while unwinding nested if/while/for inside the
    // body -- see the runList/loop checks -- and is cleared once we're back
    // out of the function.)
    if (state.returnFlag) { status = state.returnStatus; state.returnFlag = false; }
    return status;
}

// A word is an assignment iff it's `NAME=...` where NAME is a valid shell
// identifier: a leading letter or underscore, then letters/digits/underscores,
// up to the first `=`. Returns the index of that `=` (so NAME = word[0..idx),
// VALUE = word[idx+1..]), or std::string::npos if the word isn't an
// assignment. The `=` must come BEFORE any quote sentinel -- `x="a=b"` is an
// assignment of x, but `"x=y"` (a fully-quoted word) is a literal command
// word, not an assignment.
static size_t assignmentEq(const std::string& word) {
    if (word.empty()) return std::string::npos;
    char c0 = word[0];
    if (!(std::isalpha((unsigned char)c0) || c0 == '_')) return std::string::npos;
    for (size_t i = 0; i < word.size(); i++) {
        char c = word[i];
        if (c == '=') return i > 0 ? i : std::string::npos; // empty name -> not an assignment
        if (c == '\x01' || c == '\x02') return std::string::npos; // hit a quote before '=' -> not an assignment
        if (!(std::isalnum((unsigned char)c) || c == '_')) return std::string::npos;
    }
    return std::string::npos; // no '=' at all
}

static int runCommand(Node* cmd, ShellState& state) {
    // Leading NAME=value words are assignments (bash: they precede the
    // command, if any). Peel them off the raw (pre-expansion) words -- the
    // NAME and '=' are literal, only the VALUE gets expanded, and never
    // word-split (that's why expandNoSplit, not expandWords, is used on it).
    size_t firstCmdWord = 0;
    std::vector<std::pair<std::string, std::string>> assignments;
    while (firstCmdWord < cmd->words.size()) {
        size_t eq = assignmentEq(cmd->words[firstCmdWord]);
        if (eq == std::string::npos) break;
        std::string name = cmd->words[firstCmdWord].substr(0, eq);
        std::string value = expandNoSplit(cmd->words[firstCmdWord].substr(eq + 1), state);
        assignments.emplace_back(std::move(name), std::move(value));
        firstCmdWord++;
    }

    if (firstCmdWord == cmd->words.size() && !assignments.empty()) {
        // Pure assignment(s), no command: set shell variables persistently.
        for (auto& [name, value] : assignments) {
            state.vars[name] = value;
            ::setenv(name.c_str(), value.c_str(), 1); // keep the process env in
                                                       // sync so $(...) subshells
                                                       // and children see it too
        }
        return 0;
    }

    // Assignment(s) followed by a command: bash applies them to that command's
    // environment only, not the shell's own vars. Simplest correct realization
    // with the current model: setenv them (so external commands and $(...)
    // inherit them), run, then restore the prior environment values. Also mirror
    // into state.vars for the duration so an ark-side expansion in the same
    // command (rare, but e.g. a builtin) sees them.
    std::vector<std::pair<std::string, std::string>> savedVars;   // (name, prior value or "" sentinel)
    std::vector<std::pair<std::string, bool>> savedEnvPresent;    // (name, was-set-before)
    for (auto& [name, value] : assignments) {
        auto vit = state.vars.find(name);
        savedVars.emplace_back(name, vit != state.vars.end() ? vit->second : std::string());
        const char* prev = getenv(name.c_str());
        savedEnvPresent.emplace_back(name, prev != nullptr);
        if (prev) savedVars.back().second = prev; // prefer the real env value for restore
        state.vars[name] = value;
        ::setenv(name.c_str(), value.c_str(), 1);
    }
    struct EnvRestore {
        ShellState& st;
        const std::vector<std::pair<std::string, std::string>>& sv;
        const std::vector<std::pair<std::string, bool>>& se;
        bool active;
        ~EnvRestore() {
            if (!active) return;
            for (size_t i = 0; i < se.size(); i++) {
                const std::string& name = se[i].first;
                if (se[i].second) ::setenv(name.c_str(), sv[i].second.c_str(), 1);
                else ::unsetenv(name.c_str());
                // state.vars: a temp assignment shouldn't leave a shell var
                // behind unless one already existed with that name.
                if (se[i].second) st.vars[name] = sv[i].second;
                else st.vars.erase(name);
            }
        }
    } envRestore{state, savedVars, savedEnvPresent, !assignments.empty()};

    std::vector<std::string> tempWords(cmd->words.begin() + firstCmdWord, cmd->words.end());

    // Alias expansion on the command word (bash expands aliases before
    // function/builtin lookup, so an alias can shadow either). The first word
    // is matched RAW: a quoted command word (`"ls"`) is sentinel-wrapped and
    // won't match a bare alias name, which is exactly bash's "quoting
    // suppresses alias expansion" rule for free. A `visited` set stops
    // recursive aliases (`alias ll='ll -x'`) from looping. Only alias values
    // that lex to pure words are spliced -- an alias whose value contains a
    // pipe/operator (`alias x='a | b'`) can't be represented by word
    // substitution alone and is left unexpanded (a known limitation).
    {
        std::set<std::string> visited;
        while (!tempWords.empty()) {
            std::string first = tempWords[0];
            auto ait = state.aliases.find(first);
            if (ait == state.aliases.end() || visited.count(first)) break;
            visited.insert(first);
            Lexer alex(ait->second);
            auto toks = alex.tokenize();
            std::vector<std::string> aliasWords;
            bool onlyWords = true;
            for (auto& t : toks) {
                if (t.kind == TokKind::End) break;
                if (t.kind == TokKind::Word) aliasWords.push_back(t.text);
                else { onlyWords = false; break; }
            }
            if (!onlyWords || aliasWords.empty()) break;
            tempWords.erase(tempWords.begin());
            tempWords.insert(tempWords.begin(), aliasWords.begin(), aliasWords.end());
        }
    }

    auto argv = expandWords(tempWords, state);
    if (argv.empty()) return 0;

    auto fnIt = state.functions.find(argv[0]);
    if (fnIt != state.functions.end()) {
        return callFunction(fnIt->second, argv, state);
    }

    auto& reg = builtinRegistry();
    auto it = reg.find(argv[0]);

    // zsh auto_cd: a bare word that names an existing directory (and isn't a
    // builtin/function) is treated as `cd <that dir>` instead of an attempt to
    // execute it. Only for a single word with no redirects -- `./foo` with
    // args is still a command. Requires the word to look path-like (contain
    // '/', or be '.'/'..') OR be an existing directory entry, so a stray typo
    // that happens to match a dir name in $PATH-ish spots isn't surprising.
    bool autoCdOff = getenv("ARK_AUTOCD") && std::string(getenv("ARK_AUTOCD")) == "0"; // config toggle
    if (!autoCdOff && it == reg.end() && argv.size() == 1 && cmd->redirects.empty()) {
        struct stat st;
        if (stat(argv[0].c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            return reg.at("cd")({"cd", argv[0]}, state);
        }
    }

    if (it != reg.end() && cmd->redirects.empty()) {
        return it->second(argv, state);
    }
    if (it != reg.end()) {
        // Builtin WITH redirects: run IN-PROCESS with the redirects applied via
        // save/restore, NOT in a fork. A forked builtin's state changes (cd,
        // read, export, set, unset, local) would be lost to the child -- e.g.
        // `cd dir >/dev/null` or `read x < file` must affect the real shell.
        // The redirect still applies to the builtin's own stdin/stdout for its
        // duration, then the shell's fds are restored.
        std::cout.flush();
        fflush(stdout);
        auto saved = applyRedirectsSaving(cmd->redirects, state);
        int rc = it->second(argv, state);
        std::cout.flush();
        fflush(stdout); // flush the builtin's output to the redirected fd first
        restoreRedirects(saved);
        return rc;
    }

    maybeAutocorrect(argv); // ARK_AUTOCORRECT=1: fix a typo'd command before spawning

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    std::vector<int> heredocFds; // parent-side here-doc temp fds, closed after spawn
    applyRedirectsFileActions(cmd->redirects, actions, state, heredocFds);

    // Give this foreground command its own process group and hand off the
    // controlling terminal to it -- exactly like runPipeline() already does
    // for multi-stage pipelines, but this path (a single plain command) never
    // had it. Without this, standard job control (Ctrl-Z/SIGTSTP suspend,
    // signals reaching the right process group) simply doesn't work for a
    // bare foreground command like `./someprog` -- a bug found in testing.
    // posix_spawn's
    // attr-based POSIX_SPAWN_SETPGROUP sets the child's pgid atomically as
    // part of the spawn itself (pgroup=0 means "use the child's own pid"),
    // so there's no fork+exec-style race window to close here.
    // ALSO resets SIGINT/SIGTSTP/etc. to default (see foregroundDefaultSignals)
    // -- without this, the process-group handoff alone still isn't enough:
    // the child would be born with those signals inherited as ignored from
    // ark and just never react to Ctrl-C/Ctrl-Z at all.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    sigset_t defaultSigs = foregroundDefaultSignals();
    posix_spawnattr_setsigdefault(&attr, &defaultSigs);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);

    // Same SIGCHLD race as above, guarded the same way.
    BlockSigchld guard;
    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], &actions, &attr, cargv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    for (int fd : heredocFds) close(fd); // parent-side here-doc temp fds
    if (rc != 0) {
        reportCommandNotFound(argv[0], rc, /*allowPrompt=*/true); // plain foreground command
        return 127;
    }

    pid_t shellPgid = getpgrp();
    // Only hand the terminal to the child if WE actually own it right now. When
    // runCommand runs inside a background wrapper (`cmd &`), our pgid is the
    // wrapper's -- NOT the terminal's foreground group -- so grabbing the tty
    // here (ark ignores SIGTTOU, so it'd succeed) would steal it from ark and
    // then "restore" it to the wrapper's group, leaving ark not-foreground.
    bool isForeground = tcgetpgrp(STDIN_FILENO) == shellPgid;
    if (isForeground) tcsetpgrp(STDIN_FILENO, pid); // pid doubles as the job's pgid
    int status = 0;
    waitpidRetry(pid, &status, WUNTRACED); // WUNTRACED: notice Ctrl-Z/SIGTSTP
                                            // (a stop), not just an exit
    if (isForeground) tcsetpgrp(STDIN_FILENO, shellPgid);

    if (WIFSTOPPED(status)) {
        int jobId = 0;
        if (state.jobs) {
            jobId = state.jobs->add(pid, {pid}, buildCmdline(cmd));
            Job* j = state.jobs->find(jobId);
            if (j) j->state = Job::State::Stopped; // JobTable::add() defaults
                                                     // to Running; correct it
                                                     // immediately since this
                                                     // job is already stopped
        }
        std::cerr << "\n[" << jobId << "]+  Stopped                 " << argv[0] << "\n";
        return 128 + WSTOPSIG(status); // matches the 128+signal convention
                                        // used elsewhere for signal-based
                                        // termination status
    }
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

// Handles break/continue after a loop-body execution. Returns true if the
// enclosing loop should STOP (a break that targets this level, or a
// break/continue targeting an OUTER level -- which we let propagate by
// leaving loopCtl set and decrementing the level). Sets `doContinue` when
// this iteration should just advance to the next.
static bool consumeLoopCtl(ShellState& state, bool& stopLoop) {
    stopLoop = false;
    if (state.loopCtl == ShellState::LoopCtl::None) return false;
    if (state.loopCtlLevels > 1) {
        // Targets an outer loop: consume one level here and propagate up by
        // stopping this loop with loopCtl still set.
        state.loopCtlLevels--;
        stopLoop = true;
        return true;
    }
    // Targets THIS loop.
    bool wasBreak = state.loopCtl == ShellState::LoopCtl::Break;
    state.loopCtl = ShellState::LoopCtl::None;
    state.loopCtlLevels = 0;
    stopLoop = wasBreak;      // break stops the loop; continue does not
    return true;              // handled (caller checks stopLoop / else continues)
}

static int runWhile(Node* wn, ShellState& state) {
    int status = 0;
    state.loopDepth++;
    struct DepthGuard { int& d; ~DepthGuard() { d--; } } dg{state.loopDepth};
    while (execNode(wn->children[0].get(), state) == 0) {
        if (state.returnFlag) break; // condition itself may have returned
        status = execNode(wn->children[1].get(), state);
        if (state.returnFlag) break; // `return` inside the loop body
        bool stopLoop;
        if (consumeLoopCtl(state, stopLoop) && stopLoop) break;
    }
    // A `break N`/`continue N` that targeted more levels than actually exist
    // leaves loopCtl set as it propagates up; at the OUTERMOST loop (depth 1,
    // DepthGuard hasn't decremented yet) clear it, or it would poison every
    // statement after the loop (runList stops on any non-None loopCtl).
    if (state.loopDepth == 1) { state.loopCtl = ShellState::LoopCtl::None; state.loopCtlLevels = 0; }
    return status;
}

static int runFor(Node* fn, ShellState& state) {
    int status = 0;
    state.loopDepth++;
    struct DepthGuard { int& d; ~DepthGuard() { d--; } } dg{state.loopDepth};
    // Expand the word list with FULL expansion (glob + split), so
    // `for f in *.txt` iterates once per matching file, and `for w in $LIST`
    // iterates over the split words -- not the raw single word.
    auto items = expandWords(fn->forWords, state);
    for (const auto& item : items) {
        state.vars[fn->forVar] = item;
        status = execNode(fn->children[0].get(), state);
        if (state.returnFlag) break; // `return` inside the loop body
        bool stopLoop;
        if (consumeLoopCtl(state, stopLoop) && stopLoop) break;
    }
    // A `break N`/`continue N` that targeted more levels than actually exist
    // leaves loopCtl set as it propagates up; at the OUTERMOST loop (depth 1,
    // DepthGuard hasn't decremented yet) clear it, or it would poison every
    // statement after the loop (runList stops on any non-None loopCtl).
    if (state.loopDepth == 1) { state.loopCtl = ShellState::LoopCtl::None; state.loopCtlLevels = 0; }
    return status;
}

static int runCase(Node* cn, ShellState& state) {
    std::string word = expandWord(cn->caseWord, state);
    for (auto& clause : cn->caseClauses) {
        // A clause pattern can hold '|'-separated alternatives (e.g. `a|b*`);
        // the clause matches if ANY alternative matches.
        const std::string& pat = clause.first;
        bool matched = false;
        size_t start = 0;
        for (;;) {
            size_t bar = pat.find('|', start);
            std::string alt = pat.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (globMatch(alt, word)) { matched = true; break; }
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        if (matched) return execNode(clause.second.get(), state);
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
        // `return`, `break`, or `continue` stops the rest of this list/body;
        // the enclosing function/loop executor consumes the flag.
        if (state.returnFlag || state.loopCtl != ShellState::LoopCtl::None) break;
    }
    return status;
}

// `( list )` -- runs the body in a forked child so its variable/cd/exit
// changes are isolated from the parent shell. The child gets its own copy of
// ShellState via fork(); we just wait for its status.
static int runSubshell(Node* node, ShellState& state) {
    std::cout.flush();
    fflush(stdout);
    BlockSigchld guard;
    pid_t pid = fork();
    if (pid == 0) {
        int rc = execNode(node->children[0].get(), state);
        std::cout.flush();
        _exit(rc & 0xff);
    }
    int status = 0;
    waitpidRetry(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int execNodeDispatch(Node* node, ShellState& state) {
    switch (node->kind) {
        case NodeKind::List:
            return runList(node, state);
        case NodeKind::Command:
            return runCommand(node, state);
        case NodeKind::Pipeline:
            return runPipeline(node, state);
        case NodeKind::Subshell:
            return runSubshell(node, state);
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

// Background (`&`) is valid on any Command or Pipeline, at any nesting depth
// (POSIX allows it anywhere a pipeline appears, e.g. inside an if-branch) --
// handled centrally here so every node kind gets it uniformly instead of
// duplicating background logic in runCommand AND runPipeline separately.
static int execNodeNoRedir(Node* node, ShellState& state) {
    if (node->background && (node->kind == NodeKind::Command || node->kind == NodeKind::Pipeline ||
                             node->kind == NodeKind::Subshell)) {
        std::cout.flush(); // flush BEFORE forking -- see captureCommandOutput()
        fflush(stdout);     // for why (stale buffered output would otherwise
                            // get duplicated by the child's own flush)
        pid_t pid = fork();
        if (pid == 0) {
            setpgid(0, 0); // new process group, this child is its own leader
            int rc = execNodeDispatch(node, state); // NOT execNode -- avoids
                                                      // re-checking background
                                                      // on this same node and
                                                      // forking again
            std::cout.flush();
            _exit(rc);
        }
        setpgid(pid, pid);
        if (state.jobs) state.jobs->add(pid, {pid}, buildCmdline(node));
        // Real shells only print the "[1] <pid>" job-start announcement in
        // interactive/monitor mode -- non-interactive scripts stay silent.
        // Since the pid is non-deterministic and phase 1 has no interactive
        // mode wired up yet (Task 19), staying silent here is both correct
        // shell behavior and keeps this testable.
        return 0; // background jobs report success immediately; real status
                  // comes later via `wait`/`jobs`
    }
    int status = execNodeDispatch(node, state);
    if (node->negate) status = (status == 0) ? 1 : 0; // leading `!`
    return status;
}

// Public entry point. Compound commands (if/while/for/case) run in-shell, so
// their trailing redirects (`for ..; done > f`) are applied to the current
// process with save/restore around the body. Command/Subshell/Pipeline manage
// their own redirects (in runCommand / the forked child), so pass through.
int execNode(Node* node, ShellState& state) {
    bool wrap = !node->redirects.empty() &&
                (node->kind == NodeKind::If || node->kind == NodeKind::While ||
                 node->kind == NodeKind::For || node->kind == NodeKind::Case);
    if (!wrap) return execNodeNoRedir(node, state);
    auto saved = applyRedirectsSaving(node->redirects, state);
    int rc = execNodeNoRedir(node, state);
    std::cout.flush();
    fflush(stdout); // flush buffered output to the redirected fd before restoring
    restoreRedirects(saved);
    return rc;
}

std::string captureCommandOutput(const std::string& cmd, ShellState& state) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    BlockSigchld guard; // same reap-race guard as every other foreground wait
    // Flush BEFORE forking, not just before the child's _exit(): stdout is
    // fully buffered here (piped/non-tty), so any prior statement's output
    // that hasn't hit the fd yet is still sitting in this process's stdio
    // buffer. fork() copies that buffer into the child verbatim -- without
    // this flush, the child's own end-of-life flush would re-emit all of
    // that stale, already-pending-in-the-parent output a second time,
    // compounding on every subsequent command substitution in a script.
    std::cout.flush();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int rc = 0;
        try {
            Lexer lex(cmd);
            Parser parser(lex.tokenize());
            auto ast = parser.parse();
            rc = execNode(ast.get(), state); // fork already gave this
                                              // process its own copy of
                                              // ShellState -- real subshell
                                              // isolation for free
        } catch (const std::exception&) {
            rc = 1;
        }
        std::cout.flush();
        _exit(rc);
    }

    close(pipefd[1]);
    std::string output;
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) { output.append(buf, (size_t)n); continue; }
        if (n < 0 && errno == EINTR) continue; // same SIGALRM hazard as the
                                                 // waitpid() below -- a signal
                                                 // landing mid-read() would
                                                 // otherwise look like EOF
                                                 // and silently truncate the
                                                 // captured output
        break; // n == 0 (real EOF) or a genuine read error
    }
    close(pipefd[0]);
    int status = 0;
    waitpidRetry(pid, &status, 0);
    return output;
}
