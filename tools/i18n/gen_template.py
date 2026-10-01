#!/usr/bin/env python3
"""Generate out/Interface/Translations/MFO_ENGLISH.txt from native/i18n/Strings_keys.h.

The template is the translator's starting point: every key, the English text,
and the hint comment. It is UTF-16 LE with a BOM and CRLF line ends, the same
format the game and MCM Helper read. Lines that do not start with '$' are
comments and are ignored by the game.

    python3 tools/i18n/gen_template.py          # write the template
    python3 tools/i18n/gen_template.py --check  # exit 1 if the file is stale or the key list is broken

Run it whenever Strings_keys.h changes. CI-free on purpose: --check is cheap
enough to run by hand or in a hook.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEYS = os.path.join(ROOT, "native", "i18n", "Strings_keys.h")
OUT = os.path.join(ROOT, "out", "Interface", "Translations", "MFO_ENGLISH.txt")

ENTRY = re.compile(r'^MFO_STR\(\s*([A-Za-z0-9_]+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)\s*\)\s*(?://\s*(.*))?$')
GROUP = re.compile(r'^//\s*==\s*(.*?)\s*==\s*$')


def unescape_c(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, n))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def escape_txt(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n")


def build():
    lines, seen, errors = [], set(), []
    lines.append("; MFO translation template. Copy this file to MFO_<LANGUAGE>.txt (for example MFO_FRENCH.txt),")
    lines.append("; save it as UTF-16 LE with BOM, and replace the text after each TAB. Docs/TRANSLATING.md has the rules.")
    lines.append("; Keep every {1} {2} marker. Keep a line out and the English text is used for it.")
    lines.append("; Write a line break as \\n. Lines starting with ; are notes and are ignored.")
    n = 0
    for ln, raw in enumerate(open(KEYS, encoding="utf-8"), 1):
        raw = raw.rstrip("\n")
        g = GROUP.match(raw)
        if g:
            lines += ["", "; ==== " + g.group(1) + " ===="]
            continue
        if not raw.startswith("MFO_STR("):
            continue
        m = ENTRY.match(raw)
        if not m:
            errors.append(f"{KEYS}:{ln}: cannot parse entry: {raw[:80]}")
            continue
        key, en, argc, hint = m.group(1), unescape_c(m.group(2)), int(m.group(3)), m.group(4)
        if key in seen:
            errors.append(f"{KEYS}:{ln}: duplicate key {key}")
        seen.add(key)
        if en != en.strip():
            errors.append(f"{KEYS}:{ln}: {key}: leading or trailing space in the value (the game may trim it, so put the space in code)")
        idx = [int(x) for x in re.findall(r"\{(\d+)\}", en)]
        stripped = re.sub(r"\{\d+\}", "", en)
        if "{" in stripped or "}" in stripped:
            errors.append(f"{KEYS}:{ln}: {key}: stray brace in default")
        if idx and (min(idx) < 1 or max(idx) != argc):
            errors.append(f"{KEYS}:{ln}: {key}: argc={argc} but default uses {sorted(set(idx))}")
        if not idx and argc:
            errors.append(f"{KEYS}:{ln}: {key}: argc={argc} but default has no placeholders")
        if hint:
            lines.append("; " + hint.strip())
        lines.append(f"$MFO_{key}\t{escape_txt(en)}")
        n += 1
    return lines, n, errors


def main():
    lines, n, errors = build()
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    data = b"\xff\xfe" + ("\r\n".join(lines) + "\r\n").encode("utf-16-le")
    if "--check" in sys.argv:
        cur = open(OUT, "rb").read() if os.path.exists(OUT) else b""
        if cur != data:
            print(f"{OUT} is stale: run tools/i18n/gen_template.py", file=sys.stderr)
            return 1
        print(f"ok: {n} keys, template current")
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(data)
    print(f"wrote {OUT}: {n} keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
