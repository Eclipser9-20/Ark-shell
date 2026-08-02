#!/bin/sh
# Stops the "can't scroll ls" bug from ever coming back.
#
# `ls` broke scrollback because it was a builtin that hand-rolled fork+execvp to
# run the real /bin/ls, writing straight to the tty and bypassing the scrollback
# ring entirely. The fix routes such passthroughs through runProgramForBuiltin(),
# which captures line output into the ring (or hands a TUI a clean full screen).
#
# This guard makes that structural: ANY raw process spawn in src/builtins.cpp is
# banned, so no future builtin can silently bypass capture again. A spawn that is
# genuinely meant to be raw (e.g. `exec`, which REPLACES the process) must say so
# on the same line with the marker  ARK_RAW_SPAWN_OK  -- an explicit, greppable
# opt-out that shows up in review. Everything else must use runProgramForBuiltin.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

# Spawn primitives a builtin must not CALL directly. Match only actual calls --
# the identifier immediately followed by '(' (optionally spaces) -- so the word
# appearing in a comment ("execvp searches PATH") never trips the guard.
pat='(execvp|execv|execlp|execle|execvpe|execl|posix_spawnp|posix_spawn|fork)[[:space:]]*\('

# Ignore pure-comment lines (leading // or *) so prose can mention these freely.
offenders="$(grep -nE "$pat" src/builtins.cpp | grep -vE '^[0-9]+:[[:space:]]*(//|\*)' | grep -v 'ARK_RAW_SPAWN_OK' || true)"

if [ -n "$offenders" ]; then
    echo "FAIL: builtin_spawn_guard -- raw process spawn in src/builtins.cpp:"
    echo "$offenders" | sed 's/^/  /'
    echo "  A builtin must run external programs via runProgramForBuiltin() (exec.h)"
    echo "  so their output is captured into scrollback. If a raw spawn is truly"
    echo "  intended (like 'exec' replacing the process), append the marker"
    echo "  'ARK_RAW_SPAWN_OK' with a reason on that line."
    exit 1
fi
echo "builtin spawn guard: no unsanctioned fork/exec in builtins.cpp"
exit 0
