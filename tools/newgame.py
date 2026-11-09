#!/usr/bin/env python3
"""Bring a new game into the repository: from the .app to games/<id>/ and a profile stub.

  python newgame.py GAME.app --v2-libs DIR --v3-libs DIR [--id ID] [--name "Display Name"]
                    [--cxxfilt PATH | --no-cxxfilt] [--work DIR] [--root DIR] [--dry-run] [--force]

Runs triage.py (step0 -> imports_join -> v2_to_v3 -> step0 -> prediction), which prints the one-page triage report, then
  * creates games/<id>/map.csv and a game.json stub (status not_tested, no baseline yet; the game counts as "pending")
  * creates profiles/<UID3>.prf with the values triage could guess (screen size, display mode), the rest as comments
  * checks the new game with check_manifest, so a stub that would fail the checks is reported at once
Reads  : GAME.app, the v2 and v3 SDK libs, src/.   Writes: <work>/ (intermediate files, default <root>/work),
         games/<id>/, profiles/<UID3>.prf.
Safety : an existing profile is never touched. An existing games/<id> is refused; --force replaces it only while it is
         still pending (no baseline.log, no expect numbers): an accepted game is never overwritten. A UID3 that already
         belongs to another game is refused (a second version of a game is added by hand, see docs/tools/newgame.md).
         --dry-run runs triage and says what would be created, but writes nothing in games/ or profiles/.
Exit   : 0 done, 1 the new game failed its own check, 2 cannot run (bad input, id or UID3 taken, triage failed).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def profile_stub(name, uid3, guess):
    """Text of profiles/<UID3>.prf. Only keys the host's profile parser knows (checked by tests/test_newgame.py)."""
    L = ["# SymBridge profile for %s (UID3 %s). Read after default.prf; only the keys in this file override it." % (name, uid3.upper()),
         "# Check default profile (default.prf) to understand the operation of each key.",
         "# Remove the # to override default values (# before each line means it's commented)",
         "# Guessed by tools/triage.py from the code: check each value on a device, then delete this line.",
         ""]
    scr = guess.get("screen")
    if scr and 32 <= scr[0] <= 640 and 32 <= scr[1] <= 640:
        L += ["screen_width=%d" % scr[0], "screen_height=%d" % scr[1]]
    else:
        L += ["#screen_width=176", "#screen_height=208      (triage found no screen size: the game may ask the device)"]
    mode = guess.get("display_mode")
    if mode in (7, 10):
        L += ["display_mode=%d" % mode]
    elif mode is not None:
        L += ["#display_mode=7     (triage saw mode %d; the host supports 7 = 64K and 10 = 4K)" % mode]
    else:
        L += ["#display_mode=7"]
    L += ["",
          "#thread_stack_min=0x10000",
          "#thread_high_priority=normal     (less | normal | more)",
          "#link_report=1                   (1 = write <app>_links.txt next to the game on the first run)",
          "#keys_plain=0",
          "#auto_cmd=0",
          "# The exit trick writes into the game's own memory at a place only Captain Dynamo's layout is known to have",
          "# (docs/LAYOUT_FACTS.md, section 5). A new game starts with it OFF; delete the next line only if the log shows",
          "# the game does not stop by itself and you know its control block (stop_state_tls / _field / _value).",
          "stop_state_tls=0",
          "\n# Settings that configs.cfg also has. These values will override config file values (remove the # to override)\n",
          "#show_fps=0",
          "#max_fps=0",
          "#softkey_left_cmd=0",
          "#softkey_right_cmd=0",
          ""]
    return "\r\n".join(L)


def fatal(msg, code=2):
    print("newgame: " + msg, file=sys.stderr)
    sys.exit(code)


def other_game_with_uid3(root, uid3, skip_id):
    """id of another game whose game.json has this uid3, or None. Unreadable manifests are ignored here (check_manifest reports them)."""
    gdir = os.path.join(root, "games")
    if not os.path.isdir(gdir):
        return None
    for d in sorted(os.listdir(gdir)):
        if d == skip_id:
            continue
        try:
            with open(os.path.join(gdir, d, "game.json"), "r", encoding="utf-8") as f:
                if str(json.load(f).get("uid3", "")).lower() == uid3:
                    return d
        except (OSError, ValueError, AttributeError):
            pass
    return None


def is_pending(gdir):
    """True when games/<id> holds no accepted run: no baseline.log and no baseline/expect in game.json."""
    if os.path.exists(os.path.join(gdir, "baseline.log")):
        return False
    try:
        with open(os.path.join(gdir, "game.json"), "r", encoding="utf-8") as f:
            js = json.load(f)
        return not any(v.get("baseline") or v.get("expect") for v in js.get("versions", []))
    except (OSError, ValueError, AttributeError):
        return True      # no readable manifest: nothing worth protecting


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app")
    ap.add_argument("--v2-libs", required=True)
    ap.add_argument("--v3-libs", required=True)
    ap.add_argument("--id", help="folder name under games/ (default: the file name in lower case, letters and digits only)")
    ap.add_argument("--name", help="display name (default: the file name)")
    ap.add_argument("--cxxfilt", default="c++filt")
    ap.add_argument("--no-cxxfilt", action="store_true")
    ap.add_argument("--root", default=os.path.dirname(HERE))
    ap.add_argument("--work", help="folder for triage's intermediate files (default <root>/work)")
    ap.add_argument("--dry-run", action="store_true", help="run triage, create nothing in games/ or profiles/")
    ap.add_argument("--force", action="store_true", help="replace games/<id> if it exists and is still pending")
    a = ap.parse_args()

    if not os.path.isfile(a.app):
        fatal("%s is not a file" % a.app)
    for opt, d in (("--v2-libs", a.v2_libs), ("--v3-libs", a.v3_libs)):
        if not os.path.isdir(d):
            fatal("%s %s is not a folder" % (opt, d))
    if not os.path.isdir(os.path.join(a.root, "src")):
        fatal("%s has no src folder (use --root)" % a.root)

    base = os.path.splitext(os.path.basename(a.app))[0]
    gid = a.id or re.sub(r"[^a-z0-9]", "", base.lower())
    if not re.fullmatch(r"[a-z0-9]+", gid):
        fatal("cannot make an id from %r: give --id (lowercase letters and digits only)" % base)
    name = a.name or base
    gdir = os.path.join(a.root, "games", gid)
    replacing = os.path.exists(gdir)
    if replacing:
        if not a.force:
            fatal("games/%s exists already (use --force while it is pending, or another --id)" % gid)
        if not is_pending(gdir):
            fatal("games/%s has an accepted run (baseline); --force does not replace it. Use another --id or remove it by hand" % gid)
    work = a.work or os.path.join(a.root, "work")

    cmd = [os.path.join(HERE, "triage.py"), a.app, "--v2-libs", a.v2_libs, "--v3-libs", a.v3_libs,
           "--src", os.path.join(a.root, "src"), "--out", work, "--root", a.root]
    cmd += ["--no-cxxfilt"] if a.no_cxxfilt else ["--cxxfilt", a.cxxfilt]
    r = subprocess.run([sys.executable, "-I"] + cmd, stdout=subprocess.PIPE, universal_newlines=True)
    sys.stdout.write(r.stdout)       # triage's own messages (errors) already went to stderr
    if r.returncode:
        fatal("triage failed (exit %d); nothing was created in games/" % r.returncode, r.returncode)
    verdict = next((l.strip() for l in r.stdout.splitlines() if l.strip().startswith("VERDICT")), None)

    wdir = os.path.join(work, base)
    try:
        with open(os.path.join(wdir, base + ".guess.json"), "r", encoding="utf-8") as f:
            guess = json.load(f)
        map_src = os.path.join(wdir, base + ".map.csv")
        if not os.path.exists(map_src):
            raise OSError("missing " + map_src)
        uid3 = str(guess["uid3"]).lower()
        if not re.fullmatch(r"[0-9a-f]{8}", uid3):
            raise ValueError("bad uid3 %r" % uid3)
    except (OSError, ValueError, KeyError) as ex:
        fatal("triage output not found or unusable (%s)" % ex)
    other = other_game_with_uid3(a.root, uid3, gid)
    if other:
        fatal("UID3 %s already belongs to games/%s. A second build of the same game is an extra version of that game "
              "(map.alt.csv + a versions entry), not a new game. See docs/tools/newgame.md" % (uid3.upper(), other))

    pdir = os.path.join(a.root, "profiles")
    prf = os.path.join(pdir, uid3.upper() + ".prf")
    made_prf = not os.path.exists(prf)
    print("")
    print("=" * 70)
    if a.dry_run:
        print("DRY RUN: would create games/%s/map.csv and game.json%s" % (gid, " (replacing the pending game)" if replacing else ""))
        print(("would create profiles/%s.prf" % uid3.upper()) if made_prf else "profiles/%s.prf exists, would not be touched" % uid3.upper())
        if verdict:
            print(verdict)
        return 0

    manifest = {"id": gid, "name": name, "uid3": uid3, "app_file": os.path.basename(a.app), "status": "not_tested",
                "profile": uid3.upper() + ".prf", "notes": "",
                "versions": [{"id": "main", "map": "map.csv", "notes": ""}]}
    try:
        os.makedirs(gdir, exist_ok=True)
        shutil.copyfile(map_src, os.path.join(gdir, "map.csv"))
        with open(os.path.join(gdir, "game.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        if made_prf:
            os.makedirs(pdir, exist_ok=True)
            with open(prf, "w", encoding="latin-1", newline="") as f:
                f.write(profile_stub(name, uid3, guess))
    except OSError as ex:
        if not replacing:
            shutil.rmtree(gdir, ignore_errors=True)
        fatal("cannot write the game folder (%s)" % ex)

    # the stub must pass the repository's own manifest check
    sys.path.insert(0, HERE)      # not on sys.path when run with -I
    import check_manifest
    errs = []
    check_manifest.check_game(gdir, errs, {}, {}, [])
    if errs:
        print("newgame: the new game does not pass check_manifest:", file=sys.stderr)
        for e in errs:
            print("  " + e, file=sys.stderr)
        return 1

    print("created games/%s/map.csv and game.json (status not_tested, pending: no baseline yet)" % gid)
    print(("created profiles/%s.prf" % uid3.upper()) if made_prf else "profiles/%s.prf exists, not touched" % uid3.upper())
    print("triage page: %s" % os.path.join(work, base + ".triage.md"))
    if verdict:
        print(verdict)
    print("")
    print("NEXT STEPS")
    print("  1. python tools/gen_importmap.py          (merges games/%s/map.csv into src/importmap_data.cpp)" % gid)
    print("  2. rebuild the host and the SIS (profiles\\ is packaged to c:\\system\\apps\\symbridge\\profiles\\)")
    print("  3. copy the game to E:\\symbridge\\<folder>\\ on the phone and run it (profile link_report=1 for the first run)")
    print("  4. after a good run:  python tools/accept.py %s PATH_TO_DEVICE.log --status plays" % gid)
    print("     (writes baseline.log and the expected numbers; mkmanifest.py is called by it)")
    print("  5. python tools/coverage_all.py   then   python tools/check_all.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
