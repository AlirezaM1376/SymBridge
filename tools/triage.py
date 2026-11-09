#!/usr/bin/env python3
"""
triage.py - one-page triage report for a candidate pre-Symbian-9 game: will the host run it, and what would block it?

  python triage.py GAME.app --v2-libs S60v2_libs --v3-libs S60v3_libs [--src DIR] [--out DIR] [--strict]
                   [--check-game ID | --expect shim=N,direct=N,trap=N] [--baseline-app KNOWN.app] [--sizeof v2_sizeof.csv]

Runs the whole static pipeline for one binary and writes ONE page, <out>/<GAME>.triage.md and .txt (also printed):
    step0.py --dump-imports      -> import skeleton
    imports_join.py              -> v2 names
    v2_to_v3.py                  -> v2 -> v3 map
    step0.py --map --json        -> thunks, vtables, callers, reachability
    registry (import_coverage)   -> predicted shim / direct / trap, exactly as TIatLinker would decide
Nothing is written into the host sources. Intermediate files stay in <out>/<GAME>/ so they can be diffed: the map
(<GAME>.map.csv), the step-0 analysis (.s0.json), the per-import table (.imports.csv) and <GAME>.guess.json (uid3, the most
likely screen size and display mode, read by newgame.py).

Reads  : GAME.app, the S60v2 and S60v3 SDK libs, src/ (shim registry and import map), c++filt, optionally --sizeof.
Writes : <out>/ (default <root>/work). Never touches src/ or games/.
Exit   : 0 = report written; 1 = --strict and the verdict is not READY (see the first section of the report);
         2 = a pipeline step failed or an input is unusable (reason on stderr / last lines of the failing step).

Prediction rule (same as import_coverage.classify): registry says shim/runtime -> shim; registry says DIRECT and
the map has an exact v3 match -> direct; everything else -> trap. For imports the merged importmap_data.cpp does
not know yet, the game's own map row is used as if it had been merged (gen_importmap.py would do that).

The first section ends with a VERDICT line: BLOCKED (the host cannot link or decode the game), NEEDS WORK (unresolved
imports are called or reachable from start-up) or READY TO TRY (nothing found that stops a first run; static analysis only).
Optional comparison with known numbers: --expect, or --check-game ID (the expect block of games/ID/game.json, main version).
Details and a guide to every section: docs/tools/triage.md
"""
import argparse
import collections
import csv
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import import_coverage as IC          # registry / map readers (no side effects on import)

FRAMEWORK_DLLS = {"cone", "eikcore", "avkon", "apparc", "eikcoctl", "apgrfx"}
EXPENSIVE_BASES = re.compile(r"^(CActive|CTimer|CPeriodic|CIdle|CAknView|\w*Dialog|\w*ListBox|\w*Form|\w*Edwin"
                             r"|CAknControlContainer|CCoeControl|CEikBorderedControl)$")
DISPLAY_MODES = {0: "ENone", 1: "EGray2", 2: "EGray4", 3: "EGray16", 4: "EGray256", 5: "EColor16", 6: "EColor256",
                 7: "EColor64K", 8: "EColor16M", 9: "ERgb", 10: "EColor4K"}      # check against the game's SDK
RESOLUTIONS = {(128, 160), (176, 208), (176, 220), (208, 176), (240, 320), (320, 240), (352, 416), (416, 352),
               (240, 400), (360, 640), (640, 360), (320, 480), (480, 320), (160, 128), (160, 160)}

RE_FLOAT_HELPER = re.compile(r"^__\w*(?:df|sf)\w*$")
RE_FLOAT_FUNC = re.compile(r"^(?:sqrt|sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|log|log10|pow|floor|ceil|"
                           r"fabs|fmod|ldexp|frexp|modf|atof|strtod|cbrt|hypot|fabsf|sqrtf|sinf|cosf|powf)$")
RE_FLOAT_SIG = re.compile(r"\b(?:double|float|TReal|TReal32|TReal64)\b|\bTInt64\b(?!\s*&)|\blong long\b(?!\s*&)")
RE_VARIADIC = re.compile(r"^(?:printf|sprintf|snprintf|fprintf|vsprintf|vprintf|scanf|sscanf|fscanf)$|::Format\(|\.\.\.\)")
RE_THREAD = re.compile(r"^(?:RThread|RSemaphore|RMutex|RCriticalSection|RProcess|RTimer|RChunk|RCondVar|CPeriodic|CTimer)::")
RE_SCREEN = re.compile(r"DirectScreenAccess|CFbsBitmap::(?:DataAddress|LockHeap|UnlockHeap|ScanLineLength|"
                       r"SetScanLine|DataStride)|TScreenInfo|ScreenInfo|CWsScreenDevice::.*Screen|DisplayMemory")
RE_NEW = re.compile(r"(?:^|::)operator new\(|^CBase::newL\(")
RE_CTOR = re.compile(r"^(\w+)::\1\(")
RE_DTOR = re.compile(r"::~")
RE_LEAVE = re.compile(r"^(?:CleanupStack::|User::Leave|User::LeaveIf|TTrap::|User::Panic|CBase::newL|operator new)")
RE_ASYNC = re.compile(r"^(?:CPeriodic|CTimer|CActive|CIdle|RTimer|CActiveScheduler|User::WaitForRequest|"
                      r"User::WaitForAnyRequest|User::After)\b")
RE_ASYNC2 = re.compile(r"^(?:CPeriodic|CTimer|CActive|CIdle|RTimer|CActiveScheduler|User::WaitFor)")
RE_CALLBACK = re.compile(r"\(\*\)|TCallBack|\bqsort\b|\bbsearch\b|\batexit\b|\bsignal\b|\bsetjmp\b|\blongjmp\b")
SUBSYSTEM_DLLS = {"cone", "eikcore", "avkon", "apparc", "eikcoctl", "apgrfx", "bitgdi", "fbscli", "ws32", "gdi", "efsrv",
                  "mediaclientaudio", "mediaclientaudiostream", "plpvariant", "hal", "etel", "sysagt", "estor", "ezlib"}
GROUPS = [("A", "DIRECT candidates (plain function, rules R1-R5 do not object; add to directbind.cpp after review)"),
          ("B", "needs a shim in an existing subsystem table"),
          ("C", "leave / cleanup stack / timers / active objects (old trap runtime; never direct)"),
          ("D", "compiler helpers (integer and float)"),
          ("E", "variadic: read the format strings first (R5), then direct or shim"),
          ("F", "map row not exact (ambiguous / name_only / ctor / dtor): decide by hand"),
          ("G", "no v3 export at all (missing): shim or stub")]


# --------------------------------------------------------------------------- running the other scripts
def fatal(msg):
    print("triage: " + msg, file=sys.stderr)
    sys.exit(2)


def run(cmd, what):
    r = subprocess.run([sys.executable, "-I"] + cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = r.stdout.decode("utf-8", "replace")
    if r.returncode != 0:
        fatal("%s failed (exit %d):\n%s" % (what, r.returncode, "\n".join(text.splitlines()[-15:])))
    return text


def pipeline(app, v2, v3, workdir, cxx=("--cxxfilt", "c++filt")):
    """Run step0/join/v2_to_v3/step0 for one binary; return (json, map rows, step0 text)."""
    os.makedirs(workdir, exist_ok=True)
    g = os.path.splitext(os.path.basename(app))[0]
    p = lambda ext: os.path.join(workdir, g + ext)
    run([os.path.join(HERE, "step0.py"), app, "--dump-imports", p(".skel.csv")], "step0 --dump-imports")
    run([os.path.join(HERE, "imports_join.py"), p(".skel.csv"), p(".joined.csv"), v2], "imports_join")
    run([os.path.join(HERE, "v2_to_v3.py"), "--v2", v2, "--v3", v3, "--imports", p(".joined.csv"), "-o", p(".map.csv")]
        + list(cxx), "v2_to_v3")
    txt = run([os.path.join(HERE, "step0.py"), app, "--map", p(".joined.csv"), "--json", p(".s0.json")], "step0")
    with open(p(".step0.txt"), "w", encoding="utf-8") as fh:
        fh.write(txt)
    with open(p(".s0.json"), encoding="utf-8") as fh:
        js = json.load(fh)
    with open(p(".map.csv"), newline="", encoding="utf-8-sig") as fh:
        rows = list(csv.DictReader(fh))
    return js, rows


# --------------------------------------------------------------------------- prediction
def predict(rows, registry, base_map):
    """-> list of dicts, one per import (script order)."""
    out = []
    for r in rows:
        dll, old = r["dll"].lower(), int(r["v2_ordinal"])
        reg = registry.get((dll, old))
        cls, why = "trap", ""
        if reg and reg[0] in ("shim", "runtime"):
            cls, why = "shim", reg[1]
        elif reg and reg[0] == "direct":
            m = base_map.get((dll, old))
            if m is not None:
                ok = m["kind"] == 0 and m["v3"] != 0
                why = reg[1] if ok else "DIRECT entry without a usable map row"
            else:       # not in the merged map yet: judge by the game's own row
                ok = r["status"] == "exact" and not r.get("v3_alt") and r.get("v3_ordinal", "") not in ("", "0")
                why = reg[1] + " (map row of this game)" if ok else "DIRECT entry, but this game's row is %s" % r["status"]
            cls = "direct" if ok else "trap"
        known = (dll, old) in base_map
        out.append(dict(index=int(r["index"]), dll=dll, old=old, name=r["v2_name"], status=r["status"],
                        v3=r.get("v3_ordinal", ""), cls=cls, why=why, known=known))
    return out


# --------------------------------------------------------------------------- ARM code reading
FILE_EXT = ("pck dat wav mbm mbg txt ini cfg bin sav raw mp mid amr aac mp3 ogg snd sfx lvl map tmx gfx pal rsc "
            "dll app exe zip bmp png gif jpg").split()
MIDDLEWARE = ("Hekkus", "Sound System", "MikMod", "libmad", "zlib", "inflate", "SDL", "FMOD", "Copyright", "(c) ")


def find_strings(raw, minlen=4):
    """ASCII and UTF-16LE printable runs of the whole file: [(offset, 'a'|'u', text)]."""
    out = []
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, raw):
        out.append((m.start(), "a", m.group().decode("ascii")))
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % minlen, raw):
        out.append((m.start(), "u", m.group().decode("utf-16le")))
    return out


def strings_of_interest(raw):
    """Hard-coded paths, data file names, middleware banners. Returns (absolute, files, banners)."""
    absolute, files, banners, seen = [], [], [], set()
    for off, kind, t in find_strings(raw):
        t = t.strip()
        if t in seen:
            continue
        low = t.lower()
        if re.search(r"(?<![\w:])[a-z]:\\[\w.$ -]", low) or "\\system\\" in low:
            absolute.append((off, kind, t))
        elif re.fullmatch(r"[\\\w .-]{1,60}\.(%s)" % "|".join(FILE_EXT), t, re.I) or re.fullmatch(r"\\[\w .\\-]{3,60}", t):
            files.append((off, kind, t))
        elif any(k in t for k in MIDDLEWARE) and len(t) >= 8:
            banners.append((off, kind, t[:100]))
        else:
            continue
        seen.add(t)
    return absolute, files, banners


class Code(object):
    def __init__(self, app, js):
        with open(app, "rb") as fh:
            d = fh.read()
        self.b = d[js["code_file_offset"]:js["code_file_offset"] + js["code_size"]]
        self.text = js["text_size"]
        self.arm = js["mode"] == "arm" and not js["mixed"]

    def w(self, o):
        return int.from_bytes(self.b[o:o + 4], "little")

    def is_branch(self, o):
        return ((self.w(o) >> 25) & 7) == 5 and (self.w(o) >> 28) != 15

    def target(self, o):
        w = self.w(o)
        t = (w & 0xFFFFFF)
        t = t - (1 << 24) if t & 0x800000 else t
        return o + 8 + 4 * t

    def is_link(self, o):
        return bool((self.w(o) >> 24) & 1)

    def writes(self, o, reg):
        """('imm', value) | ('unk', None) | None if instruction o does not write reg."""
        w = self.w(o)
        kind = (w >> 26) & 3
        rd = (w >> 12) & 15
        if kind == 0 and rd == reg and not (w & 0x0FC000F0) == 0x00000090:           # data processing (not mul)
            op = (w >> 21) & 15
            if (w >> 25) & 1 and op == 13:
                rot = 2 * ((w >> 8) & 15)
                v = w & 0xFF
                return "imm", ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v
            if op in (8, 9, 10, 11):                                                      # tst/teq/cmp/cmn: no write
                return None
            return "unk", None
        if kind == 1 and (w >> 20) & 1 and rd == reg:                                     # ldr rd,...
            if ((w >> 16) & 15) == 15 and not (w >> 25) & 1:
                off = w & 0xFFF
                lit = o + 8 + (off if (w >> 23) & 1 else -off)
                if 0 <= lit <= len(self.b) - 4:
                    return "imm", self.w(lit)
            return "unk", None
        return None

    def reg_before(self, site, reg, window=10):
        """Constant held in reg just before the call at 'site' (None if unknown)."""
        for i in range(1, window + 1):
            o = site - 4 * i
            if o < 0:
                return None
            if self.is_branch(o):
                return None
            r = self.writes(o, reg)
            if r:
                return r[1] if r[0] == "imm" else None
        return None

    def imm_values(self):
        """(offset, value) of every mov-imm / pc-relative literal load in the text (any register)."""
        out = []
        for o in range(0, self.text - 3, 4):
            for reg in range(0, 4):
                r = self.writes(o, reg)
                if r and r[0] == "imm":
                    out.append((o, r[1]))
                    break
        return out


def split_params(sig):
    m = re.search(r"\((.*)\)(?:\s*const)?\s*$", sig)
    if not m:
        return []
    depth, cur, out = 0, "", []
    for ch in m.group(1):
        depth += ch in "(<"
        depth -= ch in ")>"
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


# --------------------------------------------------------------------------- analyses
def group_of(p, row):
    n, fn = p["name"], p["name"].split("(")[0]
    st = p["status"]
    if fn.startswith("__"):
        return "D"
    if st in ("missing", "no_v3_dso", "no_v2_lib", "v2_ord_missing"):
        return "G"
    m = RE_CTOR.match(n)
    if m and p["dll"] in FRAMEWORK_DLLS:
        return "B"                                            # base constructor of a framework class: old-layout facade
    if st == "moved" and p["dll"] == "estlib":
        return "A"                                            # lives in euser in v3 (like memset): needs a curated direct row
    if st in ("ambiguous", "name_only") or row.get("v3_variant") or row.get("v3_alt") or row.get("note") \
            or RE_DTOR.search(n):
        return "F"
    if RE_VARIADIC.search(n):
        return "E"
    if RE_LEAVE.match(fn) or RE_ASYNC2.match(fn):
        return "C"
    if RE_CALLBACK.search(n) or RE_FLOAT_HELPER.match(fn):
        return "B"
    if re.search(r"\b(?:double|float|TReal|TReal32|TReal64|TInt64|long long)\b", n.split("(", 1)[-1]):
        return "B"
    if p["dll"] in SUBSYSTEM_DLLS:
        return "B"
    return "A"


def analyse(js, pred, code, registry, rows=()):
    rows_by_idx = {int(r["index"]): r for r in rows}
    imps = js["imports"]
    called, vt_only, reach = set(js["called"]), set(js["vtable_only"]), set(js["reach"] or [])
    A = {}
    for p, i in zip(pred, imps):
        p["called"] = p["index"] in called
        p["reach"] = p["index"] in reach
        p["vt_only"] = p["index"] in vt_only
    owners = collections.defaultdict(collections.Counter)
    for (d, _o), (_pol, where) in registry.items():
        if "directbind" not in where:
            owners[d][where.split(":")[-1]] += 1
    A["owner_of"] = lambda dll: (owners[dll].most_common(1)[0][0] if owners.get(dll) else "(no table yet)")
    A["count"] = collections.Counter(p["cls"] for p in pred)
    traps = [p for p in pred if p["cls"] == "trap"]
    A["traps"] = traps
    A["trap_called"] = [p for p in traps if p["called"]]
    A["trap_reach"] = [p for p in traps if p["reach"]]
    A["trap_work"] = [p for p in traps if p["called"] or p["reach"]]
    A["trap_vt"] = [p for p in traps if p["vt_only"]]
    A["new_imports"] = [p for p in pred if not p["known"]]
    A["groups"] = collections.defaultdict(list)
    for p in A["trap_work"]:
        p["group"] = group_of(p, rows_by_idx[p["index"]])
        A["groups"][p["group"]].append(p)
    dlls = collections.Counter(p["dll"] for p in pred)
    reg_dlls = {d for d, _ in registry}
    A["dlls"] = dlls
    A["dlls_no_registry"] = sorted(d for d in dlls if d not in reg_dlls)

    def mark(p):
        return ("C" if p["called"] else "-") + ("R" if p["reach"] else "-") + ("V" if p["vt_only"] else "-")

    A["mark"] = mark
    A["float_helpers"] = [p for p in pred if RE_FLOAT_HELPER.match(p["name"])]
    def float_by_value(name):
        fn = name.split("(")[0]
        if RE_FLOAT_FUNC.match(fn.split("::")[-1]) and "::" not in fn:
            return True
        for prm in split_params(name):
            if re.search(r"\b(?:double|float|TReal|TReal32|TReal64|TInt64|long long)\b", prm) and not re.search(r"[&*]", prm):
                return True
        return False
    A["float_funcs"] = [p for p in pred if not RE_FLOAT_HELPER.match(p["name"]) and float_by_value(p["name"])]
    A["variadic"] = [p for p in pred if RE_VARIADIC.search(p["name"])]
    A["threads"] = [p for p in pred if RE_THREAD.match(p["name"]) or "RThread::" in p["name"]]
    A["screen"] = [p for p in pred if RE_SCREEN.search(p["name"])]
    # framework classes
    vt_bases = collections.Counter(v["base"] for v in js["vtables"] if v["base"] != "-")
    ctors = {}
    for p in pred:
        m = RE_CTOR.match(p["name"])
        if m and (p["dll"] in FRAMEWORK_DLLS or m.group(1) in ("CActive", "CTimer", "CPeriodic", "CIdle")):
            ctors.setdefault(m.group(1), []).append(p)
    A["vt_bases"], A["ctors"] = vt_bases, ctors
    used = collections.defaultdict(list)
    for p in pred:
        m = re.match(r"^(\w+)::", p["name"])
        if m and (EXPENSIVE_BASES.match(m.group(1)) or m.group(1) in ("RTimer", "CActiveScheduler")) and m.group(1) != "CCoeControl":
            used[m.group(1)].append(p)
    A["used_classes"] = used
    # own vtable slots of the framework classes
    own = []
    for v in js["vtables"]:
        if v["base"] != "-" and v["own_legacy"]:
            own.append((v["base"], v["start"], sorted(s for s, _ in v["own_legacy"]), v["this_adj"]))
    A["own_slots"] = own
    # operator new sizes -> framework ctors, display mode, resolutions (ARM only)
    A["new_sites"], A["modes"], A["res"], A["code_note"] = [], [], [], ""
    if not code.arm:
        A["code_note"] = ("code is %s%s: call-site analysis (allocation sizes, display mode, resolution) needs the "
                          "Thumb decoder" % (js["mode"].upper(), " (mixed)" if js["mixed"] else ""))
        return A
    thunks = {int(s): k for s, k in js["thunks"].items()}
    name_of = {p["index"]: p["name"] for p in pred}
    dll_of = {p["index"]: p["dll"] for p in pred}

    def is_fw_ctor(name, dll):
        m = RE_CTOR.match(name)
        return bool(m) and (dll in FRAMEWORK_DLLS or m.group(1) in ("CActive", "CTimer", "CPeriodic", "CIdle"))
    new_idx = [p["index"] for p in pred if RE_NEW.search(p["name"])]
    for k in new_idx:
        for site in js["callers"].get(str(k), []):
            size = code.reg_before(site, 0)
            tgt = None
            for j in range(1, 13):
                o = site + 4 * j
                if o + 4 > code.text:
                    break
                if code.is_branch(o) and code.is_link(o):
                    tgt = code.target(o)
                    break
            base = None
            if tgt is not None:
                if tgt in thunks:
                    base = name_of[thunks[tgt]] if is_fw_ctor(name_of[thunks[tgt]], dll_of[thunks[tgt]]) else None
                else:
                    o = tgt
                    for _ in range(80):
                        if o + 4 > code.text:
                            break
                        if code.is_branch(o):
                            t2 = code.target(o)
                            if t2 in thunks and is_fw_ctor(name_of[thunks[t2]], dll_of[thunks[t2]]):
                                base = name_of[thunks[t2]]
                                break
                            if not code.is_link(o) and (code.w(o) >> 28) == 14:
                                break
                        o += 4
            if size is not None and base:
                A["new_sites"].append((size, tgt, base))
    # display mode: calls to imports with a TDisplayMode parameter
    for p in pred:
        params = split_params(p["name"])
        idx = next((i for i, s in enumerate(params) if "TDisplayMode" in s), None)
        if idx is None or "::" not in p["name"]:
            continue
        reg = idx + 1
        if reg > 3:
            continue
        for site in js["callers"].get(str(p["index"]), []):
            v = code.reg_before(site, reg)
            A["modes"].append((p["name"], site, v))
    pairs = collections.Counter()
    vals = code.imm_values()
    for i, (o1, v1) in enumerate(vals):
        for o2, v2 in vals[i + 1:i + 6]:
            if o2 - o1 > 32:
                break
            if (v1, v2) in RESOLUTIONS and v1 != v2:
                pairs[(v1, v2)] += 1
    A["res"] = pairs.most_common(6)
    return A


# --------------------------------------------------------------------------- report
def verdict(js, A, n, limit):
    """-> (name, text): BLOCKED / NEEDS WORK / READY TO TRY"""
    blockers = []
    if n > limit:
        blockers.append("%d imports exceed the linker limit %d" % (n, limit))
    if js["mode"] == "thumb" or js["mixed"]:
        blockers.append("Thumb code: no decoder yet")
    if blockers:
        return "BLOCKED", "BLOCKED: " + "; ".join(blockers)
    if A["trap_work"]:
        return "NEEDS WORK", ("NEEDS WORK: %d unresolved import(s) are called or reachable from start-up (see the next "
                              "section)" % len(A["trap_work"]))
    return "READY TO TRY", ("READY TO TRY: no unresolved import is called or reachable from start-up, nothing blocks a first "
                            "run (static analysis only: the device run decides)")


def build(game, js, pred, A, args, limit, expect_note, base_js, sizeof):
    S = []                                   # (title, [lines])

    def sec(title, lines):
        S.append((title, [l for l in lines]))

    n = len(pred)
    c = A["count"]
    sec("Verdict numbers", [
        "imports: %d (linker limit %d)%s" % (n, limit, "   ** OVER THE LIMIT **" if n > limit else ""),
        "predicted: shim=%d direct=%d trap=%d%s" % (c["shim"], c["direct"], c["trap"], expect_note),
        "imports NOT yet in the merged import map: %d%s" % (
            len(A["new_imports"]), "  (run gen_importmap.py with this game's map first)" if A["new_imports"] else ""),
        "work predictor (NOT trap=%d): unresolved imports the game CALLS DIRECTLY: %d" % (c["trap"], len(A["trap_called"])),
        "work predictor 2: unresolved imports REACHABLE FROM STARTUP (%s): %d" % (js["reach_roots"], len(A["trap_reach"])),
        "unresolved but only inherited vtable slots (never called by the game): %d" % len(A["trap_vt"]),
        "=> unresolved and called or reachable: %d" % len(A["trap_work"]),
        "VERDICT: " + verdict(js, A, n, limit)[1],
    ])
    mark = A["mark"]
    lines = ["unresolved imports the game calls or reaches, grouped by what to do with them",
             "marks: * = reachable from startup, + = not in the merged import map yet, (v) = also in a vtable",
             "full list with C/R/V marks: %s" % os.path.join(args.out, game, game + ".imports.csv")]
    ng = 0
    for key, title in GROUPS:
        items = sorted(A["groups"].get(key, []), key=lambda p: (p["dll"], p["old"]))
        if not items:
            continue
        ng += 1
        lines.append("%s. %s: %d" % (key, title, len(items)))
        bydll = collections.OrderedDict()
        for p in items:
            bydll.setdefault(p["dll"], []).append(p)
        for dll, ps in bydll.items():
            nm = " ".join("%s%s%s" % (p["name"].split("(")[0], "*" if p["reach"] else "", "+" if not p["known"] else "")
                          for p in ps)
            own = "  -> table %s" % A["owner_of"](dll) if key == "B" else ""
            lines.append("   %s%s: %s" % (dll, own, nm))
    if not ng:
        lines.append("none: every trap is an inherited slot the game never calls")
    sec("Traps that matter (called or reachable)", lines)
    newc = collections.Counter(p["cls"] for p in A["new_imports"])
    if A["new_imports"]:
        sec("Imports the host has never seen (+)", [
            "%d imports are not in the merged import map: %s" % (
                len(A["new_imports"]), ", ".join("%s=%d" % kv for kv in sorted(newc.items()))),
            "run tools\\gen_importmap.py with this game's map first; until then the host treats them as unknown"])
    d_lines = ["DLLs: " + ", ".join("%s(%d)" % kv for kv in sorted(A["dlls"].items()))]
    if base_js:
        bd = {i["dll"] for i in base_js["imports"]}
        d_lines.append("DLLs the baseline game does not use: " + (", ".join(sorted(set(A["dlls"]) - bd)) or "none"))
    d_lines.append("DLLs without any registry entry (host has no subsystem for them): " +
                   (", ".join(A["dlls_no_registry"]) or "none"))
    sec("DLLs / new subsystems", d_lines)

    def fmt(lst):
        return ["%s %-8s %4d  %s  [%s]" % (mark(p), p["dll"], p["old"], p["name"], p["cls"]) for p in lst[:args.max_list]]

    allh = [p for p in pred if p["name"].startswith("__")]
    sec("Compiler helpers (float ones need the word-swap shims, rule R1)",
        ["%s %-6s %5d  %s  [%s]%s" % (mark(p), p["dll"], p["old"], p["name"], p["cls"],
                                      "   FLOAT" if RE_FLOAT_HELPER.match(p["name"]) else "") for p in allh] or ["none"])
    sec("estlib / euser functions with float, double or 64-bit by-value (rule R1)", fmt(A["float_funcs"]) or ["none"])
    sec("Variadic imports (rule R5: check format strings for %f %e %g %lld)", fmt(A["variadic"]) or ["none"])
    sec("Threads / timers", fmt(A["threads"]) or ["none"])
    sec("Direct screen access / bitmap memory", fmt(A["screen"]) or ["none"])
    cl = ["instruction set: %s%s;  import stubs: %s" % (
        js["mode"].upper(), " (MIXED)" if js["mixed"] else "",
        ", ".join("%s x %s bytes" % (v, k) for k, v in sorted(js["thunk_sizes"].items()))),
        "built %s UTC, tools %d.%02d(%d), heap %#x..%#x, stack %#x" % (
            js["built"], js["tools"] & 255, (js["tools"] >> 8) & 255, js["tools"] >> 16, js["heapmin"], js["heapmax"], js["stack"])]
    if js["heapmin"] >= 0x100000:
        cl.append("** LARGE HEAP REQUEST: minimum %d KB (typical games ask for 4 KB): check the heap the host creates for the game **"
                  % (js["heapmin"] // 1024))
    if js["mode"] == "thumb" or js["mixed"]:
        cl.append("THUMB code: needs a Thumb decoder before it can run")
    sec("Code format", cl)
    with open(args.app, "rb") as fh:
        raw = fh.read()
    absl, fil, ban = strings_of_interest(raw)
    stl = []
    if absl:
        stl.append("HARD-CODED ABSOLUTE PATHS (device drive / \\system\\apps): the host must remap them (fsshim RemapOldPath):")
        stl += ["   0x%x %s  %s" % (o, k, t) for o, k, t in absl[:args.max_list]]
    else:
        stl.append("no hard-coded absolute paths (drive letter or \\system\\apps) found")
    if fil:
        stl.append("data files / relative names (resolve against the game folder or the session path):")
        stl += ["   0x%x %s  %s" % (o, k, t) for o, k, t in fil[:args.max_list]]
    for b_ in ban:
        stl.append("middleware / banner: 0x%x %s  %s" % b_)
    fsn = sorted({p["name"].split("(")[0] for p in pred if p["dll"] == "efsrv" and re.search(
        r"SetSessionPath|GetDir|CDir::|MkDir|Delete|Replace|Rename|Entry|Seek|Write", p["name"])})
    if fsn:
        stl.append("file-system imports worth a look (session path, directory listing, writes): " + ", ".join(fsn))
    sec("Strings of interest (paths, data files, middleware)", stl)
    if A["code_note"]:
        sec("Display mode / resolution / allocation sizes", [A["code_note"]])
    else:
        ml = []
        modes = collections.Counter((nm, v) for nm, _, v in A["modes"])
        for (nm, v), k in modes.most_common(8):
            ml.append("display mode at %s: %s  x%d" % (nm, ("%d = %s" % (v, DISPLAY_MODES.get(v, "?"))) if v is not None else "not constant", k))
        if not A["modes"]:
            ml.append("no call with a constant TDisplayMode argument found")
        if A["res"]:
            ml.append("resolution candidates (width,height constants close together; heuristic): " +
                      ", ".join("%dx%d (x%d)" % (a, b, k) for (a, b), k in A["res"]))
        else:
            ml.append("no known screen size constants found (the game may read the size from the device at run time)")
        sec("Display mode / resolution", ml)
    # framework classes
    fl = []
    if A["vt_bases"]:
        fl.append("vtable owners (base class found through inherited slots): " +
                  ", ".join("%s x%d" % kv for kv in sorted(A["vt_bases"].items())))
    base_bases = set(x["base"] for x in base_js["vtables"]) if base_js else None
    if base_bases is not None:
        newb = sorted(b for b in A["vt_bases"] if b not in base_bases)
        fl.append("framework bases the baseline game does NOT derive from: " + (", ".join(newb) or "none"))
    exp = sorted(k for k in A["ctors"] if EXPENSIVE_BASES.match(k) and k != "CCoeControl")
    fl.append("base-class constructors imported: " + (", ".join(
        "%s%s" % (k, "" if len(v) == 1 else " (%d)" % len(v)) for k, v in sorted(A["ctors"].items())) or "none"))
    fl.append("derived expensive kinds (old-layout facade needed, cost driver of a new game): " + (", ".join(exp) or "none found"))
    base_used = set()
    if base_js:
        for i in base_js["imports"]:
            m_ = re.match(r"^(\w+)::", i["name"])
            if m_:
                base_used.add(m_.group(1))
    newb_set = set(newb) if base_js else set()
    for k, ps in sorted(A["used_classes"].items()):
        fl.append("USES %s through imports (%s)%s" % (
            k, ", ".join(sorted({p["name"].split("(")[0].split("::")[-1] for p in ps})),
            "" if not base_js else ("  [baseline uses it too%s]" % ("" if k not in newb_set else ", but does NOT derive from it: this game subclasses it") if k in base_used else
                                    "  ** NEW vs baseline: active object / timer, callbacks go into old code **")))
    sec("Framework classes the game derives from or uses", fl)
    sl = []
    sizes = collections.Counter((s, b) for s, _, b in A["new_sites"])
    for (s, b), k in sorted(sizes.items(), key=lambda x: x[0][0]):
        want = sizeof.get(re.match(r"^(\w+)::", b).group(1)) if sizeof and re.match(r"^(\w+)::", b) else None
        sl.append("new(%d) then %s%s%s" % (s, b, "  x%d" % k if k > 1 else "",
                                           "" if want is None else "   v2 sizeof(base)=%d -> %s" % (
                                               want, "OK (game class is at least as big)" if s >= want else "TOO SMALL?")))
    if A["code_note"]:
        sl = [A["code_note"]]
    sec("Object sizes passed to operator new before a framework base constructor",
        sl or ["none found (ARM code only; the constructor must be reached within 80 instructions of the call)"] +
        ([] if sizeof else ["(no --sizeof file: shown without comparison with the v2 sizeof)"]))
    names = collections.defaultdict(dict)                 # (class, this-adjust) -> slot -> import name seen inherited
    for J in [js] + ([base_js] if base_js else []):
        nm_of = {i["index"]: i["name"] for i in J["imports"]}
        for v in J["vtables"]:
            for sl, k in v.get("slot_imports", []):
                names[(v["base"], v["this_adj"])].setdefault(sl, nm_of[k].split("(")[0])

    def sname(base, sl, adj=0):
        return "%d=%s" % (sl, "dtor" if sl == 2 else names.get((base, adj), {}).get(sl, "?"))

    ol = []
    for base, start, slots, adj in A["own_slots"]:
        ol.append("%-30s vtable @%05x%s own: %s" % (base, start, " (secondary base, this%+d)" % adj if adj else "",
                                                    ", ".join(sname(base, x, adj) for x in slots)))
    if base_js:
        bs = collections.defaultdict(set)
        for v in base_js["vtables"]:
            if v["base"] != "-":
                bs[(v["base"], v["this_adj"])].update(s_ for s_, _ in v["own_legacy"])
        diff = []
        for base, start, slots, adj in A["own_slots"]:
            extra = sorted(set(slots) - bs.get((base, adj), set()))
            if (base, adj) in bs and extra:
                diff.append("NEW vs baseline: %s overrides %s" % (base, ", ".join(sname(base, x, adj) for x in extra)))
            if (base, adj) not in bs:
                diff.append("NEW vs baseline: %s: not derived from by the baseline" % base)
        ol += diff or ["no slot beyond the baseline's"]
    # predicted route = what ctlreg.cpp's CtlReg_LogRoute prints at startup ("input route: ..."), from static data
    appui = [x for x in A["own_slots"] if x[0] == "CEikAppUi" and x[3] == 0 and 15 in x[2]]
    if appui:
        own = set(appui[0][2])
        def called(pat):
            return any(re.search(pat, p["name"]) and p["called"] for p in pred)
        ol.insert(0, "predicted input route: key=%s, foreground=%s, AddToStackL called=%s, AddForegroundObserverL called=%s" % (
            "AppUi::HandleKeyEventL (word 8, own)" if 8 in own else "control stack (framework default)",
            "AppUi::HandleForegroundEventL (word 9, own)" if 9 in own else "no AppUi override",
            "yes" if called(r"AddToStackL") else "no", "yes" if called(r"AddForegroundObserverL") else "no"))
        ol.insert(1, "compare with the device log line 'input route: ...' written after the old ConstructL")
    fo = sorted({x for b_, _, sl_, _ in A["own_slots"] if b_ == "MCoeForegroundObserver" for x in sl_ if x != 2})
    if fo:
        ol.insert(2 if appui else 0, "foreground observer: the game itself implements MCoeForegroundObserver (own words %s = "
                  "HandleGainingForeground / HandleLosingForeground): the host must call these through the old-code "
                  "runtime on focus changes (AppUi::HandleForegroundEventL is not used)" % ", ".join(map(str, fo)))
    sec("Own vtable slots (decide input / foreground routes)", ol or ["none found"])
    return S


def render(game, S):
    md, tx = ["# Triage: %s\n" % game], ["TRIAGE: %s" % game, "=" * (8 + len(game)), ""]
    for title, lines in S:
        md.append("## %s\n" % title)
        md.append("```")
        md += lines
        md.append("```\n")
        tx.append(title)
        tx.append("-" * len(title))
        tx += lines
        tx.append("")
    return "\n".join(md), "\n".join(tx)


def main():
    ap = argparse.ArgumentParser(prog="triage.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app", help="the game binary (.app)")
    ap.add_argument("--v2-libs", required=True, help="folder with the S60v2 .lib files")
    ap.add_argument("--v3-libs", required=True, help="folder with the S60v3 .dso (or .lib) files")
    ap.add_argument("--src", help="host sources (default: <root>/src)")
    ap.add_argument("--out", help="output folder (default: <root>/work)")
    ap.add_argument("--strict", action="store_true", help="exit code 1 unless the verdict is READY TO TRY")
    ap.add_argument("--baseline-app", help="a game the host already runs: adds 'new vs baseline' lines (framework classes, "
                                           "slots the new game uses that the known one does not)")
    ap.add_argument("--cxxfilt", default="c++filt", help="path of c++filt (needed for the v3 names); default: from PATH")
    ap.add_argument("--no-cxxfilt", action="store_true", help="run without c++filt (map quality drops: no exact matches)")
    ap.add_argument("--sizeof", help="CSV class,size with the v2 sizeof of framework classes (optional)")
    ap.add_argument("--expect", help="shim=N,direct=N,trap=N: print MATCH/MISMATCH against it")
    ap.add_argument("--check-game", metavar="ID", help="take --expect from games/ID/game.json (main version)")
    ap.add_argument("--root", default=os.path.dirname(HERE), help="project root, used by --check-game")
    ap.add_argument("--max-list", type=int, default=40, help="most rows per list in the report (default 40)")
    a = ap.parse_args()
    a.src = a.src or os.path.join(a.root, "src")
    a.out = a.out or os.path.join(a.root, "work")
    if not os.path.isfile(a.app):
        fatal("%s is not a file" % a.app)

    if not os.path.isdir(a.src):
        fatal("--src %s is not a folder" % a.src)
    game = os.path.splitext(os.path.basename(a.app))[0]
    try:
        registry, tables = IC.load_shim_registry(a.src)
        base_map = IC.load_importmap_h(a.src)
        limit = IC.load_link_import_limit(a.src)
    except (OSError, ValueError) as exc:
        fatal("cannot read the host sources in %s: %s" % (a.src, exc))
    gdir = os.path.join(a.out, game)
    if a.no_cxxfilt:
        cxx = ["--no-cxxfilt"]
        print("triage: WARNING: running without c++filt, the v2 -> v3 map will have almost no exact rows")
    else:
        exe = shutil.which(a.cxxfilt)
        if not exe:
            fatal("c++filt not found (%r). Give its full path with --cxxfilt PATH, or --no-cxxfilt to run "
                  "without it (the map will be much worse)." % a.cxxfilt)
        cxx = ["--cxxfilt", exe]

    js, rows = pipeline(a.app, a.v2_libs, a.v3_libs, gdir, cxx)
    pred = predict(rows, registry, base_map)
    code = Code(a.app, js)
    A = analyse(js, pred, code, registry, rows)

    base_js = None
    if a.baseline_app:
        base_js, _ = pipeline(a.baseline_app, a.v2_libs, a.v3_libs,
                              os.path.join(a.out, "_baseline_" + os.path.splitext(os.path.basename(a.baseline_app))[0]), cxx)
    sizeof = {}
    if a.sizeof:
        with open(a.sizeof, newline="", encoding="utf-8-sig") as fh:
            sizeof = dict((r["class"], int(r["size"])) for r in csv.DictReader(fh))
    expect_note = ""
    if a.check_game and not a.expect:
        try:
            with open(os.path.join(a.root, "games", a.check_game, "game.json"), "r", encoding="utf-8") as fh:
                e = next(v for v in json.load(fh)["versions"] if v["id"] == "main")["expect"]
            a.expect = "shim=%d,direct=%d,trap=%d" % (e["shim"], e["direct"], e["trap"])
        except (OSError, ValueError, KeyError, StopIteration) as ex:
            fatal("--check-game %s: cannot read the main expect block (%r)" % (a.check_game, ex))
    if a.expect:
        m = re.fullmatch(r"shim=(\d+),\s*direct=(\d+),\s*trap=(\d+)", a.expect.replace(" ", ""))
        if not m:
            fatal("--expect: use shim=N,direct=N,trap=N")
        got = (A["count"]["shim"], A["count"]["direct"], A["count"]["trap"])
        expect_note = "   expected %s -> %s" % (a.expect, "MATCH" if got == tuple(map(int, m.groups())) else "MISMATCH")

    S = build(game, js, pred, A, a, limit, expect_note, base_js, sizeof)
    verdict_name = verdict(js, A, len(pred), limit)[0]
    md, tx = render(game, S)
    os.makedirs(a.out, exist_ok=True)
    for ext, text in ((".triage.md", md), (".triage.txt", tx)):
        with open(os.path.join(a.out, game + ext), "w", encoding="utf-8") as fh:
            fh.write(text)
    with open(os.path.join(gdir, game + ".imports.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["index", "dll", "old_ordinal", "v2_name", "map_status", "predicted", "owner", "called", "reachable",
                    "vtable_only", "in_merged_map", "group"])
        for p in pred:
            w.writerow([p["index"], p["dll"], p["old"], p["name"], p["status"], p["cls"], p["why"],
                        int(p["called"]), int(p["reach"]), int(p["vt_only"]), int(p["known"]), p.get("group", "")])
    guess = {"uid3": "%08x" % js["uid3"], "screen": None, "display_mode": None}
    if A["res"]:
        guess["screen"] = list(A["res"][0][0])
    seen = collections.Counter(v for _, _, v in A["modes"] if v is not None)
    if seen:
        guess["display_mode"] = seen.most_common(1)[0][0]
    with open(os.path.join(gdir, game + ".guess.json"), "w", encoding="utf-8") as fh:
        json.dump(guess, fh, indent=1)      # read by newgame.py for the profile stub
    print(tx)
    print("\nwritten: %s  and  %s" % (os.path.join(a.out, game + ".triage.md"), os.path.join(a.out, game + ".triage.txt")))
    return 1 if (a.strict and verdict_name != "READY TO TRY") else 0


if __name__ == "__main__":
    sys.exit(main())