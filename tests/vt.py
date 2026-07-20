#!/usr/bin/env python3
"""A minimal VT100/xterm screen model, written from scratch for ark's tests.

Why this exists: ark's prompt/chrome logic asks the terminal where the cursor is
(DSR, ESC[6n) and changes behaviour based on the answer -- fresh-line insertion,
pulling the cursor off the pinned bottom bar, the transient failed-command
prompt. A test harness that fakes that reply with a constant (e.g. always
"row 5, col 1") silently invalidates every one of those code paths: the shell
takes a branch it would never take against a real terminal, the test passes or
fails for the wrong reason, and the result is worse than no test. Two such
harnesses produced two false conclusions before this file existed.

So this models only what ark actually exercises, but models it honestly:
  * a rows x cols cell grid with real overwrite semantics
  * cursor motion, column wrap, and scrolling at the margin
  * DECSTBM scroll regions (ark pins its bars by shrinking the region)
  * DECSC/DECRC save/restore (chrome brackets its repaints with these)
  * erase-in-line / erase-in-display
  * DSR, answered with the TRUE cursor position

Deliberately NOT modelled: colours/attributes (SGR is parsed and dropped),
character sets, tabs stops, mouse encoding, the alternate screen buffer's
separate grid. Anything unrecognised is consumed and ignored rather than being
printed as text, so an unhandled sequence can't masquerade as output.
"""


class VT:
    def __init__(self, rows=24, cols=80):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.cy = self.cx = 0          # 0-indexed cursor
        self.top, self.bot = 0, rows - 1  # scroll region, 0-indexed inclusive
        self.saved = (0, 0)
        self.pending_dsr = 0           # how many ESC[6n replies are owed
        self._buf = b""

    # ---------- screen primitives ----------
    def _scroll_up(self):
        """Scroll the CURRENT region up one line; bottom line becomes blank."""
        del self.grid[self.top]
        self.grid.insert(self.bot, [" "] * self.cols)

    def _newline(self):
        if self.cy == self.bot:
            self._scroll_up()
        elif self.cy < self.rows - 1:
            self.cy += 1

    def _putc(self, ch):
        if self.cx >= self.cols:       # wrap at the right margin
            self.cx = 0
            self._newline()
        if 0 <= self.cy < self.rows:
            self.grid[self.cy][self.cx] = ch
        self.cx += 1

    # ---------- feed ----------
    def feed(self, data: bytes):
        """Consume output bytes. Returns a list of replies to write back."""
        replies = []
        self._buf += data
        i = 0
        b = self._buf
        while i < len(b):
            c = b[i]
            if c == 0x1B:              # ESC
                consumed, reply = self._escape(b, i)
                if consumed is None:   # incomplete sequence, wait for more bytes
                    break
                if reply:
                    replies.append(reply)
                i += consumed
                continue
            i += 1
            if c == 0x0A:              # LF
                self._newline()
            elif c == 0x0D:            # CR
                self.cx = 0
            elif c == 0x08:            # BS
                self.cx = max(0, self.cx - 1)
            elif c == 0x09:            # TAB
                self.cx = min(self.cols - 1, (self.cx // 8 + 1) * 8)
            elif c >= 0x20:
                # Decode one UTF-8 codepoint so wide glyphs (ark's ❯, pill caps)
                # occupy ONE cell rather than 3, which would skew every column.
                ln = 1 if c < 0x80 else (2 if c >> 5 == 0b110 else (3 if c >> 4 == 0b1110 else 4))
                self._putc(b[i - 1:i - 1 + ln].decode("utf-8", "replace"))
                i += ln - 1
        self._buf = b[i:]
        return replies

    def _escape(self, b, i):
        """Handle one escape sequence at b[i]. -> (bytes_consumed, reply|None)."""
        if i + 1 >= len(b):
            return None, None
        nxt = b[i + 1]

        if nxt == 0x37:   # ESC 7  DECSC
            self.saved = (self.cy, self.cx)
            return 2, None
        if nxt == 0x38:   # ESC 8  DECRC
            self.cy, self.cx = self.saved
            return 2, None
        if nxt == 0x4D:   # ESC M  reverse index
            if self.cy == self.top:
                self.grid.insert(self.top, [" "] * self.cols)
                del self.grid[self.bot + 1]
            else:
                self.cy = max(0, self.cy - 1)
            return 2, None
        if nxt == 0x5D:   # ESC ]  OSC -- ark sets the window title this way.
            # Runs to BEL or ST (ESC \). Must be consumed as a unit; letting the
            # payload fall through prints "2;Ark /path" as visible screen text.
            j = i + 2
            while j < len(b):
                if b[j] == 0x07:                       # BEL
                    return j - i + 1, None
                if b[j] == 0x1B and j + 1 < len(b) and b[j + 1] == 0x5C:  # ST
                    return j - i + 2, None
                j += 1
            return None, None                          # incomplete
        if nxt != 0x5B:   # not CSI -> two-byte escape, drop it
            return 2, None

        # CSI: ESC [ <private?> <params> <final>
        j = i + 2
        priv = False
        if j < len(b) and b[j:j + 1] in (b"?", b">", b"<"):
            priv = True
            j += 1
        start = j
        while j < len(b) and 0x30 <= b[j] <= 0x3B:   # digits and ';'
            j += 1
        if j >= len(b):
            return None, None                        # incomplete
        final = b[j]
        params_raw = b[start:j].decode("ascii", "replace")
        params = [int(p) if p.isdigit() else 0 for p in params_raw.split(";")] if params_raw else []
        consumed = j - i + 1

        def p(idx, default=1):
            return params[idx] if idx < len(params) and params[idx] != 0 else default

        if priv:
            return consumed, None       # ?25h/l, ?1049h/l, ?1000l, ?2026h/l ... ignored

        if final == 0x48 or final == 0x66:      # H / f  cursor position
            self.cy = min(self.rows - 1, max(0, p(0) - 1))
            self.cx = min(self.cols - 1, max(0, p(1) - 1))
        elif final == 0x41:                     # A up
            self.cy = max(0, self.cy - p(0))
        elif final == 0x42:                     # B down
            self.cy = min(self.rows - 1, self.cy + p(0))
        elif final == 0x43:                     # C right
            self.cx = min(self.cols - 1, self.cx + p(0))
        elif final == 0x44:                     # D left
            self.cx = max(0, self.cx - p(0))
        elif final == 0x47:                     # G column absolute
            self.cx = min(self.cols - 1, max(0, p(0) - 1))
        elif final == 0x64:                     # d row absolute
            self.cy = min(self.rows - 1, max(0, p(0) - 1))
        elif final == 0x4A:                     # J erase display
            mode = params[0] if params else 0
            if mode == 2:
                self.grid = [[" "] * self.cols for _ in range(self.rows)]
            elif mode == 0:
                self.grid[self.cy][self.cx:] = [" "] * (self.cols - self.cx)
                for r in range(self.cy + 1, self.rows):
                    self.grid[r] = [" "] * self.cols
            elif mode == 1:
                for r in range(0, self.cy):
                    self.grid[r] = [" "] * self.cols
                self.grid[self.cy][:self.cx + 1] = [" "] * (self.cx + 1)
        elif final == 0x4B:                     # K erase line
            mode = params[0] if params else 0
            if mode == 0:
                self.grid[self.cy][self.cx:] = [" "] * (self.cols - self.cx)
            elif mode == 1:
                self.grid[self.cy][:self.cx + 1] = [" "] * (self.cx + 1)
            else:
                self.grid[self.cy] = [" "] * self.cols
        elif final == 0x72:                     # r DECSTBM scroll region
            if params:
                self.top = max(0, p(0) - 1)
                self.bot = min(self.rows - 1, p(1, self.rows) - 1)
            else:
                self.top, self.bot = 0, self.rows - 1
            self.cy, self.cx = self.top, 0      # DECSTBM homes the cursor
        elif final == 0x6E and params and params[0] == 6:   # n -> DSR
            self.pending_dsr += 1
            return consumed, f"\x1b[{self.cy + 1};{self.cx + 1}R".encode()
        # 'm' (SGR) and everything else: consumed, no effect
        return consumed, None

    # ---------- inspection ----------
    @property
    def display(self):
        return ["".join(row) for row in self.grid]

    def dump(self, label=""):
        if label:
            print(f"===== {label} =====")
        for i, line in enumerate(self.display):
            if line.strip():
                print(f"{i:2d}| {line.rstrip()}")

    def find(self, needle):
        """Row index of the first line containing `needle`, else -1."""
        for i, line in enumerate(self.display):
            if needle in line:
                return i
        return -1
