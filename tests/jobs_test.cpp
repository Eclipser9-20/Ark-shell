#include "../src/jobs.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

static void test_add_and_find() {
    JobTable table;
    int id = table.add(1234, {1234}, "sleep 1");
    Job* j = table.find(id);
    assert(j != nullptr);
    assert(j->pgid == 1234);
    assert(j->cmdline == "sleep 1");
    assert(j->state == Job::State::Running);
}

static void test_drain_marks_done_on_real_child_exit() {
    // NOTE: this test deliberately does NOT call waitpid() itself. A pid can
    // only be reaped once -- if the test raced its own waitpid() against the
    // SIGCHLD handler's internal waitpid(-1, ...), whichever won would starve
    // the other, making this test flaky (it happened to pass 5/5 runs during
    // development, but that's luck, not a guarantee). Instead, let the
    // handler be the SOLE reaper and poll JobTable's state, which is exactly
    // the mechanism real shell usage depends on.
    JobTable table;
    installSigchldHandler();
    pid_t pid = fork();
    if (pid == 0) { _exit(0); }
    setpgid(pid, pid); // give it its own pgid so JobTable's bookkeeping is realistic
    int id = table.add(pid, {pid}, "test-child");

    Job* j = nullptr;
    for (int i = 0; i < 200; i++) { // up to ~2s
        table.drainSignalQueue();
        j = table.find(id);
        if (j->state == Job::State::Done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(j != nullptr);
    assert(j->state == Job::State::Done);
}

int main() {
    test_add_and_find();
    test_drain_marks_done_on_real_child_exit();
    std::cout << "all jobs tests passed\n";
}
