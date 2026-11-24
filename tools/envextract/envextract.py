#!/usr/bin/env python3
"""envextract: real Skyrim environments (navmesh included) for the IAUS simulator.  See README.md."""
import argparse, json, os, sys, time, pickle
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
CACHE = os.path.expanduser('~/.cache/mfo-env')
SCAN = lambda s: os.path.join(CACHE, '_scan', s + '.pkl')
CANDS = lambda s: os.path.join(CACHE, '_scan', s + '.cands.pkl')


ORDER = []   # load order of the scan in use, so ids read plugin_localid and do not depend on a plugin's position


def _k(key):
    import re
    nm = ORDER[key >> 24] if (key >> 24) < len(ORDER) else 'unknown'
    return '%s-%06X' % (re.sub(r'[^A-Za-z0-9]+', '', os.path.splitext(nm)[0])[:24], key & 0xFFFFFF)


def unit_id(u):
    return 'i_' + _k(u[1]) if u[0] == 'i' else 'x_%s_%d_%d' % (_k(u[1]), u[2], u[3])


def cmd_scan(a):
    import lists, scan
    if a.list:
        order, where, info = lists.resolve(a.list, a.profile, base_only=not a.profile)
    else:
        order, where, info = lists.resolve_set(a.set)
    print('%s: %d plugins, data %s' % (a.set, len(order), info['data']))
    sc = scan.scan_set(order, where, procs=a.procs)
    scan.save(sc, SCAN(a.set)); print('wrote', SCAN(a.set))


def _cands(a, sc):
    import library, classes, scan
    p = CANDS(a.set)
    if os.path.exists(p) and not a.rebuild and os.path.getmtime(p) > os.path.getmtime(SCAN(a.set)):
        return pickle.load(open(p, 'rb'))
    groups, cands = library.build_candidates(sc, procs=a.procs)
    by = library.ref_features(sc, cands)
    cls = {u: classes.classify(f, u[0] == 'i') for u, f in cands.items()}
    pickle.dump((cands, cls), open(p, 'wb'), protocol=4)
    return cands, cls


def _export(a, sc, units, cands, cls, outdir):
    import library, export
    groups = library.make_groups(sc)
    by = library.ref_features(sc, {u: dict(cands[u]) for u in units}, a.min_static)
    os.makedirs(outdir, exist_ok=True); rows = []
    for f in os.listdir(outdir):                      # a rebuilt library must not leave files of the old one behind
        if f.endswith('.mfoenv'): os.remove(os.path.join(outdir, f))
    for u in units:
        path = os.path.join(outdir, '%s.%s.%s.mfoenv' % (a.set, cls[u], unit_id(u)))
        feat = {k: (round(v, 4) if isinstance(v, float) else v) for k, v in cands[u].items()}
        meta = export.export(sc, u, groups, by, cls[u], feat, path, a.set, library.WIN, name=unit_id(u))
        rows.append(dict(file=os.path.relpath(path, CACHE), **{'class': cls[u]}, id=unit_id(u), interior=u[0] == 'i',
                         bytes=os.path.getsize(path), cells=[c['edid'] or c['id'] for c in meta['cells']][:9], features=feat, counts=meta['counts']))
    return rows


def _index_update(a, sc, rows):
    p = os.path.join(CACHE, 'index.json')
    idx = json.load(open(p)) if os.path.exists(p) else {'sets': {}}
    idx['formatVersion'] = 2
    s = idx['sets'].setdefault(a.set, {})
    s['plugins'] = [x['name'] for x in sc['plugins']]
    if a.mode == 'library': s['envs'] = rows
    else:
        old = {r['file']: r for r in s.get('samples', [])}; old.update({r['file']: r for r in rows}); s['samples'] = list(old.values())
    json.dump(idx, open(p, 'w'), indent=1)


def cmd_library(a):
    import scan, select_lib
    sc = scan.load(SCAN(a.set)); ORDER[:] = sc['order']; cands, cls = _cands(a, sc)
    allowed = None
    if a.changed_only:
        nb = 5; allowed = set()
        import library
        for k, (pi, nv) in sc['navm'].items():
            ni = sc['navinfo'].get(k)
            if pi >= nb and ni and (ni[3] or ni[0]): allowed.add(library.unit_of_mesh(sc, k))
        allowed = {u for u in cands if u in allowed or (u[0] == 'x' and any(('x', u[1], u[2] + dx, u[3] + dy) in allowed for dx in (-1, 0, 1) for dy in (-1, 0, 1)))}
        print('changed-only candidates:', len(allowed))
    units = select_lib.select(cands, cls, a.count, a.seed, a.max_tris, allowed)
    out = os.path.join(CACHE, a.set)
    rows = _export(a, sc, units, cands, cls, out)
    _index_update(a, sc, rows)
    print('%d environments in %s' % (len(rows), out))
    for r in rows: print('  %-22s %-34s %8d B  tris %5d refs %4d  %s' % (r['class'], r['id'], r['bytes'], r['counts']['tris'], r['counts']['refs'], ','.join(r['cells'][:2])))


def cmd_sample(a):
    import scan, select_lib
    sc = scan.load(SCAN(a.set)); ORDER[:] = sc['order']; cands, cls = _cands(a, sc)
    idx = json.load(open(os.path.join(CACHE, 'index.json'))) if os.path.exists(os.path.join(CACHE, 'index.json')) else {'sets': {}}
    have = set()
    for r in idx['sets'].get(a.set, {}).get('envs', []):
        for u in cands:
            if unit_id(u) == r['id']: have.add(u)
    units = select_lib.sample(cands, cls, a.cls, a.count, a.seed, have, a.max_tris)
    out = os.path.join(CACHE, a.set, 'sample-%s-%d' % (a.cls, a.seed))
    rows = _export(a, sc, units, cands, cls, out)
    for r in rows: print(os.path.join(CACHE, r['file']))


def cmd_svg(a):
    import svg
    out = a.out or os.path.splitext(a.file)[0] + '.svg'
    svg.render(a.file, out); print(out)


def cmd_validate(a):
    import validate
    sys.exit(validate.run(a))


def main():
    ap = argparse.ArgumentParser(description=__doc__); sub = ap.add_subparsers(dest='cmd', required=True)
    def common(p):
        p.add_argument('--set', default='Vanilla'); p.add_argument('--procs', type=int, default=6)
        p.add_argument('--min-static', type=float, default=256.0); p.add_argument('--rebuild', action='store_true')
    p = sub.add_parser('scan'); common(p); p.add_argument('--list'); p.add_argument('--profile'); p.set_defaults(fn=cmd_scan)
    p = sub.add_parser('library'); common(p); p.add_argument('--count', type=int, default=40); p.add_argument('--seed', type=int, default=1)
    p.add_argument('--max-tris', type=int, default=60000); p.add_argument('--changed-only', action='store_true'); p.set_defaults(fn=cmd_library, mode='library')
    p = sub.add_parser('sample'); common(p); p.add_argument('--class', dest='cls', required=True); p.add_argument('--seed', type=int, required=True)
    p.add_argument('--count', type=int, default=5); p.add_argument('--max-tris', type=int, default=60000); p.set_defaults(fn=cmd_sample, mode='sample')
    p = sub.add_parser('svg'); p.add_argument('file'); p.add_argument('out', nargs='?'); p.set_defaults(fn=cmd_svg)
    p = sub.add_parser('validate'); p.add_argument('files', nargs='*'); p.add_argument('--set'); p.add_argument('--procs', type=int, default=6)
    p.add_argument('--all-masters', action='store_true'); p.set_defaults(fn=cmd_validate)
    a = ap.parse_args(); a.fn(a)


if __name__ == '__main__': main()
