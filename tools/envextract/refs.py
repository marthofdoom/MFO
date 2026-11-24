"""Placed-ref classification and placement maths shared by features and the exporter."""
import math

# baseSig enum (README)
SIG_ID = {s: i + 1 for i, s in enumerate(['DOOR', 'CONT', 'ACTI', 'FURN', 'MISC', 'WEAP', 'ARMO', 'AMMO', 'BOOK', 'ALCH', 'INGR',
                                         'KEYM', 'SCRL', 'SLGM', 'NPC_', 'LVLN', 'STAT', 'MSTT', 'SCOL', 'FLOR', 'TREE', 'APPA'])}
K_DOOR, K_CONT, K_ACTI, K_FURN, K_ITEM, K_ACTOR, K_STATIC, K_FLORA, K_TREE = range(9)
SIG_KIND = {'DOOR': K_DOOR, 'CONT': K_CONT, 'ACTI': K_ACTI, 'FURN': K_FURN, 'NPC_': K_ACTOR, 'LVLN': K_ACTOR,
            'STAT': K_STATIC, 'MSTT': K_STATIC, 'SCOL': K_STATIC, 'FLOR': K_FLORA, 'TREE': K_TREE}
for _s in ('MISC', 'WEAP', 'ARMO', 'AMMO', 'BOOK', 'ALCH', 'INGR', 'KEYM', 'SCRL', 'SLGM', 'APPA'): SIG_KIND[_s] = K_ITEM
LARGE_KINDS = (K_STATIC, K_TREE)
DEFAULT_MIN_STATIC = 256.0


def euler_matrix(rx, ry, rz):
    """Skyrim REFR rotation: Z-Y-X order (rotate about X, then Y, then Z), radians; returns 3x3 row-major."""
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    return ((cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx),
            (sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx),
            (-sy, cy * sx, cy * cx))


def world_bounds(obnd, pos, rot, scale):
    """AABB (min3, max3) of the OBND box (6 x i16) placed with scale and rotation at pos."""
    if obnd is None: return (0.0,) * 3, (0.0,) * 3
    lo = [obnd[0] * scale, obnd[1] * scale, obnd[2] * scale]; hi = [obnd[3] * scale, obnd[4] * scale, obnd[5] * scale]
    m = euler_matrix(*rot)
    mn = [1e30] * 3; mx = [-1e30] * 3
    for cx in (lo[0], hi[0]):
        for cy in (lo[1], hi[1]):
            for cz in (lo[2], hi[2]):
                for a in range(3):
                    v = m[a][0] * cx + m[a][1] * cy + m[a][2] * cz + pos[a]
                    if v < mn[a]: mn[a] = v
                    if v > mx[a]: mx[a] = v
    return tuple(mn), tuple(mx)


def classify(sc, ref, min_static=DEFAULT_MIN_STATIC):
    """-> (kind, sig, flags_extra, bounds) or None when the ref is not exported.  ref = scan['refs'] value."""
    ck, fl, isactor, nm, pos, rot, scale, ep, tel, lock, cnt = ref
    b = sc['bases'].get(nm)
    if b is None: return None
    sig, edid, obnd, model = b
    kind = SIG_KIND.get(sig)
    if kind is None: return None
    if isactor != (kind == K_ACTOR): return None     # ACHR with a non-actor base or the reverse: malformed, skipped
    bmin, bmax = world_bounds(obnd, pos, rot, scale)
    if kind in LARGE_KINDS:
        if obnd is None: return None
        side = max(abs(obnd[3] - obnd[0]), abs(obnd[4] - obnd[1]), abs(obnd[5] - obnd[2])) * abs(scale)
        if side < min_static: return None
    return kind, sig, bmin, bmax
