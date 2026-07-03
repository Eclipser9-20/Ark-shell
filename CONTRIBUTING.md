# Contributing to ark

Thanks for wanting to help — ark is a from-scratch shell and there's a lot of
surface area, so contributions of all sizes are welcome.

## Getting set up

```sh
git clone https://github.com/Eclipser9-20/Ark-shell.git
cd Ark-shell
make            # -> ./ark   (needs a C++20 compiler: clang++ or g++)
make unittest   # per-module unit tests (tests/*_test.cpp)
make test       # integration tests (tests/cases/)
```

Run the shell without the status-bar chrome while developing:

```sh
ARK_CHROME_TOP=off ./ark -c 'echo $((2**10))'
```

## Before you open a PR

- **Add a test.** Bug fix → a failing test that your change makes pass. New
  behavior → a unit test in the relevant `tests/*_test.cpp` and/or an
  integration case in `tests/cases/`.
- `make unittest && make test` must be green. CI runs the same on macOS + Linux.
- Match the surrounding style (the codebase favors dense, well-commented C++
  with a *why* on the non-obvious bits).
- Keep changes focused — one logical change per PR is easier to review.

## Good first issues

- Scripting-compat gaps vs bash/zsh (report a script that misbehaves — repros are
  gold).
- Missing expansions/builtins.
- Completion and highlighting edge cases.

## Reporting bugs

Open an issue with the exact input and what you expected vs. got, e.g.:

```
ARK_CHROME_TOP=off ark -c '<your command>'
# got:      ...
# expected: ...   (what bash/zsh do)
```

That format makes almost any shell bug reproducible in seconds. Thanks!
