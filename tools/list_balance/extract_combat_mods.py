#!/usr/bin/env python3
"""Reads the settings of the combat-rule SKSE mods the simulator models from an MO2 list (READ-ONLY) and writes
<out>/<name>.combatmods.json beside the list's .sim.json. Test input only, never committed, never read by native/.

usage: extract_combat_mods.py --list /mnt/gaming/modlists/LoreRim --profile Ultra [--name LoreRim] [--out ~/.cache/mfo-list-balance]
       extract_combat_mods.py --none --name Vanilla        (writes an empty file: no such mod)

Modelled today (Docs/research/2026-11/iaus-sim-v2.md section 21):
  Chocolate Poise        ChocolatePoise.dll + .ini + .json  (formula: the mod's own source, doodlum/skyrim-poise)
  Dynamic Follower Scaling  FollowerScaling.dll + .ini       (formula: the mod's own description + its hit hook)
A mod counts only when its DLL is in an ENABLED mod of the profile; a settings file is taken from the enabled mod with
the highest priority that carries it (modlist.txt lists the highest priority first).
"""
import argparse
import configparser
import json
import os
import re
import sys


def enabled_mods(list_dir, profile):
    path = os.path.join(list_dir, "profiles", profile, "modlist.txt")
    mods = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith("+"):
                mods.append(line[1:])
    return mods   # highest priority first


def winner(list_dir, mods, rel):
    for m in mods:
        p = os.path.join(list_dir, "mods", m, rel)
        if os.path.isfile(p):
            return p, m
    return None, None


def read_ini(path):
    cp = configparser.ConfigParser(strict=False, inline_comment_prefixes=(";",), comment_prefixes=(";", "#"))
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        cp.read_file(f)
    return cp


def loose_json(path):
    t = open(path, encoding="utf-8-sig", errors="replace").read()
    try:
        return json.loads(t)
    except ValueError:
        return json.loads(re.sub(r",(\s*[}\]])", r"\1", t))   # the shipped file has trailing commas


def fget(cp, section, key, default):
    try:
        return float(cp.get(section, key).split()[0])
    except Exception:
        return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list")
    ap.add_argument("--profile")
    ap.add_argument("--name")
    ap.add_argument("--none", action="store_true")
    ap.add_argument("--out", default=os.path.expanduser("~/.cache/mfo-list-balance"))
    a = ap.parse_args()
    name = a.name or (os.path.basename(a.list.rstrip("/")) if a.list else None)
    if not name:
        sys.exit("--name is needed with --none")
    out = {"list": name, "poise": {"present": False}, "followerScaling": {"present": False}, "sources": {}}
    if not a.none:
        mods = enabled_mods(a.list, a.profile)
        dll, m = winner(a.list, mods, "SKSE/Plugins/ChocolatePoise.dll")
        if dll:
            ini, mi = winner(a.list, mods, "SKSE/Plugins/ChocolatePoise.ini")
            js, mj = winner(a.list, mods, "SKSE/Plugins/ChocolatePoise.json")
            cp = read_ini(ini) if ini else configparser.ConfigParser()
            j = loose_json(js) if js else {}
            mag = j.get("Magic Effects", {}).get("Actor Values", {}).get("Damage", {})
            out["poise"] = {
                "present": True,
                "staggerMode": int(fget(cp, "Settings", "StaggerMode", 1)),
                "baseMult": fget(cp, "Health", "BaseMult", 50.0),
                "armorMult": fget(cp, "Health", "ArmorMult", 0.2),
                "regenRate": fget(cp, "Health", "RegenRate", 0.045),
                "bash": fget(cp, "Damage", "BashMult", 1.5),
                "bow": fget(cp, "Damage", "BowMult", 1.0),
                "creature": fget(cp, "Damage", "CreatureMult", 2.5),
                "melee": fget(cp, "Damage", "MeleeMult", 1.0),
                "unarmed": fget(cp, "Damage", "UnarmedMult", 3.0),
                "toPC": fget(cp, "Damage", "ToPCMult", 1.0),
                "toNPC": fget(cp, "Damage", "ToNPCMult", 1.0),
                "weight": fget(cp, "Damage", "WeightContribution", 2.0),
                "races": j.get("Races", {}),
                "weapons": j.get("Weapons", {}).get("Damage", {}),
                "magicHealth": float(mag.get("Health", 0.0)),
            }
            out["sources"]["poise"] = {"dll": m, "ini": mi, "json": mj}
        dll, m = winner(a.list, mods, "SKSE/Plugins/FollowerScaling.dll")
        if dll:
            ini, mi = winner(a.list, mods, "SKSE/Plugins/FollowerScaling.ini")
            cp = read_ini(ini) if ini else configparser.ConfigParser()
            out["followerScaling"] = {
                "present": True,
                "mode": int(fget(cp, "Follower Scaling", "Mode", 1)),
                "reduction": fget(cp, "Follower Scaling", "ReductionPerFollower", 33.0),
            }
            out["sources"]["followerScaling"] = {"dll": m, "ini": mi}
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, name + ".combatmods.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    p, s = out["poise"], out["followerScaling"]
    print(f"{path}: poise {'ON base %.1f regen %.3f weight %.2f races %d' % (p['baseMult'], p['regenRate'], p['weight'], len(p['races'])) if p['present'] else 'absent'}; "
          f"follower scaling {'ON mode %d reduction %.0f%%' % (s['mode'], s['reduction']) if s['present'] else 'absent'}")


if __name__ == "__main__":
    main()
