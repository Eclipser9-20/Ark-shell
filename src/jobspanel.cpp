#include "jobspanel.h"
#include "jobs.h"
#include "input.h"
#include <cstdio>
#include <string>
#include <vector>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace jobspanel {

namespace {

constexpr const char* PURPLE = "\x1b[38;2;157;107;255m";
constexpr const char* PINK   = "\x1b[38;2;255;92;205m";
constexpr const char* DIM    = "\x1b[2m";
constexpr const char* RESET  = "\x1b[0m";
constexpr const char* INV    = "\x1b[7m";

const char* stateLabel(Job::State s) {
    switch (s) {
        case Job::State::Running: return "running";
        case Job::State::Stopped: return "stopped";
        default:                  return "done";
    }
}

// Draw the panel below the prompt. Returns the number of lines drawn so the
// next redraw can move back up over them. Cursor is left after the last line.
int draw(const std::vector<Job*>& list, int sel) {
    std::string out = "\r\n";
    out += std::string(PURPLE) + "╭─ Jobs " + std::string(28, ' ') + "╮" + RESET + "\r\n";
    int lines = 1;
    if (list.empty()) {
        out += std::string(PURPLE) + "│ " + DIM + "(no background or stopped jobs)     " + RESET
             + PURPLE + "│" + RESET + "\r\n";
        lines++;
    } else {
        for (size_t i = 0; i < list.size(); i++) {
            Job* j = list[i];
            char row[128];
            snprintf(row, sizeof(row), "%s[%d] %-8s %-20.20s",
                     ((int)i == sel ? "\xe2\x96\xb8 " : "  "),
                     j->id, stateLabel(j->state), j->cmdline.c_str());
            bool selected = ((int)i == sel);
            out += std::string(PURPLE) + "│ " + RESET;
            if (selected) out += INV;
            out += std::string(PINK) + row + RESET;
            out += std::string(PURPLE) + " │" + RESET + "\r\n";
            lines++;
        }
    }
    out += std::string(PURPLE) + "╰" + std::string(36, '-') + "╯" + RESET;
    out += std::string(DIM) + "  \xe2\x86\x91\xe2\x86\x93 f=fg b=bg s=stop k=kill esc=close" + RESET;
    lines++;
    printf("%s", out.c_str());
    fflush(stdout);
    return lines;
}

// Bring a job to the foreground: continue it, hand it the tty, wait until it
// stops again or exits, then reclaim the tty. Updates the job's state / removes
// it if it finished.
void doForeground(JobTable& jobs, Job* j) {
    pid_t shellPgid = getpgrp();
    int jobId = j->id;
    pid_t pgid = j->pgid;
    kill(-pgid, SIGCONT);
    tcsetpgrp(STDIN_FILENO, pgid);
    int status = 0;
    bool stopped = false;
    // Reap the whole group until it stops (Ctrl-Z again) or fully exits
    // (waitpid returns -1/ECHILD once no group members remain).
    for (;;) {
        pid_t w = waitpidRetry(-pgid, &status, WUNTRACED);
        if (w <= 0) break;                          // group gone
        if (WIFSTOPPED(status)) { stopped = true; break; }
        // else a member exited/signaled: keep reaping the rest of the group
    }
    tcsetpgrp(STDIN_FILENO, shellPgid);
    if (stopped) { if (Job* jj = jobs.find(jobId)) jj->state = Job::State::Stopped; }
    else jobs.remove(jobId);
}

} // namespace

void show(JobTable& jobs) {
    int sel = 0;
    int prevLines = 0;

    auto redraw = [&]() {
        if (prevLines > 0) {
            // Move back up over the previously drawn block and clear downward.
            printf("\x1b[%dA\r\x1b[J", prevLines);
        }
        auto list = jobs.all();
        std::vector<Job*> active;
        for (Job* j : list)
            if (j->state == Job::State::Running || j->state == Job::State::Stopped)
                active.push_back(j);
        if (sel >= (int)active.size()) sel = (int)active.size() - 1;
        if (sel < 0) sel = 0;
        prevLines = draw(active, sel);
    };

    redraw();

    for (;;) {
        char c;
        if ((ssize_t)arkinput::readByte(c, /*retryEINTR=*/true) <= 0) break;

        auto active = [&]() {
            std::vector<Job*> a;
            for (Job* j : jobs.all())
                if (j->state == Job::State::Running || j->state == Job::State::Stopped)
                    a.push_back(j);
            return a;
        };

        if (c == '\x1b') {
            // Arrow keys: ESC [ A/B. A standalone Esc closes.
            char b1;
            if ((ssize_t)arkinput::readByte(b1, false) <= 0) break;
            if (b1 == '[') {
                char b2;
                if ((ssize_t)arkinput::readByte(b2, false) <= 0) break;
                if (b2 == 'A') { sel--; redraw(); }
                else if (b2 == 'B') { sel++; redraw(); }
            } else {
                break; // Esc alone: close
            }
            continue;
        }
        if (c == 'q') break;

        // Navigation is arrows only (handled above); letters are actions, so
        // 'k' = kill (no vim j/k nav, which would collide with kill).
        auto a = active();
        Job* j = (sel >= 0 && sel < (int)a.size()) ? a[sel] : nullptr;
        if (!j) continue;
        if (c == 'k') { kill(-j->pgid, SIGTERM); redraw(); continue; }
        if (c == 's') { kill(-j->pgid, SIGSTOP); j->state = Job::State::Stopped; redraw(); continue; }
        if (c == 'b') { kill(-j->pgid, SIGCONT); j->state = Job::State::Running; redraw(); continue; }
        if (c == 'f' || c == '\r' || c == '\n') {
            // Clear the panel first, then foreground (the job takes the screen).
            if (prevLines > 0) { printf("\x1b[%dA\r\x1b[J", prevLines); prevLines = 0; }
            doForeground(jobs, j);
            break;
        }
    }

    // Clear the panel region on exit; caller redraws the prompt.
    if (prevLines > 0) { printf("\x1b[%dA\r\x1b[J", prevLines); fflush(stdout); }
}

} // namespace jobspanel
