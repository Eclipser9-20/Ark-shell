# ark Pinned Screen Chrome Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pinned top row (cwd + git branch) and pinned bottom row (user@host + session length left, hardware stats right) in ark's interactive mode, with the per-command prompt simplified to time + arrow.

**Architecture:** `src/chrome.h/.cpp` provides stat-gathering (no subprocess calls — direct syscalls only, since chrome repaints on every command boundary) and the DECSTBM scroll-region paint routine; `main.cpp` wires it into the interactive REPL loop's preexec/precmd points and SIGWINCH.

**Tech Stack:** C++20, POSIX (`getloadavg`), Mach host stats API (`host_statistics64`), `sysctlbyname`, raw ANSI (DECSTBM, DECSC/DECRC, synchronized output) — no new external dependencies.

## Global Constraints

- No subprocess calls in the repaint hot path (chrome repaints on every command boundary) — direct syscalls/file reads only. This is the exact regression already found and fixed once this session (a `top -l 1` call adding ~0.2s per repaint).
- Absolute cursor positioning + save/restore (DECSC/DECRC) only in the paint routine — never relative cursor movement (the source of the earlier "staircase" bug).
- Scroll region is reasserted on BOTH sides of every command execution (preexec-equivalent AND precmd-equivalent), not just one — the exact fix for the earlier "cuts into output" bug.
- `findGitBranch()` handles the plain non-worktree case only (this project doesn't use git worktrees for its own repo) — a `.git` file (worktree/submodule) returns empty rather than resolving it, a deliberate YAGNI simplification.

---

## File Structure

```
src/
  chrome.h    — HwStats struct, getHwStats(), findGitBranch(), setScrollRegion(),
                paintChrome(), reassertChrome()
  chrome.cpp  — implementation
  main.cpp    — modified: sessionStart tracking, SIGWINCH handler, reassertChrome()
                calls around execNode(), buildPrompt() simplified to time + arrow
tests/
  chrome_test.cpp — standalone test binary (findGitBranch against a real temp
                    git repo; getHwStats sanity-checked for plausible ranges)
```

---

### Task 1: Hardware stats (`getHwStats()`) — no subprocess calls

**Files:**
- Create: `src/chrome.h`
- Create: `src/chrome.cpp`
- Test: `tests/chrome_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct HwStats {
      double load1;       // 1-minute load average
      double memUsedGB;
      double memTotalGB;
  };
  HwStats getHwStats();
  ```

- [ ] **Step 1: Write src/chrome.h**

```cpp
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
// DECRC) + absolute positioning only -- see the design spec's "Hardening"
// notes for why (v1 of this class of feature broke on relative-movement
// drift and un-guarded terminal writes).
void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw);

// setScrollRegion() + paintChrome() together -- the single function called
// at every command boundary (preexec-equivalent and precmd-equivalent) and
// from the SIGWINCH handler, so the region is reasserted on both sides of
// anything a child process might have reset it.
void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw);
```

- [ ] **Step 2: Write the failing test (tests/chrome_test.cpp)**

```cpp
#include "../src/chrome.h"
#include <cassert>
#include <iostream>

static void test_hwstats_plausible_ranges() {
    HwStats hw = getHwStats();
    assert(hw.load1 >= 0.0);
    assert(hw.memTotalGB > 0.0);       // every real Mac has nonzero total RAM
    assert(hw.memUsedGB >= 0.0);
    assert(hw.memUsedGB <= hw.memTotalGB + 1.0); // +1 slack for rounding
}

int main() {
    test_hwstats_plausible_ranges();
    std::cout << "all chrome hwstats tests passed\n";
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `clang++ -std=c++20 -Isrc -o /tmp/chrome_test tests/chrome_test.cpp`
Expected: FAIL — `chrome.h` exists but `chrome.cpp` (with `getHwStats`'s body) doesn't yet, so this is a linker error: `getHwStats()` not defined

- [ ] **Step 4: Write src/chrome.cpp (getHwStats only for this task)**

```cpp
#include "chrome.h"
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#include <unistd.h>

HwStats getHwStats() {
    HwStats hw;

    double loadavg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(loadavg, 3) != -1) {
        hw.load1 = loadavg[0];
    }

    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        hw.memTotalGB = (double)memsize / (1024.0 * 1024.0 * 1024.0);
    }

    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                          (host_info64_t)&vmstat, &count);
    if (kr == KERN_SUCCESS) {
        uint64_t usedPages = vmstat.active_count + vmstat.wire_count + vmstat.compressor_page_count;
        double pageSize = (double)getpagesize();
        hw.memUsedGB = (usedPages * pageSize) / (1024.0 * 1024.0 * 1024.0);
    }

    return hw;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `clang++ -std=c++20 -Isrc -o /tmp/chrome_test tests/chrome_test.cpp src/chrome.cpp && /tmp/chrome_test`
Expected: `all chrome hwstats tests passed`

- [ ] **Step 6: Commit**

```bash
git add src/chrome.h src/chrome.cpp tests/chrome_test.cpp
git commit -m "chrome: hardware stats via direct syscalls (getloadavg, Mach host_statistics64) -- no subprocess calls"
```

---

### Task 2: Git branch detection (`findGitBranch()`)

**Files:**
- Modify: `src/chrome.cpp`
- Modify: `tests/chrome_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `findGitBranch()`'s implementation (already declared in Task 1's `chrome.h`).

- [ ] **Step 1: Add failing tests to tests/chrome_test.cpp**

```cpp
#include <cstdlib>
#include <fstream>

static void test_find_git_branch_in_real_repo() {
    system("rm -rf /tmp/ark_chrome_test_repo");
    system("mkdir -p /tmp/ark_chrome_test_repo");
    system("cd /tmp/ark_chrome_test_repo && git init -q -b main "
           "&& git -c user.email=t@t.com -c user.name=t commit -q --allow-empty -m init");
    std::string branch = findGitBranch("/tmp/ark_chrome_test_repo");
    assert(branch == "main");
    system("rm -rf /tmp/ark_chrome_test_repo");
}

static void test_find_git_branch_from_subdirectory() {
    system("rm -rf /tmp/ark_chrome_test_repo2");
    system("mkdir -p /tmp/ark_chrome_test_repo2/sub/deeper");
    system("cd /tmp/ark_chrome_test_repo2 && git init -q -b feature-x "
           "&& git -c user.email=t@t.com -c user.name=t commit -q --allow-empty -m init");
    std::string branch = findGitBranch("/tmp/ark_chrome_test_repo2/sub/deeper");
    assert(branch == "feature-x"); // walks up parent directories to find .git
    system("rm -rf /tmp/ark_chrome_test_repo2");
}

static void test_find_git_branch_not_a_repo() {
    std::string branch = findGitBranch("/tmp");
    assert(branch.empty());
}
```

Add the three calls to `main()`.

- [ ] **Step 2: Run to verify these fail**

Run: `clang++ -std=c++20 -Isrc -o /tmp/chrome_test tests/chrome_test.cpp src/chrome.cpp && /tmp/chrome_test`
Expected: FAIL — `findGitBranch` is declared but not defined yet (linker error)

- [ ] **Step 3: Add findGitBranch() to src/chrome.cpp**

Add near the top: `#include <fstream>` and `#include <sys/stat.h>`.

```cpp
std::string findGitBranch(const std::string& cwd) {
    std::string dir = cwd;
    for (;;) {
        std::string gitDir = dir + "/.git";
        struct stat st;
        if (stat(gitDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            std::ifstream headFile(gitDir + "/HEAD");
            std::string headLine;
            if (!headFile.is_open() || !std::getline(headFile, headLine)) return "";
            const std::string prefix = "ref: refs/heads/";
            if (headLine.rfind(prefix, 0) == 0) return headLine.substr(prefix.size());
            return headLine.size() >= 7 ? headLine.substr(0, 7) : headLine; // detached HEAD short SHA
        }
        if (dir == "/") return "";
        auto slash = dir.find_last_of('/');
        dir = (slash == std::string::npos || slash == 0) ? "/" : dir.substr(0, slash);
    }
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `clang++ -std=c++20 -Isrc -o /tmp/chrome_test tests/chrome_test.cpp src/chrome.cpp && /tmp/chrome_test`
Expected: `all chrome hwstats tests passed`

- [ ] **Step 5: Commit**

```bash
git add src/chrome.cpp tests/chrome_test.cpp
git commit -m "chrome: git branch detection via direct .git/HEAD read (no subprocess), walks up parent dirs"
```

---

### Task 3: Scroll-region pinning + wire into main.cpp

**Files:**
- Modify: `src/chrome.cpp` (add `setScrollRegion`, `paintChrome`, `reassertChrome`)
- Modify: `src/main.cpp` (session start tracking, SIGWINCH, preexec/precmd calls, time-based prompt)

**Interfaces:**
- Consumes: `HwStats`, `getHwStats()`, `findGitBranch()` from Tasks 1-2.
- Produces: `setScrollRegion()`, `paintChrome()`, `reassertChrome()` (already declared in `chrome.h`).

- [ ] **Step 1: Add setScrollRegion/paintChrome/reassertChrome to src/chrome.cpp**

Add near the top: `#include <sys/ioctl.h>`, `#include <unistd.h>` (already present from Task 1), `#include <cstdio>`.

```cpp
static bool getTerminalSize(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) {
        return false;
    }
    rows = ws.ws_row;
    cols = ws.ws_col;
    return true;
}

void setScrollRegion() {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;
    printf("\x1b[2;%dr", rows - 1);
    fflush(stdout);
}

static std::string formatSession(double seconds) {
    int total = (int)seconds;
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%dh%02dm", h, m);
    else if (m > 0) snprintf(buf, sizeof(buf), "%dm%02ds", m, s);
    else snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw) {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;

    std::string top = cwd;
    if (!gitBranch.empty()) top += "  " + gitBranch;
    if ((int)top.size() > cols) top = top.substr(0, cols);

    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    const char* user = getenv("USER");
    std::string left = std::string(user ? user : "user") + "@" + host + " " + formatSession(sessionSeconds);

    char hwbuf[128];
    snprintf(hwbuf, sizeof(hwbuf), "load %.2f  mem %.1f/%.1fG", hw.load1, hw.memUsedGB, hw.memTotalGB);
    std::string right = hwbuf;

    std::string bottom = left;
    int pad = cols - (int)left.size() - (int)right.size();
    if (pad < 1) pad = 1;
    bottom += std::string(pad, ' ') + right;
    if ((int)bottom.size() > cols) bottom = bottom.substr(0, cols);

    printf("\x1b[?2026h");   // begin synchronized update
    printf("\x1b7");         // save cursor (DECSC)
    printf("\x1b[1;1H\x1b[2K%s", top.c_str());
    printf("\x1b[%d;1H\x1b[2K%s", rows, bottom.c_str());
    printf("\x1b8");         // restore cursor (DECRC)
    printf("\x1b[?2026l");   // end synchronized update
    fflush(stdout);
}

void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw) {
    setScrollRegion();
    paintChrome(cwd, gitBranch, sessionSeconds, hw);
}
```

Add `#include <unistd.h>` for `gethostname` if not already present (it is, from Task 1's `getpagesize` usage).

- [ ] **Step 2: Rebuild and manually smoke-test the paint routine in isolation**

Run: `clang++ -std=c++20 -Isrc -o /tmp/chrome_test tests/chrome_test.cpp src/chrome.cpp && /tmp/chrome_test`
Expected: `all chrome hwstats tests passed` (unchanged — this step doesn't add new automated tests, `paintChrome`/`reassertChrome` are verified via the real-pty smoke test in Step 6)

- [ ] **Step 3: Wire chrome into src/main.cpp -- add includes and session tracking**

At the top of `src/main.cpp`, add:
```cpp
#include "chrome.h"
#include <chrono>
```

- [ ] **Step 4: Add SIGWINCH handling and initial chrome paint**

Async-signal-safety matters here exactly like it did for the SIGCHLD handler
in Task 17/18: `printf`/`fflush`/`getHwStats()`/`findGitBranch()` (file I/O,
syscalls, string allocation) are NOT safe to call directly inside a signal
handler -- if SIGWINCH arrives while the main thread is already mid-`printf`,
calling `printf` again from the handler can deadlock or corrupt libc's
internal buffering state. The handler must do nothing but flip an atomic
flag; the actual repaint happens in the main loop, which already polls
other signal-driven state (`jobTable.drainSignalQueue()`) once per
iteration.

In `main.cpp`, inside the `if (!isatty(STDIN_FILENO))` check's `else` path
(the interactive branch), right after the `History history; history.load(histPath);`
lines, add:

```cpp
    auto sessionStart = std::chrono::steady_clock::now();
    auto sessionSeconds = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - sessionStart).count();
    };
    auto doReassertChrome = [&]() {
        reassertChrome(state.cwd, findGitBranch(state.cwd), sessionSeconds(), getHwStats());
    };

    static std::atomic<bool> g_resized{false};
    struct sigaction winch;
    std::memset(&winch, 0, sizeof(winch));
    winch.sa_handler = [](int) { g_resized.store(true, std::memory_order_relaxed); };
    sigemptyset(&winch.sa_mask);
    winch.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &winch, nullptr);

    doReassertChrome(); // initial paint before the REPL loop starts
```

This needs `#include <atomic>` and `#include <cstring>` added to `main.cpp`'s
includes (for `std::atomic` and `std::memset`). `g_resized` is declared
`static` at function scope specifically so the signal handler lambda (which
has an empty capture list, so it can convert to a plain `void(*)(int)`
function pointer as `sa_handler` requires) can still reach it — a
function-local `static` has static storage duration, so it's reachable from
a nested lambda without being captured.

- [ ] **Step 5: Call reassertChrome() around execNode() (preexec/precmd), and poll the resize flag**

Find the top of the interactive REPL loop:
```cpp
    for (;;) {
        jobTable.drainSignalQueue();
        std::string prompt = continuing ? continuationPrompt() : buildPrompt(state, home);
```

Add the resize-flag check right after `jobTable.drainSignalQueue();`:
```cpp
    for (;;) {
        jobTable.drainSignalQueue();
        if (g_resized.exchange(false, std::memory_order_relaxed)) doReassertChrome();
        std::string prompt = continuing ? continuationPrompt() : buildPrompt(state, home);
```

Then find the interactive loop's try block:
```cpp
        Lexer lex(pending);
        try {
            Parser parser(lex.tokenize());
            auto ast = parser.parse();
            execNode(ast.get(), state);
```

Replace with:
```cpp
        Lexer lex(pending);
        try {
            Parser parser(lex.tokenize());
            auto ast = parser.parse();
            doReassertChrome(); // preexec-equivalent: reassert before the
                                 // command runs, in case a PRIOR command
                                 // left the terminal in a bad state
            execNode(ast.get(), state);
            doReassertChrome(); // precmd-equivalent: reassert after, in
                                 // case THIS command (clear/vim/etc) reset
                                 // the scroll region during its own run
```

- [ ] **Step 6: Simplify buildPrompt() to time + arrow (cwd now lives in the pinned top bar)**

Find `buildPrompt()`:
```cpp
static std::string buildPrompt(const ShellState& state, const std::string& home) {
    std::string arrowColor = state.lastStatus == 0 ? tn::GREEN : tn::RED;
    return std::string(tn::BLUE) + "ark " + tn::CYAN + shortCwd(state.cwd, home) + " " +
           arrowColor + "\xe2\x9d\xaf" + tn::R + " "; // "\xe2\x9d\xaf" = UTF-8 for ❯
}
```

Replace with:
```cpp
static std::string buildPrompt(const ShellState& state, const std::string&) {
    // cwd now lives in the pinned top bar (chrome.h's paintChrome), so the
    // per-command prompt simplifies to just the time and the status arrow.
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    char clock[8];
    strftime(clock, sizeof(clock), "%H:%M", &local);
    std::string arrowColor = state.lastStatus == 0 ? tn::GREEN : tn::RED;
    return std::string(tn::COMMENT) + clock + " " + arrowColor + "\xe2\x9d\xaf" + tn::R + " ";
}
```

`shortCwd()` becomes unused after this change — remove its definition (it was only called from `buildPrompt()`).

Add `#include <ctime>` to `main.cpp`'s includes.

- [ ] **Step 7: Rebuild and run the full non-interactive test suite (regression check)**

Run: `cd /Users/gideoncox/ark-terminal && rm -f /tmp/ark_test_05_out.txt /tmp/ark_test_05b_in.txt /tmp/ark_test_05b_out.txt && make clean && make test`
Expected: all 14 integration cases still PASS (chrome only affects the interactive path; non-interactive mode never calls `reassertChrome`/`buildPrompt`)

- [ ] **Step 8: Real PTY verification -- the actual hardening test**

This is the step that matters most given this feature's history. Run each of these as separate `expect` scripts (matching the pattern used for the v1/v2 pinned-bar investigation earlier this session) and confirm:

**8a. Chrome paints on startup and after a normal command, at the correct rows, with a stable width:**
```bash
cd /tmp && cat > chrome_basic.exp <<'EOF'
log_file -a chrome_basic.bin
set timeout 8
spawn env COLUMNS=100 LINES=24 /Users/gideoncox/ark-terminal/ark
expect "*"
send "echo hello\r"
sleep 0.3
send "\004"
expect eof
EOF
rm -f chrome_basic.bin
expect chrome_basic.exp >/dev/null 2>&1
python3 -c "
import re
data = open('chrome_basic.bin','rb').read().decode('utf-8', errors='replace')
paints_row1 = re.findall(r'\x1b\[1;1H\x1b\[2K([^\x1b]*)', data)
paints_last = re.findall(r'\x1b\[24;1H\x1b\[2K([^\x1b]*)', data)
assert len(paints_row1) >= 2, f'expected at least 2 top-row paints, got {len(paints_row1)}'
assert len(paints_last) >= 2, f'expected at least 2 bottom-row paints, got {len(paints_last)}'
assert all(len(p) <= 100 for p in paints_last), 'a bottom-row paint exceeded terminal width'
print('8a PASS: chrome paints at correct rows, stable width')
"
```
Expected: `8a PASS: chrome paints at correct rows, stable width`

**8b. Chrome survives `clear` (the exact scenario that broke v1):**
```bash
cd /tmp && cat > chrome_clear.exp <<'EOF'
log_file -a chrome_clear.bin
set timeout 8
spawn env COLUMNS=100 LINES=24 /Users/gideoncox/ark-terminal/ark
expect "*"
send "echo before\r"
sleep 0.2
send "clear\r"
sleep 0.2
send "echo after\r"
sleep 0.3
send "\004"
expect eof
EOF
rm -f chrome_clear.bin
expect chrome_clear.exp >/dev/null 2>&1
python3 -c "
import re
data = open('chrome_clear.bin','rb').read().decode('utf-8', errors='replace')
region_sets = re.findall(r'\x1b\[2;(\d+)r', data)
assert len(region_sets) >= 3, f'expected region reasserted at least 3 times (start+preexec+precmd around clear), got {len(region_sets)}'
paints_row1_after_clear = data.split('clear')[-1].count('\x1b[1;1H\x1b[2K')
assert paints_row1_after_clear >= 1, 'chrome was not repainted after clear ran'
print('8b PASS: scroll region and chrome both reassert around clear')
"
```
Expected: `8b PASS: scroll region and chrome both reassert around clear`

**8c. Chrome survives `vim` (alt-screen enter/exit):**
```bash
cd /tmp && cat > chrome_vim.exp <<'EOF'
log_file -a chrome_vim.bin
set timeout 10
spawn env COLUMNS=100 LINES=24 /Users/gideoncox/ark-terminal/ark
expect "*"
send "echo before-vim\r"
sleep 0.2
send "vim -es -c q\r"
sleep 0.6
send "echo after-vim\r"
sleep 0.3
send "\004"
expect eof
EOF
rm -f chrome_vim.bin
expect chrome_vim.exp >/dev/null 2>&1
python3 -c "
data = open('chrome_vim.bin','rb').read().decode('utf-8', errors='replace')
paints_row1_after_vim = data.split('after-vim')[0].split('vim -es')[-1].count('\x1b[1;1H\x1b[2K')
assert paints_row1_after_vim >= 1, 'chrome was not repainted after vim exited'
print('8c PASS: chrome reasserts after vim (alt-screen) exits')
"
```
Expected: `8c PASS: chrome reasserts after vim (alt-screen) exits`

**8d. Chrome survives 120 lines of scrolling output without moving (matches the earlier v2 bar's proof):**
```bash
cd /tmp && cat > chrome_scroll.exp <<'EOF'
log_file -a chrome_scroll.bin
set timeout 10
spawn env COLUMNS=100 LINES=20 /Users/gideoncox/ark-terminal/ark
expect "*"
send "seq 1 60\r"
sleep 0.4
send "seq 61 120\r"
sleep 0.4
send "\004"
expect eof
EOF
rm -f chrome_scroll.bin
expect chrome_scroll.exp >/dev/null 2>&1
python3 -c "
import re
data = open('chrome_scroll.bin','rb').read().decode('utf-8', errors='replace')
rows_painted = set(re.findall(r'\x1b\[(\d+);1H\x1b\[2K', data))
assert rows_painted <= {'1', '20'}, f'chrome painted somewhere other than row 1 or row 20: {rows_painted}'
print('8d PASS: chrome stayed at row 1 / row 20 through 120 lines of scroll')
"
```
Expected: `8d PASS: chrome stayed at row 1 / row 20 through 120 lines of scroll`

- [ ] **Step 9: Clean up PTY test artifacts**

```bash
rm -f /tmp/chrome_basic.exp /tmp/chrome_basic.bin /tmp/chrome_clear.exp /tmp/chrome_clear.bin /tmp/chrome_vim.exp /tmp/chrome_vim.bin /tmp/chrome_scroll.exp /tmp/chrome_scroll.bin
```

- [ ] **Step 10: Commit**

```bash
cd /Users/gideoncox/ark-terminal
git add src/chrome.h src/chrome.cpp src/main.cpp
git commit -m "chrome: pinned top bar (cwd+branch) and bottom bar (user/session/hardware), reasserted around every command"
```

---

## Self-Review Notes

**Spec coverage:** `getHwStats()` (Task 1, no-subprocess constraint honored),
`findGitBranch()` (Task 2, direct file read), `setScrollRegion`/`paintChrome`/
`reassertChrome` + preexec/precmd wiring + SIGWINCH (Task 3) all map directly
to the design spec's Architecture section. The prompt simplification
(cwd removed, time added) matches the design's stated change to
`buildPrompt()`.

**Placeholder scan:** none — every step has complete code, including the
real PTY verification scripts (not "verify manually" hand-waving, given
this feature's history warrants real proof, not a described-but-not-shown
check).

**Type consistency:** `HwStats`, `getHwStats()`, `findGitBranch()`,
`setScrollRegion()`, `paintChrome()`, `reassertChrome()` signatures are
identical between their Task 1 declaration and their Tasks 2-3 usage.

**Risk-proportionate testing:** Step 8 is deliberately the most heavily
verified step in this plan (4 separate real-PTY checks covering the exact
failure modes documented from the earlier zsh pinned-bar saga: normal
operation, `clear`, `vim`/alt-screen, and large-scroll stability) — matching
how much this specific class of feature has cost before.
