# Getting into Homebrew core (`brew install ark-shell`, no tap)

The goal: people run `brew install ark-shell` and get it from Homebrew's own
curated repo — no `brew tap`, no "trust the dev." Here's the real path.

## The catch: notability comes first

homebrew-core does **not** accept formulae to *help* a project get popular. A
formula has to clear their "acceptable formulae" bar, which is roughly:

- **Notable / maintained.** Their rule of thumb has historically been on the
  order of **30+ forks, 30+ watchers, or 75+ stars** — plus signs of active
  maintenance (recent commits, issues being handled). Numbers aren't a hard
  contract, but a brand-new repo with a handful of stars *will* be closed as
  "not notable enough yet."
- **Stable release.** A real tagged version (we have `v0.2.0`), not HEAD-only.
- **Builds from source** in their CI, on the platforms they support, with tests
  (`brew test`). Our formula already builds from source and has a `test do` block.
- **Open-source license** (MIT ✓) and no vendored/duplicated dependencies.

So the sequence is: **launch → earn stars/forks → then submit.** Until then, the
tap (`brew install eclipser9-20/ark-shell/ark-shell`) and the `curl | sh`
installer are the install story, and they work today.

## While you wait: keep the tap formula bottle-able and clean

- Every release: bump `url` to the new tag tarball and `sha256` to its checksum.
  (`shasum -a 256 <tarball>` — the Release workflow also emits a `.sha256`.)
- Keep the `test do` block meaningful — homebrew-core reviewers read it.

## When you're ready to submit to core

1. Read Homebrew's [Acceptable Formulae](https://docs.brew.sh/Acceptable-Formulae)
   and [Formula Cookbook](https://docs.brew.sh/Formula-Cookbook).
2. `brew create https://github.com/Eclipser9-20/Ark-shell/archive/refs/tags/vX.Y.Z.tar.gz`
   — or adapt the existing tap formula.
3. `brew audit --new --strict --online ark-shell` until it's clean.
4. `brew install --build-from-source ark-shell && brew test ark-shell`.
5. Open a PR to `Homebrew/homebrew-core`. CI bottles it for every platform; a
   maintainer reviews. Respond quickly to review comments.

Once it lands, `brew install ark-shell` works for everyone with zero tap — and
that's the trust signal you're really after.
