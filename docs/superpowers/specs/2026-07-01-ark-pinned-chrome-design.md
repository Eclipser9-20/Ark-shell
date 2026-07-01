# ark — Phase 2b: Pinned Screen Chrome (top bar + bottom bar)

## Vision

A permanently-pinned top row (cwd + git branch) and bottom row (user@host +
session length on the left, hardware stats on the right) that stay fixed to
the screen edges while normal command output scrolls between them. The
interactive prompt itself simplifies to just time + the status arrow, since
directory context now lives in the pinned top bar.

## Explicit risk acknowledgment

This is the same DECSTBM/scroll-region pinning technique that broke the
user's real Ghostty terminal twice earlier this session (in a zsh
implementation) — corrupting the display in ways that never reproduced in
scripted PTY tests. The user explicitly chose to attempt it again in ark
specifically, reasoning that since ark fully owns its own terminal
rendering loop (raw termios, direct write control) rather than being a
shell script fighting an independently-configured terminal, the same class
of mechanism can be made to hold up. Every lesson from that earlier failure
is deliberately re-applied here (see "Hardening" below); the user accepted
that ark spawning arbitrary child processes (vim, less, clear) still
carries real residual risk that no amount of hardening fully eliminates.

## Priorities

Performance is non-negotiable here specifically because chrome repaints
happen on EVERY command boundary (before and after execution) — any
subprocess-based stat gathering reintroduces the exact latency regression
already found and fixed once this session (a statusline calling `top -l 1`
added ~0.2s per repaint). Every stat this feature shows must come from a
direct syscall or a local file read, never a forked subprocess, in the
repaint hot path.

## Architecture

```
Shell startup:
  - query LINES/COLUMNS (ioctl TIOCGWINSZ)
  - set scroll region: \e[2;(LINES-1)r  (excludes row 1 and the last row)
  - position cursor inside the scroll region
  - paintChrome() once
  - install SIGWINCH handler -> re-query size, reset region, repaint

Per command (in main.cpp's interactive loop):
  - BEFORE execNode(): reassertRegionAndPaint()  (preexec-equivalent)
  - AFTER  execNode(): reassertRegionAndPaint()  (precmd-equivalent)
  - reasserting on BOTH boundaries is the exact fix for v1's "cuts into
    output" bug: a child process (clear/vim/etc) can reset margins during
    its own run, and we can't intercept that -- but we WILL fix it back
    before the very next thing prints, on either side of the command.

paintChrome():
  - \e[?2026h                (begin synchronized update)
  - \e7                       (save cursor -- DECSC)
  - \e[1;1H \e[2K             (row 1, clear, print cwd + git branch)
  - \e[{LINES};1H \e[2K       (last row, clear, print user@host+session
                               left-aligned, hardware stats right-aligned,
                               padded to fill COLUMNS)
  - \e8                       (restore cursor -- DECRC)
  - \e[?2026l                 (end synchronized update)
  - Absolute positioning + save/restore cursor only -- v1's staircase bug
    was relative-cursor-movement drift; this repeats v2's fix, not v1's
    mistake.

Prompt (readLine(), unchanged mechanism, changed content):
  - was: "ark <cwd> <arrow>" -- cwd is now redundant with the pinned top
    bar, so it simplifies to "<HH:MM> <arrow>"
```

## Components

- **src/chrome.h/.cpp** (new)
  - `struct HwStats { double load1; double memUsedGB; double memTotalGB; };`
  - `HwStats getHwStats();` — `getloadavg()` (POSIX, no subprocess) for
    load; Mach `host_statistics64(HOST_VM_INFO64)` + `sysctlbyname("hw.memsize")`
    (direct syscalls, no subprocess) for memory. No GPU/battery in this
    version — deliberately deferred to avoid any subprocess in the hot path
    (GPU% previously required `ioreg`, a subprocess call).
  - `std::string findGitBranch(const std::string& cwd);` — walks up parent
    directories from `cwd` looking for a `.git` entry; if found, reads
    `.git/HEAD` directly (a plain text file: either `ref: refs/heads/NAME`
    or a raw commit SHA for detached HEAD) rather than shelling out to
    `git symbolic-ref` — a file read has no fork/exec cost at all. Returns
    empty string if no `.git` found anywhere up the tree.
  - `void setScrollRegion();` — queries `TIOCGWINSZ`, emits `\e[2;(rows-1)r`.
  - `void paintChrome(const ShellState& state, time_point sessionStart);` —
    the full paint routine described above.
  - `void reassertChrome(const ShellState& state, time_point sessionStart);`
    — `setScrollRegion()` + `paintChrome()`, the single function called at
    every command boundary and from the SIGWINCH handler.
- **src/main.cpp** (modified)
  - Interactive-mode setup: record `sessionStart = steady_clock::now()`,
    call `reassertChrome()` once before the REPL loop starts, install
    `SIGWINCH` handler that calls `reassertChrome()`.
  - REPL loop: call `reassertChrome()` immediately before and immediately
    after the `execNode()` call.
  - Prompt string: replace `buildPrompt()`'s cwd segment with the current
    time (`HH:MM`), since cwd now lives in the pinned top bar.

## Data Flow

```
main() [interactive branch]
  -> sessionStart = now()
  -> reassertChrome()                         [initial paint]
  -> loop:
       reassertChrome()                       [preexec-equivalent]
       execNode(ast, state)                   [command runs; may itself
                                                 reset terminal state --
                                                 not something we can
                                                 prevent, only fix after]
       reassertChrome()                       [precmd-equivalent]
       readLine(buildPrompt(...), history)     [next prompt: time + arrow]
```

## Error Handling

- `findGitBranch()` never throws: a missing `.git` directory, an unreadable
  `.git/HEAD`, or a malformed HEAD file all just return an empty string
  (no branch segment shown), matching how a plain, non-git directory should
  look (blank, not an error message cluttering the chrome).
- `getHwStats()` never throws: `getloadavg()`/`host_statistics64()` failures
  (extremely rare, e.g. under memory pressure) fall back to `0.0` values
  rather than propagating an error into the display.
- `setScrollRegion()` guards against a degenerate terminal size (rows <= 2)
  by skipping the region-setting entirely (no chrome without at least one
  scrollable row in between).

## Testing

`tests/chrome_test.cpp` (standalone, like other module tests): asserts
`findGitBranch()` correctly finds a branch name from a real temporary git
repo (created via `git init` + a commit in a `/tmp` directory during the
test), returns empty for a non-repo directory, and handles a detached-HEAD
SHA. `getHwStats()` is checked only for "doesn't crash and returns
plausible ranges" (load >= 0, memTotalGB > 0) since exact values are
inherently non-deterministic.

The pinning mechanism itself (paintChrome/reassertChrome/SIGWINCH) is
NOT unit-testable in the traditional sense — it's verified via real PTY
smoke tests (matching the approach used for the line editor and syntax
highlighting): a scripted `expect` session that runs several commands,
resizes the terminal, and runs `clear`/`vim`, checking that the chrome rows
are repainted correctly at row 1 and the last row after each of those
events, with no stray content bleeding into the scroll region.

## Explicitly Out of Scope

- GPU%/battery in the hardware stats (would require a subprocess in the hot
  path — can revisit later behind an opt-in throttle if wanted).
- Any attempt to make the chrome survive an `exec()`'d child that itself
  manipulates DECSTBM margins and never returns control cleanly (not
  something the shell can defend against post-hoc if the child never gives
  control back before exiting).
