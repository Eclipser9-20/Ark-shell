<div align="center">

# ⚡ ark

**A from-scratch interactive shell — the best of bash, zsh, fish, and nushell, in one C++20 binary.**

No bash or zsh underneath. ark has its *own* lexer, parser, expander, globber, job control, and line editor — plus fish-style autosuggestions, live syntax highlighting, a pinned status bar, and the ability to run itself over SSH on a box that's never heard of it.

[![CI](https://github.com/Eclipser9-20/Ark-shell/actions/workflows/ci.yml/badge.svg)](https://github.com/Eclipser9-20/Ark-shell/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/tag/Eclipser9-20/Ark-shell?label=release&sort=semver)](https://github.com/Eclipser9-20/Ark-shell/releases)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)](https://en.cppreference.com/w/cpp/20)
[![platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)](#install)
[![stars](https://img.shields.io/github/stars/Eclipser9-20/Ark-shell?style=social)](https://github.com/Eclipser9-20/Ark-shell/stargazers)

<br>

<!-- Demo GIF: record with `vhs demo/ark.tape` (see demo/), commit the output to
     demo/ark.gif, then this image goes live. Until then it 404s gracefully. -->
<img src="demo/ark.gif" alt="ark in action — autosuggestions, syntax highlighting, and a nushell-style table" width="720">

<br>

⭐ **If ark makes you smile, star it** — it's how a from-scratch shell earns the notability to reach `brew install ark-shell` from Homebrew core.

</div>

## Install

**macOS / Linux — Homebrew:**

```sh
brew install eclipser9-20/ark-shell/ark-shell
```

**Any Unix — one-line installer** (builds from source, no Homebrew needed):

```sh
curl -fsSL https://raw.githubusercontent.com/Eclipser9-20/Ark-shell/main/install.sh | sh
```

<sub>Prebuilt binaries for each release are on the [Releases](https://github.com/Eclipser9-20/Ark-shell/releases) page. From source: see [Building](#building). Arch/AUR: `yay -S ark-shell` (coming soon).</sub>

## 60-second tour

```sh
ark                              # drop into ark (or set it as your login shell, below)

# it's a real shell — pipelines, redirection, arithmetic, the works
echo $((2**10))                  # 1024   (** ternary comma hex/octal all work)
ls /nope 2>&1 | head -1          # fd-redirection: stderr down the pipe
for f in *.md; do echo "$f"; done | wc -l

# the stuff bash makes you install plugins for — built in:
#  · dim ghost-text autosuggestions from your history + the current dir (→ to accept)
#  · live syntax highlighting; unknown commands flash red before you hit Enter
#  · Tab completes flags straight from a command's man page

ARK_NU_MODE=1 ark -c 'ls'        # nushell-style bordered table
uvar EDITOR nvim                 # a variable that persists across windows AND reboots
assh you@server                  # your shell + editing on a box with nothing installed
```

Everything above is one static binary. No runtime, no plugin manager, no framework.

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
