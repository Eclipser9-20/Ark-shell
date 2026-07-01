#pragma once
#include <string>

struct HwStats {
    double load1 = 0.0;
    double memUsedGB = 0.0;
    double memTotalGB = 0.0;
};

// Direct syscalls only (getloadavg, Mach host_statistics64, sysctlbyname) --
// NO subprocess calls. This is called on every command boundary in
// interactive mode, so any fork/exec here reintroduces the exact latency
// regression already found and fixed once this session (a `top -l 1` call
// added ~0.2s per call). Never throws -- a failed syscall just leaves the
// corresponding field at its zero-initialized default.
HwStats getHwStats();

// Walks up from `cwd` looking for a `.git` DIRECTORY (not a `.git` file --
// worktrees/submodules are out of scope, see the design spec) and reads
// `.git/HEAD` directly rather than shelling out to `git symbolic-ref`, so
// this has no subprocess cost either. Returns "" if no repo is found, HEAD
// is unreadable, or the repo is bare.
std::string findGitBranch(const std::string& cwd);

// Sets the DECSTBM scroll region to exclude row 1 and the last row (so
// pinned chrome painted there is never touched by normal scrolling). Skips
// entirely if the terminal has 2 or fewer rows (no room for a scrollable
// area in between).
void setScrollRegion();

// Paints the pinned chrome: row 1 = cwd + git branch; last row = user@host
// + session length (left) and hardware stats (right), padded to fill the
// terminal width. Uses synchronized output + save/restore cursor (DECSC/
// DECRC) + absolute positioning only.
void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw);

// setScrollRegion() + paintChrome() together -- the single function called
// at every command boundary (preexec-equivalent and precmd-equivalent) and
// from the SIGWINCH handler, so the region is reasserted on both sides of
// anything a child process might have reset it.
void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw);
