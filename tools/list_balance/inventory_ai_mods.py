#!/usr/bin/env python3
"""list_balance/inventory_ai_mods.py -- which ENABLED mods of an MO2 list can change enemy combat AI outside the
plugin records: SKSE DLLs (with their INI / TOML / JSON settings), behaviour and animation frameworks, combat scripts.

READ-ONLY (directory listings and small text files opened 'rb'). Output: a text report on stdout, and
~/.cache/mfo-list-balance/<name>.aimods.json (never committed).

usage: inventory_ai_mods.py --list /mnt/gaming/modlists/LoreRim --profile Ultra [--grep word,word]
"""
import argparse, json, os, re, sys

# Name fragments that mark a mod as combat-relevant (DLL names, folder names). Deliberately broad: the report is read by a person.
HINT = re.compile(r'precision|valhalla|combat|block|dodge|parry|stagger|poise|attack|scar|skysa|abr|mco|bfco|ads|distar|tk.?dodge|tudm|'
                  r'dmco|ultimate|wildcat|blade.?and.?blunt|vigor|valravn|smoothcam|true.?directional|tdm|lock|maxsu|elden|'
                  r'npc|enemy|ai\b|behaviou?r|nemesis|pandora|open.?animation|oar|dar|payload|hitstop|recoil|stamina|'
                  r'requiem|simon|know.?your.?enemy|kye|resist|armor|damage|spell|magic|cast|archery|projectile|arrow|'
                  r'speed|movement|sprint|retreat|flee|morale|aggro|detect|stealth|sneak|taunt|follower|nff|aft|swiftly', re.I)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--list', required=True); ap.add_argument('--profile', required=True); ap.add_argument('--name'); ap.add_argument('--grep', default='')
    a = ap.parse_args(); name = a.name or os.path.basename(a.list.rstrip('/'))
    prof = os.path.join(a.list, 'profiles', a.profile)
    mods = [l[1:] for l in open(os.path.join(prof, 'modlist.txt'), 'rb').read().decode('utf-8', 'replace').splitlines() if l.startswith('+')]
    rows = []
    for m in mods:
        root = os.path.join(a.list, 'mods', m); sk = os.path.join(root, 'SKSE', 'Plugins')
        try: files = os.listdir(sk)
        except OSError: files = []
        dlls = [f for f in files if f.lower().endswith('.dll')]
        cfgs = [f for f in files if f.lower().endswith(('.ini', '.toml', '.json', '.yaml')) and os.path.isfile(os.path.join(sk, f))]
        behav = os.path.isdir(os.path.join(root, 'meshes', 'actors', 'character', 'behaviors')) or os.path.isdir(os.path.join(root, 'Nemesis_Engine')) or os.path.isdir(os.path.join(root, 'Pandora_Engine'))
        if not dlls and not behav: continue
        hit = HINT.search(m) or any(HINT.search(d) for d in dlls)
        rows.append(dict(mod=m, dlls=dlls, cfgs=cfgs, behaviour=behav, hint=bool(hit), dir=sk))
    out = os.path.expanduser('~/.cache/mfo-list-balance/%s.aimods.json' % name)
    json.dump(rows, open(out, 'w'), indent=1)
    words = [w for w in a.grep.split(',') if w]
    print('%s / %s: %d enabled mods, %d with an SKSE DLL or behaviour files, %d match a combat hint' % (name, a.profile, len(mods), len(rows), sum(r['hint'] for r in rows)))
    for r in rows:
        if not r['hint']: continue
        if words and not any(w.lower() in (r['mod'] + ' '.join(r['dlls'])).lower() for w in words): continue
        print('  %-64s %s%s  cfg: %s' % (r['mod'][:64], ', '.join(r['dlls'])[:70], ' [behaviour]' if r['behaviour'] else '', ', '.join(r['cfgs'])[:90]))
    print(out)


if __name__ == '__main__':
    main()
