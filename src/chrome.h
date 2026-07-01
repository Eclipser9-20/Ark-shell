#pragma once
#include <string>

struct HwStats {
    double load1 = 0.0;
    double memUsedGB = 0.0;
    double memTotalGB = 0.0;
    double cpuPercent = 0.0; // host-wide CPU utilization, see getHwStats()
};

// Direct syscalls only (getloadavg, Mach host_statistics64, sysctlbyname) --
// NO subprocess calls. This is called on every command boundary AND roughly
// once per second while idle at the prompt in interactive mode, so any
// fork/exec here reintroduces the exact latency regression already found and
// fixed once this session (a `top -l 1` call added ~0.2s per call). Never
// throws -- a failed syscall just leaves the corresponding field at its
// zero-initialized default.
//
// cpuPercent is computed from the delta between this call's
// host_statistics(HOST_CPU_LOAD_INFO) tick counts and the PREVIOUS call's
// (kept in function-static state) -- there's no "instantaneous CPU usage"
// syscall on macOS, only cumulative tick counters since boot, so a single
// call can't produce a percentage. The very first call in a process's
// lifetime has no prior sample yet and reports 0.
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
//
// `reseedToPromptRow`, when true, ignores wherever the cursor's TRUE
// position was and explicitly places it at the start of the scroll region
// (row 2, col 1) after painting, instead of restoring the prior position.
// Real bug found live: `clear`'s own \x1b[H leaves the cursor at (1,1) --
// the DECSC/DECRC save/restore dance faithfully preserves that, handing row
// 1 right back to the next prompt draw and printing it on top of the pinned
// top bar. Pass true for any call site that ISN'T mid-line-editing (command
// boundaries, resize, startup) where a foreground command could have left
// the cursor anywhere; pass false only when reasserting DURING active
// typing (readLine()'s idle tick), where the user's in-progress cursor
// position must be preserved exactly instead of reset.
void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw,
                     bool reseedToPromptRow = false);
