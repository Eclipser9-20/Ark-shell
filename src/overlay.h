#pragma once
#include <string>
#include <vector>

// ── Overlay compositor (ARK_OVERLAY=1, experimental) ────────────────────────
// The only way to have a FIXED top bar AND working scrollback in a terminal is
// to stop relying on the terminal's own scrollback and keep our own. So under
// ARK_OVERLAY ark runs each foreground command through a PTY it owns:
//   * output is mirrored live into the "deadzone" (the scroll region rows
//     2..N-1), so the pinned top/bottom bars are never disturbed;
//   * every output line is also appended to ark's own scrollback ring buffer;
//   * the mouse wheel scrolls THAT buffer inside the deadzone while the bars
//     stay put -- a real overlay, the way tmux draws its status line.
// A full-screen TUI (it switches to the alternate screen, \x1b[?1049h) is
// detected and passed straight through -- scrollback of a TUI is meaningless,
// and the region is dropped for it so it gets the whole screen.
//
// This header is intentionally standalone: overlayEnabled() lets the exec path
// decide whether to route a foreground external command through overlayRun()
// instead of a plain posix_spawn, with a single branch.

namespace overlay {

// ARK_OVERLAY=1 turns the compositor on. Off by default (experimental).
bool enabled();

// Run `argv` as a foreground command through ark's PTY, mirroring its output
// into the current scroll region and capturing it for scrollback. Handles
// window-resize propagation and alt-screen pass-through. Returns the child's
// exit status (128+signal if it was killed), or -1 if the PTY couldn't be set
// up (caller should fall back to a normal spawn). `env` is the child environ.
int run(const std::vector<std::string>& argv, char** env);

} // namespace overlay
