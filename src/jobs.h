#pragma once
#include <string>
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
