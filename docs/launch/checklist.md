# Launch-day checklist

Goal: a clean, honest launch that earns real stars — the notability that later
unlocks `brew install ark-shell` from Homebrew **core** (no tap).

## Before you post (do these first — a broken first impression is fatal)

- [ ] `main` builds green in CI on macOS + Linux (badge is green in the README)
- [ ] Tag `v0.2.0` pushed; the Release workflow attached prebuilt binaries
- [ ] `brew install eclipser9-20/ark-shell/ark-shell` works from a clean machine
- [ ] `curl -fsSL .../install.sh | sh` works on a fresh macOS **and** a fresh Linux box
- [ ] `demo/ark.gif` recorded and visible at the top of the README
- [ ] **Set the GitHub repo description + topics** (see `pitch.md` → they drive search/discovery; the description is currently empty)
- [ ] Repo has a clear README, LICENSE, and CONTRIBUTING (add one if missing)
- [ ] Pin the repo on your GitHub profile

## Post (space these out over a day or two, not all at once)

- [ ] **Show HN** — best Tue–Thu, ~9–11am ET. Title + body in `show-hn.md`. Reply to every comment for the first few hours; that's what keeps it on the front page.
- [ ] **r/commandline** (and maybe r/unix) — body in `reddit-commandline.md`. Reddit hates ads; lead with the interesting *technical* thing (from-scratch, `assh`).
- [ ] **Lobsters** (needs an invite) — `lobsters.md`. Tag `unix`, `c` / `cpp`.
- [ ] Post the GIF on X/Mastodon/Bluesky with the one-liner from `pitch.md`.
- [ ] Drop it in relevant Discords/Matrix (r/commandline-adjacent, terminal nerds).

## The rules that make or break it

1. **Never buy/beg for stars or fake engagement.** HN/Reddit sniff it out and it backfires. Let the work speak.
2. **Be present.** The single biggest lever is replying thoughtfully to every comment in the first 2–3 hours.
3. **Be honest about status.** "It's a young project, here's what works and what doesn't" earns way more goodwill (and stars) than overclaiming.
4. **Lead with the hook, not the feature list.** People click for *"someone wrote a whole shell from scratch that runs itself over SSH"* — not for bullet points.

## After the spike

- [ ] Triage issues/PRs fast while attention is high; a responsive maintainer converts drive-by visitors into contributors.
- [ ] When you cross Homebrew-core's notability bar (roughly: ~75+ stars, 30+ forks/watchers, a stable tagged release, active maintenance), open a PR to `homebrew/homebrew-core` with the formula. See `homebrew-core.md`.
