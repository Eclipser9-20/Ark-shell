# ark Tab Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tab-completion in ark's interactive line editor — filesystem paths everywhere, command names (builtins + `$PATH`) when the word under the cursor is in command position.

**Architecture:** `src/complete.h/.cpp` provides pure word/position logic (no I/O) plus filesystem/`$PATH` scanning (via `opendir`/`readdir`/`access`, never a subprocess); `edit.cpp`'s Tab-key handler wires it together with bash-style common-prefix completion and list-on-ambiguous display.

**Tech Stack:** C++20, POSIX `dirent.h`/`unistd.h` — no new external dependencies.

## Global Constraints

- No subprocess calls anywhere in completion — `opendir`/`readdir`/`access(X_OK)` only, matching the constraint already applied to chrome's stats/git-branch detection.
- Hidden files (dotfiles) excluded from path completion unless the prefix itself starts with `.` (matches bash).
- Ambiguous matches (2+ candidates, no further common prefix): print the full list on new line(s) below the input, then redraw — no cycling-through-matches behavior (explicitly declined in favor of simplicity).

---

## File Structure

```
src/
  complete.h    — wordUnderCursor, isCommandPosition, longestCommonPrefix,
                  completePath, completeCommand, isDirectory
  complete.cpp  — implementation
  edit.cpp      — modified: Tab key (ASCII 9) handling in the raw-mode loop
tests/
  complete_test.cpp — standalone test binary (real temp directory tree for
                       path/command completion, matching chrome_test.cpp's
                       real-temp-git-repo approach)
```

---

### Task 1: Word/position logic (no filesystem I/O)

**Files:**
- Create: `src/complete.h`
- Create: `src/complete.cpp`
- Test: `tests/complete_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor);
  bool isCommandPosition(const std::string& buf, size_t wordStart);
  std::string longestCommonPrefix(const std::vector<std::string>& items);
  ```

- [ ] **Step 1: Write src/complete.h**

```cpp
#pragma once
#include <string>
#include <utility>
#include <vector>

// Returns {wordStart, word} for the word ending exactly at `cursor` --
// scans backward from `cursor` to the nearest whitespace or start of
// `buf`. A live-editing heuristic on raw text (like highlight.cpp's
// scanner), not a full parse.
std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor);

// True if the word starting at `wordStart` is in "command position" --
// the start of `buf`, or right after |, ;, &&, ||, &, or a shell keyword
// (if/then/else/fi/while/do/done/for/in/case/esac/function). Reimplements
// (deliberately, not shared via highlight.h) the same command-position
// rule highlight.cpp's classify() uses internally -- kept as a small,
// self-contained scan here rather than exposing classify()'s internal
// state machine as a new public API just for this one caller.
bool isCommandPosition(const std::string& buf, size_t wordStart);

// Longest common prefix shared by all strings in `items`. Empty for an
// empty list; the item itself for a single-item list.
std::string longestCommonPrefix(const std::vector<std::string>& items);

// Filesystem path completion: splits `partial` into directory + filename
// prefix, lists matching entries via opendir/readdir (no subprocess).
// Hidden files excluded unless the prefix itself starts with '.'.
// Candidates are full replacement text for `partial` (including any
// directory portion `partial` itself had), not just suffixes.
std::vector<std::string> completePath(const std::string& partial);

// Command-name completion: builtin names (from builtinRegistry()) plus
// executables found by scanning each $PATH directory (opendir/readdir +
// access(path, X_OK) -- no subprocess).
std::vector<std::string> completeCommand(const std::string& partial);

// True if `path` (after expanding a leading '~' via $HOME) is a directory.
// Used to decide whether a completed path should get a trailing '/' or ' '.
bool isDirectory(const std::string& path);
```

- [ ] **Step 2: Write the failing test (tests/complete_test.cpp)**

```cpp
#include "../src/complete.h"
#include <cassert>
#include <iostream>

static void test_word_under_cursor_simple() {
    auto [start, word] = wordUnderCursor("echo hi", 7);
    assert(start == 5 && word == "hi");
}

static void test_word_under_cursor_mid_word() {
    auto [start, word] = wordUnderCursor("echo hi", 6);
    assert(start == 5 && word == "h");
}

static void test_word_under_cursor_empty_at_start() {
    auto [start, word] = wordUnderCursor("", 0);
    assert(start == 0 && word.empty());
}

static void test_word_under_cursor_after_space() {
    auto [start, word] = wordUnderCursor("echo ", 5);
    assert(start == 5 && word.empty());
}

static void test_command_position_at_start() {
    assert(isCommandPosition("", 0) == true);
}

static void test_command_position_after_command_word() {
    // "echo hi" -- completing "hi" (wordStart=5) is NOT command position
    assert(isCommandPosition("echo hi", 5) == false);
}

static void test_command_position_after_pipe() {
    // "echo hi | gr" -- completing "gr" (wordStart=10) IS command position
    assert(isCommandPosition("echo hi | gr", 10) == true);
}

static void test_command_position_after_keyword() {
    // "if tr" -- completing "tr" (wordStart=3) IS command position (a
    // keyword doesn't consume command position, matching highlight.cpp)
    assert(isCommandPosition("if tr", 3) == true);
}

static void test_longest_common_prefix() {
    assert(longestCommonPrefix({"foo", "foobar", "foobaz"}) == "foo");
    assert(longestCommonPrefix({"abc"}) == "abc");
    assert(longestCommonPrefix({}) == "");
    assert(longestCommonPrefix({"abc", "xyz"}) == "");
}

int main() {
    test_word_under_cursor_simple();
    test_word_under_cursor_mid_word();
    test_word_under_cursor_empty_at_start();
    test_word_under_cursor_after_space();
    test_command_position_at_start();
    test_command_position_after_command_word();
    test_command_position_after_pipe();
    test_command_position_after_keyword();
    test_longest_common_prefix();
    std::cout << "all complete word/position tests passed\n";
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `clang++ -std=c++20 -Isrc -o /tmp/complete_test tests/complete_test.cpp`
Expected: FAIL — `complete.cpp` doesn't exist yet (linker error)

- [ ] **Step 4: Write src/complete.cpp (this task's functions only)**

```cpp
#include "complete.h"
#include <unordered_set>

std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor) {
    size_t start = cursor;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t') start--;
    return {start, buf.substr(start, cursor - start)};
}

static const std::unordered_set<std::string>& completionKeywords() {
    static const std::unordered_set<std::string> kw = {
        "if", "then", "else", "fi", "while", "do", "done",
        "for", "in", "case", "esac", "function",
    };
    return kw;
}

bool isCommandPosition(const std::string& buf, size_t wordStart) {
    bool atCommandPos = true;
    size_t i = 0;
    while (i < wordStart) {
        char c = buf[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '|' || c == '&' || c == ';') {
            atCommandPos = true;
            i++;
            if (i < wordStart && buf[i] == c) i++; // && / ||
            continue;
        }
        size_t wstart = i;
        while (i < wordStart && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '|' && buf[i] != '&' && buf[i] != ';') i++;
        std::string word = buf.substr(wstart, i - wstart);
        if (atCommandPos && completionKeywords().count(word)) {
            // keyword: command position stays true
        } else {
            atCommandPos = false;
        }
    }
    return atCommandPos;
}

std::string longestCommonPrefix(const std::vector<std::string>& items) {
    if (items.empty()) return "";
    std::string prefix = items[0];
    for (size_t i = 1; i < items.size(); i++) {
        size_t j = 0;
        while (j < prefix.size() && j < items[i].size() && prefix[j] == items[i][j]) j++;
        prefix = prefix.substr(0, j);
        if (prefix.empty()) break;
    }
    return prefix;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `clang++ -std=c++20 -Isrc -o /tmp/complete_test tests/complete_test.cpp src/complete.cpp && /tmp/complete_test`
Expected: `all complete word/position tests passed`

- [ ] **Step 6: Commit**

```bash
git add src/complete.h src/complete.cpp tests/complete_test.cpp
git commit -m "complete: word-under-cursor, command-position detection, longest-common-prefix (pure logic, no I/O)"
```

---

### Task 2: Filesystem path + command-name completion

**Files:**
- Modify: `src/complete.cpp` (add `completePath`, `completeCommand`, `isDirectory`)
- Modify: `tests/complete_test.cpp`

**Interfaces:**
- Consumes: `builtinRegistry()` from `src/builtins.h` (already exists).
- Produces: `completePath()`, `completeCommand()`, `isDirectory()` (already declared in Task 1's `complete.h`).

- [ ] **Step 1: Add failing tests to tests/complete_test.cpp**

```cpp
#include <algorithm>
#include <cstdlib>
#include <fstream>

static void test_complete_path_in_temp_dir() {
    system("rm -rf /tmp/ark_complete_test_dir");
    system("mkdir -p /tmp/ark_complete_test_dir/sub");
    std::ofstream(("/tmp/ark_complete_test_dir/foo.txt")).close();
    std::ofstream(("/tmp/ark_complete_test_dir/foobar.txt")).close();
    std::ofstream(("/tmp/ark_complete_test_dir/.hidden")).close();

    auto results = completePath("/tmp/ark_complete_test_dir/fo");
    std::sort(results.begin(), results.end());
    assert(results.size() == 2);
    assert(results[0] == "/tmp/ark_complete_test_dir/foo.txt");
    assert(results[1] == "/tmp/ark_complete_test_dir/foobar.txt");

    auto hidden = completePath("/tmp/ark_complete_test_dir/.hid");
    assert(hidden.size() == 1);
    assert(hidden[0] == "/tmp/ark_complete_test_dir/.hidden");

    auto notHidden = completePath("/tmp/ark_complete_test_dir/");
    assert(std::find(notHidden.begin(), notHidden.end(),
                      "/tmp/ark_complete_test_dir/.hidden") == notHidden.end());

    system("rm -rf /tmp/ark_complete_test_dir");
}

static void test_complete_command_finds_builtins() {
    auto results = completeCommand("ec");
    assert(std::find(results.begin(), results.end(), "echo") != results.end());
}

static void test_is_directory() {
    system("mkdir -p /tmp/ark_complete_isdir_test");
    assert(isDirectory("/tmp/ark_complete_isdir_test") == true);
    assert(isDirectory("/tmp/ark_complete_isdir_test_nonexistent") == false);
    system("rm -rf /tmp/ark_complete_isdir_test");
}
```

Add the three calls to `main()`.

- [ ] **Step 2: Run to verify these fail**

Run: `clang++ -std=c++20 -Isrc -o /tmp/complete_test tests/complete_test.cpp src/complete.cpp src/builtins.cpp && /tmp/complete_test`
Expected: FAIL — `completePath`/`completeCommand`/`isDirectory` declared but not defined (linker error)

- [ ] **Step 3: Add completePath/completeCommand/isDirectory to src/complete.cpp**

Add near the top: `#include "builtins.h"`, `#include <algorithm>`, `#include <cstdlib>`, `#include <dirent.h>`, `#include <sys/stat.h>`, `#include <unistd.h>`.

```cpp
static std::string expandHome(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(expandHome(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> completePath(const std::string& partial) {
    std::string dir, prefix;
    auto slash = partial.find_last_of('/');
    if (slash == std::string::npos) {
        dir = "";
        prefix = partial;
    } else {
        dir = partial.substr(0, slash + 1); // keep trailing slash
        prefix = partial.substr(slash + 1);
    }
    std::string lookupDir = expandHome(dir.empty() ? "." : dir);

    std::vector<std::string> results;
    DIR* d = opendir(lookupDir.c_str());
    if (!d) return results;

    bool showHidden = !prefix.empty() && prefix[0] == '.';
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (!showHidden && !name.empty() && name[0] == '.') continue;
        if (name.rfind(prefix, 0) == 0) {
            results.push_back(dir + name); // `dir` (possibly "" or "~/") preserved as typed
        }
    }
    closedir(d);
    std::sort(results.begin(), results.end());
    return results;
}

std::vector<std::string> completeCommand(const std::string& partial) {
    std::vector<std::string> results;
    for (const auto& kv : builtinRegistry()) {
        if (kv.first.rfind(partial, 0) == 0) results.push_back(kv.first);
    }

    const char* pathEnv = getenv("PATH");
    if (pathEnv) {
        std::string pathStr = pathEnv;
        size_t pos = 0;
        while (pos <= pathStr.size()) {
            size_t colon = pathStr.find(':', pos);
            std::string dir = (colon == std::string::npos) ? pathStr.substr(pos) : pathStr.substr(pos, colon - pos);
            if (!dir.empty()) {
                DIR* d = opendir(dir.c_str());
                if (d) {
                    struct dirent* entry;
                    while ((entry = readdir(d)) != nullptr) {
                        std::string name = entry->d_name;
                        if (name.rfind(partial, 0) == 0) {
                            std::string full = dir + "/" + name;
                            if (access(full.c_str(), X_OK) == 0) results.push_back(name);
                        }
                    }
                    closedir(d);
                }
            }
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
    }

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `clang++ -std=c++20 -Isrc -o /tmp/complete_test tests/complete_test.cpp src/complete.cpp src/builtins.cpp && /tmp/complete_test`
Expected: `all complete word/position tests passed`

- [ ] **Step 5: Commit**

```bash
git add src/complete.cpp tests/complete_test.cpp
git commit -m "complete: path completion (opendir/readdir) and command completion (builtins + \$PATH scan), no subprocess calls"
```

---

### Task 3: Wire Tab into the line editor

**Files:**
- Modify: `src/edit.cpp`

**Interfaces:**
- Consumes: `wordUnderCursor`, `isCommandPosition`, `longestCommonPrefix`, `completePath`, `completeCommand`, `isDirectory` from Tasks 1-2.

- [ ] **Step 1: Add #include "complete.h" to src/edit.cpp**

- [ ] **Step 2: Add Tab (ASCII 9) handling in the raw-mode key loop**

Find the key-handling chain in `readLine()` (right after the Backspace
handling, before the `\x1b` escape-sequence branch):

```cpp
        if (c == 127 || c == 8) { // Backspace
            if (cursor > 0) { buf.erase(cursor - 1, 1); cursor--; redraw(); }
            continue;
        }
```

Add immediately after it:

```cpp
        if (c == 9) { // Tab
            auto [wordStart, word] = wordUnderCursor(buf, cursor);
            bool cmdPos = isCommandPosition(buf, wordStart);
            auto candidates = cmdPos ? completeCommand(word) : completePath(word);
            if (candidates.empty()) { continue; }

            std::string prefix = longestCommonPrefix(candidates);
            if (prefix.size() > word.size()) {
                buf.replace(wordStart, word.size(), prefix);
                cursor = wordStart + prefix.size();
                if (candidates.size() == 1) {
                    char sep = (!cmdPos && isDirectory(prefix)) ? '/' : ' ';
                    buf.insert(cursor, 1, sep);
                    cursor++;
                }
                redraw();
            } else if (candidates.size() > 1) {
                std::cout << "\n";
                for (size_t i = 0; i < candidates.size(); i++) {
                    std::cout << candidates[i];
                    if (i + 1 < candidates.size()) std::cout << "  ";
                }
                std::cout << "\n";
                redraw();
            }
            continue;
        }
```

- [ ] **Step 3: Rebuild and run the full non-interactive test suite (regression check)**

Run: `cd /Users/gideoncox/ark-terminal && rm -f /tmp/ark_test_05_out.txt /tmp/ark_test_05b_in.txt /tmp/ark_test_05b_out.txt && make clean && make test`
Expected: all 14 integration cases still PASS (Tab-completion only affects the interactive line editor's key handling, not parsing/execution)

- [ ] **Step 4: Real PTY verification**

**4a. Single unambiguous match completes and adds a trailing space (command) or slash (directory):**
```bash
mkdir -p /tmp/ark_tab_test_dir/onlyfile_subdir
cd /tmp && cat > tab_complete.exp <<'EOF'
log_file -a tab_complete.bin
set timeout 8
spawn env COLUMNS=100 LINES=24 /Users/gideoncox/ark-terminal/ark
stty rows 24 columns 100 < $spawn_out(slave,name)
expect "*"
send "cd /tmp/ark_tab_test_dir\r"
sleep 0.2
send "cd onlyfile_su\t"
sleep 0.2
send "\r"
sleep 0.2
send "pwd\r"
sleep 0.3
send "\004"
expect eof
EOF
rm -f tab_complete.bin
expect tab_complete.exp >/dev/null 2>&1
python3 -c "
data = open('tab_complete.bin','rb').read().decode('utf-8', errors='replace')
assert 'onlyfile_subdir' in data, 'directory name was not completed'
assert '/tmp/ark_tab_test_dir/onlyfile_subdir' in data, 'cd into the completed directory did not land in the right place (pwd output missing)'
print('4a PASS: single-match directory completion works, trailing slash allowed cd to proceed')
"
```
Expected: `4a PASS: single-match directory completion works, trailing slash allowed cd to proceed`

**4b. Ambiguous match prints a candidate list:**
```bash
mkdir -p /tmp/ark_tab_test_dir2
touch /tmp/ark_tab_test_dir2/foo.txt /tmp/ark_tab_test_dir2/foobar.txt
cd /tmp && cat > tab_ambiguous.exp <<'EOF'
log_file -a tab_ambiguous.bin
set timeout 8
spawn env COLUMNS=100 LINES=24 /Users/gideoncox/ark-terminal/ark
stty rows 24 columns 100 < $spawn_out(slave,name)
expect "*"
send "cd /tmp/ark_tab_test_dir2\r"
sleep 0.2
send "cat fo\t"
sleep 0.3
send "\003"
sleep 0.2
send "\004"
expect eof
EOF
rm -f tab_ambiguous.bin
expect tab_ambiguous.exp >/dev/null 2>&1
python3 -c "
data = open('tab_ambiguous.bin','rb').read().decode('utf-8', errors='replace')
assert 'foo.txt' in data and 'foobar.txt' in data, 'ambiguous candidates were not both listed'
print('4b PASS: ambiguous match prints both candidates')
"
```
Expected: `4b PASS: ambiguous match prints both candidates`

- [ ] **Step 5: Clean up test artifacts**

```bash
rm -f /tmp/tab_complete.exp /tmp/tab_complete.bin /tmp/tab_ambiguous.exp /tmp/tab_ambiguous.bin
rm -rf /tmp/ark_tab_test_dir /tmp/ark_tab_test_dir2
```

- [ ] **Step 6: Commit**

```bash
git add src/edit.cpp
git commit -m "complete: wire Tab-completion into the interactive line editor"
```

---

## Self-Review Notes

**Spec coverage:** `wordUnderCursor`/`isCommandPosition`/`longestCommonPrefix`
(Task 1), `completePath`/`completeCommand`/`isDirectory` (Task 2), Tab-key
wiring with common-prefix + list-on-ambiguous display (Task 3) all map
directly to the design spec's Architecture/Data-Flow sections.

**Placeholder scan:** none — every step has complete code and real PTY
verification (not "test manually").

**Type consistency:** `wordUnderCursor` returns `std::pair<size_t, std::string>`
consistently between Task 1's declaration, its test usage (structured
bindings `auto [start, word] = ...`), and Task 3's usage in `edit.cpp`.
`completePath`/`completeCommand` both return `std::vector<std::string>`
consistently everywhere they're used.

**Deliberate deviation from the design spec:** the design spec described
`isCommandPosition` as conceptually reusing highlight.cpp's state machine;
this plan reimplements a small, self-contained version directly in
`complete.cpp` instead of exposing `classify()`'s internal `atCommandPos`
as a new public API on `highlight.h` just for this one caller -- simpler
and avoids coupling two otherwise-independent modules for one boolean.
