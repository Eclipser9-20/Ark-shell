# ark demo — record with:  vhs demo/ark.tape   (produces demo/ark.gif)
# Install VHS once:  brew install vhs   (https://github.com/charmbracelet/vhs)
#
# Tweak FontSize/Width/Height to taste; keep the GIF under ~5 MB so it embeds
# fast in the README and on social cards.

Output demo/ark.gif

Set Shell "bash"
Set FontSize 18
Set Width 1200
Set Height 720
Set Padding 22
Set Theme "Catppuccin Mocha"
Set TypingSpeed 55ms
Set PlaybackSpeed 1.0

# Launch ark (plain chrome so the GIF isn't dominated by the status bars;
# drop ARK_CHROME_TOP=off to show the pinned bars instead).
Type "ARK_CHROME_TOP=off ark"
Enter
Sleep 1200ms

# 1. Real shell: arithmetic that bash needs $(( )) gymnastics for
Type "echo $((2**10)) $((0xff)) $((5 > 3 ? 42 : 0))"
Sleep 600ms
Enter
Sleep 900ms

# 2. fd-redirection down a pipeline
Type "ls /nope 2>&1 | head -1"
Sleep 500ms
Enter
Sleep 900ms

# 3. Loop piped into a command (compound-in-pipeline)
Type "for f in one two three; do echo $f; done | wc -l"
Sleep 500ms
Enter
Sleep 1000ms

# 4. Autosuggestion: start typing, the dim ghost text completes from history
Type "echo $((2"
Sleep 1400ms
Ctrl+U

# 5. nushell-style table
Type "ARK_NU_MODE=1 ark -c 'ls' | head -8"
Sleep 500ms
Enter
Sleep 1600ms

Type "exit"
Enter
Sleep 800ms
