#!/usr/bin/env python3
"""Check a host log from a device run against the matching baseline log.

Usage:
    python tools/check_log.py LOG games/<id>/baseline.log
    python tools/check_log.py LOG --game <id>            (the same, baseline taken from games/<id>/baseline.log)

Reads  : the new device log (the FULL log the host wrote) and the game's baseline.log (nothing else).
Writes : nothing; each check is printed as PASS/FAIL with a reason when it fails.
Checks : a) link counts equal the baseline        b) no "STOPPED at import"
         c) SUMMARY exit=normal and frames > 0    d) no unresolved import reached that the baseline did not reach
         e) the game thread ended by itself       f) none of the known failure lines (a leave in the focus handlers,
            an observer without a vtable, "REAL STREAM -> silent fake") - ordinary "foreground:" lines are fine
         g) every audio facade that was created was freed   h) frame rate within +/-30% of the baseline
         i) the host's "GUI exit" line is there and CRASHWATCH says type=0   j) the log is from the same game (uid3)
Note   : the baseline is a REDUCED log (see mkbaseline.py): it has no "GUI exit" line, so checking a baseline against
         itself fails check i; check a real device log.
Exit   : 0 all checks passed, 1 at least one check failed, 2 cannot run (bad usage, a file is missing).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def read_lines(path: Path):
    if not path.exists():
        raise FileNotFoundError(f"missing file: {path}")
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def parse_link_counts(lines):
    for line in lines:
        m = re.search(r"link:\s*direct=(\d+)\s+shim=(\d+)\s+trap=(\d+)", line)
        if m:
            return tuple(int(v) for v in m.groups())
    return None


def parse_summary(lines):
    for line in lines:
        if "SUMMARY" not in line:
            continue
        m = re.search(
            r"SUMMARY .*?imports=(\d+).*?direct=(\d+).*?shim=(\d+).*?trap=(\d+).*?reached=(\d+).*?frames=(\d+).*?exit=([A-Za-z]+).*?secs=(\d+)",
            line,
            flags=re.IGNORECASE,
        )
        if m:
            return {
                "imports": int(m.group(1)),
                "direct": int(m.group(2)),
                "shim": int(m.group(3)),
                "trap": int(m.group(4)),
                "reached": int(m.group(5)),
                "frames": int(m.group(6)),
                "exit": m.group(7).lower(),
                "secs": int(m.group(8)),
            }
    return None


def parse_hit_indexes(lines):
    hits = set()
    for line in lines:
        for match in re.finditer(r"hits\s*#\s*(\d+)", line, flags=re.IGNORECASE):
            hits.add(int(match.group(1)))
    return hits


def parse_total_frames_rate(lines):
    for line in lines:
        m = re.search(r"gfx:\s*total\s+frames\s+(\d+)\s+in\s+(\d+)\s*ms", line, flags=re.IGNORECASE)
        if m:
            frames = int(m.group(1))
            ms = int(m.group(2))
            if ms <= 0:
                return None
            return frames / (ms / 1000.0)
    return None


def parse_crashwatch_type(lines):
    for line in lines:
        m = re.search(r"CRASHWATCH:.*?type=(\d+)", line, flags=re.IGNORECASE)
        if m:
            return int(m.group(1))
    return None


def has_pattern(lines, needle):
    needle_l = needle.lower()
    for line in lines:
        if needle_l in line.lower():
            return True
    return False


def check_a(log_lines, base_lines):
    a = parse_link_counts(log_lines)
    b = parse_link_counts(base_lines)
    if a is None:
        return False, "no link: direct=... shim=... trap=... line found in log"
    if b is None:
        return False, "no link: direct=... shim=... trap=... line found in baseline"
    if a != b:
        return False, f"link counts differ: log={a} baseline={b}"
    return True, "link counts match baseline"


def check_b(log_lines):
    offenders = [line for line in log_lines if "STOPPED at import" in line]
    if offenders:
        return False, f"found STOPPED at import: {offenders[0]}"
    return True, "no STOPPED at import lines"


def check_c(log_lines):
    summary = parse_summary(log_lines)
    if summary is None:
        return False, "no SUMMARY line found"
    if summary["exit"] != "normal":
        return False, f"SUMMARY exit is {summary['exit']!r}, expected 'normal'"
    if summary["frames"] <= 0:
        return False, f"frames={summary['frames']} <= 0"
    return True, f"SUMMARY exit=normal, frames={summary['frames']}"


def check_d(log_lines, base_lines):
    new_hits = parse_hit_indexes(log_lines)
    base_hits = parse_hit_indexes(base_lines)
    extra = sorted(new_hits - base_hits)
    if extra:
        return False, f"new log reaches more unresolved imports than baseline: {extra}"
    return True, f"hit-index set is not larger than baseline ({sorted(new_hits)})"


def check_e(log_lines):
    ended = any("ended by themselves" in line.lower() for line in log_lines)
    killed = [line for line in log_lines if "exit: game thread 0" in line.lower()]
    if not ended:
        return False, "no 'ended by themselves' line found"
    if killed:
        return False, f"thread was killed: {killed[0]}"
    return True, "thread ended by itself and no killed line present"


# Lines the host writes only when something went wrong. Plain "foreground: gain=..." lines are normal.
FORBIDDEN = (
    "foreground: old code left with",
    "foreground: old AppUi::HandleForegroundEventL",
    "has no valid vtable",
    "REAL STREAM -> silent fake",
)


def check_f(log_lines):
    bad = [needle for needle in FORBIDDEN if has_pattern(log_lines, needle)]
    if bad:
        return False, f"forbidden pattern(s) found: {', '.join(bad)}"
    return True, "no failure lines from the focus handlers or the audio stream"


def check_g(log_lines):
    active = set()
    freed_without_new = []
    for line in log_lines:
        m = re.search(r"->\s*facade\s+([0-9A-Fa-fx]+)", line)
        if m:
            active.add(m.group(1).lower())
        m = re.search(r"facade\s+([0-9A-Fa-fx]+)\s+freed", line)
        if m:
            addr = m.group(1).lower()
            if addr not in active:
                freed_without_new.append(addr)
            else:
                active.remove(addr)

    if freed_without_new:
        return False, f"facade freed without matching NewL: {sorted(freed_without_new)[:5]}"
    if active:
        return False, f"facade(s) still open at exit: {sorted(active)}"
    return True, "audio facade lifecycle is balanced"


def check_h(log_lines, base_lines):
    log_rate = parse_total_frames_rate(log_lines)
    base_rate = parse_total_frames_rate(base_lines)
    if log_rate is None:
        return False, "no 'gfx: total frames ... in ... ms' line found in log"
    if base_rate is None:
        return False, "no 'gfx: total frames ... in ... ms' line found in baseline"
    if base_rate <= 0:
        return False, "baseline frame rate is zero or invalid"
    ratio = log_rate / base_rate
    if not (0.70 <= ratio <= 1.30):
        return False, f"frame rate ratio outside +/-30%: log_rate={log_rate:.2f}, baseline={base_rate:.2f}, ratio={ratio:.2f}"
    return True, f"frame rate within +/-30% of baseline ({log_rate:.2f} vs {base_rate:.2f})"


GUI_EXIT = ("SymBridge GUI exit", "legacyhost GUI exit")      # the host was renamed; logs of older builds say legacyhost


def check_i(log_lines):
    text = "\n".join(log_lines)
    if not any(name in text for name in GUI_EXIT):
        return False, "missing 'SymBridge GUI exit' line (the host did not close normally)"
    crash_type = parse_crashwatch_type(log_lines)
    if crash_type is None:
        return False, "missing CRASHWATCH type line"
    if crash_type != 0:
        return False, f"CRASHWATCH type={crash_type}, expected 0"
    return True, "GUI exit seen and CRASHWATCH type=0"


def parse_uid3(lines):
    for line in lines:
        m = re.search(r"SUMMARY\s.*?uid3=([0-9A-Fa-f]{8})", line)
        if m:
            return m.group(1).lower()
    return None


def check_j(log_lines, base_lines):
    a, b = parse_uid3(log_lines), parse_uid3(base_lines)
    if a is None or b is None:
        return False, "no SUMMARY uid3 in the " + ("log" if a is None else "baseline")
    if a != b:
        return False, f"the log is from uid3 {a}, the baseline from {b}: wrong game?"
    return True, f"same game (uid3 {a})"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="the full device log")
    ap.add_argument("baseline", nargs="?", help="games/<id>/baseline.log")
    ap.add_argument("--game", help="game id: use <root>/games/<id>/baseline.log as the baseline")
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent), help="project root (with --game)")
    a = ap.parse_args()
    if bool(a.baseline) == bool(a.game):
        ap.error("give either BASELINE or --game ID")

    log_path = Path(a.log)
    base_path = Path(a.baseline) if a.baseline else Path(a.root) / "games" / a.game / "baseline.log"

    try:
        log_lines = read_lines(log_path)
        base_lines = read_lines(base_path)
    except OSError as exc:
        print(f"check_log: {exc}", file=sys.stderr)
        return 2

    checks = [
        ("a", "link counts match baseline", lambda: check_a(log_lines, base_lines)),
        ("b", "no STOPPED at import", lambda: check_b(log_lines)),
        ("c", "SUMMARY exit normal and frames > 0", lambda: check_c(log_lines)),
        ("d", "unresolved hits not larger than baseline", lambda: check_d(log_lines, base_lines)),
        ("e", "thread ended by itself", lambda: check_e(log_lines)),
        ("f", "no forbidden warning strings", lambda: check_f(log_lines)),
        ("g", "audio facade balance", lambda: check_g(log_lines)),
        ("h", "frame-rate within +/-30% of baseline", lambda: check_h(log_lines, base_lines)),
        ("i", "exit and crashwatch state", lambda: check_i(log_lines)),
        ("j", "same game as the baseline", lambda: check_j(log_lines, base_lines)),
    ]

    failed = 0
    for letter, label, fn in checks:
        ok, reason = fn()
        status = "PASS" if ok else "FAIL"
        print(f"{status}: {letter}) {label} - {reason}")
        if not ok:
            failed += 1

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
