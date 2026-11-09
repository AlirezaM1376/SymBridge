#!/usr/bin/env python3
"""Reduce a full device log to the lines the offline checks need (the baseline.log of a game).

  python mkbaseline.py FULL.log [-o games/<id>/baseline.log] [--all-runs]      (default: print to stdout)

Kept: the start line, build / OS / client rect / locator / profile / config lines, game chunk + app lines, the shim
registry line, every 'link:' line, 'hits:', the total-frames line, SUMMARY, the thread and exit lines that tell how
the game ended, and CRASHWATCH. Everything else (audio, key, fs, sched, gfx details ...) is dropped.
If the log holds several runs (several 'SymBridge GUI start' lines) only the last run is kept, and a note says so;
--all-runs keeps them all (the offline checks read only one run, so that is rarely what you want).
Reads  : the full log.   Writes : the reduced log with CRLF line ends (to -o or stdout).
Exit   : 0 done, 2 cannot run (log missing or unreadable, no SUMMARY line = not a complete run, -o folder missing).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamelog  # noqa: E402

KEEP = [re.compile(p) for p in (
    r"^SymBridge GUI start",
    r"^build:",
    r"^OS version ",
    r"^client rect ",
    r"^locator:",
    r"^profile:",
    r"^config:",
    r"^game: (chunk|app=)",
    r"^shim registry:",
    r"^link:",
    r"^hits:",
    r"^gfx: total frames ",
    r"^SUMMARY ",
    r"^thread: game thread",
    r"^exit: game thread",
    r"^CRASHWATCH:",
)]


def reduce_lines(lines):
    return [l for l in lines if any(p.match(l) for p in KEEP)]


def fatal(msg):
    print("mkbaseline: " + msg, file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("-o", "--out")
    ap.add_argument("--all-runs", action="store_true", help="keep every run in the log, not only the last")
    a = ap.parse_args()
    try:
        lines = gamelog.read_lines(a.log)
    except OSError as ex:
        fatal("cannot read %s (%s)" % (a.log, ex.strerror or ex))
    runs = 1
    if not a.all_runs:
        lines, runs = gamelog.select_run(lines)
    out = reduce_lines(lines)
    if not any(l.startswith("SUMMARY ") for l in out):
        fatal("%s: no SUMMARY line; this is not a complete run log" % a.log)
    if runs > 1:
        print("mkbaseline: the log holds %d runs; kept the last one" % runs, file=sys.stderr)
    text = "".join(l + "\r\n" for l in out)
    if a.out:
        if not os.path.isdir(os.path.dirname(os.path.abspath(a.out))):
            fatal("the folder of %s does not exist" % a.out)
        gamelog.write_text(a.out, text)
        print("wrote %s (%d lines kept)" % (a.out, len(out)))
    else:
        sys.stdout.flush()
        sys.stdout.buffer.write(text.encode("latin-1"))     # bytes: Windows text mode would turn CRLF into CR CR LF
    return 0


if __name__ == "__main__":
    sys.exit(main())
