# Pitch, taglines & GitHub metadata

## GitHub repo description (SET THIS — it's currently empty)

Paste into the repo's "About" box (gear icon on the repo home page). Keep it
under ~120 chars, front-load the hook:

> A from-scratch interactive shell in C++20 — autosuggestions, syntax highlighting, and it runs itself over SSH with nothing installed remotely.

Shorter alt:

> From-scratch C++20 shell: fish-style autosuggest + highlighting, POSIX-ish scripting, and `assh` — your shell on any box, zero remote install.

## GitHub topics (Add topics → drives search/discovery)

```
shell  cli  terminal  command-line  cpp  cplusplus  bash  zsh  fish  nushell
posix  interactive-shell  ssh  developer-tools  macos  linux
```

Also: set the website field to the repo's Releases page (or a landing page if
you make one), and pin the repo on your profile.

## One-liners (X / Mastodon / Bluesky / link previews)

- "ark: a shell written from scratch in C++ — autosuggestions, highlighting, and it runs *itself* over SSH with nothing installed on the far end."
- "Most 'modern shells' are zsh with a hat on. ark isn't — own lexer, parser, expander, job control. One binary. `brew install` and go."
- "I wrote a whole shell so I could `assh you@server` and have my shell, editing, and prompt on a box that's never heard of it. Then it deletes itself."

## Elevator paragraph (for blog posts / newsletters / Discord)

ark is a from-scratch interactive shell in C++20 — not a config layer over zsh.
Because it's a real binary (its own lexer/parser/expander/job-control/line-editor)
it can do things a plugin can't: `assh user@host` runs *your* shell on any remote
with nothing installed there, and autosuggestions, live syntax highlighting, a
pinned status bar, and man-page flag completion are all built in. One binary,
MIT-licensed, tested in CI on macOS and Linux.

## The honest framing (use everywhere — it converts better than hype)

Young project. POSIX-ish scripting works and it's tested, but it's not yet a
drop-in for every zsh script. Said plainly, that earns trust; overclaiming loses it.
