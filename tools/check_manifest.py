#!/usr/bin/env python3
"""Check every games/<id>/ folder against its game.json.

  python check_manifest.py [--root DIR]      (default: the parent of tools\\)

Rules (each failure prints one line, exit code 1 if any):
  * game.json exists, is valid JSON, "id" equals the folder name
  * ids are unique, uid3 values are unique
  * status is one of plays / partial / menu / not_tested
  * there is a "main" version; every version's map file exists; no map*.csv in the
    folder is missing from the manifest
  * a game without baseline/expect is "pending": allowed only with status not_tested and no baseline.log (newgame.py)
  * the main version has a baseline log; its SUMMARY uid3 and counts equal the
    manifest (uid3, expect.imports/direct/shim/trap/reached)
  * imports == direct + shim + trap
Inputs : games/<id>/game.json, map*.csv, baseline*.log
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamelog  # noqa: E402

STATUS = ("plays", "partial", "menu", "not_tested")


def check_game(gdir, errs, ids, uids, pending):
    gid = os.path.basename(gdir)

    def err(msg):
        errs.append("%s: %s" % (gid, msg))

    path = os.path.join(gdir, "game.json")
    if not os.path.exists(path):
        err("game.json missing")
        return
    try:
        with open(path, "r", encoding="utf-8") as f:
            m = json.load(f)
    except ValueError as e:
        err("game.json is not valid JSON (%s)" % e)
        return
    if m.get("id") != gid:
        err("id %r does not match the folder name" % m.get("id"))
    if m.get("id") in ids:
        err("duplicate id %r (also in %s)" % (m.get("id"), ids[m.get("id")]))
    ids[m.get("id")] = gid
    uid = str(m.get("uid3", "")).lower()
    if not re.fullmatch(r"[0-9a-f]{8}", uid):
        err("uid3 %r is not 8 hex digits" % m.get("uid3"))
    elif uid in uids:
        err("duplicate uid3 %s (also in %s)" % (uid, uids[uid]))
    else:
        uids[uid] = gid
    if m.get("status") not in STATUS:
        err("status %r is not one of %s" % (m.get("status"), "/".join(STATUS)))

    versions = m.get("versions") or []
    main_v = next((v for v in versions if v.get("id") == "main"), None)
    if main_v is None:
        err('no "main" version')
        return
    listed = set()
    for v in versions:
        mp = v.get("map")
        listed.add(mp)
        if not mp or not os.path.exists(os.path.join(gdir, mp)):
            err("version %r: map file %r missing" % (v.get("id"), mp))
    for fn in os.listdir(gdir):
        if re.fullmatch(r"map(\.alt\d*)?\.csv", fn) and fn not in listed:
            err("%s is not listed in game.json" % fn)

    if "baseline" not in main_v and "expect" not in main_v:
        # a game that newgame.py just created: allowed only as "not_tested" and without a baseline.log file
        if m.get("status") != "not_tested":
            err('main version has no baseline/expect, so status must be "not_tested"')
        if os.path.exists(os.path.join(gdir, "baseline.log")):
            err("baseline.log exists but game.json does not list it (run accept.py or mkmanifest.py)")
        pending.append(gid)
        return
    bl = main_v.get("baseline")
    blp = os.path.join(gdir, bl or "")
    if not bl or not os.path.exists(blp):
        err("main version: baseline log %r missing" % bl)
        return
    try:
        info = gamelog.parse_log(blp)
    except ValueError as e:
        err(str(e))
        return
    if info["uid3"] != uid:
        err("uid3 in game.json (%s) != SUMMARY in %s (%s)" % (uid, bl, info["uid3"]))
    exp = main_v.get("expect") or {}
    for k in gamelog.KEYS:
        if exp.get(k) != info["expect"][k]:
            err("expect.%s=%r but %s says %d" % (k, exp.get(k), bl, info["expect"][k]))
    if exp.get("imports") != sum(exp.get(k, 0) for k in ("direct", "shim", "trap")):
        err("expect: imports != direct + shim + trap")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.dirname(here))
    a = ap.parse_args()
    games = os.path.join(a.root, "games")
    if not os.path.isdir(games):
        print("no games folder: %s" % games)
        return 1
    errs, ids, uids, pending = [], {}, {}, []
    dirs = sorted(d for d in os.listdir(games) if os.path.isdir(os.path.join(games, d)))
    for d in dirs:
        check_game(os.path.join(games, d), errs, ids, uids, pending)
    for e in errs:
        print("FAIL " + e)
    for g in pending:
        print("PENDING %s: no baseline yet (run accept.py after the first good run)" % g)
    print("%s: %d game(s), %d problem(s), %d pending" % ("PASS" if not errs else "FAIL", len(dirs), len(errs), len(pending)))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
