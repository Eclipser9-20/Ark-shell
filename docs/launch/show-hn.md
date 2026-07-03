# Show HN post

## Title (pick one — keep it plain, HN dislikes hype)

- `Show HN: Ark – a from-scratch interactive shell in C++ (not a zsh wrapper)`
- `Show HN: Ark – a shell that runs itself over SSH with nothing installed on the remote`
- `Show HN: Ark – bash/zsh/fish/nushell ideas in one from-scratch C++20 binary`

## URL

https://github.com/Eclipser9-20/Ark-shell

## Body (first comment — post this right after submitting)

I've been building ark, an interactive shell written from scratch in C++20. It's
not a config layer or a plugin bundle on top of zsh — it has its own lexer,
parser, expander, globber, job control, and line editor. That let me build a few
things a plugin can't:

- **`assh user@host`** copies the matching prebuilt ark to the remote over SSH,
  runs it as your interactive shell, and deletes it on exit — so your
  autosuggestions, highlighting, and prompt work on a box that's never heard of
  ark, and nothing is left behind. This is the feature I actually use daily.
- **Autosuggestions + live syntax highlighting + a pinned status bar are built
  in**, not plugins. Tab-completion pulls flags straight from a command's man page.
- **One static-ish binary.** `brew install` or `curl | sh`. No runtime, no
  framework, no plugin manager.

It's a genuinely young project (v0.2.0) — I just did a big correctness sweep
(arithmetic `**`/ternary/hex, `2>&1` and fd-duplication, compound commands in
pipelines like `for..done | wc`, in-process builtin redirects, etc.), all under
unit + integration tests that run in CI on macOS and Linux. POSIX-ish scripting
works; it is not yet a drop-in replacement for every zsh script you own, and I'd
rather say that up front.

Why write a whole shell instead of more zsh plugins? Partly because I wanted to
understand shells all the way down, and partly because "the shell is the one
program you can't easily swap the internals of" bugged me. `assh` fell out of
having a real binary instead of a config.

Design notes, the full feature list, and the ark-vs-bash/zsh/fish comparison are
in the README. Happy to go deep on any of the internals — the expander and the
job-control/pipeline wiring were the most interesting parts to get right.

Feedback very welcome, especially on scripting-compat gaps and anything that
feels un-shell-like.
