#!/usr/bin/env python3
"""One offline gate for the whole project. No Symbian SDK, no device, no prompts.

  python check_all.py                      all steps, all games
  python check_all.py --game rodland       only that game in the coverage step
  python check_all.py --only manifest,coverage
  python check_all.py --verbose            also print the output of passing steps
  python check_all.py --cxx PATH           host C++ compiler for the hosttest step (default: g++/c++/clang++ on PATH)

Steps (in this order)
  manifest   check_manifest.py: games/<id>/game.json agrees with the folder and baseline.log
  importmap  gen_importmap.py --check: src/importmap_data.cpp is what games/*/map*.csv + importmap_curated.csv generate
  direct     check_direct.py: every direct bind in src/directbind.cpp is consistent with the import map
  tests      tools/tests/test_*.py (unittest): the checks themselves behave (e.g. a commented-out direct-bind row is ignored)
  hosttest   compiles tools/test_importmap_host.cpp with a host g++ and runs it (SKIP when no host g++ is found;
             the Symbian GCCE g++ is never used)
  report     coverage_all.py --check: docs/COVERAGE.md is what the current games/ and src/ generate
  coverage   import_coverage.py once per game: predicted shim/direct/trap equals the numbers in the manifest
             (the main version of each game, expect block)

Inputs : <root>/games, <root>/src, <root>/tools   (--root, default: the parent of tools/)
Output : a table on stdout, then the output of every failed or skipped step. Exit code 0 = everything passed (SKIP is
         not a failure), 1 = something failed, 2 = bad usage.
Not part of it: check_log.py (needs a device log) and the tools that write files (gen_importmap.py, coverage_all.py,
accept.py, newgame.py).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = ("manifest", "importmap", "direct", "tests", "hosttest", "coverage", "report")


def run(cmd, cwd=None):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       universal_newlines=True, errors="replace")
    return p.returncode, p.stdout


def py(script, *args):
    return [sys.executable, os.path.join(HERE, script)] + list(args)


def find_host_cxx(explicit):
    if explicit:
        return explicit
    cands = []
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for n in ("g++", "g++.exe", "c++", "clang++"):
            f = os.path.join(d, n)
            if os.path.isfile(f) and f not in cands:
                cands.append(f)
    for f in cands:
        if "symbian" not in f.lower():
            return f
    return None


def step_hosttest(root, cxx):
    cxx = find_host_cxx(cxx)
    if not cxx:
        return "SKIP", "no host g++/clang++ on PATH (use --cxx)"
    tools, src = os.path.join(root, "tools"), os.path.join(root, "src")
    tmp = tempfile.mkdtemp(prefix="symbridge_")
    try:
        exe = os.path.join(tmp, "t_importmap" + (".exe" if os.name == "nt" else ""))
        rc, out = run([cxx, "-Wall", "-I", os.path.join(tools, "hostshim"), "-I", src,
                       os.path.join(tools, "test_importmap_host.cpp"), os.path.join(src, "importmap.cpp"),
                       os.path.join(src, "importmap_data.cpp"), "-o", exe])
        if rc:
            return "FAIL", "compile failed:\n" + out
        rc, out2 = run([exe])
        return ("PASS" if rc == 0 else "FAIL"), out + out2
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def coverage_for(root, gdir):
    gid = os.path.basename(gdir)
    try:
        with open(os.path.join(gdir, "game.json"), "r", encoding="utf-8") as f:
            m = json.load(f)
        v = next(v for v in m["versions"] if v["id"] == "main")
        if "expect" not in v:
            return "SKIP", "pending: no baseline yet (accept.py after the first good run)"
        e = v["expect"]
        expect = "shim=%d,direct=%d,trap=%d" % (e["shim"], e["direct"], e["trap"])
        csv_path = os.path.join(gdir, v["map"])
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as ex:
        return "FAIL", "cannot read the main version of %s/game.json: %r" % (gid, ex)
    rc, out = run(py("import_coverage.py", "--src", os.path.join(root, "src"), "--csv", csv_path, "--expect", expect))
    head = "expect %s" % expect
    if rc:
        keep = [l for l in out.splitlines() if "MISMATCH" in l.upper() or "mismatch" in l or "cannot" in l]
        return "FAIL", head + "\n" + "\n".join(keep or out.splitlines()[:12])
    return "PASS", head


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.dirname(HERE))
    ap.add_argument("--game", help="coverage step: only this game id")
    ap.add_argument("--only", help="comma list of steps: " + ",".join(STEPS))
    ap.add_argument("--cxx", help="host C++ compiler for the hosttest step")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    steps = STEPS
    if a.only:
        steps = tuple(s.strip() for s in a.only.split(",") if s.strip())
        bad = [s for s in steps if s not in STEPS]
        if not steps:
            bad = ["(none given)"]
        if bad:
            print("unknown step(s): %s (known: %s)" % (", ".join(bad), ", ".join(STEPS)), file=sys.stderr)
            return 2

    if a.game and "coverage" not in steps:
        print("note: --game only applies to the coverage step, which is not selected", file=sys.stderr)
    results = []   # (name, result, detail)
    for s in steps:
        if s == "manifest":
            rc, out = run(py("check_manifest.py", "--root", root))
            results.append(("manifest", "PASS" if rc == 0 else "FAIL", out))
        elif s == "importmap":
            rc, out = run(py("gen_importmap.py", "--root", root, "--check"))
            results.append(("importmap up to date", "PASS" if rc == 0 else "FAIL", out))
        elif s == "direct":
            rc, out = run(py("check_direct.py", "--root", root))
            results.append(("direct binds", "PASS" if rc == 0 else "FAIL", out))
        elif s == "tests":
            rc, out = run([sys.executable, "-m", "unittest", "discover", "-s", os.path.join(root, "tools", "tests"), "-t",
                           os.path.join(root, "tools", "tests")])
            results.append(("unit tests", "PASS" if rc == 0 else "FAIL", out))
        elif s == "hosttest":
            r, d = step_hosttest(root, a.cxx)
            results.append(("host lookup test", r, d))
        elif s == "report":
            rc, out = run(py("coverage_all.py", "--root", root, "--check"))
            results.append(("COVERAGE.md up to date", "PASS" if rc == 0 else "FAIL", out))
        elif s == "coverage":
            games = os.path.join(root, "games")
            ids = sorted(d for d in os.listdir(games) if os.path.isdir(os.path.join(games, d))) if os.path.isdir(games) else []
            if a.game:
                if a.game not in ids:
                    print("no such game: %s (have: %s)" % (a.game, ", ".join(ids)), file=sys.stderr)
                    return 2
                ids = [a.game]
            if not ids:
                results.append(("coverage", "FAIL", "no games found in %s" % games))
            for gid in ids:
                r, d = coverage_for(root, os.path.join(games, gid))
                results.append(("coverage " + gid, r, d))

    width = max(len(n) for n, _, _ in results)
    print("%-*s  %s" % (width, "check", "result"))
    for n, r, d in results:
        print("%-*s  %s" % (width, n, r))
    failed = [x for x in results if x[1] == "FAIL"]
    for n, r, d in results:
        if r == "FAIL" or (a.verbose and d.strip()) or r == "SKIP":
            print("\n--- %s: %s ---\n%s" % (n, r, d.strip()))
    print("\n%s: %d check(s), %d failed, %d skipped" % (
        "FAILED" if failed else "PASSED", len(results), len(failed), sum(1 for x in results if x[1] == "SKIP")))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
