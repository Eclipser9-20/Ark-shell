# Demo assets

The README hero image is `demo/ark.gif`. Regenerate it whenever the UI changes.

## Record the GIF (recommended: VHS)

[VHS](https://github.com/charmbracelet/vhs) turns a script into a clean, reproducible GIF — no screen recorder, no cursor jitter.

```sh
brew install vhs        # one-time
vhs demo/ark.tape       # writes demo/ark.gif
git add demo/ark.gif && git commit -m "demo: refresh GIF"
```

Edit `ark.tape` to change what's shown, the theme, or the size. Keep the GIF under ~5 MB so it loads fast in the README and unfurls nicely on social cards.

## Alternative: asciinema

If you'd rather have a play/pause, copy-pasteable cast:

```sh
brew install asciinema agg
asciinema rec demo/ark.cast -c "ARK_CHROME_TOP=off ark"
# ...run the tour from the README, then exit...
agg demo/ark.cast demo/ark.gif      # convert cast -> gif
```

## What to show (keep it ~30–40s)

1. Arithmetic that "just works" (`2**10`, hex, ternary)
2. `2>&1 | head` — real fd-redirection
3. A loop piped into a command
4. Ghost-text autosuggestion accepted with `→`
5. `ARK_NU_MODE=1` bordered table
6. (bonus) `assh you@host` — your shell over SSH with nothing installed remotely
