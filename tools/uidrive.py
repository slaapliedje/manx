#!/usr/bin/env python3
"""uidrive.py - drive a full-screen program and look at its screen.

    tools/uidrive.py [--tt] COMMAND [ARGS...] < steps

Runs COMMAND in a pseudo-terminal (or, with --tt, logs in to the TT over
telnet and runs COMMAND there, from /work/dev/ub), keeps a terminal
emulator (pyte) of what it draws, and follows a step script on stdin, one
step per line:

    keys TEXT          type TEXT; \\r \\t \\e and {UP} {DOWN} {LEFT} {RIGHT}
                       {PGUP} {PGDN} {HOME} {END} {BTAB} {ESC}
    wait SECONDS       let the program run
    until TEXT [SECS]  wait until TEXT is on screen (default 30 s); prints
                       how long the run has taken
    expect TEXT        fail (exit 1) unless TEXT is on screen now
    show               print the screen (ATTRS=1 adds R/U/B markers)

Environment: ROWS, COLS (default 24x80). --tt: ASV_HOST (default
192.168.3.250), ASV_TOOLS (where .asvpass lives, default
~/dev/OpenUA/data/work/asv; the password never leaves this machine),
TTCS (the UB_CHARSET given to the program there, default latin1).
The cursor-position probe is answered as a UTF-8 xterm would.

Needs pyte (pip install pyte).
"""
import os
import re
import select
import socket
import sys
import time

import pyte

ROWS = int(os.environ.get("ROWS", 24))
COLS = int(os.environ.get("COLS", 80))
KEYS = {"UP": "\x1b[A", "DOWN": "\x1b[B", "RIGHT": "\x1b[C", "LEFT": "\x1b[D",
        "PGDN": "\x1b[6~", "PGUP": "\x1b[5~", "HOME": "\x1b[H", "END": "\x1b[F",
        "BTAB": "\x1b[Z", "ESC": "\x1b"}
CPR_QUERY, CPR_ANSWER = b"\x1b[6n", b"\x1b[1;2R"


class Pty:
    """COMMAND in a local pseudo-terminal."""

    def __init__(self, argv):
        import fcntl
        import pty
        import struct
        import termios
        size = struct.pack("HHHH", ROWS, COLS, 0, 0)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, size)
            os.environ.setdefault("TERM", "xterm")
            os.execvp(argv[0], argv)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, size)

    def send(self, b):
        os.write(self.fd, b)

    def recv(self, t):
        r, _, _ = select.select([self.fd], [], [], t)
        if not r:
            return None
        try:
            return os.read(self.fd, 65536) or b""
        except OSError:
            return b""

    def close(self):
        os.kill(self.pid, 9)


class Telnet:
    """COMMAND on the TT, as root, over telnet (the asvsh.py login)."""
    IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

    def __init__(self, command):
        host = os.environ.get("ASV_HOST", "192.168.3.250")
        tools = os.environ.get("ASV_TOOLS",
                               os.path.expanduser("~/dev/OpenUA/data/work/asv"))
        self.s = socket.create_connection((host, 23), timeout=15)
        self.wait_for(b"ogin:", 20)
        self.send(b"root\r\n")
        if b"assword:" in self.wait_for(b"assword:", 10):
            with open(os.path.join(tools, ".asvpass")) as f:
                self.send(f.read().strip().encode() + b"\r\n")
        got = self.wait_for(b"# ", 20)
        if b"erminal" in got or b"TERM" in got:
            self.send(b"xterm\r\n")
            self.wait_for(b"# ", 10)
        self.send(b"stty -echo; cd /work/dev/ub\r\n")
        self.wait_for(b"# ", 10)
        # (a Bourne shell doesn't export VAR=x put before exec: export)
        env = "TERM=xterm LINES=%d COLUMNS=%d UB_HOME=/work/dev/ub/home " \
              "UB_CHARSET=%s" % (ROWS, COLS, os.environ.get("TTCS", "latin1"))
        names = " ".join(v.split("=")[0] for v in env.split())
        self.send(("%s; export %s; exec %s\r\n" % (env, names, command)).encode())

    def send(self, b):
        self.s.sendall(b)

    def recv(self, t):
        """Data (None: nothing within t seconds, b"": closed). Telnet
        negotiation is answered and left out."""
        end = time.time() + t
        while True:
            r, _, _ = select.select([self.s], [], [], max(0.0, end - time.time()))
            if not r:
                return None
            d = self.s.recv(65536)
            if not d:
                return b""
            out = self.strip(d)
            if out:
                return out

    def strip(self, d):
        out, i = bytearray(), 0
        while i < len(d):
            c = d[i]
            if c == self.IAC and i + 2 < len(d) and d[i + 1] in (
                    self.DO, self.DONT, self.WILL, self.WONT):
                answer = self.WONT if d[i + 1] in (self.DO, self.DONT) else self.DONT
                self.s.sendall(bytes([self.IAC, answer, d[i + 2]]))
                i += 3
                continue
            if c == self.IAC and i + 1 < len(d) and d[i + 1] == self.SB:
                j = d.find(bytes([self.IAC, self.SE]), i)
                i = j + 2 if j >= 0 else len(d)
                continue
            out.append(c)
            i += 1
        return bytes(out)

    def wait_for(self, pat, t):
        end, got = time.time() + t, b""
        while time.time() < end and pat not in got:
            d = self.recv(0.3)
            if d == b"":
                break
            got += d or b""
        return got

    def close(self):
        try:
            self.send(b"q")
            time.sleep(1)
        except OSError:
            pass
        self.s.close()


def main():
    args = sys.argv[1:]
    tt = bool(args) and args[0] == "--tt"
    if tt:
        args = args[1:]
    if not args:
        sys.exit(__doc__)
    conn = Telnet(" ".join(args)) if tt else Pty(args)
    screen = pyte.Screen(COLS, ROWS)
    stream = pyte.ByteStream(screen)
    t0 = time.time()

    def pump(t):
        end = time.time() + t
        while True:
            d = conn.recv(max(0.0, end - time.time()))
            if not d:
                return
            if CPR_QUERY in d:
                conn.send(CPR_ANSWER)
            stream.feed(d)

    def on_screen(text):
        return any(text in line for line in screen.display)

    def show():
        attrs = os.environ.get("ATTRS")
        print("+" + "-" * COLS + "+")
        for i, line in enumerate(screen.display):
            row = screen.buffer[i]
            a = "".join("R" if row[c].reverse else "U" if row[c].underscore
                        else "B" if row[c].bold else " " for c in range(COLS))
            print("|" + line + "|" + ("  " + a.rstrip() if attrs else ""))
        print("+" + "-" * COLS + "+", flush=True)

    status = 0
    try:
        pump(2.0 if tt else 1.0)
        for step in sys.stdin:
            step = step.rstrip("\n")
            if not step.strip() or step.startswith("#"):
                continue
            cmd, _, arg = step.partition(" ")
            if cmd == "keys":
                s = re.sub(r"\{(\w+)\}", lambda m: KEYS[m.group(1)], arg)
                s = s.replace("\\r", "\r").replace("\\t", "\t").replace("\\e", "\x1b")
                for ch in s:
                    conn.send(ch.encode("latin1") if ord(ch) < 256 else ch.encode())
                    time.sleep(0.05 if tt else 0.02)
                pump(1.0 if tt else 0.5)
            elif cmd == "wait":
                pump(float(arg))
            elif cmd == "until":
                m = re.match(r"(.*?)(?:\s+(\d+(?:\.\d+)?))?$", arg)
                text, secs = m.group(1), float(m.group(2) or 30)
                end = time.time() + secs
                while time.time() < end and not on_screen(text):
                    pump(0.3)
                print("[%s %s after %.1f s]" % (
                    text, "seen" if on_screen(text) else "NOT seen",
                    time.time() - t0), flush=True)
            elif cmd == "expect":
                if not on_screen(arg):
                    print("[expected %r: not on screen]" % arg, flush=True)
                    show()
                    status = 1
                    break
            elif cmd == "show":
                show()
            else:
                sys.exit("uidrive: unknown step %r" % step)
    finally:
        conn.close()
    sys.exit(status)


if __name__ == "__main__":
    main()
