# ark

A from-scratch interactive shell for macOS, written in C++20 — the best of
bash, zsh, fish, and nushell, plus a few ideas of its own. No bash/zsh
underneath: ark has its own lexer, parser, expander, job control, and line
editor.

## Highlights

- **Full POSIX-ish language** — pipelines, `&&`/`||`/`;`, subshells, `if`/`elif`/
  `else`, `for`/`while`, `case`, functions, `local`, `break`/`continue`/`return`,
  here-docs (`<<`, `<<-`, `<<'EOF'`), redirections, background jobs + job control.
- **Rich expansion** — `${VAR:-def}` `${VAR#pfx}` `${VAR%sfx}` `${VAR/a/b}`
  `${VAR:off:len}`, `$(cmd)` / backticks, arithmetic `$(( ))`, brace expansion
  `{a,b}` / `{1..9}`, globs including recursive `**`, and tilde.
- **fish-style autosuggestions** — dim ghost text from history/files, accepted
  with →/Ctrl-F/Tab. Context-aware: a match from the current directory wins.
- **Real-time syntax highlighting** — commands, flags, strings, and variables
  colored as you type; unknown commands can be flagged red before you hit Enter.
- **Powerful completion** — Tab completes commands, paths, a background index of
  your whole home tree, and flags read from a command's **man page**.
- **nushell mode** — `ls` renders as a bordered table (`ARK_NU_MODE=1`).
- **Advanced metadata globbing** — `*.log(.mh-1)` (files modified in the last
  hour), `*(/)` (dirs), `*(.L+1000)` (files over 1000 bytes), and more.
- **Universal variables** — `uvar NAME VALUE` persists across every window and
  survives reboot.
- **Shared history** — live across all tabs/panes; `private` mode pauses it.
- **Intelligent spelling correction** — `gti` → "did you mean `git`?", with an
  opt-in auto-fix mode; unknown-but-real tools suggest how to install them.
- **Pinned status chrome** — a top bar (cwd + git branch) and a bottom bar
  (user@host, session time, live CPU/memory), drawn with a scroll region so
  normal output never disturbs them. Fully themeable / disableable.

## Build

Requires a C++20 compiler (clang++).

```sh
make            # -> ./ark
make test       # integration tests
make unittest   # per-module unit tests
make install    # -> /usr/local/bin/ark  (PREFIX overridable)
```

## Use as your login shell

```sh
echo /usr/local/bin/ark | sudo tee -a /etc/shells
chsh -s /usr/local/bin/ark
```

## Configuration

On first run ark writes `~/.config/ark/ark.config` — a fully commented catalogue
of every setting and feature, ready to uncomment. Edit it any time with the
`ark-settings` builtin (opens it in `$EDITOR`). Every feature is toggle-able via
an `ARK_*` environment variable documented in that file.

## License

MIT — see [LICENSE](LICENSE).
