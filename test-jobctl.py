#!/usr/bin/env python3
#
# Automated keystroke tests for the TTY / job control project.
# Boots xv6 in QEMU, types into its console like a person would
# (including Ctrl-C, Ctrl-Z, Ctrl-T and arrow keys) and checks
# what comes back.
#
#   ./test-jobctl.py            run every test
#   ./test-jobctl.py fg ctrl    run tests whose name contains "fg" or "ctrl"
#   ./test-jobctl.py -v ...     also show everything xv6 prints
#
# Needs: python3-pexpect, and `make qemu` working in this directory.

import argparse, re, sys, time
import pexpect

CTRL_C = "\x03"
CTRL_D = "\x04"
CTRL_P = "\x10"
CTRL_T = "\x14"
CTRL_Z = "\x1a"
UP     = "\x1b[A"
LEFT   = "\x1b[D"

ANSI = re.compile(r"\x1b\[[0-9]*[A-Za-z]")


class TestFailure(Exception):
    pass


class XV6:
    """One booted xv6, driven through its console."""

    def __init__(self, verbose):
        self.c = pexpect.spawn("make qemu", encoding="utf-8",
                               codec_errors="replace", timeout=15)
        if verbose:
            self.c.logfile_read = sys.stdout
        self.c.expect(r"\$ ")
        self.buf = ""
        time.sleep(0.3)

    def type(self, keys, pause=0.3):
        """Send keys, one at a time, like typing."""
        for k in keys:
            self.c.send(k)
            time.sleep(0.01)
        time.sleep(pause)

    def run(self, line, pause=0.3):
        self.type(line + "\r", pause)

    def _read(self):
        try:
            self.buf += ANSI.sub("", self.c.read_nonblocking(4096, timeout=0.2))
        except pexpect.TIMEOUT:
            pass

    def expect(self, regex, timeout=10, why=""):
        """Wait until regex appears in the output (ANSI codes removed).
        Returns the output up to the match; the rest is kept."""
        deadline = time.time() + timeout
        while True:
            m = re.search(regex, self.buf)
            if m:
                out, self.buf = self.buf[: m.end()], self.buf[m.end():]
                return out
            if time.time() > deadline:
                raise TestFailure("expected /%s/ %s\n--- got ---\n%s" %
                                  (regex, why, self.buf[-600:]))
            self._read()

    def collect(self, seconds):
        """Everything printed during the next few seconds."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self._read()
        out, self.buf = self.buf, ""
        return out

    def expect_none(self, regex, seconds, why=""):
        out = self.collect(seconds)
        if re.search(regex, out):
            raise TestFailure("did not expect /%s/ %s\n--- got ---\n%s" %
                              (regex, why, out[-600:]))
        return out

    def quit(self):
        try:
            self.c.send("\x01x") # QEMU: Ctrl-A x
            self.c.expect(pexpect.EOF, timeout=5)
        except Exception:
            self.c.terminate(force=True)


# ------------------------------------------------------------------
# tests: each gets a freshly booted xv6

def test_ctrl_c_kills_foreground(x):
    x.run("counter")
    x.expect(r"counter\[\d+\]: 2", why="(counter should be running)")
    x.type(CTRL_C)
    x.expect(r"\^C")
    x.expect_none(r"counter\[\d+\]: \d", 2.5, "(counter should be dead)")
    x.run("echo still-alive")
    x.expect(r"still-alive", why="(the shell must survive Ctrl-C)")


def test_ctrl_z_bg_fg(x):
    x.run("counter")
    x.expect(r"counter\[\d+\]: 2")
    x.type(CTRL_Z)
    x.expect(r"\[1\]\+ Stopped\s+counter")
    x.expect_none(r"counter\[\d+\]: \d", 2.5, "(a stopped job must not run)")
    x.run("bg")
    x.expect(r"\[1\]\+ Running\s+counter &")
    x.expect(r"counter\[\d+\]: \d", why="(job continues in the background)")
    x.run("jobs")
    x.expect(r"\[1\]\+ Running\s+counter")
    x.run("fg")
    x.expect(r"counter\[\d+\]: \d", why="(job is in the foreground again)")
    x.type(CTRL_C)
    x.expect_none(r"counter\[\d+\]: \d", 2.5, "(Ctrl-C reaches the job after fg)")
    x.run("jobs")
    x.run("echo end-of-list")
    out = x.expect(r"\nend-of-list")
    if "[1]" in out:
        raise TestFailure("job list should be empty:\n" + out)


def test_ctrl_z_cpu_bound(x):
    # spin never sleeps: it must be stopped through the timer interrupt.
    x.run("spin")
    time.sleep(1)
    x.type(CTRL_Z)
    x.expect(r"\[1\]\+ Stopped\s+spin")
    x.type(CTRL_P)
    x.expect(r"\d+ \d+ stop\s+spin", why="(procdump should show spin stopped)")
    x.run("kill %1")
    x.run("jobs")
    x.expect(r"\[1\].*Killed\s+spin")


def test_background_read_stops(x):
    x.run("cat &")
    x.expect(r"\[1\] \d+")
    time.sleep(0.5)
    x.run("jobs")
    x.expect(r"\[1\]\+ Stopped \(tty input\)\s+cat")
    x.run("fg")
    x.type("hello\r")
    x.expect(r"hello\s+hello", why="(cat should echo the line once in the fg)")
    x.type(CTRL_D)
    x.run("echo cat-done")
    x.expect(r"cat-done")


def test_pipeline_is_one_job(x):
    x.run("counter | cat")
    x.expect(r"counter\[\d+\]: 2")
    x.type(CTRL_Z)
    x.expect(r"\[1\]\+ Stopped\s+counter \| cat")
    x.type(CTRL_P)
    out = x.expect(r"\d+ (\d+) stop\s+cat")
    pg = re.search(r"\d+ (\d+) stop\s+counter", out)
    pc = re.search(r"\d+ (\d+) stop\s+cat", out)
    if not pg or not pc or pg.group(1) != pc.group(1):
        raise TestFailure("counter and cat should be stopped in one group:\n" + out)
    x.run("fg")
    x.expect(r"counter\[\d+\]: \d")
    x.type(CTRL_C)
    time.sleep(0.5)
    x.type(CTRL_P)
    x.expect_none(r"counter|cat", 1.5, "(Ctrl-C must kill the whole pipeline)")


def test_noecho_mode(x):
    x.run("ttydemo noecho")
    x.expect(r"password: ")
    x.type("secret\r")
    out = x.expect(r"you typed 6 characters")
    if "secret" in out.split("password:")[-1]:
        raise TestFailure("password was echoed:\n" + out)


def test_raw_mode(x):
    x.run("ttydemo raw")
    x.expect(r"raw mode")
    x.type("a")
    x.expect(r"key 'a' = 97", why="(raw: no need to press Enter)")
    x.type(CTRL_C)
    x.expect(r"key \^C = 3", why="(raw: Ctrl-C is just a byte)")
    x.type("q")
    x.run("echo back-to-cooked")
    x.expect(r"back-to-cooked")


def test_cooked_mode_editing(x):
    x.run("ttydemo cooked")
    x.expect(r"cooked mode")
    x.type("abx\x7fd\r") # backspace erases x
    x.expect(r"read\(\) returned 4 bytes: abd")
    x.type("one two\x17three\r") # Ctrl-W erases "two"
    x.expect(r"read\(\) returned 10 bytes: one three")
    x.type("q\r")


def test_history_and_line_editing(x):
    x.run("echo first")
    x.expect(r"first")
    x.run("echo second")
    x.expect(r"second")
    x.type(UP + UP + "\r")
    x.expect(r"\nfirst", why="(up up enter = echo first)")
    x.type("echo hllo" + LEFT * 3 + "e\r")
    x.expect(r"\nhello", why="(cursor movement + insert)")
    x.run("history")
    x.expect(r"1  echo first\s+2  echo second")


def test_second_terminal(x):
    x.type(CTRL_T)
    x.expect(r"\[tty1\]")
    x.run("echo on-tty1")
    x.expect(r"on-tty1")
    x.run("counter")
    x.expect(r"counter\[\d+\]: 1")
    x.type(CTRL_T)
    x.expect(r"\[tty0\]")
    x.expect_none(r"counter\[\d+\]", 2.5, "(tty1 output must not show on tty0)")
    x.run("echo on-tty0")
    x.expect(r"on-tty0")
    x.type(CTRL_T)
    x.expect(r"\[tty1\][\s\S]*counter\[\d+\]: \d", why="(saved tty1 output)")
    x.type(CTRL_C)
    x.run("echo tty1-ok")
    x.expect(r"tty1-ok")


def test_exit_with_stopped_job(x):
    # the stopped job is orphaned when the shell exits; the kernel must
    # not leave it stopped forever (SIGHUP + SIGCONT, like Unix).
    x.run("counter")
    x.expect(r"counter\[\d+\]: 1")
    x.type(CTRL_Z)
    x.expect(r"Stopped\s+counter")
    x.run("exit")
    x.expect(r"init: starting sh on console")
    time.sleep(0.5)
    x.type(CTRL_P)
    x.expect_none(r"counter", 1.5, "(orphaned stopped job should be gone)")


def test_ctrl_c_at_prompt(x):
    x.type("half a line")
    x.type(CTRL_C)
    x.expect(r"\^C")
    x.run("echo fresh-line")
    x.expect(r"\nfresh-line")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", action="store_true", help="show xv6 output")
    ap.add_argument("names", nargs="*", help="run tests whose name contains this")
    args = ap.parse_args()

    tests = [(n, f) for n, f in globals().items()
             if n.startswith("test_") and callable(f)]
    if args.names:
        tests = [(n, f) for n, f in tests if any(s in n for s in args.names)]

    failed = 0
    for name, f in tests:
        print("%-36s" % name, end="", flush=True)
        x = XV6(args.v)
        try:
            f(x)
            print("OK")
        except (TestFailure, pexpect.ExceptionPexpect) as e:
            failed += 1
            print("FAIL")
            print("   ", str(e).replace("\n", "\n    "))
        finally:
            x.quit()
    print("%d/%d tests passed" % (len(tests) - failed, len(tests)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
