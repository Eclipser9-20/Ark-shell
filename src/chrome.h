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

// The top-bar content (rounded [dir][branch] pills) as a STRING. Printed inline
// as a header above each prompt (main.cpp) rather than pinned to row 1 -- so it
// scrolls with output into the terminal's scrollback. Pinning row 1 forced the
// scroll region to start at row 2, which disables scrollback entirely.
std::string topBar(const std::string& cwd, const std::string& gitBranch);

// Paints the pinned BOTTOM bar only (last row: user@host + session length, and
// hardware stats), padded to fill the width. The top bar is now an inline
// header (see topBar()), so this touches just the one pinned row via absolute
// positioning; the caller (reassertChrome) handles cursor save/restore.
void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw);

// How reassertChrome() should handle the cursor after painting. Three
// distinct situations need three distinct answers -- an earlier version
// collapsed this to one bool ("always reseed to row 2"), which fixed `clear`
// leaving the cursor on the pinned bar but broke the common case: a normal
// multi-line command's output should let the next prompt continue right
// where that output left off (scrolling down normally), not get yanked back
// to row 2 after every single command -- which looked like text printing
// upward instead of down.
enum class CursorPolicy {
    // Restore to the TRUE prior position (DECSC/DECRC). Correct whenever
    // there's a real in-progress position to protect: readLine()'s idle
    // tick (mid-typing -- the user's cursor could be anywhere in their
    // line), and right before a command runs (readLine() already left the
    // cursor at a fresh, valid line after Enter).
    Preserve,
    // Skip save/restore entirely; explicitly place the cursor at (row 2,
    // col 1). Correct only at startup, where there's no earlier "real"
    // position to protect -- whatever the parent shell left is irrelevant.
    ForceReseed,
    // Restore to the TRUE prior position like Preserve, then verify it via
    // a DSR cursor-position query (\x1b[6n) and correct to (row 2, col 1)
    // ONLY if that position turns out to be outside the scroll region (row
    // 1, or the last row). Correct for right after a foreground command
    // finishes: most commands leave the cursor somewhere valid (preserving
    // it is right), but `clear`-family commands reset it to (1,1) via their
    // own \x1b[H (on the pinned top bar), which naive preservation would
    // otherwise hand straight to the next prompt draw.
    VerifyAndCorrect,
};

// setScrollRegion() + paintChrome() together -- the single function called
// at every command boundary (preexec-equivalent and precmd-equivalent) and
// from the SIGWINCH handler, so the region is reasserted on both sides of
// anything a child process might have reset it.
void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw,
                     CursorPolicy policy = CursorPolicy::Preserve);

// One-shot neofetch-style startup panel: an ASCII ⚡ bolt (ark's mark) beside
// system facts gathered purely from sysctl (OS/kernel/host/CPU/mem/uptime) --
// no subprocess. Printed once, just before the first prompt. ARK_BANNER=0
// (from ark.config) suppresses it.
void printStartupBanner();

// True (once) if the most recent reassertChrome() detected a terminal resize
// and did a full screen-clear repaint -- which wipes the visible screen,
// including whatever prompt/line the caller had drawn. readLine() polls this
// right after its idle tick and reprints its prompt+buffer when it fires, so
// a resize mid-typing doesn't leave the input line blank until the next
// keystroke. Reading it clears it (consume-once).
bool chromeConsumeResizeRepaint();
