# ark Syntax Highlighting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Live syntax highlighting in ark's interactive line editor — commands, keywords, strings, variables, and operators colored as the user types, using the TokyoNight palette.

**Architecture:** A standalone `highlight.h/.cpp` module scans the raw (unprocessed) input buffer into classified spans, independent of the parser's `Lexer` (which strips quotes/escapes and would corrupt the displayed text). `edit.cpp`'s `redraw()` runs the buffer through this classifier before printing.

**Tech Stack:** C++20, no new dependencies (matches the rest of ark).

## Global Constraints

- No external dependencies — hand-written scanner, matching ark's from-scratch ethos.
- Must never alter the user's typed bytes — only wrap them in ANSI color codes.
- Must never throw/crash on malformed input (e.g. an unterminated quote mid-typing is completely normal, not an error).
- Colors: TokyoNight palette — Command `#7aa2f7`, Keyword `#bb9af7`, String `#9ece6a`, Variable `#7dcfff`, Operator `#565f89`, Plain = no color code.

---

## File Structure

```
src/
  highlight.h   — SpanKind/Span types, classify(), highlightLine()
  highlight.cpp — the scanner + color-wrapping implementation
  edit.cpp      — modified: redraw() calls highlightLine(buf) instead of printing buf directly
tests/
  highlight_test.cpp — standalone test binary (same pattern as lexer_test.cpp etc.)
```

---

### Task 1: Span classifier (`classify()`)

**Files:**
- Create: `src/highlight.h`
- Create: `src/highlight.cpp`
- Test: `tests/highlight_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class SpanKind { Command, Keyword, String, Variable, Operator, Plain };
  struct Span { size_t start, end; SpanKind kind; };
  std::vector<Span> classify(const std::string& raw);
  ```
  (`highlightLine()` is Task 2 — this task only needs `classify()` working and tested.)

- [ ] **Step 1: Write src/highlight.h**

```cpp
#pragma once
#include <string>
#include <vector>

enum class SpanKind { Command, Keyword, String, Variable, Operator, Plain };

struct Span {
    size_t start, end;
    SpanKind kind;
};

// Scans raw (unprocessed, exactly-as-typed) input into a list of spans that
// cover the whole string with no gaps. Never throws -- an unterminated quote
// or a lone trailing '$' at the end of the buffer is completely normal
// mid-typing input, not an error; it's classified as running to the end of
// the buffer.
std::vector<Span> classify(const std::string& raw);

// Wraps each span from classify() in its TokyoNight ANSI color and
// concatenates -- the original bytes are never altered, only wrapped.
std::string highlightLine(const std::string& raw);
```

- [ ] **Step 2: Write the failing test (tests/highlight_test.cpp)**

```cpp
#include "../src/highlight.h"
#include <cassert>
#include <iostream>

static void test_plain_command() {
    auto spans = classify("echo hi");
    assert(spans.size() == 3);
    assert(spans[0].start == 0 && spans[0].end == 4 && spans[0].kind == SpanKind::Command); // "echo"
    assert(spans[1].start == 4 && spans[1].end == 5 && spans[1].kind == SpanKind::Plain);    // " "
    assert(spans[2].start == 5 && spans[2].end == 7 && spans[2].kind == SpanKind::Plain);    // "hi"
}

static void test_pipeline_resets_command_position() {
    auto spans = classify("echo hi | grep h");
    // echo(Command) " "(Plain) hi(Plain) " "(Plain) |(Operator) " "(Plain) grep(Command) " "(Plain) h(Plain)
    assert(spans.size() == 9);
    assert(spans[0].kind == SpanKind::Command);
    assert(spans[4].kind == SpanKind::Operator);
    assert(spans[4].start == 8 && spans[4].end == 9); // the '|' itself
    assert(spans[6].kind == SpanKind::Command); // "grep", command position after |
}

static void test_keyword_does_not_consume_command_position() {
    auto spans = classify("if true");
    // if(Keyword) " "(Plain) true(Command) -- 'true' is still a command name,
    // not a keyword, and command position stays true after 'if'
    assert(spans.size() == 3);
    assert(spans[0].kind == SpanKind::Keyword);
    assert(spans[0].start == 0 && spans[0].end == 2);
    assert(spans[2].kind == SpanKind::Command);
}

static void test_quoted_string_closed() {
    auto spans = classify("echo 'hi'");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::String);
    assert(spans[2].start == 5 && spans[2].end == 9); // includes both quote chars
}

static void test_quoted_string_unterminated_runs_to_end() {
    auto spans = classify("echo 'hi");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::String);
    assert(spans[2].start == 5 && spans[2].end == 8); // runs to end of buffer, no crash
}

static void test_variable() {
    auto spans = classify("echo $HOME");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::Variable);
    assert(spans[2].start == 5 && spans[2].end == 10);
}

static void test_braced_variable() {
    auto spans = classify("echo ${HOME}");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::Variable);
    assert(spans[2].start == 5 && spans[2].end == 12);
}

static void test_and_operator() {
    auto spans = classify("a && b");
    assert(spans.size() == 5);
    assert(spans[2].kind == SpanKind::Operator);
    assert(spans[2].start == 2 && spans[2].end == 4); // "&&" as one span, not two "&"s
    assert(spans[4].kind == SpanKind::Command); // command position reset after &&
}

int main() {
    test_plain_command();
    test_pipeline_resets_command_position();
    test_keyword_does_not_consume_command_position();
    test_quoted_string_closed();
    test_quoted_string_unterminated_runs_to_end();
    test_variable();
    test_braced_variable();
    test_and_operator();
    std::cout << "all highlight classify tests passed\n";
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `clang++ -std=c++20 -Isrc -o /tmp/highlight_test tests/highlight_test.cpp`
Expected: FAIL — compile error, `highlight.cpp`'s `classify` not defined (linker error) since only the header exists so far

- [ ] **Step 4: Write src/highlight.cpp**

```cpp
#include "highlight.h"
#include <cctype>
#include <unordered_set>

static bool isNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static bool isOperatorStart(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

static const std::unordered_set<std::string>& keywordSet() {
    static const std::unordered_set<std::string> kw = {
        "if", "then", "else", "fi", "while", "do", "done",
        "for", "in", "case", "esac", "function",
    };
    return kw;
}

std::vector<Span> classify(const std::string& raw) {
    std::vector<Span> spans;
    size_t i = 0;
    bool atCommandPos = true;

    while (i < raw.size()) {
        char c = raw[i];

        if (c == ' ' || c == '\t') {
            size_t start = i;
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) i++;
            spans.push_back(Span{start, i, SpanKind::Plain});
            continue;
        }

        if (c == '\'') {
            size_t start = i;
            i++;
            while (i < raw.size() && raw[i] != '\'') i++;
            if (i < raw.size()) i++; // consume closing quote if present
            spans.push_back(Span{start, i, SpanKind::String});
            continue;
        }

        if (c == '"') {
            size_t start = i;
            i++;
            while (i < raw.size() && raw[i] != '"') i++;
            if (i < raw.size()) i++;
            spans.push_back(Span{start, i, SpanKind::String});
            continue;
        }

        if (c == '$') {
            size_t start = i;
            size_t j = i + 1;
            if (j < raw.size() && raw[j] == '{') {
                j++;
                while (j < raw.size() && raw[j] != '}') j++;
                if (j < raw.size()) j++; // consume closing brace if present
                spans.push_back(Span{start, j, SpanKind::Variable});
                i = j;
                continue;
            }
            if (j < raw.size() && (isNameChar(raw[j]) || std::isdigit((unsigned char)raw[j]))) {
                while (j < raw.size() && isNameChar(raw[j])) j++;
                spans.push_back(Span{start, j, SpanKind::Variable});
                i = j;
                continue;
            }
            // bare '$' with nothing valid following it -- just Plain
            spans.push_back(Span{start, start + 1, SpanKind::Plain});
            i = start + 1;
            continue;
        }

        if (c == '2' && i + 1 < raw.size() && raw[i + 1] == '>') {
            spans.push_back(Span{i, i + 2, SpanKind::Operator});
            i += 2;
            atCommandPos = true;
            continue;
        }

        if (isOperatorStart(c)) {
            size_t start = i;
            size_t j = i + 1;
            if ((c == '&' && j < raw.size() && raw[j] == '&') ||
                (c == '|' && j < raw.size() && raw[j] == '|') ||
                (c == '>' && j < raw.size() && raw[j] == '>')) {
                j++;
            }
            spans.push_back(Span{start, j, SpanKind::Operator});
            i = j;
            atCommandPos = true;
            continue;
        }

        // bare word: runs until whitespace/quote/$/operator-start/`2>`
        size_t start = i;
        while (i < raw.size()) {
            char wc = raw[i];
            if (wc == ' ' || wc == '\t' || wc == '\'' || wc == '"' || wc == '$' ||
                isOperatorStart(wc)) break;
            if (wc == '2' && i + 1 < raw.size() && raw[i + 1] == '>') break;
            i++;
        }
        std::string word = raw.substr(start, i - start);
        if (atCommandPos && keywordSet().count(word)) {
            spans.push_back(Span{start, i, SpanKind::Keyword});
            // command position stays true -- a keyword like 'if' is
            // followed by another command, not an argument
        } else if (atCommandPos) {
            spans.push_back(Span{start, i, SpanKind::Command});
            atCommandPos = false;
        } else {
            spans.push_back(Span{start, i, SpanKind::Plain});
        }
    }

    return spans;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `clang++ -std=c++20 -Isrc -o /tmp/highlight_test tests/highlight_test.cpp src/highlight.cpp && /tmp/highlight_test`
Expected: `all highlight classify tests passed`

- [ ] **Step 6: Commit**

```bash
git add src/highlight.h src/highlight.cpp tests/highlight_test.cpp
git commit -m "highlight: span classifier for live syntax highlighting (command/keyword/string/variable/operator)"
```

---

### Task 2: `highlightLine()` + wire into the line editor

**Files:**
- Modify: `src/highlight.h` (no signature change — `highlightLine` is already declared)
- Modify: `src/highlight.cpp` (add the implementation)
- Modify: `src/edit.cpp` (`redraw()` calls `highlightLine(buf)`)
- Test: `tests/highlight_test.cpp` (add color-wrapping assertions)

**Interfaces:**
- Consumes: `classify()`, `Span`, `SpanKind` from Task 1.
- Produces: `std::string highlightLine(const std::string& raw)` (already declared in `highlight.h`).

- [ ] **Step 1: Add failing tests to tests/highlight_test.cpp**

```cpp
static void test_highlight_wraps_command_in_blue() {
    std::string out = highlightLine("echo hi");
    assert(out.find("\x1b[38;2;122;162;247mecho\x1b[0m") != std::string::npos);
}

static void test_highlight_preserves_plain_text_unwrapped() {
    std::string out = highlightLine("echo hi");
    // "hi" is Plain -- no color code wraps it, but the literal text is present
    assert(out.find("hi") != std::string::npos);
}

static void test_highlight_wraps_string_in_green() {
    std::string out = highlightLine("echo 'hi'");
    assert(out.find("\x1b[38;2;158;206;106m'hi'\x1b[0m") != std::string::npos);
}

static void test_highlight_wraps_variable_in_cyan() {
    std::string out = highlightLine("echo $HOME");
    assert(out.find("\x1b[38;2;125;207;255m$HOME\x1b[0m") != std::string::npos);
}
```

Add the four calls to `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `clang++ -std=c++20 -Isrc -o /tmp/highlight_test tests/highlight_test.cpp src/highlight.cpp && /tmp/highlight_test`
Expected: FAIL — linker error, `highlightLine` not defined (only declared so far)

- [ ] **Step 3: Add highlightLine() to src/highlight.cpp**

Append to the end of the file:

```cpp
namespace {
const char* colorFor(SpanKind kind) {
    switch (kind) {
        case SpanKind::Command:  return "\x1b[38;2;122;162;247m";
        case SpanKind::Keyword:  return "\x1b[38;2;187;154;247m";
        case SpanKind::String:   return "\x1b[38;2;158;206;106m";
        case SpanKind::Variable: return "\x1b[38;2;125;207;255m";
        case SpanKind::Operator: return "\x1b[38;2;86;95;137m";
        case SpanKind::Plain:    return nullptr;
    }
    return nullptr;
}
} // namespace

std::string highlightLine(const std::string& raw) {
    auto spans = classify(raw);
    std::string out;
    out.reserve(raw.size() + 32);
    for (const auto& sp : spans) {
        std::string text = raw.substr(sp.start, sp.end - sp.start);
        const char* color = colorFor(sp.kind);
        if (color) {
            out += color;
            out += text;
            out += "\x1b[0m";
        } else {
            out += text;
        }
    }
    return out;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `clang++ -std=c++20 -Isrc -o /tmp/highlight_test tests/highlight_test.cpp src/highlight.cpp && /tmp/highlight_test`
Expected: `all highlight classify tests passed`

- [ ] **Step 5: Wire highlightLine() into src/edit.cpp's redraw()**

In `src/edit.cpp`, add `#include "highlight.h"` to the includes, then find:

```cpp
    auto redraw = [&]() {
        std::cout << "\r\x1b[K" << prompt << buf;
        size_t back = buf.size() - cursor;
        if (back > 0) std::cout << "\x1b[" << back << "D";
        std::cout << std::flush;
    };
```

Replace the `<< buf` with `<< highlightLine(buf)`:

```cpp
    auto redraw = [&]() {
        std::cout << "\r\x1b[K" << prompt << highlightLine(buf);
        size_t back = buf.size() - cursor;
        if (back > 0) std::cout << "\x1b[" << back << "D";
        std::cout << std::flush;
    };
```

Note: the cursor-back-up math (`back = buf.size() - cursor`) is unaffected —
it's based on the plain-text `buf` length, not the colorized output length,
so ANSI codes inserted by `highlightLine()` don't throw off cursor
positioning.

- [ ] **Step 6: Rebuild ark and manually verify in a real pty**

Run: `make clean && make`
Expected: builds clean.

Run (real pty smoke test, matching the approach used for Task 19's line
editor verification):
```bash
cd /tmp && cat > highlight_smoke.exp <<'EOF'
log_file -a highlight_smoke.bin
set timeout 8
spawn /Users/gideoncox/ark-terminal/ark
expect "*"
send "echo 'hi' \$HOME\r"
sleep 0.3
send "\004"
expect eof
EOF
expect highlight_smoke.exp >/dev/null 2>&1
python3 -c "
data = open('highlight_smoke.bin','rb').read().decode('utf-8', errors='replace')
assert '\x1b[38;2;122;162;247mecho' in data, 'command not colored blue'
assert \"\x1b[38;2;158;206;106m'hi'\" in data, 'string not colored green'
assert '\x1b[38;2;125;207;255m\$HOME' in data, 'variable not colored cyan'
print('all highlight colors verified in real pty output')
"
```
Expected: `all highlight colors verified in real pty output`

- [ ] **Step 7: Run the full test suite to confirm no regressions**

Run: `cd /Users/gideoncox/ark-terminal && make test`
Expected: all 14 integration test cases still PASS (highlighting only touches the interactive line editor's display, not parsing/execution).

- [ ] **Step 8: Commit**

```bash
git add src/highlight.h src/highlight.cpp src/edit.cpp tests/highlight_test.cpp
git commit -m "highlight: wire live syntax highlighting into the interactive line editor"
```

---

## Self-Review Notes

**Spec coverage:** classify() covers all 6 span kinds from the design (Command,
Keyword, String, Variable, Operator, Plain); highlightLine() wraps each in
its TokyoNight color per the design's color table; edit.cpp wiring confirmed
not to break cursor math (uses plain `buf`, not the colorized string).

**Placeholder scan:** none found — every step has complete, real code.

**Type consistency:** `Span`, `SpanKind`, `classify()`, `highlightLine()`
signatures are identical between Task 1's declaration and Task 2's usage.
