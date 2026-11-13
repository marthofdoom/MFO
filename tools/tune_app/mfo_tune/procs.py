"""Process detection via /proc: tune_driver / sim processes, their --out dir, CPU use. Stop = SIGTERM to the process group (never other groups)."""
import os
import signal
import time

_CLK = os.sysconf("SC_CLK_TCK")


def _cmdline(pid):
    try:
        with open("/proc/%s/cmdline" % pid, "rb") as f:
            return [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return []


def _stat(pid):
    try:
        with open("/proc/%s/stat" % pid) as f:
            t = f.read()
        rest = t[t.rindex(")") + 2:].split()
        return {"ppid": int(rest[1]), "pgid": int(rest[2]), "utime": int(rest[11]), "stime": int(rest[12]), "start": int(rest[19])}
    except (OSError, ValueError, IndexError):
        return None


def _boot():
    try:
        with open("/proc/stat") as f:
            for ln in f:
                if ln.startswith("btime"):
                    return int(ln.split()[1])
    except OSError:
        pass
    return 0


def _arg(args, name):
    for i, a in enumerate(args):
        if a == name and i + 1 < len(args):
            return args[i + 1]
        if a.startswith(name + "="):
            return a.split("=", 1)[1]
    return None


def scan():
    """-> list of {pid, pgid, kind: driver|sim|wrapper, cmd, out, cpu_s, age_s}. wrapper = a shell script running a driver (round.sh)."""
    me = os.getpid()
    boot = _boot()
    now = time.time()
    res = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == me:
            continue
        args = _cmdline(pid)
        if not args:
            continue
        joined = " ".join(args)
        kind = None
        if any(a.endswith("tune_driver.py") or a.endswith("tune_driver.frozen.py") for a in args[:4]) and "python" in args[0]:
            kind = "driver"
        elif os.path.basename(args[0]).startswith("sim") and ("--serve" in args or "--classes" in args or "--fights" in args or "--seeds" in args):
            kind = "sim"
        elif os.path.basename(args[0]) in ("bash", "sh") and len(args) > 1 and args[1].endswith(".sh") and "tune" in args[1]:
            kind = "wrapper"
        if not kind:
            continue
        st = _stat(pid)
        if not st:
            continue
        res.append({"pid": int(pid), "pgid": st["pgid"], "ppid": st["ppid"], "kind": kind, "is_driver": kind == "driver", "cmd": joined,
                    "out": _arg(args, "--out"), "cpu_s": (st["utime"] + st["stime"]) / _CLK,
                    "age_s": max(0.0, now - (boot + st["start"] / _CLK))})
    return res


def group_cpu(procs, pgid):
    return sum(p["cpu_s"] for p in procs if p["pgid"] == pgid)


class CpuMeter:
    """CPU cores in use per process group, from two scans."""
    def __init__(self):
        self.prev = {}
        self.t = time.time()

    def update(self, procs):
        now = time.time()
        dt = max(1e-3, now - self.t)
        cur = {}
        for p in procs:
            cur[p["pgid"]] = cur.get(p["pgid"], 0.0) + p["cpu_s"]
        rate = {g: max(0.0, (cur[g] - self.prev.get(g, cur[g])) / dt) for g in cur}
        self.prev, self.t = cur, now
        return rate


def stop_group(pgid):
    """SIGTERM the whole process group of a run. Refuses our own group and pgid <= 1."""
    if pgid <= 1 or pgid == os.getpgrp():
        raise ValueError("refusing to signal pgid %s" % pgid)
    os.killpg(pgid, signal.SIGTERM)
