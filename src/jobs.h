#pragma once
#include <csignal>
#include <string>
#include <sys/types.h>
#include <vector>

struct Job {
    int id;
    pid_t pgid;
    std::vector<pid_t> pids;
    std::string cmdline;
    enum class State { Running, Stopped, Done } state = State::Running;
};

class JobTable {
public:
    int add(pid_t pgid, std::vector<pid_t> pids, std::string cmdline);
    void drainSignalQueue();
    Job* find(int id);
    std::vector<Job*> all();
    void remove(int id);

private:
    std::vector<Job> jobs_;
    int nextId_ = 1;
};

// Installs a SIGCHLD handler that ONLY records (pid, status) pairs into a
// lock-free static ring buffer — no malloc/iostream/locks inside the handler
// itself. JobTable::drainSignalQueue() (called from the main loop) reads
// that buffer and updates job state safely outside signal context.
void installSigchldHandler();

// RAII guard: blocks SIGCHLD for the guard's lifetime, restoring the prior
// mask on destruction. Needed around every foreground waitpid(specific_pid,
// ...) call -- the SIGCHLD handler installed above does its own
// waitpid(-1, WNOHANG, ...) asynchronously, and WILL race a plain foreground
// wait: if the handler reaps the child first, the foreground waitpid() call
// gets ECHILD, and code that doesn't check for that failure silently treats
// the (untouched, zero-initialized) status as "exited with code 0" --
// turning a failing command into a false success. Blocking SIGCHLD doesn't
// stop waitpid() from working (it queries the kernel's process table
// directly, not via signal delivery) — it just defers the async handler
// until after the foreground reap has already happened.
class BlockSigchld {
public:
    BlockSigchld();
    ~BlockSigchld();
    BlockSigchld(const BlockSigchld&) = delete;
    BlockSigchld& operator=(const BlockSigchld&) = delete;

private:
    sigset_t old_;
};

// Real bug found live: `waitpid(pid, &status, 0)` (a blocking, foreground
// wait -- every one of these in exec.cpp/builtins.cpp) returns -1/EINTR if
// a signal interrupts it, and installIdleTicker()'s 1-second SIGALRM
// (deliberately NOT SA_RESTART, so it can interrupt readLine()'s blocking
// read()) does exactly that to ANY blocking syscall, including this one --
// meaning any foreground command running past a 1-second tick boundary had
// its wait aborted early. Code that didn't check for EINTR just fell
// through treating the STILL-RUNNING child's untouched `status` as if it
// had exited, returning control to the caller (and printing the next
// prompt) while the real process kept running in the background -- exactly
// the "draws the next prompt before the program finishes" bug reported
// live. This wraps every blocking waitpid() call so a signal landing
// mid-wait just retries instead of giving up.
pid_t waitpidRetry(pid_t pid, int* status, int options);
