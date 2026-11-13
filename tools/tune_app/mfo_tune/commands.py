"""Command builders for the driver and the simulator. Pure functions: a dict of options -> argv list; the GUI shows `shell(argv)` before launch."""
import os
import shlex
import subprocess


def shell(argv):
    return " ".join(shlex.quote(a) for a in argv)


def driver_argv(o, settings):
    """o keys: out, bin, smoke, for_class, classes[], stages[], worlds[], knobs, no_race, no_emit_runs, rejudge, sim_flags, jobs, builtin_worth,
    extra (free text), taskset (str or ''), nice (int or None)."""
    py = os.path.join(settings["repo"], "tools", "iaus_sim", "tune_driver.py")
    a = ["python3", py, "--bin", o["bin"], "--out", o["out"]]
    if o.get("smoke"):
        a.append("--smoke")
    if o.get("for_class"):
        a += ["--for-class", o["for_class"]]          # sets classes + stages itself: the driver refuses both together
    else:
        if o.get("classes"):
            a += ["--classes", ",".join(o["classes"])]
        if o.get("stages"):
            a += ["--stages", ",".join(o["stages"])]
    if o.get("worlds"):
        a += ["--worlds", ",".join(o["worlds"])]
    if o.get("knobs"):
        a += ["--knobs", o["knobs"]]
    if o.get("rejudge") is not None and o.get("rejudge_set"):
        a += ["--rejudge", o["rejudge"]]
    if o.get("sim_flags"):
        a += ["--sim-flags", o["sim_flags"]]
    if o.get("builtin_worth"):
        a += ["--builtin-worth", o["builtin_worth"]]
    if o.get("jobs"):
        a += ["--jobs", str(o["jobs"])]
    if o.get("no_race"):
        a.append("--no-race")
    if o.get("no_emit_runs"):
        a.append("--no-emit-runs")
    if o.get("extra"):
        a += shlex.split(o["extra"])
    return wrap(a, o)


def wrap(a, o):
    pre = []
    if o.get("nice") is not None:
        pre += ["nice", "-n", str(o["nice"])]
    if o.get("taskset"):
        pre = ["taskset", "-c", o["taskset"]] + pre
    return pre + a


def sim_argv(o, settings):
    """o keys: bin, list (path), classes[], scenario[] , oracle, oracle_k, oracle_h, seed, seeds, fights, threads, curves[], sets, class_oracle,
    gap_scenarios, rating, decline, label, json_path, flags (free text), taskset, nice."""
    a = [o["bin"]]
    if o.get("list"):
        a += ["--list", o["list"]]
    if o.get("classes"):
        a += ["--classes", ",".join(o["classes"])]
    if o.get("scenario"):
        a += ["--scenario", ",".join(o["scenario"])]
    if o.get("oracle"):
        a.append("--oracle")
        if o.get("oracle_k"):
            a += ["--oracle-k", str(o["oracle_k"])]
        if o.get("oracle_h"):
            a += ["--oracle-h", str(o["oracle_h"])]
    if o.get("seeds"):
        a += ["--seeds", o["seeds"]]
    elif o.get("seed"):
        a += ["--seed", str(o["seed"])]
    if o.get("fights"):
        a += ["--fights", str(o["fights"])]
    if o.get("threads"):
        a += ["--threads", str(o["threads"])]
    for c in o.get("curves", []):
        a += ["--curve", c]
    if o.get("sets"):
        a += ["--set", o["sets"]]
    if o.get("class_oracle") is not None and o.get("class_oracle") != "":
        a += ["--class-oracle", str(o["class_oracle"])]
    if o.get("gap_scenarios") is not None and o.get("gap_scenarios") != "":
        a += ["--gap-scenarios", str(o["gap_scenarios"])]
    if o.get("rating"):
        a += ["--rating", str(o["rating"])]
    if o.get("decline"):
        a += ["--decline", str(o["decline"])]
    if o.get("label"):
        a += ["--label", o["label"]]
    if o.get("json_path"):
        a += ["--json", o["json_path"]]
    if o.get("flags"):
        a += shlex.split(o["flags"])
    return wrap(a, o)


def launch_detached(argv, log_path, cwd=None):
    """setsid + nohup semantics: the child is its own session/process group, stdout+stderr to log_path, and survives the app. -> pid (the
    session leader, equal to its pgid)."""
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    with open(log_path, "ab") as lf:
        p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=lf, stderr=subprocess.STDOUT, cwd=cwd, start_new_session=True, close_fds=True)
    return p.pid


def build_sim(settings, out_bin):
    """tools/iaus_sim/run.sh builds into SIM_OUT then runs the sim; SIM_CLITEST=1 would run assertions instead of a run, so instead we build with
    `--print-classes` (cheap) and leave the binary at SIM_OUT. Returns (rc, combined output)."""
    run_sh = os.path.join(settings["repo"], "tools", "iaus_sim", "run.sh")
    env = dict(os.environ, SIM_OUT=out_bin)
    p = subprocess.run(["bash", run_sh, "--print-classes"], capture_output=True, text=True, env=env, cwd=settings["repo"])
    return p.returncode, (p.stdout or "")[-2000:] + (p.stderr or "")[-6000:]
