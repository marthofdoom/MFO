"""Coverage tags, rejection reasons and the coverage-driven selection (pure python).

A place is a candidate (library.py).  Rejection (`reject_reasons`) drops every place with any anomaly.  Tags (`TAGS`) say which terrain
features a place carries; the selection picks places until every tag and every COMMON tag pair is represented at least N times.
Thresholds live in TAGS only (to add a feature: add a row)."""
import random

# (tag, predicate on the feature dict).  Features: library.py / features.py / anomaly.py.  area = areaUnits2 (units^2)
def _share(f, k): return f.get(k, 0.0) / max(f.get('area3d', 0.0), 1.0)


def _t(f, interior_value, exterior_value):
    """threshold by context: interiors and exterior windows differ by an order of magnitude on most measures"""
    return interior_value if f['interior'] else exterior_value


# Thresholds are the top quartile (top decile / vicinity for the rare tags) of the vanilla pool, measured separately for interiors and exterior windows
# (pool feature quantiles printed by `library`; retune here).  A tag is a MEASURED property, not a place type.
TAGS = [
    ('interior',        lambda f: f['interior']),
    ('exterior',        lambda f: not f['interior']),
    ('slope_flat',      lambda f: _share(f, 'a0') >= 0.85),
    ('slope_gentle',    lambda f: _share(f, 'a1') >= _t(f, 0.14, 0.43)),
    ('slope_steep',     lambda f: _share(f, 'a2') >= _t(f, 0.077, 0.216)),
    ('slope_cliff',     lambda f: _share(f, 'a3') >= _t(f, 0.02, 0.008)),
    ('stairs_run',      lambda f: f.get('steepRise', 0) >= _t(f, 265, 1440)),
    ('stairs_long',     lambda f: f.get('steepRise', 0) >= _t(f, 400, 2400)),
    ('drops_low',       lambda f: f.get('dropLow', 0) >= 1),
    ('drops_mid',       lambda f: f.get('dropMid', 0) >= 1),
    ('drops_high',      lambda f: f.get('dropHigh', 0) >= 1),
    ('narrow',          lambda f: f['narrow'] >= _t(f, 0.96, 0.51)),
    ('doorway_portals', lambda f: f.get('doorTris', 0) >= 3),
    ('multi_floor',     lambda f: f['layer'] >= _t(f, 0.13, 0.003)),
    ('large_open',      lambda f: f['wide'] >= 0.28),
    ('clutter_dense',   lambda f: f.get('clutter', 0) >= _t(f, 138, 0.62)),
    ('occluders_dense', lambda f: f.get('occl', 0) >= _t(f, 95, 4.3)),
    ('cover_marked',    lambda f: f.get('coverTris', 0) / max(f.get('tris', 1), 1) >= 0.37),
    ('water_edge',      lambda f: f.get('waterEdge', 0) >= _t(f, 0.006, 0.013)),
    ('water_large',     lambda f: f.get('water', 0) >= 0.15),
    ('islands',         lambda f: f.get('islands', 0) >= _t(f, 1, 3)),
    ('corridors',       lambda f: f.get('pathy', 0) >= _t(f, 0.715, 0.6) and f.get('gnodes', 0) >= 20),
    ('dead_ends',       lambda f: f.get('deadEnds', 0) >= _t(f, 42, 123)),
    ('loops',           lambda f: f.get('loops', 0) >= _t(f, 49, 615)),
    ('choke_points',    lambda f: f.get('chokes', 0) >= _t(f, 100, 200)),
    ('high_ground',     lambda f: f.get('highGround', 0) >= _t(f, 0.287, 0.412)),
    ('scale_short',     lambda f: 0 < f.get('span', 0) < _t(f, 3300, 16000)),
    ('scale_mid',       lambda f: _t(f, 3300, 16000) <= f.get('span', 0) < _t(f, 20000, 28000)),
    ('scale_long',      lambda f: f.get('span', 0) >= _t(f, 20000, 28000)),
]
TAGNAMES = [t for t, _ in TAGS]
REASONS = ['displaced_mesh', 'doubtful_mesh', 'plugin_moved_refs', 'one_way_or_not_swapped_edges', 'stale_or_broken_links', 'owner_less_links',
           'kind3_portals', 'small_main_component']


def tags_of(f): return frozenset(t for t, p in TAGS if p(f))


def reject_reasons(f, flags, min_main_share):
    """-> list of reasons this place is rejected for (empty = clean).  flags: set of reason names precomputed per place (mesh / ref based)."""
    r = [x for x in REASONS[:3] if x in flags]
    if f.get('oneway', 0) or f.get('notswapped', 0): r.append(REASONS[3])
    if f.get('stale', 0): r.append(REASONS[4])
    if f.get('ownerless', 0): r.append(REASONS[5])
    if f.get('kind3', 0): r.append(REASONS[6])
    if f.get('mainShare', 1.0) < min_main_share: r.append(REASONS[7])
    return r


def common_pairs(tagsets, share):
    """tag pairs present together in at least `share` of the places"""
    from itertools import combinations
    cnt = {}
    for ts in tagsets:
        for p in combinations(sorted(ts), 2): cnt[p] = cnt.get(p, 0) + 1
    need = max(2, int(share * len(tagsets)))
    return {p for p, c in cnt.items() if c >= need}


def pick(cands, tagsets, n_min, cap, seed, pair_share=0.03, exclude=(), fill=None, sep=None):
    """Greedy coverage pick.  cands: {unit: features}; tagsets {unit: frozenset}.  Picks the place covering the most unmet (tag, pair) needs until
    every tag and common pair has >= n_min picks or `cap` is reached; then `fill(chosen)` may add diverse extras up to cap.
    sep(u, chosen) -> bool says whether u may join chosen (spacing rule).  -> (chosen list, unmet needs dict)"""
    rng = random.Random(seed); pool = [u for u in sorted(cands) if u not in exclude]
    pairs = common_pairs([tagsets[u] for u in pool], pair_share)
    from itertools import combinations
    need = {('t', t): n_min for t in TAGNAMES if any(t in tagsets[u] for u in pool)}
    for p in pairs: need[('p', p)] = n_min
    chosen = []
    def items(u):
        ts = tagsets[u]; out = [('t', t) for t in ts]
        out += [('p', p) for p in combinations(sorted(ts), 2) if p in pairs]
        return out
    while len(chosen) < cap and any(v > 0 for v in need.values()):
        best = None; bs = 0.0; order = pool[:]; rng.shuffle(order)
        for u in order:
            if u in chosen or (sep is not None and not sep(u, chosen)): continue
            sc = sum((3.0 if k[0] == 't' else 1.0) * (1 if need[k] > 0 else 0) for k in items(u) if k in need)
            if sc > bs: bs, best = sc, u
        if best is None: break
        chosen.append(best)
        for k in items(best):
            if k in need and need[k] > 0: need[k] -= 1
    if fill is not None and len(chosen) < cap: chosen = fill(chosen, cap)
    unmet = {k: v for k, v in need.items() if v > 0}
    return chosen, unmet


def table(cands, tagsets, chosen, heldout, n_min):
    """printable coverage table: tag -> (places with it, in library, in held-out)"""
    rows = []
    for t in TAGNAMES:
        a = sum(1 for u in cands if t in tagsets[u]); b = sum(1 for u in chosen if t in tagsets[u]); c = sum(1 for u in heldout if t in tagsets[u])
        rows.append((t, a, b, c, 'ok' if (b >= n_min and (not heldout or c >= n_min)) or a == 0 else ('NONE AVAILABLE' if a == 0 else 'SHORT')))
    return rows
