# ark audit — features & bugs people will complain about / want

Cross-validated 2026 by a static read of every subsystem + differential testing
vs `/bin/bash` and `/bin/zsh` + an ASAN/UBSAN build. Every item below is a
**confirmed probe**, framed as the user complaint. Priority = complaint impact ×
how common the trigger is. `E` = rough effort (S/M/L).

> **Good news up top:** no memory-corruption under ASAN/UBSAN on any malformed
> input; the recursion guard holds in the shipping build; Ctrl-R is a real
> incremental search; `$(( ))` (with `**`/ternary/comma/hex/octal), brace ranges,
> `${x^^}`/`${x,,}`, suffix/prefix strip, job control basics, and quoting/splitting
> of `"$@"` are all correct. The gaps are breadth, not rot.

---

## TIER 0 — Silent wrong behavior (fix FIRST: these make ark *untrustworthy*)
These accept input and do the WRONG thing with no error — the worst kind of bug,
because scripts "work" then corrupt/misbehave.

0.1  `set -e` / `-u` / `-o pipefail` / `-x` accepted but **ignored** — `set -euo pipefail` is a silent no-op. **dealbreaker · E:M**
0.2  Changing `$IFS` does **not** affect word-splitting (`IFS=:; set -- $p` → 1 field) — data parsing silently breaks. **dealbreaker · E:M**
0.3  `read -p/-s/-n/-t/-a/-d` flags are treated as **variable names** (only `-r` handled) — no prompt/silent/timeout/array. **high · E:M**
0.4  `VAR=x cmd` env-prefix **breaks inside any pipeline** (`TZ=UTC date | ...` → "VAR=x: command not found"). **high · E:M**
0.5  Permission-denied exit code is **127, should be 126**. **medium · E:S**
0.6  Unbalanced `"`/`'`/`$(`/`${`/`$((` silently **succeed** instead of a syntax error → typos give wrong results. **medium · E:M**
0.7  `read < missingfile` **HANGS** on the terminal instead of failing (rc 1). **high · E:S**
0.8  Missing input-redirect file → bogus "cmd: command not found" (+spell hint), rc 127, should be "No such file", rc 1. **medium · E:S**

## TIER 1 — Missing core syntax (loud, common script breakage)
1.1  **Arrays**: `a=(1 2 3)`, `${a[@]}`, `${a[0]}`, `${#a[@]}`, `+=(...)`, `declare -A`. Whole feature absent. **dealbreaker · E:L**
1.2  **`[[ ... ]]`** conditional incl `=~` regex and `<`/`>` string compare. **dealbreaker · E:L**
1.3  **`{ ...; }`** command grouping (not a subshell) — `{ x=2; }; echo $x`. **dealbreaker · E:M**
1.4  **`$'...'`** ANSI-C quoting (`$'a\tb'`, `\n`, `\x41`). **dealbreaker · E:M**
1.5  **`<<<`** here-strings. **high · E:S**
1.6  **`for ((i=0;i<n;i++))`** C-style loop. **high · E:M**
1.7  **`until`** loops. **high · E:S**
1.8  **`((expr))`** arithmetic command form + **`let`**. **high · E:S**
1.9  **Process substitution** `<(cmd)` / `>(cmd)`. **high · E:M**
1.10 `case` patterns only match `*` — `?` and `[...]` classes don't. **high · E:S**
1.11 `func() ( ... )` subshell-body function. **medium · E:S**
1.12 Anchored `${x/#pat/r}` / `${x/%pat/r}`, `case ;&`/`;;&` fallthrough, `select`, extglob `@(...)`/`!(...)`. **low · E:M**

## TIER 2 — Missing builtins & special vars
2.1  **`printf`** as a builtin (currently the external — no `%q`, no `-v var`). **high · E:M**
2.2  **`eval`**. **dealbreaker · E:M**
2.3  **`shift`** (positional params). **dealbreaker · E:S**
2.4  **`:`** colon no-op (`while :`, `: ${X:=def}`). **high · E:S**
2.5  **`trap`** EXIT/INT/signal handlers. **high · E:M**
2.6  **`getopts`** (must be in-shell to set `$OPTIND`/`$opt`). **high · E:M**
2.7  **`exec`** (replace process; `exec >file` redirect self). **high · E:M**
2.8  Special vars: **`$RANDOM`**, **`$SECONDS`**, **`$FUNCNAME`**, `$LINENO`, `$-`, `$_` (wrong). **high · E:S**
2.9  **`${!var}`** indirect, **`~user`** tilde, **`${x:?msg}`** error form. **medium · E:S**
2.10 **`$PIPESTATUS`** (needs arrays). **medium · E:M**
2.11 `let`, `declare`/`typeset`, `readonly`. **medium · E:M**
2.12 `type -a`/`-p` flags (parsed as command names), `export -p` (prints nothing), `cd -` doesn't echo target. **medium · E:S**
2.13 Make real builtins: `command`, `wait %job`, `kill %job`, `umask`, `hash`, `times`; and `true`/`false`/`:` (perf in `while true`). **low-med · E:S–M**

## TIER 3 — Interactive parity (what the fish/zsh crowd switches *for*)
3.1  **Programmable per-command completion** — `git <sub>`/branches, `cd` dir-only, `kill` PIDs, `ssh` hosts, `make` targets. Today: files + man-flags + PATH only. THE headline gap. **dealbreaker · E:L**
3.2  **Bracketed paste** — pasting a multi-line block runs each line early (data-loss footgun). Enable `?2004h`, buffer between `200~`/`201~`. **high · E:S**
3.3  **Prompt customization** (`PS1`-like) + **git dirty/ahead/behind** in prompt (today: branch-name only, no status). **high · E:M+M**
3.4  **`wcwidth`** — emoji/CJK/combining chars break cursor math & wrapping (1-col-per-codepoint approximation). **high · E:M**
3.5  **Prefix history search on Up** (type `git ` + Up → last git cmd). **medium · E:S**
3.6  **History dedup** / `HISTIGNORE` / `HISTCONTROL` (today: unconditional append). **medium · E:S**
3.7  **History expansion** `!!`, `!$`, `!abc`, `^old^new`. **medium · E:M**
3.8  **Ctrl-L** clear-screen (unbound). **low-med · E:S**
3.9  **vi mode** (`set -o vi` silently no-ops — at minimum error loudly). **medium · E:L / S**
3.10 kill-ring **yank-pop** (M-y) + **undo** (C-_). **medium · E:M**
3.11 True multi-line editing of a recalled block. **low · E:L**

---

## Recommended execution order
1. **Trust wave (Tier 0)** — mostly S/M, kills the silent-wrong-behavior class. Highest trust-per-effort. Includes the `read < missing` hang and 126/127.
2. **Quick-win syntax** — `<<<`, `until`, `((`/`let`, `:`, `shift`, `eval`, `case ?/[...]`, `$RANDOM`/`$SECONDS`/`$FUNCNAME`, `$'...'`, `{ }` groups. Small each, huge coverage gain.
3. **Big-ticket scripting** — arrays (1.1) → unlocks `$PIPESTATUS`; `[[ ]]` (1.2); `printf` builtin; `trap`; `getopts`; process substitution.
4. **Interactive parity** — bracketed paste (small, removes footgun) → programmable completion (the headline) → prompt/PS1 + git status → wcwidth → prefix history search.

Key files: `src/parser.cpp` `src/lexer.cpp` (syntax), `src/expand.cpp` (arrays/IFS/`$'...'`/special vars), `src/exec.cpp` (pipefail/env-prefix/redirect errors/set -e), `src/builtins.cpp` (builtins + `set` options + `read`), `src/complete.cpp` (completion), `src/edit.cpp` (paste/wcwidth/history/keys), `src/main.cpp` (prompt).
