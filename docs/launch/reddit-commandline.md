# r/commandline (and r/unix) post

Reddit is allergic to marketing. Lead with the interesting technical thing, show
the GIF, be a person. Don't cross-post the identical text everywhere on the same
day — reword per sub.

## Title

`ark: a from-scratch interactive shell in C++ that can run itself over SSH with nothing installed on the remote`

## Body

I got tired of "modern shells" all being config layers on top of zsh, so I wrote
one from scratch in C++20 — its own lexer, parser, expander, globber, job
control, and line editor.

The feature I didn't expect to love: `assh user@host` ships the matching ark
binary to the remote over SSH, runs it as your interactive shell, and removes it
on exit. Your autosuggestions/highlighting/prompt work on any box, and nothing's
left behind.

Other built-ins (not plugins): fish-style autosuggestions, live syntax
highlighting, man-page-driven flag completion, a pinned status bar, `uvar`
variables that persist across windows and reboots, and an `ARK_NU_MODE` that
renders `ls` as a nushell-style table.

It's early (v0.2.0) and POSIX-ish scripting works, but I want to be clear it's
not yet a 100% drop-in for every zsh script. Everything's tested in CI on macOS
+ Linux.

[GIF]

Repo (MIT): https://github.com/Eclipser9-20/Ark-shell
Install: `brew install eclipser9-20/ark-shell/ark-shell` or `curl -fsSL .../install.sh | sh`

Would love feedback from people who live in the terminal — especially where it
feels un-shell-like or where scripting breaks.

---

## r/unix angle (reword the title)

`I wrote a POSIX-ish shell from scratch in C++ (own parser/expander/job control) — feedback welcome`

Keep the body similar but lean more on the internals (parser/expander/job
control) and less on the eye-candy, since r/unix skews lower-level.
