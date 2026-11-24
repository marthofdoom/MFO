#!/usr/bin/env python3
"""iaus_sim/live/run_round.py -- one tune round, start to tuning files, STOP / RESUME SAFE, at idle priority by default.

usage: run_round.py --out <round dir> [--priority idle|low|normal|high] [--bin <sim>] [--dest <dir>] [--label <text>] [--no-generate]
                    [--unshipped] [--] <tune_driver.py arguments, without --bin and --out>

  example   run_round.py --out /mnt/.../tune-rounds/r3 -- --cells all --worlds builtin,LoreRim,Tuxborn,Vanilla
  STOP      SIGTERM or Ctrl-C to this process (or `systemctl --user stop $(cat <out>/unit)`): the whole round stops, the simulators too.
  RESUME    run THE SAME COMMAND again. Every simulator result the stopped round finished is in <out>/cache.jsonl and is not run again;
            the result is the one an uninterrupted round gives (test_live.py proves it on a small round).

WHAT IT DOES, IN ORDER
  1. <out>/round.json holds the driver arguments of the round. A resume with OTHER arguments is refused (a round is one question):
     use a new --out, or --new-args to say the change is meant.
  2. The simulator: --bin, or <out>/bin/sim, built ONCE by tools/iaus_sim/run.sh (the cache is keyed by the binary's hash, so a round keeps
     its binary; delete <out>/bin to rebuild on purpose).
  3. A cache.jsonl that a kill cut off mid-line gets its newline, so the next result does not join the broken line.
  4. The driver and every simulator it starts run inside ONE systemd user scope, `mfo-tune-<out name>-<6 hex of its path>`, whose name is
     written to <out>/unit. Its priority is a LEVEL of priority.py (the one table): chosen by --priority, else the level remembered in
     <out>/priority, else idle. It can be changed while the round runs: `priority.py <out> <level>` (the tune app's drop-down calls the
     same function). Nothing sets nice, chrt or ionice.
     NO SYSTEMD USER MANAGER: the round runs at nice 19 in its own process group and says loudly that live change is off (<out>/unit = none).
  5. When the driver ends with 0: gen_class_files.py writes <dest> (default <out>/MFO_Tuning). Its refusals are this command's exit code 2.
  6. <out>/round-status.json: state (running / stopped / failed / done / refused), the unit, the level, times, the driver's exit code.

Exit codes: 0 done, 2 the generator refused, 130 / 143 stopped by a signal, else the driver's.
"""
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIM_DIR = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import priority as P  # noqa: E402


def say(msg):
    print("run_round: %s" % msg, flush=True)


def unit_name(out):
    full = os.path.abspath(out)
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.basename(full))[:60] or "round"
    return "mfo-tune-%s-%s.scope" % (safe, hashlib.sha256(full.encode()).hexdigest()[:6])


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1)
    os.replace(tmp, path)


def status(out, **kw):
    p = os.path.join(out, "round-status.json")
    try:
        st = json.load(open(p))
    except (OSError, ValueError):
        st = {}
    st.update(kw)
    st["updated"] = time.strftime("%Y-%m-%d %H:%M:%S")
    write_json(p, st)


def split_args(argv):
    """(own arguments, driver arguments): everything after a bare `--`, or after the last own option, is the driver's."""
    if "--" in argv:
        i = argv.index("--")
        return argv[:i], argv[i + 1:]
    own, i, takes = [], 0, {"--out", "--priority", "--bin", "--dest", "--label"}
    flags = {"--no-generate", "--unshipped", "--new-args", "--accept-log-mismatch"}
    while i < len(argv):
        if argv[i] in takes and i + 1 < len(argv):
            own += argv[i:i + 2]
            i += 2
        elif argv[i] in flags:
            own.append(argv[i])
            i += 1
        else:
            break
    return own, argv[i:]


def fix_cache_tail(out):
    p = os.path.join(out, "cache.jsonl")
    if os.path.exists(p) and os.path.getsize(p) > 0:
        with open(p, "rb+") as f:
            f.seek(-1, os.SEEK_END)
            if f.read(1) != b"\n":
                f.write(b"\n")
                say("cache.jsonl ended mid-line (a kill): closed the line; that one result runs again")


def ensure_bin(out, given):
    if given:
        if not os.access(given, os.X_OK):
            raise SystemExit("run_round: --bin %s is not an executable" % given)
        return os.path.abspath(given)
    b = os.path.join(os.path.abspath(out), "bin", "sim")
    if not os.access(b, os.X_OK):
        say("building the simulator once -> %s" % b)
        os.makedirs(os.path.dirname(b), exist_ok=True)
        env = dict(os.environ, SIM_OUT=b)
        r = subprocess.run([os.path.join(SIM_DIR, "run.sh"), "--print-classes"], env=env, stdout=subprocess.DEVNULL)
        if r.returncode != 0 or not os.access(b, os.X_OK):
            raise SystemExit("run_round: the simulator did not build (tools/iaus_sim/run.sh, exit %d)" % r.returncode)
    return b


def main(argv):
    own, drv = split_args(argv[1:])
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--priority", choices=P.ORDER)
    ap.add_argument("--bin")
    ap.add_argument("--dest")
    ap.add_argument("--label", default="")
    ap.add_argument("--no-generate", action="store_true")
    ap.add_argument("--unshipped", action="store_true")
    ap.add_argument("--accept-log-mismatch", action="store_true")
    ap.add_argument("--new-args", action="store_true", help="this resume changes the driver arguments on purpose")
    a = ap.parse_args(own)
    if any(x in ("--bin", "--out") or x.startswith(("--bin=", "--out=")) for x in drv):
        raise SystemExit("run_round: the driver arguments may not name --bin or --out (this command owns both)")
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)

    # 1. one round = one set of driver arguments
    rj = os.path.join(out, "round.json")
    if os.path.exists(rj):
        try:
            old = json.load(open(rj)).get("driver_args")
        except ValueError:
            old = None
        if old is not None and old != drv and not a.new_args:
            raise SystemExit("run_round: %s was started with other driver arguments:\n  then: %s\n  now:  %s\nResume with the same command, use a new --out, or pass --new-args."
                             % (out, " ".join(old), " ".join(drv)))
    write_json(rj, {"driver_args": drv, "started_or_resumed": time.strftime("%Y-%m-%d %H:%M:%S")})

    unit = unit_name(out)
    if P.unit_active(P.unit_of(out)) or P.unit_active(unit):
        raise SystemExit("run_round: this round is already running (unit %s). Stop it first: systemctl --user stop %s" % (unit, unit))

    binary = ensure_bin(out, a.bin)
    fix_cache_tail(out)

    # 4. the level: asked, else remembered, else the default
    level = a.priority or P.get(out)["level"]
    cmd = [sys.executable, os.path.join(SIM_DIR, "tune_driver.py"), "--bin", binary, "--out", out] + drv
    use_systemd = P.have_systemd()
    if use_systemd:
        with open(os.path.join(out, "unit"), "w") as f:
            f.write(unit + "\n")
        P.set_level(out, level)   # remembered; the unit is not up yet, the -p flags below apply it
        full = ["systemd-run", "--user", "--scope", "--quiet", "--collect", "--unit=" + unit, "--description=MFO tune round " + out]
        for p in P.props(level):
            full += ["-p", p]
        full += cmd
        child = subprocess.Popen(full)
        say("round %s: unit %s, priority %s (%s); change it live with: %s %s <%s>" % (out, unit, level, " ".join(P.props(level)), os.path.join(HERE, "priority.py"), out, "|".join(P.ORDER)))
    else:
        with open(os.path.join(out, "unit"), "w") as f:
            f.write("none\n")
        with open(os.path.join(out, "priority"), "w") as f:
            f.write(level + "\n")
        say("=" * 100)
        say("NO SYSTEMD USER MANAGER: the round runs at nice 19 and its PRIORITY CANNOT BE CHANGED LIVE (the level %r is only remembered)." % level)
        say("=" * 100)
        child = subprocess.Popen(cmd, start_new_session=True, preexec_fn=lambda: os.nice(19))
    status(out, state="running", unit=unit if use_systemd else "none", priority=level, pid=child.pid, started=time.strftime("%Y-%m-%d %H:%M:%S"), driver_exit=None, driver_args=drv, bin=binary)

    stopped = {"sig": 0}

    def stop(sig, _frame):
        if stopped["sig"]:
            return
        stopped["sig"] = sig
        say("signal %d: stopping the round (every simulator of it)" % sig)
        if use_systemd:
            subprocess.run(["systemctl", "--user", "stop", unit], capture_output=True)
        else:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    rc = child.wait()
    if stopped["sig"]:
        status(out, state="stopped", driver_exit=rc, stopped=time.strftime("%Y-%m-%d %H:%M:%S"))
        say("STOPPED. Resume with the same command.")
        return 128 + stopped["sig"]
    if rc != 0:
        status(out, state="failed", driver_exit=rc)
        say("the driver ended with exit %d (its message is above; <out>/progress.log has the last stage). Fix the cause and run the same command: finished results are kept." % rc)
        return rc
    if a.no_generate:
        status(out, state="done", driver_exit=0, generated=None)
        say("DONE (no tuning files asked: --no-generate)")
        return 0
    gen = [sys.executable, os.path.join(HERE, "gen_class_files.py"), "--out", out] + (["--dest", a.dest] if a.dest else []) + (["--label", a.label] if a.label else []) \
        + (["--unshipped"] if a.unshipped else []) + (["--accept-log-mismatch"] if a.accept_log_mismatch else [])
    g = subprocess.run(gen)
    status(out, state="done" if g.returncode == 0 else "refused", driver_exit=0, generated=a.dest or os.path.join(out, "MFO_Tuning") if g.returncode == 0 else None, generator_exit=g.returncode)
    say("DONE" if g.returncode == 0 else "the round finished and the generator REFUSED to write tuning files (its reason is above)")
    return g.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
