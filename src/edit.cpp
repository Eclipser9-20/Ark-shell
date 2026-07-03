#include "edit.h"
#include "chrome.h"
#include "complete.h"
#include "arkfeatures.h"
#include "highlight.h"
#include "input.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

// Current terminal width in columns (fallback 80 if unknown). Read fresh each
// redraw so a resize is handled without restarting the line.
static int terminalCols() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

RawMode::RawMode() {
    tcgetattr(STDIN_FILENO, &orig);
    termios raw = orig;
    // IEXTEN matters as much as ICANON here: on BSD/macOS it enables
    // "extended input processing", which intercepts several control bytes
    // BEFORE they're delivered to read() -- notably VREPRINT (Ctrl-R, 0x12)
    // and VLNEXT (Ctrl-V), plus VDSUSP (Ctrl-Y) as delayed-suspend. Leaving
    // it on meant Ctrl-R never reached the reverse-search handler at all (the
    // byte was swallowed by the tty driver). Clearing IXON likewise lets
    // Ctrl-S/Ctrl-Q through as ordinary bytes instead of XON/XOFF output
    // flow control -- a real shell owns those keys, not the terminal driver.
    raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}
RawMode::~RawMode() { tcsetattr(STDIN_FILENO, TCSANOW, &orig); }

static std::atomic<bool> g_tick{false};

void installIdleTicker() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int) { g_tick.store(true, std::memory_order_relaxed); };
    sigemptyset(&sa.sa_mask);
    // Deliberately NOT SA_RESTART: this must interrupt a blocking read() so
    // the loop below gets a chance to check the flag even while sitting idle
    // waiting for the next keystroke.
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);

    // Route SIGWINCH (terminal resize) through the SAME flag as the tick, and
    // WITHOUT SA_RESTART, so a resize interrupts readLine()'s blocking read()
    // immediately -- the loop then calls onIdleTick() -> reassertChrome(),
    // which detects the geometry change and does its full-clear repaint on the
    // very next iteration instead of waiting up to a full second for the 1Hz
    // ticker to come around. That 1s lag was the "takes a second before it
    // refreshes into the clean version" artifact.
    struct sigaction sw;
    std::memset(&sw, 0, sizeof(sw));
    sw.sa_handler = [](int) { g_tick.store(true, std::memory_order_relaxed); };
    sigemptyset(&sw.sa_mask);
    sw.sa_flags = 0;
    sigaction(SIGWINCH, &sw, nullptr);

    struct itimerval timer;
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 1;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, nullptr);
}

// Reads exactly one byte, transparently retrying on EINTR (SIGALRM/SIGWINCH
// landing mid-syscall). Used for every raw read in this file so a
// badly-timed signal can never truncate a multi-byte escape sequence --
// the outer loop in readLine() is what checks g_tick, once per full
// keystroke/action, not here.
// Routed through the shared FIFO (arkinput) so the chrome's DSR cursor query and
// this editor never steal each other's bytes -- see input.h.
static ssize_t readByte(char& out) {
    return (ssize_t)arkinput::readByte(out, /*retryEINTR=*/true);
}

// True if a byte is already available (or arrives within `timeoutMs`) on
// stdin. A real escape SEQUENCE (arrow keys etc.) arrives as a fast burst --
// the terminal constructs and sends the whole multi-byte sequence for one
// physical keypress essentially at once -- while a standalone Escape key
// press sends JUST the 0x1B byte, with nothing following. Used to
// distinguish the two instead of blocking forever waiting for continuation
// bytes that, for a standalone Escape, will never arrive on their own (real
// bug found live: pressing bare Escape hung ark's line editor indefinitely).
static bool byteAvailableSoon(int timeoutMs) {
    return arkinput::available(timeoutMs);
}

// Move cursor left past any whitespace then the whole preceding word --
// matches readline's backward-word (Alt+B / Alt+Left / Meta-b).
static void moveWordBackward(const std::string& buf, size_t& cursor) {
    while (cursor > 0 && std::isspace((unsigned char)buf[cursor - 1])) cursor--;
    while (cursor > 0 && !std::isspace((unsigned char)buf[cursor - 1])) cursor--;
}

// Move cursor right past the current word then any following whitespace --
// matches readline's forward-word (Alt+F / Alt+Right / Meta-f).
static void moveWordForward(const std::string& buf, size_t& cursor) {
    while (cursor < buf.size() && std::isspace((unsigned char)buf[cursor])) cursor++;
    while (cursor < buf.size() && !std::isspace((unsigned char)buf[cursor])) cursor++;
}

// The index (into history.lines()) of the position Ctrl-W should erase back
// to -- same traversal as moveWordBackward, but as a query rather than a
// mutation, since the kill needs both the target position AND the erased
// text (saved for Ctrl-Y).
static size_t wordBackwardIndex(const std::string& buf, size_t cursor) {
    size_t i = cursor;
    while (i > 0 && std::isspace((unsigned char)buf[i - 1])) i--;
    while (i > 0 && !std::isspace((unsigned char)buf[i - 1])) i--;
    return i;
}

// Most recent history entry (searching backward from `startIdx`, exclusive)
// containing `query` as a substring, or -1 if none match. An empty query
// never matches anything -- reverse-i-search shows no match until the user
// actually types something, matching bash.
static int searchHistoryBackward(const std::vector<std::string>& lines, const std::string& query, int startIdx) {
    if (query.empty()) return -1;
    for (int i = startIdx - 1; i >= 0; i--) {
        if (lines[i].find(query) != std::string::npos) return i;
    }
    return -1;
}

std::optional<std::string> readLine(const std::string& prompt, History& history,
                                     const std::function<void()>& onIdleTick,
                                     const std::function<bool(const std::string&)>& isValidCommand) {
    RawMode raw;
    std::string buf;
    size_t cursor = 0;
    int histIndex = (int)history.lines().size(); // one-past-the-end = "not browsing history"
    std::string killBuffer; // last text erased by Ctrl-K/U/W, pastable with Ctrl-Y

    // Context-Aware Autosuggestions: prefer history entries that were run in
    // THIS directory. Captured once at readLine entry (cwd can't change mid-
    // line without a command running).
    std::string curCwd;
    { char cb[4096]; if (getcwd(cb, sizeof(cb))) curCwd = cb; }

    std::cout << prompt << std::flush;

    // fish-style autosuggestion: the tail of the most recent history entry
    // that starts with the current buffer, shown as dimmed ghost text after
    // the cursor and accepted with Right-arrow / Ctrl-F / End. Empty if the
    // buffer is empty or nothing in history extends it. Context-aware: a match
    // from the CURRENT directory wins over an equally-recent match elsewhere.
    auto currentSuggestion = [&]() -> std::string {
        if (buf.empty()) return "";
        if (const char* g = getenv("ARK_GHOST_TEXT"); g && std::string(g) == "0") return ""; // config toggle
        const auto& lines = history.lines();
        const auto& cwds = history.cwds();
        std::string fallback; // best match regardless of directory
        for (int i = (int)lines.size() - 1; i >= 0; i--) {
            if (lines[i].size() > buf.size() && lines[i].compare(0, buf.size(), buf) == 0) {
                // A same-directory hit is returned immediately (most recent
                // wins); otherwise remember the first (most recent) hit as the
                // fallback and keep scanning for a cwd-local one.
                if (i < (int)cwds.size() && !cwds[i].empty() && cwds[i] == curCwd)
                    return lines[i].substr(buf.size());
                if (fallback.empty()) fallback = lines[i].substr(buf.size());
            }
        }
        if (!fallback.empty()) return fallback;
        // No history match -> fall back to a filesystem/command completion, but
        // ONLY when there's a single candidate that EXTENDS the typed word
        // (ghost text is an append; a cross-directory replacement like
        // ~/bin/foo can't be shown this way -- that stays Tab's job). This is
        // what makes ghost text work for files and commands, not just history.
        auto [wordStart, word] = wordUnderCursor(buf, cursor);
        if (word.size() >= 2) {
            bool cmdPos = isCommandPosition(buf, wordStart);
            auto cands = cmdPos ? completeCommand(word) : completePath(word);
            std::string only;
            int extending = 0;
            for (const auto& c : cands) {
                if (c.size() > word.size() && c.compare(0, word.size(), word) == 0) { only = c; extending++; }
            }
            if (extending == 1) return only.substr(word.size());
        }
        return "";
    };

    // Display width of a string: skip ANSI escape sequences (0 columns) and
    // count UTF-8 code points (1 column each; wide CJK/emoji are an accepted
    // approximation). Used to reason about line WRAPPING for the redraw.
    auto dispWidth = [](const std::string& s) -> int {
        int w = 0;
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '\x1b') { // ESC [ ... <final @-~>  (or a short 2-byte escape)
                i++;
                if (i < s.size() && s[i] == '[') { i++; while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) i++; if (i < s.size()) i++; }
                else if (i < s.size()) i++;
                continue;
            }
            if (((unsigned char)s[i] & 0xC0) != 0x80) w++; // count non-continuation bytes
            i++;
        }
        return w;
    };
    int plen = dispWidth(prompt);       // prompt is fixed for this readLine call
    int lastRows = 1;                    // rows the previous refresh occupied (incl. ghost)
    size_t lastPos = 0;                  // cursor byte-pos at the previous refresh

    // Multi-line-aware refresh (linenoise-style). The old naive "\r\x1b[K" only
    // cleared ONE row, so a line + ghost that WRAPPED past the terminal width
    // smeared on every keystroke. Now we track how many rows the last render
    // used and where the cursor was, clear exactly those rows, reprint, and
    // reposition -- correct for arbitrarily long / wrapped input.
    auto redraw = [&]() {
        int cols = terminalCols();
        const char* hl = getenv("ARK_SYNTAX_HIGHLIGHT");
        bool highlight = !(hl && std::string(hl) == "0");
        const char* vv = getenv("ARK_VALIDATE");
        bool validate = isValidCommand && !(vv && std::string(vv) == "0");
        std::string rendered = !highlight ? buf
                             : validate   ? highlightLineValidated(buf, isValidCommand)
                                          : highlightLine(buf);
        // Ghost only at end of line; capped so it can't push the line onto extra
        // wrapped rows (a huge history match with trailing spaces used to balloon
        // the redraw). It fills at most to the end of the current visual row.
        std::string sug = (cursor == buf.size()) ? currentSuggestion() : "";
        int bw = dispWidth(buf);
        if (!sug.empty()) {
            int room = cols - ((plen + bw) % cols) - 1; // cols left on the cursor's row
            if (room < 1) sug.clear();
            else if ((int)sug.size() > room) sug = sug.substr(0, room);
        }
        int glen = dispWidth(sug);
        int total = plen + bw + glen;                    // full display width incl ghost
        int rows = cols > 0 ? (total + cols) / cols : 1; if (rows < 1) rows = 1;
        int curCol = plen + dispWidth(buf.substr(0, cursor)); // cursor's display column (absolute)

        std::string out;
        // 1. Go to the last row of the OLD render, then clear each row upward.
        int oldCursorRow = cols > 0 ? (plen + (int)lastPos) / cols + 1 : 1; // approx (byte≈col for ascii)
        if (lastRows - oldCursorRow > 0) out += "\x1b[" + std::to_string(lastRows - oldCursorRow) + "B";
        for (int j = 0; j < lastRows - 1; j++) out += "\r\x1b[K\x1b[1A";
        out += "\r\x1b[K";
        // 2. Reprint prompt + buffer + ghost.
        out += prompt;
        out += rendered;
        if (!sug.empty()) out += std::string("\x1b[38;2;86;95;137m") + sug + "\x1b[0m";
        // 3. Reposition the cursor (which is now at the end) back to its spot.
        int endRow = cols > 0 ? total / cols : 0;
        int curRow = cols > 0 ? curCol / cols : 0;
        if (endRow - curRow > 0) out += "\x1b[" + std::to_string(endRow - curRow) + "A";
        int col = cols > 0 ? curCol % cols : curCol;
        out += "\r";
        if (col > 0) out += "\x1b[" + std::to_string(col) + "C";

        std::cout << out << std::flush;
        lastRows = rows;
        lastPos = cursor;
    };

    // Finalize the visible line before returning/cancelling: move the cursor
    // past the real input and erase to end-of-line, so any dim ghost-text
    // suggestion sitting to the right of the cursor is wiped instead of being
    // left frozen on the submitted line (real bug: the ghost stayed on screen
    // after Enter). Then emit the newline.
    auto endLine = [&]() {
        if (cursor < buf.size()) std::cout << "\x1b[" << (buf.size() - cursor) << "C";
        std::cout << "\x1b[K\n" << std::flush;
        cursor = buf.size();
    };

    for (;;) {
        // Checked once per full keystroke/action (not just on a select()
        // timeout) so a continuous typing burst or a terminal paste -- where
        // read() keeps returning data immediately and never actually blocks
        // -- can't starve the tick indefinitely; it fires within one
        // keystroke of the SIGALRM that set it.
        if (onIdleTick && g_tick.exchange(false, std::memory_order_relaxed)) {
            onIdleTick();
            // If that tick's chrome repaint was a resize-driven full screen
            // clear, our prompt+buffer got wiped along with everything else --
            // reprint it (chrome left the cursor at row 2, col 1, ready for it)
            // so the input line doesn't vanish until the next keystroke.
            if (chromeConsumeResizeRepaint()) redraw();
        }

        char c;
        // retryEINTR=false here, deliberately: the auto-retry would swallow the
        // SIGALRM interruption before this loop ever sees it, so while genuinely
        // idle (blocked in read(), no bytes arriving) the tick would never fire.
        // Letting EINTR propagate back to the top of the loop is where the tick
        // actually gets checked before retrying. (arkinput drains any FIFO'd
        // bytes -- e.g. paste handed back by the DSR reader -- before touching
        // the fd, so pushed-back input is never lost to a tick.)
        ssize_t n = (ssize_t)arkinput::readByte(c, /*retryEINTR=*/false);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::nullopt; // EOF/error

        if (c == '\r' || c == '\n') { endLine(); return buf; }
        if (c == 3) {
            // Ctrl-C at the prompt: leave the typed text on screen, show "^C",
            // drop to a fresh prompt, and DISCARD the line (return empty so the
            // REPL loop re-prompts, running nothing). We echo "^C" ourselves
            // because raw mode has ISIG off (the kernel never sees Ctrl-C as a
            // signal here). Move past the real input + clear any ghost first.
            // ARK_CTRLC = "line" (default): "^C" on its OWN line, between the
            //   command and the next prompt:   cmd / ^C / ❯
            // ARK_CTRLC = "append": bash style, "^C" right after the command:
            //   cmd^C / ❯
            if (cursor < buf.size()) std::cout << "\x1b[" << (buf.size() - cursor) << "C";
            const char* mode = getenv("ARK_CTRLC");
            bool append = mode && std::string(mode) == "append";
            std::cout << "\x1b[K" << (append ? "^C\n" : "\n^C\n") << std::flush;
            return std::string();
        }
        if (c == 4) { // Ctrl-D: EOF on an EMPTY line, else delete-char-forward (like bash)
            if (buf.empty()) { endLine(); return std::nullopt; }
            if (cursor < buf.size()) { buf.erase(cursor, 1); redraw(); }
            continue;
        }
        if (c == 127 || c == 8) { // Backspace
            if (cursor > 0) { buf.erase(cursor - 1, 1); cursor--; redraw(); }
            continue;
        }
        if (c == 1) { cursor = 0; redraw(); continue; }            // Ctrl-A: start of line
        if (c == 5) { cursor = buf.size(); redraw(); continue; }   // Ctrl-E: end of line
        if (c == 6) { // Ctrl-F: forward one char, or accept the autosuggestion at end of line
            if (cursor < buf.size()) { cursor++; redraw(); }
            else { std::string s = currentSuggestion(); if (!s.empty()) { buf += s; cursor = buf.size(); redraw(); } }
            continue;
        }
        if (c == 11) { // Ctrl-K: kill to end of line
            killBuffer = buf.substr(cursor);
            buf.erase(cursor);
            redraw();
            continue;
        }
        if (c == 21) { // Ctrl-U: kill to start of line
            killBuffer = buf.substr(0, cursor);
            buf.erase(0, cursor);
            cursor = 0;
            redraw();
            continue;
        }
        if (c == 23) { // Ctrl-W: kill the word before the cursor
            size_t start = wordBackwardIndex(buf, cursor);
            killBuffer = buf.substr(start, cursor - start);
            buf.erase(start, cursor - start);
            cursor = start;
            redraw();
            continue;
        }
        if (c == 25) { // Ctrl-Y: yank (paste) the last killed text
            if (!killBuffer.empty()) {
                buf.insert(cursor, killBuffer);
                cursor += killBuffer.size();
                redraw();
            }
            continue;
        }
        if (c == 18) { // Ctrl-R: reverse incremental history search
            std::string savedBuf = buf;
            size_t savedCursor = cursor;
            std::string query;
            int matchIdx = -1;

            auto redrawSearch = [&]() {
                std::string matched = matchIdx >= 0 ? history.lines()[matchIdx] : "";
                std::cout << "\r\x1b[K(reverse-i-search)'" << query << "': " << matched << std::flush;
            };
            redrawSearch();

            bool cancelled = false;
            bool accepted = false;
            bool runIt = false; // Enter accepts AND runs; other accept-keys just populate
            for (;;) {
                char sc;
                ssize_t sn = (ssize_t)arkinput::readByte(sc, /*retryEINTR=*/true);
                if (sn < 0 && errno == EINTR) continue;
                if (sn <= 0) { cancelled = true; break; }

                if (sc == 18) { // Ctrl-R again: find the next (older) match
                    int searchFrom = matchIdx >= 0 ? matchIdx : (int)history.lines().size();
                    int next = searchHistoryBackward(history.lines(), query, searchFrom);
                    if (next >= 0) matchIdx = next; // no match further back: keep current one, like bash
                    redrawSearch();
                    continue;
                }
                if (sc == 7) { cancelled = true; break; } // Ctrl-G: cancel
                if (sc == '\x1b') {
                    // Standalone Escape cancels. An escape SEQUENCE (arrow / Home /
                    // End / ...) ends the search accepting the current match -- and
                    // we must CONSUME the rest of the sequence here, or its bytes
                    // ([C / [D / ...) leak into the edit buffer as literal text.
                    if (byteAvailableSoon(50)) {
                        char b2;
                        if (readByte(b2) > 0 && b2 == '[') {
                            char fin;
                            do { if (readByte(fin) <= 0) break; }
                            while (!((fin >= 'A' && fin <= 'Z') || (fin >= 'a' && fin <= 'z') || fin == '~'));
                        }
                        accepted = true; break; // populate the match, don't run
                    }
                    cancelled = true; break;
                }
                if (sc == '\r' || sc == '\n') { accepted = true; runIt = true; break; } // Enter: accept AND run
                if (sc == 127 || sc == 8) { // Backspace: shrink the query, re-search from the top
                    if (!query.empty()) query.pop_back();
                    matchIdx = searchHistoryBackward(history.lines(), query, (int)history.lines().size());
                    redrawSearch();
                    continue;
                }
                if ((unsigned char)sc >= 32 && (unsigned char)sc < 127) { // printable
                    query += sc;
                    matchIdx = searchHistoryBackward(history.lines(), query, (int)history.lines().size());
                    redrawSearch();
                    continue;
                }
                // Any other key (arrows, Tab, etc.): accept the current match
                // into the edit buffer and fall through to normal editing,
                // without executing it (matches bash's "any other command
                // key ends the search" behavior, simplified -- the
                // triggering keystroke itself is just dropped rather than
                // re-interpreted, e.g. an arrow won't also move the cursor).
                accepted = true;
                break;
            }

            if (accepted && matchIdx >= 0) {
                buf = history.lines()[matchIdx];
                cursor = buf.size();
                if (runIt) { endLine(); return buf; } // Enter: run the match immediately
                redraw();                             // arrow/other key: populate + keep editing
                continue;
            }
            // No match (or cancelled): resume editing whatever was there before.
            buf = savedBuf;
            cursor = savedCursor;
            redraw();
            continue;
        }
        if (c == 9) { // Tab
            // If a ghost autosuggestion is showing (cursor at end of line),
            // Tab ACCEPTS it -- so you never need to reach for Right-arrow.
            // Only when there's no suggestion does Tab fall through to normal
            // completion of the word under the cursor.
            if (cursor == buf.size()) {
                std::string s = currentSuggestion();
                if (!s.empty()) { buf += s; cursor = buf.size(); redraw(); continue; }
            }
            auto [wordStart, word] = wordUnderCursor(buf, cursor);
            bool cmdPos = isCommandPosition(buf, wordStart);
            auto candidates = cmdPos ? completeCommand(word) : completePath(word);
            // Also pull from the whole-filesystem index (if built) so Tab
            // finds a file/program anywhere, not just cwd/$PATH. Deduped.
            for (auto& hit : completeFromIndex(word, cmdPos)) candidates.push_back(hit);
            // Dynamic Man-Page Completions: a flag argument (-x/--long, not in
            // command position) gets its options from the command's man page.
            const char* mpOff = getenv("ARK_MANPAGE_COMPLETE");
            if (!cmdPos && !word.empty() && word[0] == '-' && !(mpOff && std::string(mpOff) == "0")) {
                std::string firstWord = buf.substr(0, buf.find_first_of(" \t"));
                for (auto& f : manPageFlags(firstWord, word)) candidates.push_back(f);
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
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
        if (c == '\x1b') { // escape sequence (arrow keys, Alt+word-jumps) OR
                            // a standalone Escape key press
            if (!byteAvailableSoon(50)) {
                // Nothing followed within 50ms -- a real arrow-key sequence
                // would have. Treat as a standalone Escape: give it a
                // defined behavior (cancel the line, matching Ctrl-C)
                // instead of the previous silent hang waiting for
                // continuation bytes that were never coming.
                std::cout << "\n";
                return std::string();
            }
            char first;
            if (readByte(first) <= 0) continue;

            // Alt+Left / Alt+Right word jumps: macOS terminals commonly send
            // these as the 2-byte Meta-prefixed readline forms (ESC b / ESC
            // f) rather than a CSI sequence -- handle both this and the CSI
            // form below, since which one a given terminal/config sends
            // isn't something ark controls.
            if (first == 'b') { moveWordBackward(buf, cursor); redraw(); continue; }
            if (first == 'f') { moveWordForward(buf, cursor); redraw(); continue; }
            if (first != '[') continue; // unrecognized 2-byte escape, discard

            // CSI sequence: ESC [ <params...> <final-letter>. The previous
            // version assumed every sequence was exactly 3 bytes total (ESC
            // [ X) -- true for a plain arrow key, but NOT for anything with
            // modifier parameters like Alt+Right's xterm form ESC[1;3C. That
            // fixed-length assumption meant reading a byte too few for a
            // longer sequence, treating whatever came next (unrelated,
            // possibly the user's very next real keystroke) as if it were
            // part of this one. Read until a letter (or ~) terminates it.
            std::string params;
            char final = 0;
            for (;;) {
                char b;
                if (readByte(b) <= 0) break;
                if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~') { final = b; break; }
                params += b;
                if (params.size() > 8) break; // sanity guard against a malformed/runaway sequence
            }

            if (final == 'C' && params.empty()) { // Right: forward char, or accept ONE WORD of the suggestion
                if (cursor < buf.size()) { cursor++; redraw(); }
                else {
                    std::string s = currentSuggestion();
                    if (!s.empty()) {
                        // Accept just the next word (fish-style): skip leading
                        // spaces, take the word, include one trailing space.
                        // Ctrl-F / Tab still accept the whole suggestion.
                        size_t adv = 0;
                        while (adv < s.size() && s[adv] == ' ') adv++;
                        while (adv < s.size() && s[adv] != ' ') adv++;
                        if (adv < s.size() && s[adv] == ' ') adv++;
                        buf += s.substr(0, adv);
                        cursor = buf.size();
                        redraw();
                    }
                }
            }
            else if (final == 'D' && params.empty() && cursor > 0) { cursor--; redraw(); }
            else if (final == 'C' && params == "1;3") { moveWordForward(buf, cursor); redraw(); }  // xterm Alt+Right
            else if (final == 'D' && params == "1;3") { moveWordBackward(buf, cursor); redraw(); } // xterm Alt+Left
            else if (final == 'H' && params.empty()) { cursor = 0; redraw(); }           // Home
            else if (final == 'F' && params.empty()) { cursor = buf.size(); redraw(); }  // End
            else if (final == 'A' && params.empty()) { // Up: older history
                if (histIndex > 0) { histIndex--; buf = history.lines()[histIndex]; cursor = buf.size(); redraw(); }
            } else if (final == 'B' && params.empty()) { // Down: newer history
                if (histIndex < (int)history.lines().size()) {
                    histIndex++;
                    buf = histIndex == (int)history.lines().size() ? "" : history.lines()[histIndex];
                    cursor = buf.size();
                    redraw();
                }
            }
            continue;
        }
        // Only insert printable ASCII (0x20-0x7e) or UTF-8 bytes (high bit set).
        // Any other unhandled control byte -- Ctrl-B (0x02), Ctrl-T, a stray
        // 0x7f, a leaked escape-sequence remnant -- is dropped instead of being
        // embedded as a raw control char into the command that then runs.
        if ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f) {
            buf.insert(cursor, 1, c);
            cursor++;
            redraw();
        }
    }
}
