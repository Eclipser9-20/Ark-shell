#!/usr/bin/env python3
"""Self-test for tests/vt.py -- the emulator must be trustworthy before any
conclusion drawn with it means anything."""
import sys
from vt import VT

fails = []
def check(label, got, want):
    if got != want:
        fails.append(f"{label}: got {got!r}, want {want!r}")
        print(f"  FAIL {label}: got {got!r} want {want!r}")
    else:
        print(f"  ok   {label}")

print("basic output + cursor")
v = VT(5, 10)
v.feed(b"abc")
check("text lands", v.display[0].rstrip(), "abc")
check("cursor col", v.cx, 3)
v.feed(b"\r\n")
check("CR/LF -> row 1 col 0", (v.cy, v.cx), (1, 0))

print("column wrap")
v = VT(3, 4)
v.feed(b"abcdef")
check("wrapped rows", [v.display[0], v.display[1].rstrip()], ["abcd", "ef"])

print("scrolling at bottom")
v = VT(3, 6)
v.feed(b"1\r\n2\r\n3\r\n4")
check("scrolled content", [l.rstrip() for l in v.display], ["2", "3", "4"])

print("DECSTBM region scroll (ark pins bars this way)")
v = VT(5, 6)
v.feed(b"\x1b[2;4r")                    # region = rows 2..4 (1-indexed)
check("region set", (v.top, v.bot), (1, 3))
check("DECSTBM homes cursor", (v.cy, v.cx), (1, 0))
v.feed(b"A\r\nB\r\nC\r\nD")             # 4 lines into a 3-line region
check("only region scrolled", [l.rstrip() for l in v.display], ["", "B", "C", "D", ""])

print("DECSC / DECRC")
v = VT(4, 8)
v.feed(b"\x1b[3;5H\x1b7\x1b[1;1H\x1b8")
check("cursor restored", (v.cy, v.cx), (2, 4))

print("erase line / display")
v = VT(3, 6)
v.feed(b"abcdef\x1b[1;3H\x1b[K")
check("erase to EOL", v.display[0].rstrip(), "ab")
v = VT(3, 6)
v.feed(b"abc\r\ndef\x1b[2J")
check("erase display", [l.strip() for l in v.display], ["", "", ""])

print("DSR reports TRUE position (the whole point)")
v = VT(10, 20)
v.feed(b"\x1b[7;13H")
replies = v.feed(b"\x1b[6n")
check("DSR reply", replies, [b"\x1b[7;13R"])
v = VT(10, 20)
v.feed(b"hello")
check("DSR after text", v.feed(b"\x1b[6n"), [b"\x1b[6;1R".replace(b"6;1", b"1;6")])

print("UTF-8 glyph occupies one cell")
v = VT(2, 8)
v.feed("❯ x".encode())
check("wide glyph width", v.cx, 3)
check("glyph stored", v.display[0].rstrip(), "❯ x")

print("private modes and SGR are consumed, not printed")
v = VT(2, 12)
v.feed(b"\x1b[?1000l\x1b[38;2;1;2;3mZ\x1b[0m")
check("only Z printed", v.display[0].rstrip(), "Z")

print("split escape across feeds")
v = VT(3, 8)
v.feed(b"\x1b[2")
v.feed(b";3HX")
check("reassembled CSI", (v.cy, v.cx, v.display[1].rstrip()), (1, 3, "  X"))

print("OSC title consumed, not printed")
v = VT(2, 20)
v.feed(b"\x1b]2;Ark  /tmp\x07OK")
check("OSC swallowed", v.display[0].rstrip(), "OK")
v = VT(2, 20)
v.feed(b"\x1b]0;title\x1b\\OK")
check("OSC ST form", v.display[0].rstrip(), "OK")

print()
if fails:
    print(f"SELFTEST FAILED ({len(fails)})")
    sys.exit(1)
print("SELFTEST PASSED -- emulator is trustworthy")
