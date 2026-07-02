#include "jobs.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/wait.h>

namespace {
constexpr size_t kQueueSize = 64;
struct PidStatus { pid_t pid; int status; };
PidStatus g_queue[kQueueSize];
std::atomic<size_t> g_head{0}; // next slot the signal handler will write
std::atomic<size_t> g_tail{0}; // next slot drainSignalQueue() will read

void sigchldHandler(int) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break;
        size_t h = g_head.load(std::memory_order_relaxed);
        size_t next = (h + 1) % kQueueSize;
        if (next == g_tail.load(std::memory_order_acquire)) break; // queue full, drop (best-effort)
        g_queue[h] = PidStatus{pid, status};
        g_head.store(next, std::memory_order_release);
    }
}
} // namespace

void installSigchldHandler() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchldHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, nullptr);
}

BlockSigchld::BlockSigchld() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &old_);
}

BlockSigchld::~BlockSigchld() {
    sigprocmask(SIG_SETMASK, &old_, nullptr);
}

pid_t waitpidRetry(pid_t pid, int* status, int options) {
    for (;;) {
        pid_t r = waitpid(pid, status, options);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

int JobTable::add(pid_t pgid, std::vector<pid_t> pids, std::string cmdline) {
    int id = nextId_++;
    jobs_.push_back(Job{id, pgid, std::move(pids), std::move(cmdline), Job::State::Running});
    return id;
}

void JobTable::drainSignalQueue() {
    size_t t = g_tail.load(std::memory_order_relaxed);
    size_t h = g_head.load(std::memory_order_acquire);
    while (t != h) {
        PidStatus ps = g_queue[t];
        t = (t + 1) % kQueueSize;
        for (auto& job : jobs_) {
            for (pid_t pid : job.pids) {
                if (pid != ps.pid) continue;
                if (WIFSTOPPED(ps.status)) job.state = Job::State::Stopped;
                else if (WIFCONTINUED(ps.status)) job.state = Job::State::Running;
                else if (WIFEXITED(ps.status) || WIFSIGNALED(ps.status)) job.state = Job::State::Done;
            }
        }
    }
    g_tail.store(t, std::memory_order_release);
}

Job* JobTable::find(int id) {
    for (auto& j : jobs_) if (j.id == id) return &j;
    return nullptr;
}

std::vector<Job*> JobTable::all() {
    std::vector<Job*> out;
    for (auto& j : jobs_) out.push_back(&j);
    return out;
}

void JobTable::remove(int id) {
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                [id](const Job& j) { return j.id == id; }),
                jobs_.end());
}
