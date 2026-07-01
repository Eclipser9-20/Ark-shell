# ark — Phase 2c: Tab Completion

## Vision

Tab-completion for the interactive line editor: filesystem paths everywhere,
and command names (builtins + `$PATH` executables) when the word under the
cursor is in command position. Completes this session's Phase 2 UX work
alongside syntax highlighting (already shipped).

## Priorities

Same as the rest of ark: no subprocess calls in the interactive hot path.
`$PATH` scanning uses `opendir`/`readdir`/`access(X_OK)` directly rather than
shelling out to `compgen`/`which`, matching the constraint already applied
to chrome's hardware stats and git-branch detection.

## Architecture

```
Tab pressed in edit.cpp's raw-mode loop
      |
   extractWordUnderCursor(buf, cursor) -> (wordStart, word)
      |         (scans backward from cursor to the nearest whitespace or
      |          start-of-buffer -- a live-editing heuristic on raw text,
      |          same style as highlight.cpp's scanner, not a full parse)
      |
   isCommandPosition(buf, wordStart) -> bool
      |         (reuses the exact same command-position state machine
      |          concept as highlight.cpp's classify() -- scan from the
      |          start of buf up to wordStart, tracking the same
      |          "after |/;/&&/||/& -> true" rule)
      |
   bool ? completeCommand(word) : completePath(word)
      |         -> vector<string> candidates (each a full replacement for
      |            `word`, not just a suffix)
      |
   applyCompletion(candidates, word) -> one of:
      - 0 candidates: no-op (beep or silently do nothing)
      - 1 candidate: replace word with it + trailing space/slash
      - 2+ candidates, common prefix longer than `word`: replace word with
        the common prefix (still may need more Tabs/typing)
      - 2+ candidates, no further common prefix: print full candidate list
        on new line(s) below the input, then redraw prompt+buffer beneath it
```

## Components

- **src/complete.h/.cpp** (new)
  - `std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor);`
    — returns the start offset and text of the word ending at `cursor`.
  - `bool isCommandPosition(const std::string& buf, size_t wordStart);`
  - `std::vector<std::string> completePath(const std::string& partial);`
    — splits into directory + filename prefix, `opendir`/`readdir`, prefix
    filter, hidden-file rule.
  - `std::vector<std::string> completeCommand(const std::string& partial);`
    — builtin names (from `builtinRegistry()`) + executables found by
    scanning each `$PATH` directory via `opendir`/`readdir` +
    `access(path, X_OK)`.
  - `std::string longestCommonPrefix(const std::vector<std::string>& items);`
- **src/edit.cpp** (modified) — Tab key handling: call the above, then either
  extend `buf`/`cursor` in place (redraw as normal) or print the candidate
  list on new lines (temporarily leaving the input line, printing below,
  then redrawing prompt+buffer under the list — same technique history
  recall already uses for redraw, just with extra lines printed first).

## Data Flow

```
Tab key
  -> (wordStart, word) = wordUnderCursor(buf, cursor)
  -> candidates = isCommandPosition(buf, wordStart)
                    ? completeCommand(word)
                    : completePath(word)
  -> if candidates.empty(): do nothing
  -> prefix = longestCommonPrefix(candidates)
  -> if prefix.size() > word.size():
       buf.replace(wordStart, word.size(), prefix)
       cursor = wordStart + prefix.size()
       if candidates.size() == 1:
         buf.insert(cursor, isDirectory(prefix) ? "/" : " ")
         cursor++
       redraw()
     else if candidates.size() > 1:
       print each candidate, space-separated, wrapped to terminal width,
       on new line(s) below the current input
       redraw()  // reprints prompt + current (unchanged) buf beneath the list
```

## Error Handling

- A partial word that doesn't resolve to any existing directory (for path
  completion) simply yields zero candidates — no error, Tab just does
  nothing that keystroke.
- `$PATH` entries that don't exist or aren't readable directories are
  skipped silently while scanning (matches how a missing `$PATH` entry is
  normally just ignored by any shell).
- No candidate list ever includes `.`/`..` unless the user has explicitly
  typed enough of a hidden-file prefix to match them (`.` itself would
  technically match, but is excluded as a special case — completing to `.`
  or `..` isn't useful).

## Testing

`tests/complete_test.cpp` (standalone, matching the other module tests):
`wordUnderCursor` against representative buffer/cursor positions,
`isCommandPosition` against command-position and argument-position cases
(reusing the same test inputs as `highlight_test.cpp`'s command-position
tests, since the logic is conceptually the same rule applied to a
differently-shaped scan), `completePath`/`completeCommand` against a real
temporary directory tree created during the test (matching how
`chrome_test.cpp` creates a real temp git repo), and
`longestCommonPrefix` against representative candidate lists including the
empty and single-item cases. The candidate-list-printing behavior (multiple
new lines below the input) is verified via a real PTY smoke test, matching
the approach used for the line editor and chrome.

## Explicitly Out of Scope

- Autosuggestions (fish-style ghost text from history) — separate,
  already-deferred feature.
- Completing command arguments contextually (e.g. `git <TAB>` suggesting
  git subcommands, or `cd <TAB>` suggesting only directories not files) —
  phase 1's scope is generic path/command completion only; per-command
  argument-aware completion is a much larger, separate feature.
- Cycling through matches on repeated Tab (zsh/fish menu-style) — the
  user chose the simpler bash-style list-on-ambiguous behavior instead.
