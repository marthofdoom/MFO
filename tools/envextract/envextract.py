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


def _graphs(a, sc, groups, cands):
    """coarse-graph features of EVERY candidate (rejected ones too, so the bias table can tag them); cached beside the scan"""
    import library
    p = os.path.join(CACHE, '_scan', a.set + '.graph.pkl')
    if os.path.exists(p) and not a.rebuild and os.path.getmtime(p) > os.path.getmtime(CANDS(a.set)):
        gf = pickle.load(open(p, 'rb'))
        if set(gf) >= set(cands): return gf
    gf = library.graph_features(sc, groups, list(cands), a.procs)
    pickle.dump(gf, open(p, 'wb'), protocol=4)
    return gf


def _print_bias(rows):
    print('\nrejected share of the candidates (before = round-4 rule with plugin_moved_refs and any one-way edge; after = this round)')
    print('  %-6s %-30s %7s %9s %9s' % ('', 'tag / class', 'places', 'before', 'after'))
    for kind, name, n, ob, na in rows: print('  %-6s %-30s %7d %8.1f%% %8.1f%%' % (kind, name, n, 100.0 * ob / n, 100.0 * na / n))


def cmd_library(a):
    """coverage library: reject anomalous places, split into library / held-out by one global hash, coverage pick of each, export, report"""
    import scan, select_lib, library, coverage
    sc = scan.load(SCAN(a.set)); ORDER[:] = sc['order']; cands, cls = _cands(a, sc)
    groups = library.make_groups(sc)
    flags = library.place_flags(sc, cands)
    library.exterior_reach(cands, coverage.has_content)
    gf = _graphs(a, sc, groups, cands)
    for u, f in cands.items(): f.update(gf[u]); f['movedRefs'] = 1 if 'plugin_moved_refs' in flags[u] else 0
    rej = {u: coverage.reject_reasons(f, flags[u]) for u, f in cands.items()}
    old = {u: coverage.legacy_reject(f, flags[u]) for u, f in cands.items()}
    first = {}; total = {}
    for u, rs in rej.items():
        for i, r in enumerate(rs):
            total[r] = total.get(r, 0) + 1
            if i == 0: first[r] = first.get(r, 0) + 1
    clean = {u: f for u, f in cands.items() if not rej[u]}
    print('candidates %d, rejected %d, clean %d' % (len(cands), len(cands) - len(clean), len(clean)))
    print('  %-32s %9s %9s' % ('reject reason', 'first', 'any'))
    for r in coverage.REASONS: print('  %-32s %9d %9d' % (r, first.get(r, 0), total.get(r, 0)))
    print('  plugin_moved_refs (a recorded feature, not a rejection): %d candidates' % sum(1 for f in cands.values() if f['movedRefs']))
    bias = coverage.bias_table(cands, cls, rej, old); _print_bias(bias)
    parsed = {u: coverage.parse_unit_id(unit_id(u)) for u in cands}
    elig = {u: f for u, f in clean.items() if coverage.centre_ok(parsed[u])}
    allowed = None
    if a.changed_only:
        allowed = _changed(sc, elig)
        print('changed-only clean candidates:', len(allowed))
    side = {u: coverage.side(parsed[u]) for u in elig}
    print('eligible (clean, window inside its block): %d; library side %d, held-out side %d' % (len(elig), sum(1 for v in side.values() if v == 'lib'), sum(1 for v in side.values() if v == 'held')))
    pools = {}
    for sd, n in (('lib', a.pool), ('held', max(1, a.pool * 2 // 3))):
        part = {u: f for u, f in elig.items() if side[u] == sd}
        pools[sd] = {u: part[u] for u in select_lib.select(part, cls, n, a.seed, a.max_tris, allowed)}
    pickle.dump(pools, open(os.path.join(CACHE, '_scan', a.set + '.pool.pkl'), 'wb'), protocol=4)      # features of the pools, to retune tag thresholds
    tab = select_lib._norm_table({**pools['lib'], **pools['held']})
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
    sep = lambda u, chosen: select_lib._far(u, chosen)
    tagsets = {u: coverage.tags_of(f) for p in pools.values() for u, f in p.items()}
    main, unmet = coverage.pick(pools['lib'], tagsets, a.coverage_n, a.count, a.seed, a.pair_share, sep=sep, fill=lambda ch, cap: fill_from(pools['lib'], ch, cap))
    held, unmet_h = coverage.pick(pools['held'], tagsets, a.coverage_n, a.heldout_count, a.seed + 1, a.pair_share, sep=sep, fill=lambda ch, cap: fill_from(pools['held'], ch, cap))
    both = {**pools['lib'], **pools['held']}
    print('\ncoverage (N=%d): pool %d library-side + %d held-out-side places, library %d, held-out %d' % (a.coverage_n, len(pools['lib']), len(pools['held']), len(main), len(held)))
    print('  %-17s %6s %8s %9s  %s' % ('feature', 'pool', 'library', 'held-out', 'status'))
    table = coverage.table(both, tagsets, main, held, a.coverage_n)
    for t, p, m, h, st in table: print('  %-17s %6d %8d %9d  %s' % (t, p, m, h, st))
    print('  common pairs unmet in library: %d, in held-out: %d' % (sum(1 for k in unmet if k[0] == 'p'), sum(1 for k in unmet_h if k[0] == 'p')))
    for k, v in sorted(unmet.items(), key=lambda kv: str(kv[0]))[:40]: print('   library needs %d more of %s' % (v, k[1]))
    for k, v in sorted(unmet_h.items(), key=lambda kv: str(kv[0]))[:40]: print('   held-out needs %d more of %s' % (v, k[1]))
    out = os.path.join(CACHE, a.set)
    cls2 = dict(cls)
    rows = _export(a, sc, main, both, cls2, out, tagsets)
    hrows = _export(a, sc, held, both, cls2, os.path.join(out, 'heldout'), tagsets)
    _index_update(a, sc, rows, dict(heldout=hrows, rejects=dict(first=first, any=total, candidates=len(cands), clean=len(clean)),
                                    biasByTagAndClass=[dict(kind=k, name=n, places=c, rejectedBefore=ob, rejectedAfter=na) for k, n, c, ob, na in bias],
                                    coverage=[dict(tag=t, pool=p, library=m, heldout=h, status=st) for t, p, m, h, st in table],
                                    unmetPairsLibrary=sorted('+'.join(k[1]) for k in unmet if k[0] == 'p'), unmetTagsLibrary=sorted(k[1] for k in unmet if k[0] == 't'),
                                    unmetTagsHeldout=sorted(k[1] for k in unmet_h if k[0] == 't')))
    for nm, rr in (('library', rows), ('held-out', hrows)):
        print('%s: %d environments' % (nm, len(rr)))
        for r in rr: print('  %-26s %-34s %8d B  tris %5d refs %4d  %s' % (r['class'], r['id'], r['bytes'], r['counts']['tris'], r['counts']['refs'], ','.join(r['tags'][:6])))


def cmd_coverage(a):
    """coverage of the three sets TOGETHER (index.json): every tag and every common tag pair at least N times in the library and in the held-out places"""
    import coverage
    from itertools import combinations
    idx = json.load(open(os.path.join(CACHE, 'index.json')))['sets']
    for part in ('envs', 'heldout'):
        rows = [r for s in idx.values() for r in s.get(part, [])]
        cnt = {t: sum(1 for r in rows if t in r['tags']) for t in coverage.TAGNAMES}
        pc = {}
        for r in rows:
            for p in combinations(sorted(r['tags']), 2): pc[p] = pc.get(p, 0) + 1
        avail = {t for s in idx.values() for c in s.get('coverage', []) if c['pool'] for t in [c['tag']]}
        print('%s: %d places over %d sets' % ('library' if part == 'envs' else 'held-out', len(rows), len(idx)))
        short = [(t, cnt[t]) for t in coverage.TAGNAMES if t in avail and cnt[t] < a.coverage_n]
        print('  tags unmet (fewer than %d): %s' % (a.coverage_n, short or 'none'))
        gone = [t for t in coverage.TAGNAMES if t not in avail]
        if gone: print('  tags with no place in any pool: %s' % gone)
        need = coverage.common_pairs([frozenset(r['tags']) for r in rows], a.pair_share)
        print('  common pairs under %d: %s' % (a.coverage_n, sorted(p for p in need if pc.get(p, 0) < a.coverage_n) or 'none'))


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
    p.add_argument('--pool', type=int, default=1200); p.add_argument('--pair-share', type=float, default=0.03)
    p = sub.add_parser('coverage'); p.add_argument('--coverage-n', type=int, default=2); p.add_argument('--pair-share', type=float, default=0.03); p.set_defaults(fn=cmd_coverage)
    p = sub.add_parser('sample'); common(p); p.add_argument('--class', dest='cls', required=True); p.add_argument('--seed', type=int, required=True)
    p.add_argument('--count', type=int, default=5); p.add_argument('--max-tris', type=int, default=60000); p.set_defaults(fn=cmd_sample, mode='sample')
    p = sub.add_parser('svg'); p.add_argument('file'); p.add_argument('out', nargs='?'); p.set_defaults(fn=cmd_svg)
    p = sub.add_parser('validate'); p.add_argument('files', nargs='*'); p.add_argument('--set'); p.add_argument('--procs', type=int, default=6)
    p.add_argument('--all-masters', action='store_true'); p.set_defaults(fn=cmd_validate)
    a = ap.parse_args()
    if getattr(a, 'heldout_count', 1) is None: a.heldout_count = a.count
    a.fn(a)


if __name__ == '__main__': main()
