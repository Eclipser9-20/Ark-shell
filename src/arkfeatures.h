#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// ── ark power features (settings-gated where noted) ─────────────────────────
// A grab-bag module for the "best of all worlds" features that don't belong to
// one existing translation unit: private mode, universal variables, spelling
// correction, and man-page-driven flag completion. Each is small and
// self-contained; the shared home is here so main/edit/exec/builtins can all
// reach them without a web of cross-includes.

// ── Private Mode ────────────────────────────────────────────────────────────
// While on, the shell writes NOTHING to the history file or in-memory history
// (nothing to disk). Toggled by the `private` builtin; also primed at startup
// from ARK_PRIVATE=1.
bool arkPrivateMode();
void arkSetPrivateMode(bool on);

// ── Universal Variables (fish-style) ────────────────────────────────────────
// Persist across ALL windows and survive reboot, via a single file
// (~/.config/ark/universal). Every interactive session loads them at startup
// and re-syncs at each prompt, so a `uvar` set in one window shows up in the
// others. Stored as NAME=VALUE lines; also exported into the environment so
// child processes see them.
namespace uvar {
    std::string path();
    // Load the file into `vars` (overwriting those keys) and setenv() each.
    // Returns true if the file's contents changed since the last load (so the
    // caller can skip work when nothing moved).
    bool loadInto(std::unordered_map<std::string, std::string>& vars);
    void set(const std::string& name, const std::string& value); // persist + setenv
    void unset(const std::string& name);
    std::unordered_map<std::string, std::string> all();
}

// ── Intelligent Spelling Correction ─────────────────────────────────────────
int levenshtein(const std::string& a, const std::string& b);
// Closest real command name to a not-found `typo` (builtins + $PATH), or "" if
// nothing is close enough to be worth suggesting.
std::string suggestCommand(const std::string& typo);

// True if `name` is a runnable command: a builtin or a $PATH executable. Backed
// by a set built once (same source as suggestCommand), so it's cheap enough to
// call per-keystroke from the syntax highlighter. Does NOT know about aliases /
// functions (the caller layers those in) or slash-paths (caller uses access()).
bool commandExists(const std::string& name);

// ── Homebrew "command-not-found" (the apt-get suggestion, for brew) ─────────
// The Homebrew formula that PROVIDES command `cmd`, so an unknown command can
// suggest `brew install <formula>` (Ubuntu's command-not-found, brew edition).
// Uses `brew which-formula` when the homebrew/command-not-found tap is present
// (handles cmd != formula, e.g. rg -> ripgrep), else falls back to matching
// `cmd` against the full formula-name list. Returns "" if brew is absent or
// nothing provides it. Cached per command. ARK_BREW_SUGGEST=0 short-circuits it.
std::string brewFormulaFor(const std::string& cmd);

// ── Dynamic Man-Page Completions ────────────────────────────────────────────
// Option flags (-x / --long) parsed out of `man <cmd>`, cached per command so
// the subprocess runs at most once per command per session. `prefix` filters
// (e.g. "--co" -> "--color", "--count"); empty prefix returns all flags.
std::vector<std::string> manPageFlags(const std::string& cmd, const std::string& prefix);
