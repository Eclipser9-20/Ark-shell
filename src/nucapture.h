#pragma once
#include <string>
#include <vector>

// ── Nu-mode structured auto-format (ARK_NU_MODE=1) ──────────────────────────
// The nushell-flavoured half of ark: when nu mode is on and you run a plain
// foreground command whose output lands on the terminal, ark runs it through a
// PTY it owns (so the child still thinks it's on a real terminal -- colors,
// columns, progress bars all intact) and buffers the output. If that output is
// a single JSON document, ark renders it as a bordered nu table via the
// value.cpp engine instead of dumping raw text; anything else is streamed
// through byte-for-byte, so nothing regresses.
//
// This is the "it just works" complement to the explicit `from json` builtin:
// `curl …/api` formats itself, no `| from json` needed.

namespace nucapture {

// ARK_NU_MODE=1 turns the auto-format path on (same switch as nu-style `ls`).
bool enabled();

// Commands that OWN the terminal (editors, pagers, full-screen TUIs, REPLs)
// must not be captured -- they need the real tty and normal job control. For
// these the caller uses the plain spawn path; nucapture is skipped.
bool isInteractiveCommand(const std::string& name);

// Run argv as a foreground command through a PTY. If the whole output parses
// as one JSON value, render it as a nu table; otherwise stream it through
// unchanged. A full-screen TUI (alt-screen) is detected and passed straight
// through. Returns the child's exit status (128+signal if it was killed), or
// -1 if the PTY couldn't be created (caller should fall back to a normal
// spawn). `env` is accepted for symmetry; the child inherits environ via fork.
int run(const std::vector<std::string>& argv, char** env);

} // namespace nucapture
