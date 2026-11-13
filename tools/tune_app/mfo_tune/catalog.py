"""What the sim and the driver accept: class slugs, scenarios, stages, worlds. Read from the sim binary when it can be run (--print-classes, an
unknown --scenario lists the known ones); static fallbacks otherwise."""
import re
import subprocess

STAGES = ["prep", "rejudge", "classes", "joint", "edge", "gap", "globals", "consts", "worth", "floor", "rating", "recalib", "logistics",
          "loot", "summary"]
DEFAULT_STAGES = "prep,rejudge,classes,joint,edge,gap,globals,consts,floor,rating,recalib,logistics,summary"
WORLDS = ["builtin", "LoreRim", "Tuxborn", "Vanilla"]
CLASSES_FALLBACK = ("melee ranged mage knight warrior mercenary archer thief evoker conjurer healer warlock melee-off ranged-off mage-off").split()
BETA1 = CLASSES_FALLBACK
SCENARIOS_FALLBACK = ("tank3 mage-dragon bandit-pack focus-healer interrupt weakness predictive-heal peel overkill double-heal mp-budget splash "
                      "control-setup kite knight-boss knight-boss-healer hurt-knight thief-cover thief-open caster-flee melee-flee ambush "
                      "hit-and-hide shadowcloak flank assassinate").split()


def _run(args, timeout=20):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return (p.stdout or "") + (p.stderr or "")
    except (OSError, subprocess.SubprocessError):
        return ""


def classes(sim_bin):
    out = _run([sim_bin, "--print-classes"])
    cs = []
    for ln in out.splitlines():
        m = re.match(r"^\s+(?:T\d\s+)?(\S+)\s+(melee|ranged|mage)\s", ln)
        if m:
            cs.append(m.group(1))
    return cs or CLASSES_FALLBACK


def scenarios(sim_bin):
    out = _run([sim_bin, "--scenario", "__list__"])
    m = re.search(r"Known:(.*)", out)
    return m.group(1).split() if m else SCENARIOS_FALLBACK


def driver_flags(driver_py):
    """The long options this driver accepts (so a driver older or newer than the app's list greys out the controls it lacks)."""
    out = _run(["python3", driver_py, "--help"])
    return set(re.findall(r"(--[a-z][a-z0-9-]*)", out)) if out else set()
