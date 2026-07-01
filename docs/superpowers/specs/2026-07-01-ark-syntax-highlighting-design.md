# ark — Phase 2a: Live Syntax Highlighting

## Vision

The first slice of Phase 2 (fish-style interactive UX). As the user types at
the interactive prompt, the line is colorized live — commands, keywords,
strings, variables, and operators each get a distinct color, matching the
TokyoNight palette already used for the prompt itself.

Autosuggestions and smart completion (the rest of Phase 2) are separate,
later specs — this covers highlighting only.

## Priorities

Same as phase 1: customizability first (the classifier is a small,
extensible function — new token classes are easy to add), performance
second (re-highlighting on every keystroke must stay imperceptible; a
line-length scan is O(n) and negligible at typical command-line lengths).

## Why not reuse the parser's Lexer

`Lexer` (src/lexer.h/.cpp) strips quote characters, processes backslash
escapes, and wraps quoted spans in `\x01`/`\x02` sentinel markers — all
necessary for the parser/expander, but wrong for display: highlighting must
preserve the user's exact original bytes and only wrap them in color, never
alter them. A separate, simpler scanner avoids entangling two different
concerns (semantic tokenization vs. raw-text classification) in one module.

## Architecture

```
raw buffer (what the user has typed so far, unprocessed)
      |
   Highlighter::classify()  -> vector<Span{start, end, Kind}>
      |                         (Kind: Command, Keyword, String, Variable,
      |                          Operator, Plain; spans cover the WHOLE
      |                          buffer, no gaps)
      |
   highlightLine()          -> original bytes with ANSI color codes
                                inserted around each span's boundaries
      |
   edit.cpp's redraw()       -> prints the colorized string instead of
                                the plain buf
```

## Components

- **src/highlight.h/.cpp** (new)
  - `enum class SpanKind { Command, Keyword, String, Variable, Operator, Plain };`
  - `struct Span { size_t start, end; SpanKind kind; };`
  - `std::vector<Span> classify(const std::string& raw);` — the scanner.
  - `std::string highlightLine(const std::string& raw);` — calls `classify()`,
    wraps each span in the matching TokyoNight ANSI color, concatenates.
- **src/edit.cpp** (modified) — `redraw()` calls `highlightLine(buf)` instead
  of printing `buf` directly.

## Classification rules (in scan order)

1. Whitespace: copied through as Plain, also resets "at command position"
   tracking is unaffected by whitespace itself.
2. `'...'` (single-quoted span, unterminated-at-buffer-end is fine — color
   to end of buffer) → String.
3. `"..."` (double-quoted span, same unterminated handling) → String.
4. `$NAME` or `${...}` → Variable. `$` alone (no following name/brace) →
   Plain (not a real expansion).
5. Operator run (`|`, `&&`, `||`, `;`, `&`, `<`, `>`, `>>`, `2>`, `(`, `)`) →
   Operator. An operator resets "at command position" to true for the NEXT
   word (a new command starts after `|`/`;`/`&&`/`||`/`&`).
6. A bare word: if "at command position" is true AND the word matches the
   keyword list (`if then else fi while do done for in case esac function`)
   → Keyword (keywords also do NOT count as consuming "command position" —
   `if` is followed by another command position, e.g. `if true`). If "at
   command position" is true and it's not a keyword → Command, and command
   position becomes false until the next operator/keyword. Otherwise →
   Plain (an argument).

## Colors (TokyoNight, matching main.cpp's `tn::` palette)

| Kind | Color | Hex |
|---|---|---|
| Command | blue | #7aa2f7 |
| Keyword | purple | #bb9af7 |
| String | green | #9ece6a |
| Variable | cyan | #7dcfff |
| Operator | comment-gray | #565f89 |
| Plain | default foreground (no color code) | — |

## Error Handling

The scanner never throws and never fails: an unterminated quote at the end
of the buffer (very common mid-typing, e.g. the user just typed the opening
`"` and hasn't closed it yet) is simply colored as a String span running to
the end of the buffer — not an error, just how it looks until they close it.
There is no "invalid input" case for a raw byte scanner; every byte belongs
to some span.

## Testing

`tests/highlight_test.cpp` (standalone, like the other module tests):
asserts `classify()` produces the expected span list/kinds for representative
inputs (plain command, command with args, pipeline, keyword, quoted string
open and closed, unterminated quote, variable, operator run). A visual
smoke-test in a real pty (same `expect`-based approach used for the line
editor in Task 19) confirms the ANSI codes actually render as intended
colors, since the unit tests only check span boundaries/kinds, not the
final color-coded string.

## Explicitly Out of Scope

- Autosuggestions (ghost text from history) — separate spec.
- Smart completion (Tab) — separate spec.
- Nested/nuanced highlighting (e.g. distinguishing an `if`'s condition
  command from its body) — command-position tracking is a simple state
  machine, not a real parse; it's a display heuristic, not required to be
  perfectly correct for every edge case (e.g. deeply nested subshells,
  which phase 1 doesn't even support yet).
