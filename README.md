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
- **`arky` — an IDE inside the shell.** Tabs, file explorer, mouse, Python + C/C++ (clangd when you have it). See below.

## arky

`arky [file]` is ark's editor as a full IDE: a menu bar, tabs, a file explorer, and the mouse — all in the terminal, all built into the one binary. `arky` is a symlink to `ark`, so it works from any shell, not just from inside ark. `arky-settings` opens its own config.

- **Explorer** — `^E` focuses it: `↑↓` move, `Enter` opens, `←`/`Backspace` go up, letters jump by first character, `[` and `]` resize it, `^E` returns to the editor.
- **Tabs** — `^T` next, `^Y` previous, `Ctrl+PageUp`/`Ctrl+PageDown` either way. Opening a file from the explorer opens a tab.
- **Command palette** — `^P`. Everything reachable by key is reachable here by name, so nothing hides behind a chord you have to already know.
- **Mouse** — click to place the cursor, click a tab to switch, click a file to open, wheel to scroll.

### C and C++

The file extension picks the toolchain. `^R` compiles and runs; `^B` builds — and if the file sits anywhere inside a CMake project, `^B` builds the *project*, not the one file.

If `clangd` is installed, arky speaks LSP to it: real diagnostics from a real compiler front end, and semantic colour that knows your own types from your functions. Without clangd you still get a full C/C++ lexer, so nothing looks broken while it indexes — or if you never install it.

```ini
cc     = cc          # your compilers; cc/c++ are the defaults
cxx    = c++
cflags = -O2 -Wall -Wextra -std=c++20
cmake  = on          # a buffer inside a CMake project builds the project
clangd = on          # keep build/compile_commands.json fresh

menu = on            # arky chrome — each of these can be turned off,
tabs = on            # or turned ON for plain ark-py
explorer = on
explorer_width = 24
mouse = on
dialog = off         # prompts as a centered box with the page dimmed
```

## ark-py

`ark-py [file.py]` is the same editor with none of the chrome — just the buffer. It opens a full editor in the terminal — no language server, no Node, no plugins. The Python intelligence is a from-scratch analyzer built into ark.

- **Live diagnostics** — errors appear inline, to the right of the offending line, as you type.
- **Completion** — scope-aware, with signatures. It indexes what you actually `import`: the stdlib, site-packages, and your own modules sitting next to the file. Nothing is executed, only parsed.
- **Inline suggestions** — dim ghost text ahead of the cursor. **Tab** takes the whole thing, **shift+Tab** one word, **^N** opens the list instead.
- **Run and build** — `^R` runs the buffer (`^G` sets `sys.argv`); `^B` compiles, to bytecode or natively via your own toolchain.
- `^O` open · `^S` save · `^A` select all · `^X`/`^C`/`^V` cut/copy/paste via the system clipboard · `^K` hover · `^]` go to definition · `^Q` quit.

Settings live in `~/.config/ark/arkpy.config`, written fully commented on first run:

```ini
ghost      = on     # inline suggestions
min_prefix = 2      # characters before one appears
args       = --verbose in.txt    # passed to your program as sys.argv[1:]
python     = python3
model_port = 1234   # optional completion server, see below
```

**Optional completion server.** Point `model_port` at anything that speaks one JSON line each way, and ark-py will ask it for a suggestion — but *only* when its own analyzer has nothing, so the editor never waits on it:

```
->  {"prefix":"pri","path":"x.py","line":3,"col":4,"before":"...","after":"..."}
<-  {"completion":"nt()"}
```

A bare line of text works as a reply too, so a useful server can be a dozen lines of Python. Requests run on a worker thread and are matched back to the prefix that asked, so a slow server degrades to *no suggestion* — never a wrong one.

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

When a new ark release adds settings, they're **appended** to your existing config — your own aliases and functions are never rewritten, even when a shipped example shares their name. A backup is written first, and deleting a suggestion you don't want makes it stay gone. Anything you actively set that ark no longer understands is reported at startup, with a nearest-match hint if it looks like a typo.

## Building

Requires a C++20 compiler (clang++ or g++).

```sh
make            # -> ./ark
make test       # integration tests
make install    # -> /usr/local/bin/ark   (PREFIX overridable)
```

## License

MIT — see [LICENSE](LICENSE).
