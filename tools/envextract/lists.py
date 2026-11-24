"""Resolve a plugin set (Vanilla or an MO2 list+profile) to an ordered plugin list and file locations.
Read-only.  The Data folder comes from the list's own ModOrganizer.ini (gamePath), never a filesystem search."""
import os, re

MODLISTS = '/mnt/gaming/modlists'
SETS = {   # name -> (list root, profile or None for vanilla-only)
    'Vanilla': ('LoreRim', None),
    'LoreRim': ('LoreRim', 'Ultra'),
    'Tuxborn': ('Tuxborn', 'Tuxborn - Deck'),
}
BASE = ['Skyrim.esm', 'Update.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm']


def game_path(root):
    """gamePath of root/ModOrganizer.ini as a Linux path."""
    with open(os.path.join(root, 'ModOrganizer.ini'), encoding='utf-8', errors='replace') as f: txt = f.read()
    m = re.search(r'^gamePath\s*=\s*(.+)$', txt, re.M)
    if not m: raise SystemExit('no gamePath in ModOrganizer.ini of ' + root)
    v = m.group(1).strip()
    v = re.sub(r'^@ByteArray\((.*)\)$', r'\1', v)
    v = re.sub(r'^[A-Za-z]:', '', v)
    v = re.sub(r'[\\/]+', '/', v)
    return v


def resolve(listroot, profile, base_only=False):
    """-> (order [plugin file names, load order], where {lower name: path}, info dict)"""
    gp = game_path(listroot); data = os.path.join(gp, 'Data')
    where = {}
    dirs = []
    if profile and not base_only:
        prof = os.path.join(listroot, 'profiles', profile)
        mods = [l[1:].rstrip('\r') for l in open(os.path.join(prof, 'modlist.txt'), encoding='utf-8', errors='replace').read().splitlines()
                if l.startswith('+')]
        dirs = [os.path.join(listroot, 'overwrite')] + [os.path.join(listroot, 'mods', m) for m in mods]   # file order = priority, top wins
    dirs.append(data)
    for d in dirs:
        try: names = os.listdir(d)
        except OSError: continue
        for nm in names:
            if nm.lower().endswith(('.esp', '.esm', '.esl')): where.setdefault(nm.lower(), os.path.join(d, nm))
    implicit = list(BASE)
    ccc = os.path.join(gp, 'Skyrim.ccc')
    if not base_only and os.path.isfile(ccc):
        implicit += [l.strip() for l in open(ccc, 'rb').read().decode('utf-8', 'replace').splitlines() if l.strip()]
    order = [m for m in implicit if m.lower() in where]
    if profile and not base_only:
        pl = os.path.join(listroot, 'profiles', profile, 'plugins.txt')
        have = {o.lower() for o in order}
        for l in open(pl, encoding='utf-8', errors='replace').read().splitlines():
            if l.startswith('*'):
                n = l[1:].strip()
                if n.lower() not in have and n.lower() in where: order.append(n); have.add(n.lower())
    return order, where, dict(gamePath=gp, data=data, profile=profile)


def resolve_set(name):
    root, prof = SETS[name]
    return resolve(os.path.join(MODLISTS, root), prof, base_only=(prof is None))
