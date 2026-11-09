#!/usr/bin/env python3
"""Write or refresh games/<id>/game.json from the game's baseline.log.

  python mkmanifest.py games/captaindynamo/baseline.log [--name "Captain Dynamo"] [--status plays] [--force]

The log gives: uid3, the counts (imports/direct/shim/trap/reached) and the .app
file name. Existing hand-written fields (name, status, notes, profile, code_size,
version notes) are NEVER overwritten; only uid3, app_file and the main version's
expect block are refreshed. A new manifest also lists every map.alt*.csv found
next to the log as an "alt" version.

When game.json already has a uid3 that differs from the log's, nothing is written (the log is probably another game's):
--force overwrites it. game.json is written through a temp file, so a failure leaves the old one intact.

Inputs : <game dir>/baseline.log, optional <game dir>/game.json, map.alt*.csv
Output : <game dir>/game.json
Exit   : 0 done, 1 uid3 differs (without --force), 2 cannot run (log or game.json unreadable, log not usable).
"""
import argparse
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamelog  # noqa: E402


def alt_maps(gdir):
    names = [n for n in os.listdir(gdir) if re.fullmatch(r"map\.alt\d*\.csv", n)]
    return sorted(names)


def alt_id(fname):
    return re.fullmatch(r"map\.(alt\d*)\.csv", fname).group(1)


class UidMismatch(ValueError):
    pass


def update_manifest(gdir, log, name=None, status=None, force=False):
    """Create or refresh gdir/game.json from the log; returns True when the file was new.

    Raises ValueError (log or game.json unusable) and UidMismatch (the log's uid3 is not the one in game.json, no force);
    in both cases game.json is left as it was."""
    gid = os.path.basename(gdir)
    info = gamelog.parse_log(log)
    path = os.path.join(gdir, "game.json")

    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            m = json.load(f)
        if not isinstance(m, dict):
            raise ValueError("%s is not a JSON object" % path)
        old = str(m.get("uid3", "")).lower()
        if old and old != info["uid3"] and not force:
            raise UidMismatch("game.json says uid3 %s but the log is from %s" % (old, info["uid3"]))
        created = False
    else:
        m = {"id": gid, "name": name or gid, "status": "not_tested", "notes": ""}
        created = True

    m["uid3"] = info["uid3"]
    if info["app_file"]:
        m["app_file"] = info["app_file"]
    if created:
        m["profile"] = (info["uid3"].upper() + ".prf") if info["game_prf"] else "none"

    versions = m.setdefault("versions", [])
    main_v = next((v for v in versions if v.get("id") == "main"), None)
    if main_v is None:
        main_v = {"id": "main", "map": "map.csv", "baseline": "baseline.log", "notes": ""}
        versions.insert(0, main_v)
    main_v.setdefault("baseline", "baseline.log")
    main_v["expect"] = info["expect"]
    if status:
        m["status"] = status
    have = {v.get("map") for v in versions}
    for fn in alt_maps(gdir):
        if fn not in have:
            versions.append({"id": alt_id(fn), "map": fn, "notes": ""})

    order = ["id", "name", "uid3", "code_size", "app_file", "status", "profile", "notes", "versions"]
    out = {k: m[k] for k in order if k in m}
    out.update({k: v for k, v in m.items() if k not in out})
    text = json.dumps(out, indent=2, ensure_ascii=False) + "\n"
    fd, tmp = tempfile.mkstemp(prefix=".tmp_", dir=gdir)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return created


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="path to games/<id>/baseline.log")
    ap.add_argument("--name", help="display name (only used when game.json is new)")
    ap.add_argument("--status", choices=("plays", "partial", "menu", "not_tested"), help="also set the status")
    ap.add_argument("--force", action="store_true", help="overwrite a different uid3")
    a = ap.parse_args()
    if not os.path.isfile(a.log):
        print("mkmanifest: %s is not a file" % a.log, file=sys.stderr)
        return 2
    gdir = os.path.dirname(os.path.abspath(a.log))
    try:
        created = update_manifest(gdir, a.log, a.name, a.status, a.force)
    except UidMismatch as ex:
        print("mkmanifest: %s; nothing written (--force overwrites)" % ex, file=sys.stderr)
        return 1
    except (ValueError, OSError) as ex:
        print("mkmanifest: %s" % ex, file=sys.stderr)
        return 2
    print("%s: %s %s" % (os.path.basename(gdir), "created" if created else "updated", os.path.join(gdir, "game.json")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
