#!/usr/bin/env python3
"""MCM consistency gate -- run beside audit_esp.py; a FAIL stops the release.

Bug #55 ("a new toggle draws as an empty/unresponsive checkbox on an existing
save") was, at root, a MISSING DEFAULTS FILE: MCM Helper REGISTERS every
ModSetting from MCM/Config/<mod>/settings.ini at plugin-load, and MFO shipped
only the mutable user store MCM/Settings/MFO.ini (which MO2 shadows with a stale
overwrite). The fix ships the defaults file; THIS gate makes sure it -- and every
other place a toggle is wired -- can never drift out of sync again.

A MCM ModSetting is wired in FIVE places; all must agree or the control is dead:
  C  out/MCM/Config/MFO/config.json   -- the control (id="key:Section", sourceType)
  D  out/MCM/Config/MFO/settings.ini  -- DEFAULTS (what MCM Helper registers from)
  U  out/MCM/Settings/MFO.ini         -- seed USER store (fresh-install values)
  H  native/Config.cpp kMcmDefaults[] -- the self-heal table
  P  native/Config.cpp ReadFile switch-- a_key=="key" + the setB/setI/setF setter

Exit non-zero with one line per violation. No args: audits the repo's out/ tree.
"""
import json, re, sys, struct  # noqa: F401
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG_JSON  = ROOT / "out/MCM/Config/MFO/config.json"
DEFAULTS_INI = ROOT / "out/MCM/Config/MFO/settings.ini"
USER_INI     = ROOT / "out/MCM/Settings/MFO.ini"
CONFIG_CPP   = ROOT / "native/Config.cpp"

# Keys that legitimately live in an ini store but are NOT MCM controls (seed- or
# dev-only). Orphan check exempts these. Add here when you intentionally add one.
STORE_ALLOWLIST = set()

errors = []
def fail(msg): errors.append(msg)


def parse_ini(path):
    """{section: {key: raw_value_str}} -- tolerant of MCM Helper's UTF-8 BOM."""
    out, section = {}, None
    text = path.read_text(encoding="utf-8-sig")  # utf-8-sig strips the BOM
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        m = re.match(r"\[(.+)\]$", line)
        if m:
            section = m.group(1); out.setdefault(section, {}); continue
        if "=" in line and section is not None:
            k, v = line.split("=", 1)
            out[section][k.strip()] = v.strip()
    return out


def walk_controls(node, acc):
    """Every config.json item that is a ModSetting-bound control."""
    if isinstance(node, dict):
        vo = node.get("valueOptions")
        if node.get("id") and isinstance(vo, dict) and str(vo.get("sourceType", "")).startswith("ModSetting"):
            acc.append(node)
        for v in node.values():
            walk_controls(v, acc)
    elif isinstance(node, list):
        for v in node:
            walk_controls(v, acc)


def prefix_of(key):
    return key[0] if key and key[0] in "bifs" else "?"


# sourceType <-> Hungarian prefix <-> Config.cpp setter
SRC_BY_PREFIX = {"b": "ModSettingBool", "i": "ModSettingInt",
                 "f": "ModSettingFloat", "s": "ModSettingString"}
SETTER_BY_PREFIX = {"b": "setB", "i": "setI", "f": "setF", "s": "setS"}


def val_eq(prefix, a, b):
    """Type-aware equality between two raw strings (or a JSON scalar + string)."""
    try:
        if prefix == "b":
            return (int(float(str(a))) != 0) == (int(float(str(b))) != 0)
        if prefix == "i":
            return int(float(str(a))) == int(float(str(b)))
        if prefix == "f":
            return abs(float(a) - float(b)) < 1e-6
        return str(a) == str(b)
    except (ValueError, TypeError):
        return False


def main():
    for p in (CONFIG_JSON, DEFAULTS_INI, USER_INI, CONFIG_CPP):
        if not p.exists():
            print(f"FAIL: missing {p.relative_to(ROOT)}", file=sys.stderr)
            return 1

    cfg = json.loads(CONFIG_JSON.read_text(encoding="utf-8-sig"))
    controls = []
    walk_controls(cfg, controls)

    D = parse_ini(DEFAULTS_INI)
    U = parse_ini(USER_INI)
    cpp = CONFIG_CPP.read_text(encoding="utf-8")

    # H: kMcmDefaults[] pairs { "key", "val" }
    hm = re.search(r"kMcmDefaults\[\]\s*=\s*\{(.+?)\};", cpp, re.S)
    H = dict(re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\}', hm.group(1))) if hm else {}
    if not hm:
        fail("kMcmDefaults[] table not found in native/Config.cpp")

    # P: parse switch keys + their setter (the line usually reads
    #    else if (a_key == "key")  setX(g_...);)
    P = {}
    for km, setter in re.findall(r'a_key\s*==\s*"([^"]+)"\s*\)\s*(setB|setI|setF|setS)?', cpp):
        P[km] = setter or P.get(km)

    d_flat = {k for sec in D.values() for k in sec}
    u_flat = {k for sec in U.values() for k in sec}
    seen_keys = set()

    for c in controls:
        cid = c["id"]
        if cid.count(":") != 1:
            fail(f"{cid} | id must be exactly 'key:Section'"); continue
        key, section = cid.split(":")
        seen_keys.add(key)
        pfx = prefix_of(key)
        vo = c["valueOptions"]
        src = vo.get("sourceType", "")
        dv = vo.get("defaultValue")

        # (1) THE #55 INVARIANT: control must exist in the DEFAULTS file, same section.
        if section not in D or key not in D[section]:
            fail(f"{key} | NOT in defaults file [{section}] (settings.ini) -> will not REGISTER (#55)")
        # (2) seed user store, same section
        if section not in U or key not in U[section]:
            fail(f"{key} | NOT in seed user store [{section}] (Settings/MFO.ini)")
        # (3) heal table
        if key not in H:
            fail(f"{key} | NOT in kMcmDefaults[] heal table (Config.cpp)")
        # (4) parse switch
        if key not in P:
            fail(f"{key} | no a_key==\"{key}\" branch in Config.cpp ReadFile -> control is never read")
        # (6) sourceType <-> prefix, and setter <-> prefix
        if pfx in SRC_BY_PREFIX and src != SRC_BY_PREFIX[pfx]:
            fail(f"{key} | sourceType {src} != {SRC_BY_PREFIX[pfx]} expected for '{pfx}' prefix")
        if key in P and P[key] and P[key] != SETTER_BY_PREFIX.get(pfx):
            fail(f"{key} | parse setter {P[key]} != {SETTER_BY_PREFIX.get(pfx)} expected for '{pfx}' prefix")
        # (7) section must be a real header in the stores
        if section not in D and section not in U:
            fail(f"{key} | section [{section}] not present in either store")
        # (5) default-value agreement across C/D/U/H (type-aware)
        for label, store in (("settings.ini", D.get(section, {})),
                             ("Settings/MFO.ini", U.get(section, {})),
                             ("kMcmDefaults", H)):
            if key in store and dv is not None and not val_eq(pfx, dv, store[key]):
                fail(f"{key} | default {dv!r} (config.json) != {store[key]!r} ({label})")
        # (9) enum bounds
        if c.get("type") == "enum":
            opts = vo.get("options") or []
            try:
                if not (0 <= int(dv) < len(opts)):
                    fail(f"{key} | enum defaultValue {dv} out of range 0..{len(opts)-1}")
            except (ValueError, TypeError):
                fail(f"{key} | enum defaultValue {dv!r} not an index")

    # (8) orphans: a key seeded in a store but bound to no control (dead rot)
    for k in (d_flat | u_flat) - seen_keys - STORE_ALLOWLIST:
        fail(f"{k} | in a store file but NO config.json control (orphan) -- remove it or allowlist")

    print(f"MFO MCM audit -- {len(controls)} control(s), "
          f"{len(d_flat)} default key(s), {len(H)} heal key(s)")
    if errors:
        print(f"\nFAIL ({len(errors)}):", file=sys.stderr)
        for e in sorted(errors):
            print("  " + e, file=sys.stderr)
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
