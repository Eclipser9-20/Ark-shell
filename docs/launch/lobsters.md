# Lobste.rs post

Lobsters is invite-only and low-tolerance for self-promo, but a genuinely
technical from-scratch project fits. You must have an account (invite required).

## Submit as

- URL: https://github.com/Eclipser9-20/Ark-shell
- Tags: `unix`, `c` (or `cpp` if available), `release` (mark it as authored if it's your own work — Lobsters requires the "I am the author" checkbox)

## Suggested comment (Lobsters wants substance, not a pitch)

A from-scratch interactive shell in C++20 — own lexer/parser/expander/globber,
plus real job control and a from-scratch line editor. The part I found most fun
to build was `assh`: it copies the matching prebuilt binary to a remote over SSH,
execs it as the login shell, and cleans up on exit, so you get your shell on a
box with nothing installed. Happy to talk about the pipeline/job-control wiring
or the expander design. It's early (v0.2.0); scripting is POSIX-ish but not yet a
full zsh drop-in.
