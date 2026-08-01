#pragma once
#include <string>
#include <vector>

class JobTable;

// Session-level owned scrollback ("mini-tmux"). Off unless ARK_SCROLLBACK=1.
// Ark keeps a session-lifetime ring of COMPLETED output lines; wheel/Shift+PgUp
// at the prompt scrolls back through it with the pinned bars in place. The live
// prompt / editing line is still drawn directly by readLine -- the ring holds
// only finished lines (see docs/superpowers/specs/2026-07-31-ark-session-mux-design.md).
namespace scrollback {

// True when the session mux is active (ARK_SCROLLBACK=1, interactive tty).
bool enabled();

// Initialize / re-measure the session ring's viewport from the current terminal
// size. Safe to call repeatedly (e.g. on SIGWINCH). No-op when disabled.
void init();

// Feed raw terminal output (bytes ark itself writes via std::cout -- builtin
// output, tables, banners) into the ring through a persistent line model, so
// builtin commands' output is scrollable too. Splits into logical lines on '\n'
// and pushes each. Safe to call with partial chunks. No-op when disabled OR when
// capture is off (see setCapturing).
void recordOutput(const char* data, size_t n);

// Gate for recordOutput(). std::cout carries BOTH command output (builtins --
// which we want in the ring) AND readLine's prompt/line-editing redraws (cursor
// moves like "\x1b[<n>C", ghost text, reprints -- which we must NOT capture, or
// they duplicate the prompt and leak escape fragments like "30C" into the ring).
// main() sets this true only while a command actually executes.
void setCapturing(bool on);

// Record a completed output line into the scrollback ring. Does NOT print --
// the caller has already displayed it live (this just remembers it for
// scroll-back). `text` is one logical line (may contain SGR), no trailing '\n'.
// Returns the line's absolute sequence number (for recolorLine), or -1 if the
// mux is disabled / nothing was recorded.
long record(const std::string& text);

// Recolor an already-recorded line in place (e.g. redraw a failed command's
// prompt line with a red arrow + exit code). `seq` is a value returned by
// record(). Repaints the live tail if currently at live so the change shows
// immediately. No-op if disabled or the line has scrolled out of the ring.
void recolorLine(long seq, const std::string& text);

// Scroll the viewport by `delta` physical rows (delta<0 = older). Enters scroll
// mode and repaints the region from the ring. No-op when disabled.
void scrollBy(int delta);

// True when the viewport is pinned to the live bottom (not scrolled up).
bool atLive();

// Snap back to the live bottom and repaint the live prompt area. Call when any
// non-scroll key arrives while scrolled up. No-op when disabled or already live.
void snapToLive();

// Number of physical rows a Shift+PgUp/PgDn should move (region height).
int pageRows();

// Handle a terminal resize: re-measure the region from the new size, re-wrap the
// ring, and repaint the current view (the scrolled-up window if scrolled, else
// the live tail) so the output survives a SIGWINCH instead of being cleared away
// by the chrome's full-screen resize repaint. No-op when disabled.
void onResize();

// Run a foreground command on a session-owned PTY: display its output live AND
// record it into the scrollback ring so it survives scroll-back after the
// command ends. Returns the child exit status (WEXITSTATUS / 128+sig), or -1 if
// the mux couldn't start it (caller falls back to the normal spawn path). A
// Ctrl-Z'd command becomes a Stopped JobTable job (returns 128+SIGTSTP).
int runForeground(const std::vector<std::string>& argv, JobTable& jobs);

// True if `jobId` is a suspended mux job (its PTY is held for resume()).
bool isSuspendedJob(int jobId);

// Resume a suspended mux job in the foreground (SIGCONT + re-attach its PTY).
// Returns its exit status, 128+SIGTSTP if it stops again, or -1 if unknown.
int resume(JobTable& jobs, int jobId);

}
