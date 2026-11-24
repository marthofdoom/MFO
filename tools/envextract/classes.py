"""CAVEAT: class names say what was MEASURED (area, layering, width, slope, counts of placed refs), not what the place is.
A 'multilevel_statics' window is not necessarily a fort and 'actors_containers' is not necessarily a camp (wildlife counts as actors).
THE rule table mapping measured features to an environment class.  First matching row wins, per interior/exterior.
To add a class or retune: edit RULES (nothing else reads thresholds).  Features: see features.py / library.py.
Units: game units (areaUnits2 = units^2).  Numbers were set from the distribution of the vanilla masters (README)."""

# (class, interior, {feature: (op, value)}), op in '>=' '<' ; all conditions must hold
RULES = [
    # interiors: small multi-level first (a tower), then size/shape
    ('interior_small_multilevel', True,  {'layer': ('>=', 0.15), 'areaUnits2': ('<', 3.0e6)}),
    ('interior_small',        True,  {'areaUnits2': ('<', 1.5e6), 'layer': ('<', 0.15), 'nActor': ('<', 8)}),
    ('interior_sloped_tall',  True,  {'zSpan': ('>=', 600), 'steep': ('>=', 0.04), 'areaUnits2': ('>=', 1.5e6), 'layer': ('<', 0.08)}),
    ('interior_large_multilevel',   True,  {'layer': ('>=', 0.05), 'areaUnits2': ('>=', 3.0e6)}),
    ('interior_wide',    True,  {'wide': ('>=', 0.02), 'areaUnits2': ('>=', 1.5e6)}),
    ('interior_narrow',       True,  {'narrow': ('>=', 0.9), 'areaUnits2': ('>=', 1.5e6)}),
    ('interior_other',        True,  {}),
    # exterior 3x3 windows
    ('exterior_many_doors',  False, {'nTele': ('>=', 5)}),
    ('exterior_multilevel_statics',  False, {'layer': ('>=', 0.004), 'nLarge': ('>=', 10)}),
    ('exterior_actors_containers',  False, {'nActor': ('>=', 3), 'nTele': ('<', 2), 'nStuff': ('>=', 4), 'layer': ('<', 0.004)}),
    ('exterior_door_and_statics',  False, {'nTele': ('>=', 1), 'nLarge': ('>=', 40)}),
    ('exterior_steep', False, {'steep': ('>=', 0.22)}),
    ('exterior_wide_open',  False, {'steep': ('<', 0.22), 'wide': ('>=', 0.10)}),
    ('exterior_other', False, {}),
]
CLASSES = [r[0] for r in RULES]

# features (and weights) the farthest-point selection measures distance in, each scaled to 0..1 by its observed 5..95 percentile
SELECT_FEATURES = {'areaUnits2': 1.0, 'layer': 1.5, 'narrow': 1.0, 'wide': 1.0, 'steep': 1.0, 'zSpan': 0.7, 'nActor': 0.7,
                   'nTele': 0.7, 'nLarge': 0.7, 'nCont': 0.5, 'water': 0.5}

# classes with a quota of the library (share of --count); the rest share the remainder equally
QUOTA = {'interior_small': 3, 'interior_narrow': 3, 'interior_sloped_tall': 4, 'interior_large_multilevel': 4, 'interior_wide': 2,
         'interior_small_multilevel': 3, 'interior_other': 2, 'exterior_many_doors': 4, 'exterior_multilevel_statics': 4, 'exterior_actors_containers': 4,
         'exterior_door_and_statics': 2, 'exterior_steep': 2, 'exterior_wide_open': 3, 'exterior_other': 2}


def classify(f, interior):
    for name, inter, cond in RULES:
        if inter != interior: continue
        ok = True
        for k, (op, v) in cond.items():
            x = f.get(k, 0)
            if (op == '>=' and not x >= v) or (op == '<' and not x < v): ok = False; break
        if ok: return name
    raise AssertionError('no class')
