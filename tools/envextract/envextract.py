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


def _export(a, sc, units, cands, cls, outdir, tagsets=None):
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
                         tags=sorted(tagsets[u]) if tagsets and u in tagsets else [], bytes=os.path.getsize(path), cells=[c['edid'] or c['id'] for c in meta['cells']][:9], features=feat, counts=meta['counts']))
    return rows


def _index_update(a, sc, rows, extra=None):
    p = os.path.join(CACHE, 'index.json')
    idx = json.load(open(p)) if os.path.exists(p) else {'sets': {}}
    idx['formatVersion'] = 2
    s = idx['sets'].setdefault(a.set, {})
    s['plugins'] = [x['name'] for x in sc['plugins']]
    if a.mode == 'library': s['envs'] = rows; s.update(extra or {})
    else:
        old = {r['file']: r for r in s.get('samples', [])}; old.update({r['file']: r for r in rows}); s['samples'] = list(old.values())
    json.dump(idx, open(p, 'w'), indent=1)


def cmd_library(a):
    """coverage library: reject anomalous places, pool, coverage pick (main + held-out), export, report"""
    import scan, select_lib, library, coverage
    sc = scan.load(SCAN(a.set)); ORDER[:] = sc['order']; cands, cls = _cands(a, sc)
    groups = library.make_groups(sc)
    flags = library.place_flags(sc, cands)
    rej = {u: coverage.reject_reasons(f, flags[u], a.min_main_share) for u, f in cands.items()}
    first = {}; total = {}
    for u, rs in rej.items():
        for i, r in enumerate(rs):
            total[r] = total.get(r, 0) + 1
            if i == 0: first[r] = first.get(r, 0) + 1
    clean = {u: f for u, f in cands.items() if not rej[u]}
    print('candidates %d, rejected %d, clean %d' % (len(cands), len(cands) - len(clean), len(clean)))
    print('  %-32s %9s %9s' % ('reject reason', 'first', 'any'))
    for r in coverage.REASONS: print('  %-32s %9d %9d' % (r, first.get(r, 0), total.get(r, 0)))
    allowed = None
    if a.changed_only:
        allowed = _changed(sc, clean)
        print('changed-only clean candidates:', len(allowed))
    pool_units = select_lib.select(clean, cls, a.pool, a.seed, a.max_tris, allowed)
    gf = library.graph_features(sc, groups, pool_units, a.procs)
    pool = {}
    for u in pool_units:
        f = dict(clean[u]); f.update(gf[u]); pool[u] = f
    pickle.dump(pool, open(os.path.join(CACHE, '_scan', a.set + '.pool.pkl'), 'wb'), protocol=4)      # features of the pool, to retune tag thresholds
    tagsets = {u: coverage.tags_of(f) for u, f in pool.items()}
    tab = select_lib._norm_table(pool)
    def fill_from(src, chosen, cap):
        rest = [u for u in sorted(src) if u not in chosen]
        vs = {u: select_lib.vec(src[u], tab) for u in rest + list(chosen)}
        chosen = list(chosen)
        while len(chosen) < cap:
            best = None; bd = -1.0
            for u in rest:
                if u in chosen or not select_lib._far(u, chosen): continue
                d = min((sum((x - y) ** 2 for x, y in zip(vs[u], vs[v])) for v in chosen), default=1.0)
                if d > bd: bd, best = d, u
            if best is None: break
            chosen.append(best)
        return chosen
    fill = lambda ch, cap: fill_from(pool, ch, cap)
    sep = lambda u, chosen: select_lib._far(u, chosen)
    main, unmet = coverage.pick(pool, tagsets, a.coverage_n, a.count, a.seed, a.pair_share, sep=sep, fill=fill)
    near = {u for u in pool if not select_lib._far(u, main) or u in main}
    held_pool = {u: f for u, f in pool.items() if u not in near}
    held, unmet_h = coverage.pick(held_pool, {u: tagsets[u] for u in held_pool}, a.coverage_n, a.heldout_count, a.seed + 1, a.pair_share, sep=sep, fill=lambda ch, cap: fill_from(held_pool, ch, cap))
    print('\ncoverage (N=%d): pool %d places, library %d, held-out %d' % (a.coverage_n, len(pool), len(main), len(held)))
    print('  %-17s %6s %8s %9s  %s' % ('feature', 'pool', 'library', 'held-out', 'status'))
    for t, p, m, h, st in coverage.table(pool, tagsets, main, held, a.coverage_n): print('  %-17s %6d %8d %9d  %s' % (t, p, m, h, st))
    print('  common pairs unmet in library: %d, in held-out: %d' % (sum(1 for k in unmet if k[0] == 'p'), sum(1 for k in unmet_h if k[0] == 'p')))
    for k, v in sorted(unmet.items(), key=lambda kv: str(kv[0]))[:40]: print('   library needs %d more of %s' % (v, k[1]))
    for k, v in sorted(unmet_h.items(), key=lambda kv: str(kv[0]))[:40]: print('   held-out needs %d more of %s' % (v, k[1]))
    out = os.path.join(CACHE, a.set)
    cls2 = dict(cls)
    rows = _export(a, sc, main, pool, cls2, out, tagsets)
    hrows = _export(a, sc, held, pool, cls2, os.path.join(out, 'heldout'), tagsets)
    _index_update(a, sc, rows, dict(heldout=hrows, rejects=dict(first=first, any=total, candidates=len(cands), clean=len(clean)),
                                    coverage=[dict(tag=t, pool=p, library=m, heldout=h, status=st) for t, p, m, h, st in coverage.table(pool, tagsets, main, held, a.coverage_n)],
                                    unmetPairsLibrary=sorted('+'.join(k[1]) for k in unmet if k[0] == 'p'), unmetTagsLibrary=sorted(k[1] for k in unmet if k[0] == 't')))
    for nm, rr in (('library', rows), ('held-out', hrows)):
        print('%s: %d environments' % (nm, len(rr)))
        for r in rr: print('  %-26s %-34s %8d B  tris %5d refs %4d  %s' % (r['class'], r['id'], r['bytes'], r['counts']['tris'], r['counts']['refs'], ','.join(r['tags'][:6])))


def _changed(sc, clean):
    import library
    nb = 5; touched = set()
    for k, (pi, nv) in sc['navm'].items():
        ni = sc['navinfo'].get(k)
        if pi >= nb and ni and (ni[3] or ni[0]): touched.add(library.unit_of_mesh(sc, k))
    return {u for u in clean if u in touched or (u[0] == 'x' and any(('x', u[1], u[2] + dx, u[3] + dy) in touched for dx in (-1, 0, 1) for dy in (-1, 0, 1)))}


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
    p.add_argument('--coverage-n', type=int, default=2, help='each feature and each common feature pair at least N times'); p.add_argument('--heldout-count', type=int, default=None)
    p.add_argument('--pool', type=int, default=1200); p.add_argument('--pair-share', type=float, default=0.03); p.add_argument('--min-main-share', type=float, default=0.6)
    p = sub.add_parser('sample'); common(p); p.add_argument('--class', dest='cls', required=True); p.add_argument('--seed', type=int, required=True)
    p.add_argument('--count', type=int, default=5); p.add_argument('--max-tris', type=int, default=60000); p.set_defaults(fn=cmd_sample, mode='sample')
    p = sub.add_parser('svg'); p.add_argument('file'); p.add_argument('out', nargs='?'); p.set_defaults(fn=cmd_svg)
    p = sub.add_parser('validate'); p.add_argument('files', nargs='*'); p.add_argument('--set'); p.add_argument('--procs', type=int, default=6)
    p.add_argument('--all-masters', action='store_true'); p.set_defaults(fn=cmd_validate)
    a = ap.parse_args()
    if getattr(a, 'heldout_count', 1) is None: a.heldout_count = a.count
    a.fn(a)


if __name__ == '__main__': main()
