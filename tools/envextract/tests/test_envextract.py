import os, sys, struct, tempfile, unittest, random
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, '..')); sys.path.insert(0, HERE)
import subprocess, shutil
import synth as S, plugin as P, navm as N, fmt, classes as C, select_lib as SL, lists, refs as R

SQ = [(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)]
TRIS = [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)]   # edge 1-2 of tri0 and edge 0-1 of tri1... see below


def good(): return S.nvnm(SQ, [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)], cell=0x1234)


class Navmesh(unittest.TestCase):
    def test_decode(self):
        m = N.parse(good()); self.assertEqual(len(m['verts']), 4); self.assertEqual(len(m['tris']), 2); self.assertEqual(m['cell'], 0x1234)
        self.assertEqual(m['world'], 0); self.assertEqual(m['gridDiv'], 1)
    def test_exterior_grid(self):
        m = N.parse(S.nvnm(SQ, [(0, 1, 2, -1, -1, -1, 0, 0)], world=0x3C, grid=(-5, 7)))
        self.assertEqual((m['gridX'], m['gridY']), (-5, 7))
    def test_trailing_byte_rejected(self):
        with self.assertRaises(N.NavmeshError): N.parse(good() + b'\0')
    def test_truncated_rejected(self):
        with self.assertRaises(N.NavmeshError): N.parse(good()[:-3])
    def test_vertex_range(self):
        with self.assertRaises(N.NavmeshError): N.parse(S.nvnm(SQ, [(0, 1, 9, -1, -1, -1, 0, 0)], cell=1))
    def test_external_edge_indexes_link_table(self):
        # edge 0 external (flag 1) value 0 = link row 0
        nv = S.nvnm(SQ, [(0, 1, 2, 0, -1, -1, 0x801, 0)], links=[(0, 0x777, 3)], cell=1); m = N.parse(nv); self.assertEqual(m['links'][0], (0, 0x777, 3))
        with self.assertRaises(N.NavmeshError): N.parse(S.nvnm(SQ, [(0, 1, 2, 5, -1, -1, 0x801, 0)], links=[(0, 0x777, 3)], cell=1))
    def test_neighbour_range(self):
        with self.assertRaises(N.NavmeshError): N.parse(S.nvnm(SQ, [(0, 1, 2, 7, -1, -1, 0, 0)], cell=1))
    def test_bad_version(self):
        b = bytearray(good()); b[0] = 11
        with self.assertRaises(N.NavmeshError): N.parse(bytes(b))


def build_world(tmp):
    """master.esm: interior cell 0x100 with navmesh 0x200, a door base 0x300 + two REFRs, an actor; override.esp deletes one, disables one."""
    cellrec = S.rec(b'CELL', 0x100, [(b'EDID', b'TestCell\0'), (b'DATA', b'\x01\x00')])
    navm = S.rec(b'NAVM', 0x200, [(b'NVNM', S.nvnm(SQ, [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)], cell=0x100))])
    def refr(fid, base, pos, extra=(), flags=0, comp=False):
        return S.rec(b'REFR', fid, [(b'NAME', struct.pack('<I', base)), (b'DATA', struct.pack('<6f', *pos, 0, 0, 0))] + list(extra), flags, comp)
    refs = refr(0x400, 0x300, (10, 10, 0), [(b'XTEL', struct.pack('<I6fI', 0x401, 0, 0, 0, 0, 0, 0, 0)), (b'XLOC', struct.pack('<B3xIB3x8x', 50, 0x500, 0))], comp=True) \
        + refr(0x401, 0x300, (50, 50, 0)) + refr(0x402, 0x301, (20, 20, 0), [(b'XSCL', struct.pack('<f', 2.0))]) \
        + refr(0x410, 0x300, (1, 1, 0), flags=0x800) \
        + refr(0x411, 0x300, (2, 2, 0), [(b'XESP', struct.pack('<IB3x', 0x410, 0))], flags=0x800) \
        + refr(0x412, 0x300, (3, 3, 0), [(b'XESP', struct.pack('<IB3x', 0x410, 1))]) \
        + refr(0x413, 0x300, (4, 4, 0), [(b'XESP', struct.pack('<IB3x', 0x14, 0))], flags=0x800)
    achr = S.rec(b'ACHR', 0x403, [(b'NAME', struct.pack('<I', 0x302)), (b'DATA', struct.pack('<6f', 5, 5, 0, 0, 0, 0))])
    cellgrp = S.group(0x100, 6, S.group(0x100, 9, S.rec(b'NAVM', 0x200, [(b'NVNM', S.nvnm(SQ, [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)], cell=0x100))]) + refs + achr))
    interior = S.group(b'CELL', 0, S.group(0, 2, S.group(0, 3, cellrec + cellgrp)))
    door = S.rec(b'DOOR', 0x300, [(b'EDID', b'TestDoor\0'), (b'OBND', struct.pack('<6h', -50, -5, 0, 50, 5, 200)), (b'MODL', b'door.nif\0')])
    stat = S.rec(b'STAT', 0x301, [(b'EDID', b'BigRock\0'), (b'OBND', struct.pack('<6h', -200, -200, 0, 200, 200, 300))])
    npc = S.rec(b'NPC_', 0x302, [(b'EDID', b'Guy\0')])
    m = S.plugin([], S.group(b'DOOR', 0, door) + S.group(b'STAT', 0, stat) + S.group(b'NPC_', 0, npc) + interior, flags=1)
    # override: delete ref 0x402, disable 0x401, move nothing
    cellgrp2 = S.group(0x100, 6, S.group(0x100, 9, S.rec(b'REFR', 0x402, [], flags=0x20) + refr(0x401, 0x300, (50, 50, 0), flags=0x800)))
    ov = S.plugin(['master.esm'], S.group(b'CELL', 0, S.group(0, 2, S.group(0, 3, S.rec(b'CELL', 0x100, [(b'EDID', b'TestCell\0'), (b'DATA', b'\x01\x00')]) + cellgrp2))))
    for n, d in (('master.esm', m), ('override.esp', ov)):
        with open(os.path.join(tmp, n), 'wb') as f: f.write(d)


class Plugins(unittest.TestCase):
    def test_walk_and_override(self):
        import scan
        with tempfile.TemporaryDirectory() as tmp:
            build_world(tmp); pg = P.Plugin(os.path.join(tmp, 'master.esm'))
            self.assertEqual(pg.masters, [])
            sigs = []
            for lab, s, e in pg.top_groups():
                if lab == b'CELL': sigs += [(r[0], r[2], r[5]['ctype']) for r in pg.walk(s, e)]
            self.assertIn((b'NAVM', 0x200, 9), sigs); self.assertIn((b'REFR', 0x400, 9), sigs)
            order = ['master.esm', 'override.esp']; where = {n: os.path.join(tmp, n) for n in order}
            sc = scan.scan_set(order, where, procs=1, log=lambda *a: None)
            self.assertEqual(len(sc['navm']), 1)
            ids = sorted(k & 0xFFFFFF for k in sc['refs'])
            self.assertEqual(ids, [0x400, 0x401, 0x403, 0x410, 0x411, 0x412, 0x413])   # 0x402 deleted; disabled refs are KEPT
            sd = lambda l: R.start_disabled(sc, l)
            self.assertTrue(sd(0x401))                                  # the override disables it
            self.assertTrue(sd(0x410)); self.assertTrue(sd(0x411))      # parent disabled, child follows
            self.assertFalse(sd(0x412))                                 # inverted: parent disabled -> child enabled
            self.assertFalse(sd(0x413)); self.assertFalse(sd(0x400))    # player parent is always enabled; plain ref enabled
            r = sc['refs'][0x400]; self.assertEqual(r[8][0], 0x401); self.assertEqual(r[9][0], 50)   # compressed record decoded: teleport and lock
            import refs
            c = refs.classify(sc, sc['refs'][0x403]); self.assertEqual(c[0], refs.K_ACTOR)
            c = refs.classify(sc, r); self.assertEqual(c[0], refs.K_DOOR); self.assertEqual(c[2], (-40.0, 5.0, 0.0))
    def test_light_plugin_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, 'l.esl'), 'wb') as f: f.write(S.plugin(['a.esm'], b'', flags=0x200))
            pg = P.Plugin(os.path.join(tmp, 'l.esl')); self.assertTrue(pg.light)
            self.assertEqual(pg.resolve(0x01000ABC, 'l.esl'), ('l.esl', 0xABC)); self.assertEqual(pg.resolve(0x00123456, 'l.esl'), ('a.esm', 0x123456))


class Format(unittest.TestCase):
    def test_roundtrip_and_validate(self):
        import validate
        s = fmt.Strings(); pn = s.add('skyrim.esm')
        meshes = [(1, 0, 0, fmt.NOGRID, fmt.NOGRID, 0, 4, 0, 2, 0, 0, 0, 0, 0, 0, 0, 100, 100, 0)]
        tris = [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)]
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'a.mfoenv'); fmt.write(p, dict(name='t'), [pn], s, meshes, [tuple(float(x) for x in v) for v in SQ], tris, [], [], [])
            e = fmt.read(p); self.assertEqual(e['plugins'], ['skyrim.esm']); self.assertEqual(e['meta']['name'], 't'); self.assertEqual(len(e['tris']), 2)
            self.assertEqual(validate.run_file(p), 0)
            with open(p, 'rb') as f: raw = bytearray(f.read())
            raw[:8] = b'XXXXXXXX'
            with open(p, 'wb') as f: f.write(raw)
            with self.assertRaises(ValueError): fmt.read(p)
    def test_export_from_synthetic_scan(self):
        import scan, library, export, validate, features
        with tempfile.TemporaryDirectory() as tmp:
            build_world(tmp); order = ['master.esm', 'override.esp']; where = {n: os.path.join(tmp, n) for n in order}
            sc = scan.scan_set(order, where, procs=1, log=lambda *a: None)
            unit = ('i', next(iter(sc['cells'])))
            groups = {unit: list(sc['navm'])}
            cands = {unit: {}}; by = library.ref_features(sc, cands, 256.0)
            p = os.path.join(tmp, 'e.mfoenv'); meta = export.export(sc, unit, groups, by, 'interior_small', {}, p, 'T')
            self.assertEqual(meta['counts']['refs'], 7); self.assertEqual(meta['counts']['meshes'], 1); self.assertEqual(meta['counts']['refsStartDisabled'], 3)
            self.assertEqual(meta['counts']['components'], 1)
            self.assertEqual(validate.run_file(p), 0)
            e = fmt.read(p); door = [r for r in e['refs'] if r[3] == R.K_DOOR and r[20] == 50][0]
            self.assertEqual(door[20], 50); self.assertTrue(door[5] & 16 and door[5] & 8)   # lock level, teleport + locked flags
            self.assertEqual(e['str'](door[34]), 'door.nif')


class WaterAndComponents(unittest.TestCase):
    def test_water_resolution(self):
        import export
        sc = dict(worlds={7: ('w', (0.0, -500.0))})
        self.assertEqual(export.water_height(sc, dict(flags=2, water=100.0, world=7)), (100.0, 'cell'))
        self.assertEqual(export.water_height(sc, dict(flags=2, water=3.4e38, world=7)), (-500.0, 'world'))      # FLT_MAX sentinel -> world default
        self.assertEqual(export.water_height(sc, dict(flags=2, water=-2147483648.0, world=7)), (-500.0, 'world'))
        self.assertEqual(export.water_height(sc, dict(flags=0, water=100.0, world=7)), (None, 'none'))          # no water bit
        self.assertEqual(export.water_height(sc, dict(flags=3, water=None, world=7)), (None, 'none'))           # interior has no world default
    def test_components(self):
        import export
        tris = [(0, 1, 2, 1, -1, -1, 0, 0), (0, 2, 3, 0, -1, -1, 0, 0), (0, 1, 2, -1, -1, -1, 0, 0), (0, 1, 2, -1, -1, -1, 0, 0)]
        # mesh 0 = tris 0,1; mesh 1 = tri 2; mesh 2 = tri 3.  link kind 0 joins tri1 with tri2 (walk); kind 2 joins tri2 -> tri3 (weak only); kind 3 joins nothing
        links = [(0, 1, 0, 0, 0, 1, 0, 0, 0.0), (1, 0, 0, 2, 0, 2, 0, 0, -100.0), (0, 0, 1, 3, 0, 2, 0, 0, 0.0)]
        w = export.components(tris, links, [0, 2, 3], (0,)); self.assertEqual(w[0], w[1]); self.assertEqual(w[1], w[2]); self.assertNotEqual(w[2], w[3])
        k = export.components(tris, links, [0, 2, 3], (0, 1, 2)); self.assertEqual(len(set(k)), 1)
        self.assertEqual(w[0], 0)    # biggest component is id 0
    def test_displaced_mesh_rule(self):
        import library
        def mesh(dz, flat=False): return S.nvnm([(x, y, (0 if flat else (100 if (x, y) == (0, 100) else 0)) + dz) for x, y, z in SQ], [(0, 1, 2, -1, -1, -1, 0, 0)], cell=0x55)
        sc = dict(navm={1: (0, mesh(0)), 2: (0, mesh(10)), 3: (0, mesh(-20000)), 4: (0, mesh(5))}, navinfo={k: (0, None, None, 0x55) for k in (1, 2, 3, 4)})
        self.assertEqual(library.displaced_meshes(sc), {3})
        sc2 = dict(navm={1: (0, mesh(0)), 2: (0, mesh(-20000))}, navinfo={k: (0, None, None, 0x55) for k in (1, 2)})
        self.assertEqual(library.displaced_meshes(sc2), set())      # too few neighbours to judge
        # override rule: winner from another plugin (pi 1 != owner 0), flat, far from the original; works with a single neighbour,
        # and two parked meshes do not shield each other
        sc3 = dict(navm={1: (0, mesh(0)), 2: (1, mesh(-30000, True)), 3: (1, mesh(-30000, True))}, navinfo={k: (0, None, None, 0x55) for k in (1, 2, 3)},
                   navorig={1: (0, 100), 2: (0, 100), 3: (0, 100)})
        self.assertEqual(library.displaced_meshes(sc3), {2, 3})
        # a flat override that stays near its original height (a small edit) is kept
        sc4 = dict(navm={1: (0, mesh(0)), 2: (1, mesh(40, True))}, navinfo={k: (0, None, None, 0x55) for k in (1, 2)}, navorig={1: (0, 100), 2: (0, 100)})
        self.assertEqual(library.displaced_meshes(sc4), set())


class CppReader(unittest.TestCase):
    def test_validating_reader_survives_corruption(self):
        if not shutil.which('g++'): self.skipTest('no g++')
        with tempfile.TemporaryDirectory() as tmp:
            import scan, library, export
            build_world(tmp); order = ['master.esm', 'override.esp']; where = {n: os.path.join(tmp, n) for n in order}
            sc = scan.scan_set(order, where, procs=1, log=lambda *a: None); unit = ('i', next(iter(sc['cells'])))
            by = library.ref_features(sc, {unit: {}}, 256.0)
            p = os.path.join(tmp, 'e.mfoenv'); export.export(sc, unit, {unit: list(sc['navm'])}, by, 'interior_small', {}, p, 'T')
            exe = os.path.join(tmp, 'chk')
            r = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', os.path.join(HERE, 'envfile_check.cpp'), '-o', exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[:2000])
            r = subprocess.run([exe, p, '2000'], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stdout + r.stderr[:2000])
            self.assertNotIn('accepted\n', r.stdout.replace('(only full-length)', ''))


class Selection(unittest.TestCase):
    def cands(self):
        rng = random.Random(5); c = {}; cl = {}
        for i in range(200):
            u = ('x', 60, i % 40, i // 40); c[u] = dict(areaUnits2=rng.random() * 1e8, layer=rng.random() * 0.02, narrow=rng.random(), wide=rng.random(), steep=rng.random() * 0.3,
                                                       zSpan=rng.random() * 5000, nActor=rng.randint(0, 9), nTele=rng.randint(0, 6), nLarge=rng.randint(0, 200), nCont=rng.randint(0, 9), water=rng.random(), tris=1000)
            cl[u] = C.classify(dict(c[u], nStuff=c[u]['nCont']), False)
        return c, cl
    def test_deterministic_and_separated(self):
        c, cl = self.cands(); a = SL.select(c, cl, 12, 7); b = SL.select(c, cl, 12, 7); self.assertEqual(a, b); self.assertTrue(10 <= len(a) <= 12)
        for i, u in enumerate(a):
            for v in a[:i]: self.assertGreaterEqual(max(abs(u[2] - v[2]), abs(u[3] - v[3])), SL.SEP)
        self.assertNotEqual(a, SL.select(c, cl, 12, 8))
    def test_sample_class(self):
        c, cl = self.cands(); k = next(iter(set(cl.values()))); s = SL.sample(c, cl, k, 3, 1)
        self.assertTrue(all(cl[u] == k for u in s)); self.assertEqual(s, SL.sample(c, cl, k, 3, 1))
    def test_quotas(self):
        q = SL.quotas(10, {'a': 3, 'b': 100, 'c': 0}); self.assertEqual(sum(q.values()), 10); self.assertLessEqual(q['a'], 3)
    def test_every_feature_set_has_a_class(self):
        self.assertEqual(C.classify(dict(areaUnits2=2e7, layer=0.0), True), 'interior_other'); self.assertEqual(C.classify({}, False), 'exterior_other')


class Geometry(unittest.TestCase):
    def test_bounds(self):
        import math
        mn, mx = R.world_bounds((-10, -20, -30, 10, 20, 30), (100, 0, 0), (0, 0, math.pi / 2), 1.0)
        self.assertAlmostEqual(mn[0], 80, 3); self.assertAlmostEqual(mx[1], 10, 3)
    def test_raster_layers(self):
        try: import numpy as np, features as F
        except ImportError: self.skipTest('numpy')
        v = np.array([(0, 0, 0), (640, 0, 0), (640, 640, 0), (0, 640, 0)] + [(0, 0, 300), (640, 0, 300), (640, 640, 300), (0, 640, 300)], np.float32)
        t = np.zeros(4, dtype=np.dtype([('v', '<u2', 3), ('nb', '<i2', 3), ('fl', '<u2'), ('cv', '<u2')]))
        t['v'] = [(0, 1, 2), (0, 2, 3), (4, 5, 6), (4, 6, 7)]
        R_ = F.Raster(0, 0, 10, 10); R_.add(v, t); R_.finish(); w = R_.window(0, 0, 640, 640)
        self.assertEqual(w['cells'], 100); self.assertGreater(w['layer'], 0.95); self.assertEqual(w['steep'], 0.0)


class Lists(unittest.TestCase):
    def test_game_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, 'ModOrganizer.ini'), 'w') as f: f.write('[General]\ngamePath=@ByteArray(Z:\\\\\\\\mnt\\\\gaming\\\\x y\\\\Game Root)\n')
            self.assertEqual(lists.game_path(tmp), '/mnt/gaming/x y/Game Root')


if __name__ == '__main__': unittest.main()
