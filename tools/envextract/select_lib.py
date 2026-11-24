"""Library selection (farthest-point per class) and the seeded sampler.  Pure python, no numpy."""
import random
import classes as C

SEP = 3   # exterior windows of one library must be at least this many grid cells apart (Chebyshev) in one worldspace


def _norm_table(cands):
    """feature -> (lo, hi): observed 5..95 percentile, for scaling distances"""
    t = {}
    for k in C.SELECT_FEATURES:
        v = sorted(f.get(k, 0.0) for f in cands.values())
        lo, hi = v[int(0.05 * (len(v) - 1))], v[int(0.95 * (len(v) - 1))]
        t[k] = (lo, hi if hi > lo else lo + 1.0)
    return t


def vec(f, tab):
    return [C.SELECT_FEATURES[k] * min(max((f.get(k, 0.0) - tab[k][0]) / (tab[k][1] - tab[k][0]), 0.0), 1.5) for k in C.SELECT_FEATURES]


def _far(u, chosen):
    if u[0] != 'x': return True
    return all(c[0] != 'x' or c[1] != u[1] or max(abs(c[2] - u[2]), abs(c[3] - u[3])) >= SEP for c in chosen)


def quotas(count, avail):
    """class -> number to take: C.QUOTA scaled to `count`, capped by availability, leftovers redistributed."""
    w = {c: C.QUOTA.get(c, 1) for c in avail if avail[c] > 0}
    out = {c: 0 for c in w}; left = count
    while left > 0:
        room = {c: avail[c] - out[c] for c in w if avail[c] - out[c] > 0}
        if not room: break
        tw = sum(w[c] for c in room); gave = 0
        for c in sorted(room, key=lambda c: -w[c]):
            n = min(room[c], max(1, round(left * w[c] / tw))); n = min(n, left - gave)
            out[c] += n; gave += n
            if gave >= left: break
        if gave == 0: break
        left -= gave
    return out


def select(cands, cls_of, count, seed, max_tris=60000, allowed=None):
    """-> list of units.  cands {unit: features}; cls_of {unit: class}."""
    rng = random.Random(seed); tab = _norm_table(cands)
    byc = {}
    for u, f in cands.items():
        if f['tris'] > max_tris or (allowed is not None and u not in allowed): continue
        byc.setdefault(cls_of[u], []).append(u)
    q = quotas(count, {c: len(v) for c, v in byc.items()}); out = []
    for c, n in q.items():
        pool = sorted(byc[c]); rng.shuffle(pool)
        vs = {u: vec(cands[u], tab) for u in pool}
        chosen = []
        for u in pool:
            if _far(u, out + chosen): chosen.append(u); break
        while len(chosen) < n:
            best = None; bd = -1.0
            for u in pool:
                if u in chosen or not _far(u, out + chosen): continue
                d = min(sum((a - b) ** 2 for a, b in zip(vs[u], vs[v])) for v in chosen)
                if d > bd: bd, best = d, u
            if best is None: break
            chosen.append(best)
        out += chosen
    return out


def sample(cands, cls_of, cls, count, seed, exclude=(), max_tris=60000):
    """seeded uniform draw of `count` units of class `cls` (exterior ones kept SEP cells apart), not in `exclude`."""
    rng = random.Random(seed); pool = sorted(u for u, c in cls_of.items() if c == cls and u not in exclude and cands[u]['tris'] <= max_tris)
    rng.shuffle(pool); out = []
    for u in pool:
        if _far(u, list(exclude) + out): out.append(u)
        if len(out) >= count: break
    return out
