#include "exec.h"
#include "builtins.h"
#include "expand.h"
#include "arkfeatures.h"
#include "pkgmgr.h"
#include "complete.h"
#include "overlay.h"
#include "nucapture.h"
#include "chrome.h"
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
#include <regex>
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

// Auto-path (ARK_AUTO_PATH=1): if the command word isn't on $PATH but ark's
// background file index knows exactly one executable by that name, run THAT full
// path instead of failing. Off by default; only a bare name (no slash) that PATH
// can't resolve, and only on an unambiguous single match (findIndexedExecutable
// returns "" otherwise), so ark never silently runs the wrong thing.
static void maybeAutoPath(std::vector<std::string>& argv) {
    const char* on = getenv("ARK_AUTO_PATH");
    if (!on || std::string(on) != "1" || argv.empty()) return;
    const std::string cmd = argv[0];
    if (cmd.empty() || cmd.find('/') != std::string::npos) return; // already a path
    if (commandExists(cmd)) return;                                // PATH resolves it already
    std::string full = findIndexedExecutable(cmd);
    if (!full.empty()) {
        std::cerr << "ark: " << cmd << " → " << full << "\n";
        argv[0] = full;
    }
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
        // A file-redirect target undergoes expansion (parameters/tilde/command
        // substitution), no word-splitting -- so `> $F` / `> ~/log` / `> $(f)`
        // work. addopen copies the path, so a per-iteration temporary is fine.
        std::string t = (r.kind == Redirect::Kind::In || r.kind == Redirect::Kind::Out ||
                         r.kind == Redirect::Kind::Append || r.kind == Redirect::Kind::ErrOut)
                            ? expandWord(r.target, state) : std::string();
        switch (r.kind) {
            case Redirect::Kind::In:
                posix_spawn_file_actions_addopen(&actions, r.fd >= 0 ? r.fd : STDIN_FILENO, t.c_str(), O_RDONLY, 0);
                break;
            case Redirect::Kind::Out:
                posix_spawn_file_actions_addopen(&actions, r.fd >= 0 ? r.fd : STDOUT_FILENO, t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                break;
            case Redirect::Kind::Append:
                posix_spawn_file_actions_addopen(&actions, r.fd >= 0 ? r.fd : STDOUT_FILENO, t.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                break;
            case Redirect::Kind::ErrOut:
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        std::string t = expandWord(r.target, state); // expand $VAR / ~ / $(cmd), no split
        int fd = -1, target = -1;
        switch (r.kind) {
            case Redirect::Kind::In: fd = open(t.c_str(), O_RDONLY); target = r.fd >= 0 ? r.fd : STDIN_FILENO; break;
            case Redirect::Kind::Out: fd = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); target = r.fd >= 0 ? r.fd : STDOUT_FILENO; break;
            case Redirect::Kind::Append: fd = open(t.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); target = r.fd >= 0 ? r.fd : STDOUT_FILENO; break;
            case Redirect::Kind::ErrOut: fd = open(t.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); target = STDERR_FILENO; break;
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

// siblingReadFd is the read end of THIS stage's own output pipe -- the fd the
// parent keeps for the next stage. Every stage's child inherits it and MUST
// close it: otherwise the writer is its OWN phantom reader, so when a later
// stage (e.g. `head`) closes the pipe early, the write never gets EPIPE and the
// stage hangs forever holding the whole pipeline open. -1 for the last stage.
static int runPipelineStage(Node* cmd, ShellState& state, int inFd, int outFd, int siblingReadFd, pid_t& pidOut) {
    // A subshell stage of a pipeline (`(a; b) | c` or `c | (a; b)`): fork,
    // wire the pipe fds, and run the subshell body in the child.
    // A subshell OR a compound command (if/while/for/case) as a pipeline stage:
    // fork, wire the pipe fds + the stage's own redirects, and run it in the
    // child. This is what makes `for ..; done | wc` and `cmd | while read` work.
    if (cmd->kind == NodeKind::Subshell || cmd->kind == NodeKind::If ||
        cmd->kind == NodeKind::While || cmd->kind == NodeKind::For ||
        cmd->kind == NodeKind::Case || cmd->kind == NodeKind::Group) {
        std::cout.flush();
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (inFd != -1) { dup2(inFd, STDIN_FILENO); close(inFd); }
            if (outFd != -1) { dup2(outFd, STDOUT_FILENO); close(outFd); }
            if (siblingReadFd != -1) close(siblingReadFd); // don't be our own reader
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
    if (siblingReadFd != -1) posix_spawn_file_actions_addclose(&actions, siblingReadFd);
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
            if (siblingReadFd != -1) close(siblingReadFd); // don't be our own reader
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

    // Same full-screen handoff as runCommand(): if a foreground pipeline ends in
    // a pager/TUI (`git log | less`, `dmesg | less`), it too must escape ark's
    // pinned-bar scroll region. Done once, before any stage is spawned, so no
    // stage ever draws inside the constrained band.
    if (!pipeline->background && tcgetpgrp(STDIN_FILENO) == getpgrp())
        releaseScrollRegionForChild();

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

        runPipelineStage(pipeline->children[i].get(), state, inFd, outFd, hasNext ? pipeFds[0] : -1, pids[i]);

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

// Sentinels the lexer stamps on captured `(( ... ))` / `[[ ... ]]` words (see
// lexer.cpp). Detected as the sole word of a Command node so these constructs
// never go through normal command lookup/expansion.
static const char SENT_ARITH = '\x1e';
static const char SENT_COND  = '\x1d';
static long arithMutEval(const std::string& expr, ShellState& state, bool* ok = nullptr);
static bool evalDBracket(const std::string& inner, ShellState& state);

static int runCommand(Node* cmd, ShellState& state) {
    // `(( expr ))` arithmetic command / `[[ test ]]` extended test: the lexer
    // captured either as a single sentinel-marked word. Dispatch before any
    // assignment-peeling/expansion. `(( ))` exits 0 iff the expression is nonzero
    // (and applies its side effects, e.g. `(( i++ ))`); `[[ ]]` exits 0 iff true.
    if (!cmd->words.empty() && !cmd->words[0].empty()) {
        char s0 = cmd->words[0][0];
        if (s0 == SENT_ARITH) return arithMutEval(cmd->words[0].substr(1), state) != 0 ? 0 : 1;
        if (s0 == SENT_COND)  return evalDBracket(cmd->words[0].substr(1), state) ? 0 : 1;
    }
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

    const std::string beforeFixups = argv[0];
    maybeAutocorrect(argv); // ARK_AUTOCORRECT=1: fix a typo'd command before spawning
    maybeAutoPath(argv);    // ARK_AUTO_PATH=1: resolve a non-$PATH program via the file index

    // A correction can land on a BUILTIN or a FUNCTION -- `exi` -> `exit`. The
    // registry/function lookups above ran against the PRE-correction word, so
    // without re-checking here the corrected name goes straight to the external
    // spawn path and dies as "exit: command not found" -- then, adding insult,
    // the command-not-found hook offered to `brew install execline` to provide
    // it. Re-resolve whenever a fixup actually rewrote argv[0].
    if (argv[0] != beforeFixups) {
        auto fnIt2 = state.functions.find(argv[0]);
        if (fnIt2 != state.functions.end()) return callFunction(fnIt2->second, argv, state);
        auto it2 = reg.find(argv[0]);
        if (it2 != reg.end()) {
            if (cmd->redirects.empty()) return it2->second(argv, state);
            // Same in-process save/restore path as the builtin-with-redirects
            // case above: forking would discard cd/read/export state changes.
            std::cout.flush();
            fflush(stdout);
            auto saved = applyRedirectsSaving(cmd->redirects, state);
            int rc = it2->second(argv, state);
            std::cout.flush();
            fflush(stdout);
            restoreRedirects(saved);
            return rc;
        }
    }

    // Overlay compositor (ARK_OVERLAY=1, experimental): run the command through
    // ark's own PTY so its output can be mirrored into the pinned "deadzone" AND
    // captured for ark's own scrollback. Only for a plain foreground command with
    // no redirects (those still need the posix_spawn file-actions path); a PTY
    // setup failure returns -1 and falls through to the normal spawn below.
    if (overlay::enabled() && cmd->redirects.empty()) {
        std::cout.flush();
        fflush(stdout);
        int st = overlay::run(argv, environ);
        if (st >= 0) return st;
    }

    // Nu-mode structured auto-format (ARK_NU_MODE=1): run a plain foreground
    // external command through a PTY and, if its whole output is a JSON document,
    // render it as a nu table; otherwise stream it through unchanged. Skipped for
    // redirects/pipes (output isn't terminal-bound), non-terminals, when ark
    // doesn't own the tty (a `cmd &` wrapper), for known interactive/TUI tools
    // (they keep the normal path + real job control), and for commands that don't
    // exist (so the command-not-found path below still gives its suggestions).
    if (nucapture::enabled() && cmd->redirects.empty() && isatty(STDOUT_FILENO)
        && tcgetpgrp(STDIN_FILENO) == getpgrp()
        && !nucapture::isInteractiveCommand(argv[0]) && commandExists(argv[0])) {
        std::cout.flush();
        fflush(stdout);
        int st = nucapture::run(argv, environ);
        if (st >= 0) return st;
    }

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

    // Hand the child the full screen (drop ark's pinned-bar scroll region) so a
    // pager/editor/TUI -- man, less, vim, top, lazygit -- isn't trapped in the
    // 2..N-1 band and renders correctly. Only when WE currently own the terminal
    // (a `cmd &` background wrapper must not touch the real foreground's screen).
    // reassertChrome() at the next command boundary re-establishes the region.
    if (tcgetpgrp(STDIN_FILENO) == getpgrp()) releaseScrollRegionForChild();

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
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        // Ctrl-C (SIGINT) / Ctrl-\ (SIGQUIT) leave the cursor mid-line, right
        // after the terminal's echoed ^C / ^\ (no newline follows it). Drop to a
        // fresh line so the next prompt -- or a program run right after -- doesn't
        // collide with it (the "^Csudo:" overlap). Matches bash, and mirrors the
        // leading '\n' the Ctrl-Z (WIFSTOPPED) branch above already emits.
        if (sig == SIGINT || sig == SIGQUIT) { std::cout << "\n"; std::cout.flush(); }
        return 128 + sig; // standard 128+signal termination status
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

    // C-style loop: `for (( init; cond; step ))`. Marked by forVar == "\x1e"; the
    // three arithmetic clauses live in forWords. An empty condition means "true"
    // (an infinite loop, e.g. `for ((;;))`). Clauses run through the mutable
    // arithmetic evaluator so `i++` and `i=0` take effect on shell variables.
    if (fn->forVar == std::string(1, SENT_ARITH)) {
        const std::string& init = fn->forWords.size() > 0 ? fn->forWords[0] : "";
        const std::string& cond = fn->forWords.size() > 1 ? fn->forWords[1] : "";
        const std::string& step = fn->forWords.size() > 2 ? fn->forWords[2] : "";
        if (!init.empty()) arithMutEval(init, state);
        for (;;) {
            if (!cond.empty() && arithMutEval(cond, state) == 0) break;
            status = execNode(fn->children[0].get(), state);
            if (state.returnFlag) break;
            bool stopLoop;
            if (consumeLoopCtl(state, stopLoop) && stopLoop) break;
            if (!step.empty()) arithMutEval(step, state);
        }
        if (state.loopDepth == 1) { state.loopCtl = ShellState::LoopCtl::None; state.loopCtlLevels = 0; }
        return status;
    }

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

// ── Mutable integer arithmetic evaluator: powers `(( ... ))` and C-style for ──
// Unlike expand.cpp's read-only ArithEval, this one WRITES back to shell
// variables, so assignment (`=`, `+=` …), pre/post increment (`++i`, `i++`),
// and the C-style loop step all take effect. C operator precedence; bare names
// and `$name`/`${name}` resolve to their integer value (0 if unset/non-numeric).
namespace {
struct ArithMut {
    const std::string& s;
    size_t pos = 0;
    ShellState& st;
    bool ok = true;
    ArithMut(const std::string& str, ShellState& state) : s(str), st(state) {}

    void skipSpace() { while (pos < s.size() && std::isspace((unsigned char)s[pos])) pos++; }
    long readVar(const std::string& n) {
        std::string v;
        auto it = st.vars.find(n);
        if (it != st.vars.end()) v = it->second;
        else if (const char* e = getenv(n.c_str())) v = e;
        if (v.empty()) return 0;
        return std::strtol(v.c_str(), nullptr, 0);
    }
    void writeVar(const std::string& n, long val) {
        std::string v = std::to_string(val);
        st.vars[n] = v;
        ::setenv(n.c_str(), v.c_str(), 1);
    }
    std::string readName() {
        size_t j = pos;
        while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
        std::string n = s.substr(pos, j - pos);
        pos = j;
        return n;
    }

    long parsePrimary() {
        skipSpace();
        if (pos >= s.size()) { ok = false; return 0; }
        char c = s[pos];
        if (c == '(') { pos++; long v = parseComma(); skipSpace();
                        if (pos < s.size() && s[pos] == ')') pos++; else ok = false; return v; }
        if (c == '$') {
            pos++;
            std::string n;
            if (pos < s.size() && s[pos] == '{') { pos++; n = readName(); if (pos < s.size() && s[pos] == '}') pos++; }
            else n = readName();
            return readVar(n);
        }
        if (std::isdigit((unsigned char)c)) {
            long v = std::strtol(s.c_str() + pos, nullptr, 0);
            // advance past the number (base-aware)
            size_t j = pos;
            if (s[j] == '0' && j + 1 < s.size() && (s[j+1]=='x'||s[j+1]=='X')) { j += 2; while (j<s.size() && std::isxdigit((unsigned char)s[j])) j++; }
            else while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            pos = j;
            return v;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::string n = readName();
            if (pos + 1 < s.size() && s[pos] == '+' && s[pos+1] == '+') { pos += 2; long cur = readVar(n); writeVar(n, cur + 1); return cur; }
            if (pos + 1 < s.size() && s[pos] == '-' && s[pos+1] == '-') { pos += 2; long cur = readVar(n); writeVar(n, cur - 1); return cur; }
            return readVar(n);
        }
        ok = false;
        return 0;
    }
    long parseUnary() {
        skipSpace();
        if (pos + 1 < s.size() && s[pos] == '+' && s[pos+1] == '+') { pos += 2; skipSpace(); std::string n = readName(); long v = readVar(n) + 1; writeVar(n, v); return v; }
        if (pos + 1 < s.size() && s[pos] == '-' && s[pos+1] == '-') { pos += 2; skipSpace(); std::string n = readName(); long v = readVar(n) - 1; writeVar(n, v); return v; }
        if (pos < s.size() && s[pos] == '!') { pos++; return !parseUnary(); }
        if (pos < s.size() && s[pos] == '~') { pos++; return ~parseUnary(); }
        if (pos < s.size() && s[pos] == '-') { pos++; return -parseUnary(); }
        if (pos < s.size() && s[pos] == '+') { pos++; return parseUnary(); }
        return parsePower();
    }
    long parsePower() {
        long b = parsePrimary();
        skipSpace();
        if (pos + 1 < s.size() && s[pos] == '*' && s[pos+1] == '*') { pos += 2; long e = parseUnary(); long r = 1; for (long k = 0; k < e; k++) r *= b; return r; }
        return b;
    }
    bool eat(const char* op) { skipSpace(); size_t n = std::strlen(op); if (s.compare(pos, n, op) == 0) { pos += n; return true; } return false; }
    long parseMul() { long v = parseUnary(); for (;;) { skipSpace();
        if (pos<s.size() && s[pos]=='*' && !(pos+1<s.size()&&s[pos+1]=='*')) { pos++; v = v * parseUnary(); }
        else if (pos<s.size() && s[pos]=='/') { pos++; long d = parseUnary(); v = d ? v / d : 0; }
        else if (pos<s.size() && s[pos]=='%') { pos++; long d = parseUnary(); v = d ? v % d : 0; }
        else break; } return v; }
    long parseAdd() { long v = parseMul(); for (;;) { skipSpace();
        if (pos<s.size() && s[pos]=='+' && !(pos+1<s.size()&&s[pos+1]=='+')) { pos++; v = v + parseMul(); }
        else if (pos<s.size() && s[pos]=='-' && !(pos+1<s.size()&&s[pos+1]=='-')) { pos++; v = v - parseMul(); }
        else break; } return v; }
    long parseShift() { long v = parseAdd(); for (;;) {
        if (eat("<<")) { long n = parseAdd(); v = (n>=0&&n<64) ? (long)((unsigned long)v << n) : 0; }
        else if (eat(">>")) { long n = parseAdd(); v = (n>=0&&n<64) ? (v >> n) : 0; }
        else break; } return v; }
    long parseRel() { long v = parseShift(); for (;;) {
        if (eat("<=")) v = (v <= parseShift());
        else if (eat(">=")) v = (v >= parseShift());
        else if (eat("<")) v = (v < parseShift());
        else if (eat(">")) v = (v > parseShift());
        else break; } return v; }
    long parseEq() { long v = parseRel(); for (;;) {
        if (eat("==")) v = (v == parseRel());
        else if (eat("!=")) v = (v != parseRel());
        else break; } return v; }
    long parseBAnd() { long v = parseEq(); while (true) { skipSpace(); if (pos<s.size() && s[pos]=='&' && !(pos+1<s.size()&&s[pos+1]=='&')) { pos++; v = v & parseEq(); } else break; } return v; }
    long parseBXor() { long v = parseBAnd(); while (true) { skipSpace(); if (pos<s.size() && s[pos]=='^') { pos++; v = v ^ parseBAnd(); } else break; } return v; }
    long parseBOr()  { long v = parseBXor(); while (true) { skipSpace(); if (pos<s.size() && s[pos]=='|' && !(pos+1<s.size()&&s[pos+1]=='|')) { pos++; v = v | parseBXor(); } else break; } return v; }
    long parseLAnd() { long v = parseBOr(); while (eat("&&")) { long r = parseBOr(); v = (v && r); } return v; }
    long parseLOr()  { long v = parseLAnd(); while (eat("||")) { long r = parseLAnd(); v = (v || r); } return v; }
    long parseTernary() { long c = parseLOr(); skipSpace();
        if (pos < s.size() && s[pos] == '?') { pos++; long a = parseAssign(); skipSpace(); if (pos<s.size() && s[pos]==':') pos++; else ok=false; long b = parseAssign(); return c ? a : b; }
        return c; }
    // Assignment (right-associative, below ternary): NAME op= expr.
    long parseAssign() {
        skipSpace();
        size_t save = pos;
        if (pos < s.size() && (std::isalpha((unsigned char)s[pos]) || s[pos] == '_')) {
            std::string n = readName();
            skipSpace();
            const char* op = nullptr; int adv = 0;
            if (pos < s.size() && s[pos] == '=' && !(pos+1<s.size() && s[pos+1]=='=')) { op = "="; adv = 1; }
            else if (pos+1 < s.size() && s[pos+1] == '=' && std::strchr("+-*/%&|^", s[pos])) { op = nullptr; adv = 2; }
            else if (pos+2 < s.size() && (s.compare(pos,3,"<<=")==0 || s.compare(pos,3,">>=")==0)) { adv = 3; }
            char kind = (adv == 1) ? '=' : (adv >= 2 ? s[pos] : 0);
            if (adv > 0) {
                pos += adv;
                long rhs = parseAssign();
                long cur = readVar(n);
                long res = rhs;
                switch (kind) {
                    case '=': res = rhs; break;
                    case '+': res = cur + rhs; break;
                    case '-': res = cur - rhs; break;
                    case '*': res = cur * rhs; break;
                    case '/': res = rhs ? cur / rhs : 0; break;
                    case '%': res = rhs ? cur % rhs : 0; break;
                    case '&': res = cur & rhs; break;
                    case '|': res = cur | rhs; break;
                    case '^': res = cur ^ rhs; break;
                    case '<': res = (rhs>=0&&rhs<64) ? (long)((unsigned long)cur << rhs) : 0; break; // <<=
                    case '>': res = (rhs>=0&&rhs<64) ? (cur >> rhs) : 0; break;                       // >>=
                }
                writeVar(n, res);
                return res;
            }
            (void)op;
            pos = save; // not an assignment -- rewind
        }
        return parseTernary();
    }
    long parseComma() { long v = parseAssign(); while (true) { skipSpace(); if (pos<s.size() && s[pos]==',') { pos++; v = parseAssign(); } else break; } return v; }
};
} // namespace

static long arithMutEval(const std::string& expr, ShellState& state, bool* ok) {
    ArithMut e(expr, state);
    long v = e.parseComma();
    if (ok) *ok = e.ok;
    return v;
}

int builtinLet(const std::vector<std::string>& argv, ShellState& state) {
    long last = 0;
    for (size_t k = 1; k < argv.size(); k++) last = arithMutEval(argv[k], state);
    return last != 0 ? 0 : 1;
}

// ── `[[ ... ]]` extended-test evaluator ──────────────────────────────────────
namespace {
struct DBTok { std::string raw; bool quoted; enum { WORD, LP, RP, AND, OR, NOT } type; };

// Expand a raw `[[ ]]` operand (which still carries literal quote chars):
// $-expansion applies outside quotes and inside "double" quotes, but 'single'
// quotes are literal. Quote characters are removed. No field splitting or
// pathname globbing (that's the caller's job for the RHS of ==/!=). expandWord
// only does $-expansion and leaves the \x01/\x02 sentinels in, so we never route
// quotes through it -- we strip them here and expand each unquoted/dq segment.
std::string dbExpand(const std::string& raw, const ShellState& state) {
    std::string result;
    size_t i = 0, n = raw.size();
    while (i < n) {
        char c = raw[i];
        if (c == '\'') { i++; while (i < n && raw[i] != '\'') result += raw[i++]; if (i < n) i++; }
        else if (c == '"') { i++; std::string seg; while (i < n && raw[i] != '"') seg += raw[i++]; if (i < n) i++; result += expandWord(seg, state); }
        else { std::string seg; while (i < n && raw[i] != '\'' && raw[i] != '"') seg += raw[i++]; result += expandWord(seg, state); }
    }
    return result;
}

std::vector<DBTok> dbTokenize(const std::string& in) {
    std::vector<DBTok> toks;
    size_t i = 0, n = in.size();
    auto isSep = [](char c) { return c == ' ' || c == '\t'; };
    while (i < n) {
        while (i < n && isSep(in[i])) i++;
        if (i >= n) break;
        if (in[i] == '(' ) { toks.push_back({"(", false, DBTok::LP}); i++; continue; }
        if (in[i] == ')' ) { toks.push_back({")", false, DBTok::RP}); i++; continue; }
        if (in[i] == '&' && i+1 < n && in[i+1] == '&') { toks.push_back({"&&", false, DBTok::AND}); i += 2; continue; }
        if (in[i] == '|' && i+1 < n && in[i+1] == '|') { toks.push_back({"||", false, DBTok::OR}); i += 2; continue; }
        if (in[i] == '!' && (i+1 >= n || isSep(in[i+1]))) { toks.push_back({"!", false, DBTok::NOT}); i++; continue; }
        // a word: read until whitespace or a structural boundary, keeping quotes
        std::string w; bool quoted = false;
        while (i < n && !isSep(in[i])) {
            char c = in[i];
            if (c == '\'' ) { quoted = true; w += c; i++; while (i < n && in[i] != '\'') w += in[i++]; if (i<n) { w += in[i++]; } continue; }
            if (c == '"' )  { quoted = true; w += c; i++; while (i < n && in[i] != '"')  w += in[i++]; if (i<n) { w += in[i++]; } continue; }
            if (c == '(' || c == ')') break;
            if ((c == '&' && i+1<n && in[i+1]=='&') || (c == '|' && i+1<n && in[i+1]=='|')) break;
            w += c; i++;
        }
        toks.push_back({w, quoted, DBTok::WORD});
    }
    return toks;
}

struct DBParser {
    const std::vector<DBTok>& t;
    size_t i = 0;
    ShellState& st;
    DBParser(const std::vector<DBTok>& toks, ShellState& state) : t(toks), st(state) {}

    bool isBinOp(const std::string& s) {
        static const std::set<std::string> ops = {
            "==","=","!=","=~","<",">","-eq","-ne","-lt","-le","-gt","-ge","-nt","-ot","-ef"};
        return ops.count(s) > 0;
    }
    bool isUnOp(const std::string& s) {
        static const std::set<std::string> ops = {
            "-e","-f","-d","-r","-w","-x","-s","-z","-n","-L","-h","-b","-c","-p","-S","-g","-u","-k","-t","-v","-o"};
        return ops.count(s) > 0;
    }
    bool fileTest(const std::string& op, const std::string& path) {
        struct stat sb;
        bool ex = (stat(path.c_str(), &sb) == 0);
        struct stat lsb; bool lex = (lstat(path.c_str(), &lsb) == 0);
        if (op == "-e") return ex;
        if (op == "-f") return ex && S_ISREG(sb.st_mode);
        if (op == "-d") return ex && S_ISDIR(sb.st_mode);
        if (op == "-b") return ex && S_ISBLK(sb.st_mode);
        if (op == "-c") return ex && S_ISCHR(sb.st_mode);
        if (op == "-p") return ex && S_ISFIFO(sb.st_mode);
        if (op == "-S") return ex && S_ISSOCK(sb.st_mode);
        if (op == "-L" || op == "-h") return lex && S_ISLNK(lsb.st_mode);
        if (op == "-s") return ex && sb.st_size > 0;
        if (op == "-r") return access(path.c_str(), R_OK) == 0;
        if (op == "-w") return access(path.c_str(), W_OK) == 0;
        if (op == "-x") return access(path.c_str(), X_OK) == 0;
        if (op == "-g") return ex && (sb.st_mode & S_ISGID);
        if (op == "-u") return ex && (sb.st_mode & S_ISUID);
        if (op == "-k") return ex && (sb.st_mode & S_ISVTX);
        return false;
    }

    bool parseOr()  { bool v = parseAnd(); while (i < t.size() && t[i].type == DBTok::OR)  { i++; bool r = parseAnd(); v = v || r; } return v; }
    bool parseAnd() { bool v = parseNot(); while (i < t.size() && t[i].type == DBTok::AND) { i++; bool r = parseNot(); v = v && r; } return v; }
    bool parseNot() { if (i < t.size() && t[i].type == DBTok::NOT) { i++; return !parseNot(); } return parsePrimary(); }
    bool parsePrimary() {
        if (i >= t.size()) return false;
        if (t[i].type == DBTok::LP) { i++; bool v = parseOr(); if (i < t.size() && t[i].type == DBTok::RP) i++; return v; }
        // unary test:  -f FILE  /  -z STR ...
        if (t[i].type == DBTok::WORD && !t[i].quoted && isUnOp(t[i].raw) &&
            i + 1 < t.size() && t[i+1].type == DBTok::WORD) {
            std::string op = t[i].raw;
            std::string arg = dbExpand(t[i+1].raw, st);
            i += 2;
            if (op == "-z") return arg.empty();
            if (op == "-n") return !arg.empty();
            if (op == "-v") return st.vars.count(arg) > 0 || getenv(arg.c_str()) != nullptr;
            if (op == "-o") { const char* v = getenv(("ARK_" + arg).c_str()); return v && std::string(v) == "1"; }
            return fileTest(op, arg);
        }
        // binary test:  L OP R
        if (t[i].type == DBTok::WORD && i + 2 < t.size() &&
            t[i+1].type == DBTok::WORD && !t[i+1].quoted && isBinOp(t[i+1].raw)) {
            std::string lhs = dbExpand(t[i].raw, st);
            std::string op = t[i+1].raw;
            bool rQuoted = t[i+2].quoted;
            std::string rhsRaw = t[i+2].raw;
            std::string rhs = dbExpand(rhsRaw, st);
            i += 3;
            if (op == "==" || op == "=")
                return rQuoted ? (lhs == rhs) : globMatch(rhs, lhs);
            if (op == "!=")
                return rQuoted ? (lhs != rhs) : !globMatch(rhs, lhs);
            if (op == "=~") { try { std::regex re(rhs, std::regex::extended); return std::regex_search(lhs, re); } catch (...) { return false; } }
            if (op == "<") return lhs < rhs;
            if (op == ">") return lhs > rhs;
            long a = std::strtol(lhs.c_str(), nullptr, 10), b = std::strtol(rhs.c_str(), nullptr, 10);
            if (op == "-eq") return a == b;
            if (op == "-ne") return a != b;
            if (op == "-lt") return a < b;
            if (op == "-le") return a <= b;
            if (op == "-gt") return a > b;
            if (op == "-ge") return a >= b;
            struct stat sa, sb2; bool ea = stat(lhs.c_str(), &sa) == 0, eb = stat(rhs.c_str(), &sb2) == 0;
            if (op == "-nt") return ea && (!eb || sa.st_mtime > sb2.st_mtime);
            if (op == "-ot") return eb && (!ea || sa.st_mtime < sb2.st_mtime);
            if (op == "-ef") return ea && eb && sa.st_dev == sb2.st_dev && sa.st_ino == sb2.st_ino;
            return false;
        }
        // single word: true iff non-empty after expansion
        std::string v = dbExpand(t[i].raw, st);
        i++;
        return !v.empty();
    }
};
} // namespace

static bool evalDBracket(const std::string& inner, ShellState& state) {
    auto toks = dbTokenize(inner);
    if (toks.empty()) return false;
    DBParser p(toks, state);
    return p.parseOr();
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
        case NodeKind::Group:
            // No fork, unlike Subshell: a brace group runs in the CURRENT shell,
            // so `cd`, assignments and `return` inside it are visible to the caller.
            return execNode(node->children[0].get(), state);
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
                             node->kind == NodeKind::Subshell || node->kind == NodeKind::Group)) {
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
                 node->kind == NodeKind::For || node->kind == NodeKind::Case ||
                 node->kind == NodeKind::Group);
    if (!wrap) return execNodeNoRedir(node, state);
    // Flush BEFORE swapping fd 1, not just after. stdout is fully buffered when
    // it isn't a tty, so any EARLIER statement's output can still be sitting in
    // this process's stdio buffer -- redirecting first and flushing later drains
    // that backlog into the redirect target. `echo BEFORE; if true; then echo
    // inside; fi > f` put BOTH lines in f (and printed neither) instead of just
    // "inside". Same hazard captureCommandOutput() documents around fork().
    std::cout.flush();
    fflush(stdout);
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
