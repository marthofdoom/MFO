"""The per-class hot-swap tuning files (MFO_Tuning/<class>.txt, shared.txt, potions.txt, looting.txt).

The validator here MIRRORS the in-game loader (native/iaus/TuningFile.cpp): it accepts what the loader applies and rejects what it rejects.
  <class>.txt   one spec per line, '#' starts a comment:  [<class>:]<path>=<v>   (a different class prefix is rejected)
                path = <family>.<cons>.<a|b|floor|zeroAbove|zeroBelow|w|px0-5|py0-5> | family.<family> | scalar.<name> | equip.crossover
                family / cons = an index or a Name() (names.py)
  shared.txt    comma list of  t.<scalar> | w.<family idx>.<cons idx> | aw.<family idx> | c.<cons idx>.a|b|floor  = <v>; one bad entry rejects the line
  potions.txt   worth.<knob>=<v>        looting.txt   loot.<knob>=<v>
Around a line: spaces around = and : are trimmed, a leading --set / --curve token (optionally quoted) is stripped, a leading '+' on a value is accepted.
"""
import os
import re
import shutil
import subprocess
import time

from . import names

SHARED_NAMES = ("shared.txt", "potions.txt", "looting.txt")
FLOAT_MAX = 3.4028235e38
NUM_RE = re.compile(r"^(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$")
SCALARS = ("drinkHealthLine", "touchSpentSec", "schoolPeak", "schoolFloor", "schoolShape", "schoolMasteredBump")
CURVE_PARAMS = ("a", "b", "floor", "zeroAbove", "zeroBelow")
# t.* scalars (Catalogue.cpp SetTuningScalar): all >= 0, the two HP lines 0..1
T_SCALARS = ("t.momentum", "t.postureMomentum", "t.preempt", "t.actFloor", "t.softCd", "t.occHold", "t.kite", "t.blockBase", "t.tauntCd", "t.powerCd",
             "t.disengageCd", "t.noHealDrink", "t.noHealRetreat", "t.emergencyHpHigh", "t.emergencyHpLow", "t.offClassBase", "t.groundCd", "t.blockCd",
             "t.bashCd", "t.backOffCd", "t.stepCd", "t.regroupCd")
T_UNIT = ("t.emergencyHpHigh", "t.emergencyHpLow")
INF = 3.0e38
# worth.* (ConsumableWorth.h SetWorthKnob): name -> (lo, lo is open, hi)
WORTH = {"ratioCap": (0, True, INF), "pressureFull": (0, True, INF), "dangerSpan": (0, True, INF), "hurtWeight": (0, False, INF), "magickaWinLine": (0, False, 1),
         "staminaWinLine": (0, False, 1), "winNeed": (0, False, 1), "ttlFloorSec": (0, False, INF), "ttlSpanSec": (0, True, INF), "wealthFrac": (0, False, INF),
         "wealthFloor": (0, True, INF), "costWeight": (0, False, INF), "scarcityRate": (0, False, INF), "restoreCostScale": (0, False, INF),
         "restoreUseWeight": (0, False, INF), "floor": (0, False, 1), "foodMinGain": (0, False, 1)}
# loot.* floats (LootNeed.h SetLootKnob): name -> (open, hi); ints: whole 0..1e6; consumableSellRoad 0 or 1
LOOT_F = {"takeThreshold": (False, 1), "needBase": (False, INF), "needSlope": (False, INF), "roomKnee": (True, 1), "saleGoldPerWeight": (True, INF),
          "saleWeight": (False, INF), "gold": (False, INF), "equipment": (False, INF), "valuables": (False, INF), "jewelry": (False, INF),
          "gemChargeTrigger": (False, 1), "buffScale": (False, INF), "foodScale": (False, INF), "poisonPowerRef": (True, INF), "poisonWeakScale": (False, INF),
          "stealNeedMin": (False, 1), "stealRiskWeight": (False, INF), "stealBountyPerGold": (False, INF), "stealBountyRef": (True, INF)}
LOOT_I = ("soulGemTarget", "healthTarget", "staminaTarget", "staminaTargetNoWeapon", "magickaTarget", "resistTarget", "fortifyTarget", "regenTarget",
          "invisTarget", "lockpickTarget", "poisonTarget")
KNOWN_STEMS = set(names.CLASS_SLUGS) | set(names.BASE_OFF_SLUGS) | {"shared", "potions", "looting"}


def parse_float(s):
    """The loader's ParseFloat: trimmed, one optional leading '+', a plain decimal (no inf / nan / hex), finite, fits a float."""
    s = s.strip()
    if s.startswith("+"):
        s = s[1:]
        if not s or s[0] in "+-":
            return None
    neg = s.startswith("-")
    body = s[1:] if neg else s
    if not NUM_RE.match(body):
        return None
    try:
        v = float(body)
    except ValueError:
        return None
    if v > FLOAT_MAX:
        return None
    return -v if neg else v


def _index_of(tok, table):
    """-> index or -1. An index (digits only, <= 6 chars, < n) or the first case-insensitive name match."""
    if not tok:
        return -1
    if tok.isascii() and tok.isdigit():
        return int(tok) if len(tok) <= 6 and int(tok) < len(table) else -1
    w = tok.lower()
    for i, n in enumerate(table):
        if n == w:
            return i
    return -1


def _idx(tok):
    """shared.txt index tokens: digits only, no names."""
    return int(tok) if tok and len(tok) <= 6 and tok.isascii() and tok.isdigit() else -1


def clean_line(raw):
    """Comment stripped, trimmed, a leading --set / --curve token (and a quote pair around the rest) removed. -> '' for an empty line."""
    v = raw.split("#", 1)[0].strip()
    for lead in ("--set", "--curve"):
        if v.startswith(lead) and len(v) > len(lead) and v[len(lead)].isspace():
            v = v[len(lead):].strip()
            if len(v) >= 2 and v[0] in "\"'" and v[-1] == v[0]:
                v = v[1:-1].strip()
            break
    return v


def _split_name_value(ent):
    """-> (name, value, err)."""
    if "=" not in ent:
        return None, None, "no '=' (expected <name>=<value>)"
    n, v = ent.split("=", 1)
    n = n.strip()
    if not n:
        return None, None, "empty name"
    f = parse_float(v)
    if f is None:
        return None, None, "unparsable value"
    return n, f, None


def _class_spec(path, v):
    if path.startswith("scalar."):
        return None if path[7:] in SCALARS else "unknown scalar (%s)" % " | ".join(SCALARS)
    if path.startswith("family."):
        if _index_of(path[7:], names.FAMILIES) < 0 or not v > 0:
            return "unknown family or bad weight (family.<family>=<v>, v > 0)"
        return None
    if path == "equip.crossover":
        return None
    d1 = path.find(".")
    d2 = path.find(".", d1 + 1) if d1 >= 0 else -1
    if d2 < 0:
        return "bad path (<family>.<cons>.<param> | scalar.<name> | family.<family> | equip.crossover)"
    if _index_of(path[:d1], names.FAMILIES) < 0 or _index_of(path[d1 + 1:d2], names.CONS) < 0:
        return "unknown family or consideration"
    param = path[d2 + 1:]
    if param == "w":
        return None if v > 0 else "bad weight (v > 0)"
    if param in CURVE_PARAMS or (len(param) == 3 and param[0] == "p" and param[1] in "xy" and param[2] in "012345"):
        return None
    return "unknown curve param (a | b | floor | zeroAbove | zeroBelow | w | px<0-5> / py<0-5>)"


def _class_line(stem, v):
    eq = v.find("=")
    if eq < 0:
        return "no '=' (expected [<class>:]<path>=<value>)"
    lhs = v[:eq].strip()
    f = parse_float(v[eq + 1:])
    if f is None:
        return "unparsable value"
    if ":" in lhs:
        slug, lhs = lhs.split(":", 1)
        slug, lhs = slug.strip(), lhs.strip()
        if slug != stem:
            return "names class '%s' but this is %s.txt" % (slug, stem)
    return _class_spec(lhs, f)


def _shared_entry(name, v):
    if name.startswith("w."):
        dot = name.find(".", 2)
        fi, ci = (-1, -1) if dot < 0 else (_idx(name[2:dot]), _idx(name[dot + 1:]))
        if fi < 0 or ci < 0 or fi >= len(names.FAMILIES) or ci >= len(names.CONS) or not v > 0:
            return "bad weight (w.<family index>.<cons index>=<v>, v > 0)"
        return None
    if name.startswith("aw."):
        fi = _idx(name[3:])
        return None if 0 <= fi < len(names.FAMILIES) else "bad family index (aw.<family index>=<v>)"
    if name.startswith("c."):
        dot = name.find(".", 2)
        ci = -1 if dot < 0 else _idx(name[2:dot])
        if ci < 0 or ci >= len(names.CONS):
            return "bad consideration index (c.<cons index>.a|b|floor=<v>)"
        return None if name[dot + 1:] in ("a", "b", "floor") else "unknown curve field (a | b | floor)"
    if name.startswith(("st.", "pk.", "row.")):
        return "a simulator-only knob: the game has no table for it"
    if name.startswith("worth."):
        return "belongs in potions.txt"
    if name.startswith("loot."):
        return "belongs in looting.txt"
    if name in T_SCALARS:
        if v < 0 or (name in T_UNIT and v > 1):
            return "out of range: must be 0..1" if name in T_UNIT else "out of range: must be >= 0"
        return None
    return "unknown t.<name> scalar" if name.startswith("t.") else "unknown name"


def _shared_line(v):
    for ent in v.split(","):
        ent = ent.strip()
        n, f, err = _split_name_value(ent)
        if not err and ":" in n:
            err = "a class line belongs in that class's own file"
        if not err:
            err = _shared_entry(n, f)
        if err:
            return "entry '%s': %s (the whole line is rejected)" % (ent, err)
    return None


def _potions_line(v):
    n, f, err = _split_name_value(v)
    if err:
        return err
    if not n.startswith("worth."):
        return "a potions.txt line is worth.<name>=<v>"
    k = WORTH.get(n[6:])
    if not k:
        return "unknown worth knob"
    lo, lo_open, hi = k
    if f < lo or (lo_open and f <= lo) or f > hi:
        return "out of range: must be %s%s" % ("> 0" if lo_open else ">= 0", " and <= 1" if hi < INF else "")
    return None


def _looting_line(v):
    n, f, err = _split_name_value(v)
    if err:
        return err
    if not n.startswith("loot."):
        return "a looting.txt line is loot.<name>=<v>"
    k = n[5:]
    if k in LOOT_F:
        op, hi = LOOT_F[k]
        if f < 0 or (op and f <= 0) or f > hi:
            return "out of range: must be %s%s" % ("> 0" if op else ">= 0", " and <= 1" if hi < INF else "")
        return None
    if k in LOOT_I:
        return None if f == int(f) and 0 <= f <= 1.0e6 else "out of range: must be a whole number 0..1000000"
    if k == "consumableSellRoad":
        return None if f in (0.0, 1.0) else "must be 0 or 1"
    return "unknown loot knob"


def validate_line(filename, line):
    """-> error string or None. filename decides the grammar (a class slug's file, shared.txt, potions.txt, looting.txt)."""
    v = clean_line(line)
    if not v:
        return None
    stem = filename[:-4] if filename.endswith(".txt") else filename
    if stem == "shared":
        return _shared_line(v)
    if stem == "potions":
        return _potions_line(v)
    if stem == "looting":
        return _looting_line(v)
    return _class_line(stem, v)


def kind_of(filename):
    return "set" if filename in SHARED_NAMES else "curve"


def validate_text(filename, text):
    """-> list of (lineno, message). An unknown file name (not a class slug or shared / potions / looting) is flagged at line 0."""
    errs = []
    stem = filename[:-4] if filename.endswith(".txt") else filename
    if not filename.endswith(".txt") or stem not in KNOWN_STEMS:
        errs.append((0, "unknown file name '%s': the game loads <class slug>.txt, shared.txt, potions.txt and looting.txt only" % filename))
    if text.startswith("﻿"):
        text = text[1:]
    for i, ln in enumerate(text.splitlines(), 1):
        e = validate_line(filename, ln)
        if e:
            errs.append((i, e))
    return errs


def generate(kept_specs, sets, header, covered=()):
    """-> {filename: text} from a run's kept curve specs and accepted --set specs. Every class in `covered` (the run finished its search)
    gets a file, header-only when it kept nothing, so deploying clears a value the new run no longer keeps."""
    files = {}
    by = {c: [] for c in covered}
    for sp in kept_specs:
        by.setdefault(sp.split(":", 1)[0], []).append(sp)
    for cls, specs in sorted(by.items()):
        files[cls + ".txt"] = "# %s\n%s" % (header, "\n".join(specs) + "\n" if specs else "")
    groups = {"shared.txt": [], "potions.txt": [], "looting.txt": []}
    for s in sets:
        n = s.split("=", 1)[0]
        groups["potions.txt" if n.startswith("worth.") else "looting.txt" if n.startswith("loot.") else "shared.txt"].append(s)
    for fn, lst in groups.items():
        if lst:
            files[fn] = "# %s\n%s\n" % (header, "\n".join(lst))
    return files


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def deploy_local(files, target_dir):
    """Write files into target_dir, backing up any existing file first (name.bak-<stamp>, never *.txt so a loader glob can't pick it up).
    -> list of (filename, 'written'|'backed up + written', backup_path or None)"""
    os.makedirs(target_dir, exist_ok=True)
    res = []
    st = stamp()
    for fn, text in files.items():
        dst = os.path.join(target_dir, fn)
        bak = None
        if os.path.exists(dst):
            bak = dst + ".bak-" + st
            shutil.copy2(dst, bak)
        tmp = dst + ".tmp-new"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, dst)
        res.append((fn, "backed up + written" if bak else "written", bak))
    return res


SSH_OPTS = ["-o", "ConnectTimeout=6", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new"]


def _ssh(host, cmd, timeout=20):
    return subprocess.run(["ssh"] + SSH_OPTS + [host, cmd], capture_output=True, text=True, timeout=timeout)


def _bad_host(host):
    return not host or host.startswith("-") or any(c.isspace() for c in host)


def deck_reachable(host):
    if _bad_host(host):
        return False
    try:
        return _ssh(host, "true", 12).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def deploy_deck(files, host, target_dir, tmp_root):
    """ssh backup of existing files, then scp. Raises RuntimeError with a plain message when the Deck is unreachable (asleep): the caller
    offers a retry. -> same shape as deploy_local."""
    if _bad_host(host):
        raise RuntimeError("the Deck ssh host %r is not usable (empty, starts with '-' or holds a space): fix it in Settings" % host)
    if not deck_reachable(host):
        raise RuntimeError("the Deck (%s) did not answer ssh within 12 s: it is probably asleep. Wake it and retry." % host)
    st = stamp()
    q = "'%s'" % target_dir.replace("'", "'\\''")
    r = _ssh(host, "mkdir -p %s" % q)
    if r.returncode:
        raise RuntimeError("mkdir on the Deck failed: " + (r.stderr or "").strip())
    os.makedirs(tmp_root, exist_ok=True)
    res = []
    for fn, text in files.items():
        remote = target_dir.rstrip("/") + "/" + fn
        rq = "'%s'" % remote.replace("'", "'\\''")
        b = _ssh(host, "if [ -e %s ]; then cp -p %s %s.bak-%s && echo BAK; fi" % (rq, rq, rq, st))
        had = "BAK" in (b.stdout or "")
        if b.returncode:
            raise RuntimeError("backup of %s on the Deck failed (nothing overwritten): %s" % (fn, (b.stderr or "").strip()))
        local = os.path.join(tmp_root, fn)
        with open(local, "w") as f:
            f.write(text)
        # atomic: scp to <file>.tmp-new, then mv over the real name (the in-game loader never sees a half-written file)
        c = subprocess.run(["scp", "-q"] + SSH_OPTS + [local, "%s:%s.tmp-new" % (host, remote)], capture_output=True, text=True, timeout=60)
        if c.returncode:
            raise RuntimeError("scp of %s failed: %s" % (fn, (c.stderr or "").strip()))
        m = _ssh(host, "mv -f %s.tmp-new %s" % (rq, rq))
        if m.returncode:
            raise RuntimeError("moving %s into place on the Deck failed (the old file is untouched): %s" % (fn, (m.stderr or "").strip()))
        res.append((fn, "backed up + written" if had else "written", remote + ".bak-" + st if had else None))
    return res


def list_dir_local(d):
    try:
        return {fn: open(os.path.join(d, fn), errors="replace").read() for fn in sorted(os.listdir(d)) if fn.endswith(".txt")}
    except OSError:
        return {}


def tuning_log_lines(text, n=30):
    return [ln for ln in text.splitlines() if "[tuning]" in ln][-n:]


def read_log_local(path, tail_bytes=4_000_000):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            sz = f.tell()
            f.seek(max(0, sz - tail_bytes))
            return f.read().decode(errors="replace")
    except OSError:
        return ""


def read_log_deck(host, path):
    if _bad_host(host):
        return None
    try:
        r = _ssh(host, "grep -a '\\[tuning\\]' '%s' | tail -n 40" % path.replace("'", "'\\''"), 25)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None
