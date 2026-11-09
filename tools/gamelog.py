"""Shared helpers: read the facts the tools need from a (baseline) device log.

Used by mkbaseline.py, accept.py, mkmanifest.py and check_manifest.py. No third-party packages.
"""
import os
import re
import tempfile

KEYS = ("imports", "direct", "shim", "trap", "reached")


def read_lines(path):
    with open(path, "r", encoding="latin-1", newline="") as f:
        return f.read().splitlines()


def select_run(lines):
    """(lines of the last run, number of runs). A run starts at a 'SymBridge GUI start' line; a log without one is one run."""
    starts = [i for i, l in enumerate(lines) if l.startswith("SymBridge GUI start")]
    if len(starts) <= 1:
        return lines, max(len(starts), 1)
    return lines[starts[-1]:], len(starts)


def write_text(path, text, encoding="latin-1"):
    """Write text (latin-1 unless told otherwise, no line-end conversion) through a temp file, so a failure never leaves a half-written file."""
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".tmp_", dir=d)
    try:
        with os.fdopen(fd, "w", encoding=encoding, newline="") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def parse_log(path):
    """parse_lines() of the file at path."""
    return parse_lines(read_lines(path), path)


def parse_lines(lines, name="log"):
    """Return a dict: uid3, expect{imports,direct,shim,trap,reached}, app_file, game_prf, exit.

    Raises ValueError with a readable message when the log has no SUMMARY line or
    when SUMMARY and the 'link:' line disagree. `name` only appears in the messages.
    """
    path = name
    summary = None
    link = None
    app_file = None
    profile = None
    for ln in lines:
        if ln.startswith("SUMMARY "):
            summary = dict(re.findall(r"(\w+)=(\S+)", ln))
        elif ln.startswith("link: direct="):
            m = re.match(r"link: direct=(\d+) shim=(\d+) trap=(\d+) \(total (\d+)\)", ln)
            if m:
                link = tuple(int(x) for x in m.groups())
        elif ln.startswith("locator:") and app_file is None:
            m = re.search(r"chosen '([^']*)'", ln)
            if m:
                app_file = m.group(1).replace("/", "\\").split("\\")[-1]
        elif ln.startswith("profile:") and "applied" in ln and ".prf" in ln:
            profile = ln
    if summary is None:
        raise ValueError("%s: no SUMMARY line" % path)
    try:
        expect = {k: int(summary[k]) for k in KEYS}
        uid3 = summary["uid3"].lower()
    except KeyError as e:
        raise ValueError("%s: SUMMARY has no %s" % (path, e))
    if link is not None:
        d, s, t, total = link
        if (d, s, t) != (expect["direct"], expect["shim"], expect["trap"]) or total != expect["imports"]:
            raise ValueError("%s: SUMMARY and 'link:' line disagree" % path)
    # profile: "profile: default.prf -> 0, 0548E337.prf -> 0 (applied)": 0 = file was read
    has_game_prf = False
    if profile:
        m = re.search(r"%s\.prf -> (-?\d+)" % re.escape(uid3), profile, re.I)
        has_game_prf = bool(m and m.group(1) == "0")
    return {"uid3": uid3, "expect": expect, "app_file": app_file, "game_prf": has_game_prf, "exit": summary.get("exit")}
