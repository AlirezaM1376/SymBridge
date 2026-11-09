#!/usr/bin/env python3
"""Turn a good first run into a permanent regression test.

  python accept.py ID DEVICE.log [--status plays|partial|menu] [--update] [--force] [--dry-run]

Reads  : games/<id>/game.json, games/<id>/map.csv, the full device log, src/ (shim registry, import map).
Writes : games/<id>/baseline.log (the reduced log, see mkbaseline.py), and the expect block / uid3 / status in
         games/<id>/game.json. If the log holds several runs, the last one is used.
Refuses (unless --force) when: the log's UID3 is not the game's; the run did not end normally; the static prediction
from map.csv differs from the log's link counts (wrong map version, or the host was built before gen_importmap.py).
Refuses to replace an existing baseline.log unless --update is given.
--dry-run does all the checks and says what would be written, but writes nothing.
If a step fails after baseline.log was written, the previous baseline.log is put back (or the new one removed).
Exit   : 0 accepted (or a dry run that would accept), 1 refused by a safety check, 2 cannot run (game.json, map or log
         missing or unusable).
Then run:  python tools/coverage_all.py   and   python tools/check_all.py
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gamelog          # noqa: E402
import import_coverage as IC   # noqa: E402
import mkbaseline       # noqa: E402
import mkmanifest       # noqa: E402

STATUS = ("plays", "partial", "menu")


def fatal(msg):
    print("accept: " + msg, file=sys.stderr)
    sys.exit(2)


def predicted(root, map_path):
    registry, _ = IC.load_shim_registry(os.path.join(root, "src"))
    base = IC.load_importmap_h(os.path.join(root, "src"))
    rows = IC.load_csv(map_path)
    c = {"shim": 0, "direct": 0, "trap": 0}
    for r in rows:
        c[IC.classify(r["dll"].lower(), int(r["v2_ordinal"]), registry, base)] += 1
    return c, len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("id")
    ap.add_argument("log")
    ap.add_argument("--status", choices=STATUS, default="plays")
    ap.add_argument("--update", action="store_true", help="replace an existing baseline.log")
    ap.add_argument("--force", action="store_true", help="accept although a safety check failed")
    ap.add_argument("--dry-run", action="store_true", help="check and report, write nothing")
    ap.add_argument("--root", default=os.path.dirname(HERE))
    a = ap.parse_args()

    gdir = os.path.join(a.root, "games", a.id)
    try:
        with open(os.path.join(gdir, "game.json"), "r", encoding="utf-8") as f:
            m = json.load(f)
        if not isinstance(m, dict):
            raise ValueError("not a JSON object")
    except (OSError, ValueError) as ex:
        fatal("no usable games/%s/game.json (%s); run newgame.py first" % (a.id, ex))
    main_v = next((v for v in m.get("versions", []) if v.get("id") == "main"), None)
    if main_v is None or not main_v.get("map"):
        fatal("game.json has no main version with a map")
    map_path = os.path.join(gdir, main_v["map"])
    if not os.path.isfile(map_path):
        fatal("games/%s/%s is missing" % (a.id, main_v["map"]))
    try:
        lines = gamelog.read_lines(a.log)
    except OSError as ex:
        fatal("cannot read the log %s (%s)" % (a.log, ex.strerror or ex))
    target = os.path.join(gdir, "baseline.log")
    if os.path.exists(target) and not a.update:
        print("accept: games/%s/baseline.log exists already; use --update to replace it" % a.id, file=sys.stderr)
        return 1

    lines, runs = gamelog.select_run(lines)
    if runs > 1:
        print("note: the log holds %d runs; using the last one" % runs)
    reduced = mkbaseline.reduce_lines(lines)
    try:
        info = gamelog.parse_lines(reduced, a.log)
    except ValueError as ex:
        fatal("cannot use this log: %s" % ex)
    problems = []
    if info["uid3"] != str(m.get("uid3", "")).lower():
        problems.append("UID3 in the log is %s but game.json says %s" % (info["uid3"], m.get("uid3")))
    if info["exit"] != "normal":
        problems.append("the run did not end normally (exit=%s)" % info["exit"])
    try:
        pred, n = predicted(a.root, map_path)
    except (OSError, ValueError, KeyError) as ex:
        fatal("cannot predict from %s (%s)" % (map_path, ex))
    got = {k: info["expect"][k] for k in ("shim", "direct", "trap")}
    if pred != got or n != info["expect"]["imports"]:
        problems.append("static prediction shim/direct/trap=%d/%d/%d (%d imports) differs from the log %d/%d/%d (%d): "
                        "is map.csv the version that ran, and was gen_importmap.py run before the build?" % (
                            pred["shim"], pred["direct"], pred["trap"], n,
                            got["shim"], got["direct"], got["trap"], info["expect"]["imports"]))
    if info["expect"]["reached"]:
        print("note: %d unresolved import(s) were reached in this run (see the hits: lines); accepted anyway, "
              "COVERAGE.md will list it as a gap." % info["expect"]["reached"])
    app = m.get("app_file")
    if app and info["app_file"] and app.lower() != info["app_file"].lower():
        print("note: the log ran %s, game.json lists %s; game.json will be updated" % (info["app_file"], app))
    prf = m.get("profile")
    if prf and prf != "none" and not os.path.exists(os.path.join(a.root, "profiles", prf)):
        print("note: profiles/%s does not exist" % prf)
    if problems and not a.force:
        for p in problems:
            print("REFUSED: " + p, file=sys.stderr)
        print("(--force accepts anyway)", file=sys.stderr)
        return 1
    if a.dry_run:
        print("dry run: %s would be accepted (%d lines kept, status %s); nothing written" % (a.id, len(reduced), a.status))
        return 0

    old = None
    if os.path.exists(target):
        with open(target, "rb") as f:
            old = f.read()
    try:
        gamelog.write_text(target, "".join(l + "\r\n" for l in reduced))
        mkmanifest.update_manifest(gdir, target, status=a.status, force=a.force)
    except (OSError, ValueError) as ex:
        if old is None:
            if os.path.exists(target):
                os.unlink(target)
        else:
            with open(target, "wb") as f:
                f.write(old)
        fatal("could not write the game (%s); the previous files were put back" % ex)
    print("accepted %s: baseline.log written (%d lines), expect set, status %s" % (a.id, len(reduced), a.status))
    print("next:  python tools/coverage_all.py   then   python tools/check_all.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
