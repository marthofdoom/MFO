"""Coverage tags, rejection reasons and the coverage-driven selection (pure python).

A place is a candidate (library.py).  Rejection (`reject_reasons`) drops every place with any anomaly.  Tags (`TAGS`) say which terrain
features a place carries; the selection picks places until every tag and every COMMON tag pair is represented at least N times.
Thresholds live in TAGS only (to add a feature: add a row)."""
import hashlib, random, re

# ---- the ONE global split of places into library and held-out (all sets together) --------------------------------------------------------
# The unit of the split is an interior cell, or an exterior BLOCK: BLOCK x BLOCK grid cells of one worldspace.  An exterior window is centred
# on a cell at least WIN (1) cells inside its block, so its 3x3 window lies wholly in one block and windows of different blocks never overlap.
# The block's side is a hash of its name-based id (plugin file name + local id of the worldspace, block coordinates), so it does not depend on
# a set's load order and the same place in two sets falls on the same side.  Library places come only from 'lib' blocks, held-out only from 'held'.
BLOCK = 12
HELD_PERCENT = 40


def parse_unit_id(uid):
    """'i_<cell>' -> ('i', cell); 'x_<world>_<gx>_<gy>' -> ('x', world, gx, gy).  The name part has no underscore (envextract.unit_id)."""
    m = re.fullmatch(r'i_([^_]+)', uid)
    if m: return ('i', m.group(1))
    m = re.fullmatch(r'x_([^_]+)_(-?\d+)_(-?\d+)', uid)
    if m: return ('x', m.group(1), int(m.group(2)), int(m.group(3)))
    raise ValueError('bad unit id ' + uid)


def centre_ok(u, win=1):
    """an exterior window may be a place only when the whole window lies inside one block"""
    if u[0] != 'x': return True
    return win <= u[2] % BLOCK <= BLOCK - 1 - win and win <= u[3] % BLOCK <= BLOCK - 1 - win


def block_of(u):
    return 'i:' + u[1] if u[0] == 'i' else 'x:%s:%d:%d' % (u[1], u[2] // BLOCK, u[3] // BLOCK)


def side(u):
    """'held' or 'lib' for a parsed unit id (see above)"""
    h = int(hashlib.sha1(block_of(u).encode()).hexdigest()[:8], 16)
    return 'held' if h % 100 < HELD_PERCENT else 'lib'


def window_cells(u, win=1):
    """the cells a place covers, name-based so it compares across sets"""
    if u[0] == 'i': return {u}
    return {('x', u[1], u[2] + dx, u[3] + dy) for dx in range(-win, win + 1) for dy in range(-win, win + 1)}


# Rejection limits (to retune a rejection, edit here).  Every one is a share or a count of a measured feature.
LIMITS = dict(
    oneway_share=0.005,        # a place is rejected when more than this share of its internal edges is one-way / not the swapped edge (the bad edges of the others are cut and marked)
    main_share_interior=0.6,   # share of walkable area in the main walk component, interiors
    main_share_exterior=0.4,   # exteriors: islands and cliffs are normal outdoors
    content_refs=10,           # an exterior window needs this many exported refs ...
    content_tris=1500,         # ... or this many triangles to count as having content
)

# (tag, predicate on the feature dict).  Features: library.py / features.py / anomaly.py.  area = areaUnits2 (units^2)
def _share(f, k): return f.get(k, 0.0) / max(f.get('area3d', 0.0), 1.0)


def _t(f, interior_value, exterior_value):
    """threshold by context: interiors and exterior windows differ by an order of magnitude on most measures"""
    return interior_value if f['interior'] else exterior_value


# Thresholds are the top quartile (top decile / vicinity for the rare tags) of the vanilla candidates (graph tags: of the rasterised graph, round 5), measured separately for interiors and exterior windows
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
    ('corridors',       lambda f: f.get('pathy', 0) >= _t(f, 0.267, 0.099) and f.get('gnodes', 0) >= 20 and f['narrow'] >= _t(f, 0.97, 0.58) and f['wide'] <= _t(f, 0.0, 0.10)),   # path-like graph AND narrow walkable width high AND wide low
    ('dead_ends',       lambda f: f.get('deadEnds', 0) >= _t(f, 11, 34)),
    ('loops',           lambda f: f.get('loops', 0) >= _t(f, 290, 5028)),
    ('choke_points',    lambda f: f.get('chokes', 0) >= _t(f, 28, 48)),
    ('high_ground',     lambda f: f.get('highGround', 0) >= _t(f, 0.287, 0.412)),
    ('scale_short',     lambda f: 0 < f.get('span', 0) < _t(f, 2304, 14592)),
    ('scale_mid',       lambda f: _t(f, 2304, 14592) <= f.get('span', 0) < _t(f, 14720, 20736)),
    ('scale_long',      lambda f: f.get('span', 0) >= _t(f, 14720, 20736)),
]
TAGNAMES = [t for t, _ in TAGS]
REASONS = ['displaced_mesh', 'doubtful_mesh', 'bad_edge_share', 'stale_or_broken_links', 'owner_less_links', 'kind3_portals', 'small_main_component',
           'sparse_exterior', 'unreachable_exterior']
# plugin_moved_refs is NOT a rejection: a later plugin moving refs says nothing about the place (the five big cities of a big list all carry it).  It
# is recorded as a feature (movedRefs, 0/1) and `bias_table` prints how the tags and classes fare with and without it.


def tags_of(f): return frozenset(t for t, p in TAGS if p(f))


def has_content(f):
    return f.get('nRefs', 0) >= LIMITS['content_refs'] or f.get('tris', 0) >= LIMITS['content_tris']


def reject_reasons(f, flags):
    """-> list of reasons this place is rejected for (empty = clean).  flags: set of mesh-based reason names precomputed per place
    (displaced_mesh, doubtful_mesh).  Exterior windows also need content and reachability (f['reach'], set by library.exterior_reach)."""
    r = [x for x in REASONS[:2] if x in flags]
    if (f.get('oneway', 0) + f.get('notswapped', 0)) > LIMITS['oneway_share'] * max(f.get('edges', 0), 1): r.append('bad_edge_share')
    if f.get('stale', 0): r.append('stale_or_broken_links')
    if f.get('ownerless', 0): r.append('owner_less_links')
    if f.get('kind3', 0): r.append('kind3_portals')
    if f.get('mainShare', 1.0) < LIMITS['main_share_interior' if f['interior'] else 'main_share_exterior']: r.append('small_main_component')
    if not f['interior']:
        if not has_content(f): r.append('sparse_exterior')
        if not f.get('reach', 0): r.append('unreachable_exterior')
    return r


def legacy_reject(f, flags):
    """the round-4 rule (any one-way edge, plugin-moved refs, main share 0.6 everywhere); used only for the before / after columns of bias_table"""
    r = [x for x in ('displaced_mesh', 'doubtful_mesh', 'plugin_moved_refs') if x in flags]
    if f.get('oneway', 0) or f.get('notswapped', 0): r.append('bad_edge_share')
    if f.get('stale', 0): r.append('stale_or_broken_links')
    if f.get('ownerless', 0): r.append('owner_less_links')
    if f.get('kind3', 0): r.append('kind3_portals')
    if f.get('mainShare', 1.0) < 0.6: r.append('small_main_component')
    return r


def bias_table(cands, cls, rej_new, rej_old):
    """rows (kind, name, places, rejected_before, rejected_after): the rejected share of the candidates per tag and per class, under the legacy rule and the
    current one.  Tags are computed from each place's own features (graph features included when present)."""
    ts = {u: tags_of(f) for u, f in cands.items()}
    rows = []
    for t in TAGNAMES:
        us = [u for u in cands if t in ts[u]]
        if us: rows.append(('tag', t, len(us), sum(1 for u in us if rej_old[u]), sum(1 for u in us if rej_new[u])))
    for c in sorted(set(cls.values())):
        us = [u for u in cands if cls[u] == c]
        rows.append(('class', c, len(us), sum(1 for u in us if rej_old[u]), sum(1 for u in us if rej_new[u])))
    return rows


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
    pairs = common_pairs([tagsets[u] for u in cands], pair_share)      # availability is judged on the FULL candidate set: a need the exclusions made
    from itertools import combinations                                 # unmeetable shows up as unmet instead of silently disappearing
    need = {('t', t): n_min for t in TAGNAMES if any(t in tagsets[u] for u in cands)}
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
        rows.append((t, a, b, c, 'NONE AVAILABLE' if a == 0 else 'ok' if b >= n_min and (not heldout or c >= n_min) else 'SHORT'))
    return rows
