<div align="center">

# ⚡ ark

**A from-scratch interactive shell — the best of bash, zsh, fish, and nushell, in one C++20 binary.**

No bash or zsh underneath. ark has its *own* lexer, parser, expander, globber, job control, and line editor — plus fish-style autosuggestions, live syntax highlighting, a pinned status bar, and the ability to run itself over SSH on a box that's never heard of it.

[![release](https://img.shields.io/github/v/tag/Eclipser9-20/Ark-shell?label=release&sort=semver)](https://github.com/Eclipser9-20/Ark-shell/releases)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)](https://en.cppreference.com/w/cpp/20)
[![platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)](#install)

</div>

## Install

```sh
brew tap eclipser9-20/ark-shell
brew install ark-shell
```

<sub>Or from source — see [Building](#building). Arch/AUR: `yay -S ark-shell` (coming soon).</sub>

## Why ark?

Most "modern shells" are a config layer on top of zsh. **ark isn't.** It's a real shell written from zero in C++20 — which means it can do things a plugin can't:

- **`assh user@host` — your shell, on any box, with nothing installed there.** ark ships the matching prebuilt binary over SSH, runs it as your interactive shell, and deletes it on exit. No install, no config left behind on the remote. Your fish-style editing, your prompt, everywhere.
- **A pinned status bar that survives scrolling** — cwd + git branch up top, `user@host` + session time + live CPU/memory down bottom, held in place with a real scroll region. Or go fully plain with one setting.
- **It's one static-ish binary.** `make && make install`. No runtime, no plugin manager, no framework.

## Features

- **Full POSIX-ish language** — pipelines, `&&`/`||`/`;`, subshells, `if`/`for`/`while`/`case`, functions, `local`, `break`/`continue`/`return`, here-docs (`<<`, `<<-`, `<<'EOF'`), redirections, background jobs + real job control (Ctrl-Z/`fg`/`bg`).
- **Rich expansion** — `${VAR:-def}` `${VAR#pfx}` `${VAR%sfx}` `${VAR/a/b}` `${VAR:off:len}`, `$(cmd)`, arithmetic `$(( ))`, brace expansion `{a,b}`/`{1..9}`, recursive `**` globs, tilde.
- **fish-style autosuggestions** — dim ghost text from history + files, context-aware (a match from the current directory wins). Accept with →/Ctrl-F/Tab.
- **Real-time syntax highlighting** — commands, flags, strings, variables colored as you type; unknown commands flagged before you hit Enter.
- **Powerful Tab completion** — commands, paths, a background index of your whole home tree, and flags pulled straight from a command's **man page**.
- **Metadata globbing** — `*.log(.mh-1)` (modified in the last hour), `*(/)` (dirs), `*(.L+1000)` (files over 1000 bytes), `*(x)` (executable), and more.
- **Universal variables** — `uvar NAME VALUE` persists across every window *and* survives reboot.
- **Shared history** — live across all tabs/panes; `private` mode pauses it.
- **Spelling correction & install hints** — `gti` → "did you mean `git`?"; an unknown-but-real tool offers to install it via your package manager (brew/apt/dnf/pacman…), one keypress.
- **nushell mode** — `ls` as a bordered, colorized table (`ARK_NU_MODE=1`).
- **Colorized `ls`**, spelling autocorrect, an embedded startup banner, and a fully-commented config where **every** feature is one uncomment away.

## ark vs. the field

| | ark | bash | zsh | fish |
|---|:--:|:--:|:--:|:--:|
| From-scratch (no bash/zsh under it) | ✅ | — | — | ✅ |
| POSIX-style scripting | ✅ | ✅ | ✅ | ✕ |
| Autosuggestions out of the box | ✅ | ✕ | plugin | ✅ |
| Syntax highlighting out of the box | ✅ | ✕ | plugin | ✅ |
| Pinned status bar (built in) | ✅ | ✕ | ✕ | ✕ |
| Run your shell over SSH, zero remote install | ✅ | ✕ | ✕ | ✕ |
| Single binary, no framework | ✅ | ✅ | ✅ | ✅ |

## Use ark as your login shell

```sh
echo "$(brew --prefix)/bin/ark" | sudo tee -a /etc/shells
chsh -s "$(brew --prefix)/bin/ark"
```

## Configuration

On first run ark writes `~/.config/ark/ark.config` — a fully commented catalogue of every setting, ready to uncomment. Edit it with `ark-settings`, reload live with `ark-reload`. Every feature is an `ARK_*` toggle documented there. Want it to look like a stock shell? `export ARK_DEFAULT_TERMINAL=1`.

## Building

Requires a C++20 compiler (clang++ or g++).

```sh
make            # -> ./ark
make test       # integration tests
make install    # -> /usr/local/bin/ark   (PREFIX overridable)
```

## License

MIT — see [LICENSE](LICENSE).
