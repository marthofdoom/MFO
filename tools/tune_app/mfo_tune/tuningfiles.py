"""The per-class hot-swap tuning files (MFO_Tuning/<class>.txt, shared.txt, potions.txt, looting.txt).

Line syntax = the simulator's own specs, one per line, '#' starts a comment:
  <class>.txt   --curve specs  <class>:<family>.<cons>.<param>=<v> | <class>:family.<family>=<v> | <class>:<family>.<cons>.py<i>=<v>
  shared.txt    --set specs    <name>=<v>   (t.*, w.*, aw.*, c.*, and any other --set key not routed below)
  potions.txt   --set specs    worth.*
  looting.txt   --set specs    loot.*
(The in-game loader is not part of the repository this app ships in; the file/line grammar here is the one the task brief gave.)
"""
import os
import re
import shutil
import subprocess
import time

IDENT = r"[A-Za-z_][A-Za-z0-9_]*"
NUM = r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?"
CURVE_RE = re.compile(r"^([a-z][a-z0-9-]*):(?:family\.(%s|\d+)|(%s|\d+)\.(%s|\d+)\.(%s))=(%s)$" % (IDENT, IDENT, IDENT, IDENT, NUM))
SET_RE = re.compile(r"^(%s(?:\.%s)*)=(%s)$" % (IDENT, IDENT, NUM))
SHARED_NAMES = ("shared.txt", "potions.txt", "looting.txt")


def validate_line(kind, line):
    """kind 'curve' | 'set'. -> error string or None."""
    s = line.split("#", 1)[0].strip()
    if not s:
        return None
    if kind == "curve":
        return None if CURVE_RE.match(s) else "not a --curve spec (<class>:<family>.<cons>.<param>=<v> | <class>:family.<family>=<v>)"
    return None if SET_RE.match(s) else "not a --set spec (<name>=<number>)"


def kind_of(filename):
    return "set" if filename in SHARED_NAMES else "curve"


def validate_text(filename, text):
    """-> list of (lineno, message). For a class file the class prefix of every spec must match the file name."""
    kind = kind_of(filename)
    errs = []
    cls = filename[:-4] if filename.endswith(".txt") else filename
    for i, ln in enumerate(text.splitlines(), 1):
        e = validate_line(kind, ln)
        if e:
            errs.append((i, e))
        elif kind == "curve":
            s = ln.split("#", 1)[0].strip()
            if s and s.split(":", 1)[0] != cls:
                errs.append((i, "spec is for class '%s' but the file is %s" % (s.split(":", 1)[0], filename)))
        elif kind == "set":
            s = ln.split("#", 1)[0].strip()
            if s:
                n = s.split("=", 1)[0]
                if filename == "potions.txt" and not n.startswith("worth."):
                    errs.append((i, "potions.txt holds worth.* specs"))
                if filename == "looting.txt" and not n.startswith("loot."):
                    errs.append((i, "looting.txt holds loot.* specs"))
    return errs


def generate(kept_specs, sets, header):
    """-> {filename: text} from a run's kept curve specs and accepted --set specs."""
    files = {}
    by = {}
    for sp in kept_specs:
        by.setdefault(sp.split(":", 1)[0], []).append(sp)
    for cls, specs in sorted(by.items()):
        files[cls + ".txt"] = "# %s\n%s\n" % (header, "\n".join(specs))
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


def deck_reachable(host):
    try:
        return _ssh(host, "true", 12).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def deploy_deck(files, host, target_dir, tmp_root):
    """ssh backup of existing files, then scp. Raises RuntimeError with a plain message when the Deck is unreachable (asleep): the caller
    offers a retry. -> same shape as deploy_local."""
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
        c = subprocess.run(["scp", "-q"] + SSH_OPTS + [local, "%s:%s" % (host, remote)], capture_output=True, text=True, timeout=60)
        if c.returncode:
            raise RuntimeError("scp of %s failed: %s" % (fn, (c.stderr or "").strip()))
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
    try:
        r = _ssh(host, "grep -a '\\[tuning\\]' '%s' | tail -n 40" % path.replace("'", "'\\''"), 25)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None
