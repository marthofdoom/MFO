"""THE rule table mapping measured features to an environment class.  First matching row wins, per interior/exterior.
To add a class or retune: edit RULES (nothing else reads thresholds).  Features: see features.py / library.py.
Units: game units (areaUnits2 = units^2).  Numbers were set from the distribution of the vanilla masters (README)."""

# (class, interior, {feature: (op, value)}), op in '>=' '<' ; all conditions must hold
RULES = [
    # interiors: small multi-level first (a tower), then size/shape
    ('interior_tower_stairs', True,  {'layer': ('>=', 0.15), 'areaUnits2': ('<', 3.0e6)}),
    ('interior_house',        True,  {'areaUnits2': ('<', 1.5e6), 'layer': ('<', 0.15), 'nActor': ('<', 8)}),
    ('interior_cave_ledges',  True,  {'zSpan': ('>=', 600), 'steep': ('>=', 0.04), 'areaUnits2': ('>=', 1.5e6), 'layer': ('<', 0.08)}),
    ('interior_ruin_halls',   True,  {'layer': ('>=', 0.05), 'areaUnits2': ('>=', 3.0e6)}),
    ('interior_hall_open',    True,  {'wide': ('>=', 0.02), 'areaUnits2': ('>=', 1.5e6)}),
    ('interior_narrow',       True,  {'narrow': ('>=', 0.9), 'areaUnits2': ('>=', 1.5e6)}),
    ('interior_rooms',        True,  {}),
    # exterior 3x3 windows
    ('exterior_town',  False, {'nTele': ('>=', 5)}),
    ('exterior_fort',  False, {'layer': ('>=', 0.004), 'nLarge': ('>=', 10)}),
    ('exterior_camp',  False, {'nActor': ('>=', 3), 'nTele': ('<', 2), 'nStuff': ('>=', 4), 'layer': ('<', 0.004)}),
    ('exterior_ruin',  False, {'nTele': ('>=', 1), 'nLarge': ('>=', 40)}),
    ('exterior_hills', False, {'steep': ('>=', 0.22)}),
    ('exterior_open',  False, {'steep': ('<', 0.22), 'wide': ('>=', 0.10)}),
    ('exterior_rough', False, {}),
]
CLASSES = [r[0] for r in RULES]

# features (and weights) the farthest-point selection measures distance in, each scaled to 0..1 by its observed 5..95 percentile
SELECT_FEATURES = {'areaUnits2': 1.0, 'layer': 1.5, 'narrow': 1.0, 'wide': 1.0, 'steep': 1.0, 'zSpan': 0.7, 'nActor': 0.7,
                   'nTele': 0.7, 'nLarge': 0.7, 'nCont': 0.5, 'water': 0.5}

# classes with a quota of the library (share of --count); the rest share the remainder equally
QUOTA = {'interior_house': 3, 'interior_narrow': 3, 'interior_cave_ledges': 4, 'interior_ruin_halls': 4, 'interior_hall_open': 2,
         'interior_tower_stairs': 3, 'interior_rooms': 2, 'exterior_town': 4, 'exterior_fort': 4, 'exterior_camp': 4,
         'exterior_ruin': 2, 'exterior_hills': 2, 'exterior_open': 3, 'exterior_rough': 2}


def classify(f, interior):
    for name, inter, cond in RULES:
        if inter != interior: continue
        ok = True
        for k, (op, v) in cond.items():
            x = f.get(k, 0)
            if (op == '>=' and not x >= v) or (op == '<' and not x < v): ok = False; break
        if ok: return name
    raise AssertionError('no class')
