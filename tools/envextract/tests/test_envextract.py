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
    cellrec = S.rec(b'CELL', 0x100, [(b'EDID', b'TestCell\0'), (b'DATA', b'\x01\x00'), (b'XOWN', struct.pack('<I', 0x302))])
    navm = S.rec(b'NAVM', 0x200, [(b'NVNM', S.nvnm(SQ, [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)], cell=0x100))])
    def refr(fid, base, pos, extra=(), flags=0, comp=False):
        return S.rec(b'REFR', fid, [(b'NAME', struct.pack('<I', base)), (b'DATA', struct.pack('<6f', *pos, 0, 0, 0))] + list(extra), flags, comp)
    refs = refr(0x400, 0x300, (10, 10, 0), [(b'XTEL', struct.pack('<I6fI', 0x401, 0, 0, 0, 0, 0, 0, 0)), (b'XLOC', struct.pack('<B3xIB3x8x', 50, 0x500, 0))], comp=True) \
        + refr(0x401, 0x300, (50, 50, 0)) + refr(0x402, 0x301, (20, 20, 0), [(b'XSCL', struct.pack('<f', 2.0))]) \
        + refr(0x410, 0x300, (1, 1, 0), flags=0x800) \
        + refr(0x411, 0x300, (2, 2, 0), [(b'XESP', struct.pack('<IB3x', 0x410, 0))], flags=0x800) \
        + refr(0x412, 0x300, (3, 3, 0), [(b'XESP', struct.pack('<IB3x', 0x410, 1))]) \
        + refr(0x413, 0x300, (4, 4, 0), [(b'XESP', struct.pack('<IB3x', 0x14, 0)), (b'XOWN', struct.pack('<I', 0x302)), (b'XACT', struct.pack('<I', 0x0D))], flags=0x800)
    achr = S.rec(b'ACHR', 0x403, [(b'NAME', struct.pack('<I', 0x302)), (b'DATA', struct.pack('<6f', 5, 5, 0, 0, 0, 0))])
    cellgrp = S.group(0x100, 6, S.group(0x100, 9, S.rec(b'NAVM', 0x200, [(b'NVNM', S.nvnm(SQ, [(0, 1, 2, -1, -1, 1, 0x800, 0), (0, 2, 3, 0, -1, -1, 0x800, 0)], cell=0x100))]) + refs + achr))
    interior = S.group(b'CELL', 0, S.group(0, 2, S.group(0, 3, cellrec + cellgrp)))
    door = S.rec(b'DOOR', 0x300, [(b'EDID', b'TestDoor\0'), (b'OBND', struct.pack('<6h', -50, -5, 0, 50, 5, 200)), (b'MODL', b'door.nif\0')])
    stat = S.rec(b'STAT', 0x301, [(b'EDID', b'BigRock\0'), (b'OBND', struct.pack('<6h', -200, -200, 0, 200, 200, 300))])
    npc = S.rec(b'NPC_', 0x302, [(b'EDID', b'Guy\0')])
    m = S.plugin([], S.group(b'DOOR', 0, door) + S.group(b'STAT', 0, stat) + S.group(b'NPC_', 0, npc) + interior, flags=1)
    # override: delete ref 0x402, disable 0x401, move nothing
    cellgrp2 = S.group(0x100, 6, S.group(0x100, 9, S.rec(b'REFR', 0x402, [], flags=0x20) + refr(0x401, 0x300, (50, 50, 0), flags=0x800)))
    ov = S.plugin(['master.esm'], S.group(b'CELL', 0, S.group(0, 2, S.group(0, 3, S.rec(b'CELL', 0x100, [(b'EDID', b'TestCell\0'), (b'DATA', b'\x01\x00'), (b'XOWN', struct.pack('<I', 0x302))]) + cellgrp2))))
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


class Round4(unittest.TestCase):
    def _env(self, tmp):
        import scan, library, export
        build_world(tmp); order = ['master.esm', 'override.esp']; where = {n: os.path.join(tmp, n) for n in order}
        sc = scan.scan_set(order, where, procs=1, log=lambda *a: None); unit = ('i', next(iter(sc['cells'])))
        by = library.ref_features(sc, {unit: {'areaUnits2': 1e6}}, 256.0)
        p = os.path.join(tmp, 'e.mfoenv'); meta = export.export(sc, unit, {unit: list(sc['navm'])}, by, 'interior_small', {}, p, 'T')
        return sc, p, meta
    def test_owner_xact_and_door_rows(self):
        import validate
        with tempfile.TemporaryDirectory() as tmp:
            sc, p, meta = self._env(tmp); e = fmt.read(p)
            own = [r for r in e['refs'] if r[35]]
            self.assertEqual(len(own), len(e['refs']))                       # own owner or the cell owner for every ref
            mine = [r for r in e['refs'] if (r[0] & 0xFFFFFF) == 0x413][0]
            self.assertTrue(mine[5] & 128 and not mine[5] & 256)             # own XOWN, base is an NPC_
            other = [r for r in e['refs'] if (r[0] & 0xFFFFFF) == 0x400][0]
            self.assertTrue(other[5] & 256 and other[5] & 128)               # inherited from the cell
            self.assertEqual(meta['cells'][0]['owner'] & 0xFFFFFF, 0x302)
            d = {x[0]: x for x in e['dref']}                                 # every door ref has a row
            self.assertEqual(len(e['dref']), sum(1 for r in e['refs'] if r[3] == R.K_DOOR))
            ri = [i for i, r in enumerate(e['refs']) if (r[0] & 0xFFFFFF) == 0x413][0]
            self.assertEqual(d[ri][4], 1)                                    # XACT 0x0D has the open-by-default bit
            self.assertEqual(d[[i for i, r in enumerate(e['refs']) if (r[0] & 0xFFFFFF) == 0x411][0]][4], 0)
            self.assertEqual(validate.run_file(p), 0)
    def test_door_rows_join(self):
        import export
        verts = [(0, 0, 0), (100, 0, 0), (0, 100, 0), (1000, 0, 0), (1100, 0, 0), (1000, 100, 0)]
        tris = [(0, 1, 2, -1, -1, -1, 0, 0), (0, 1, 2, -1, -1, -1, 0, 0)]
        meshes = [(1, 0, 0, 0, 0, 0, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1), (2, 0, 0, 0, 0, 3, 3, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1)]
        doors = [(1, 0, 0, 5, 7, 1)]       # mesh 1 tri 0 belongs to ref row 1
        info = [(0, (30, 30, 0), True, 25, 0, 0, 9), (1, (5000, 0, 0), False, 0, 1, 0, 0), (2, (1030, 20, 0), False, 0, 0, 1, 0)]
        rows = export.door_rows(info, doors, meshes, verts, tris)
        self.assertEqual(rows[0][1:3], (0, 0)); self.assertEqual(rows[0][4:], (1, 25, 0, 0, 9))          # nearest tri, open default, lock, key
        self.assertEqual(rows[1][1:3], (1, 0)); self.assertEqual(rows[1][3], 0.0)                         # exact join through the DOOR row
        self.assertEqual(rows[2][1:3], (1, 0))         # within 200 units of the second triangle
    def test_seam_rows(self):
        import export
        verts = [(0, 0, 0), (100, 0, 0), (0, 100, 0), (110, 0, 0), (200, 0, 0), (110, 100, 0)]   # mesh B starts 10 units to the east of A's edge
        tris = [(0, 1, 2, -1, -1, -1, 1, 0), (0, 1, 2, -1, -1, -1, 0, 0)]
        meshes = [(1, 0, 0, 0, 0, 0, 3, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1), (2, 0, 0, 0, 0, 3, 3, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1)]
        links = [(0, 0, 0, 0, 0, 1, 0, 0, 0.0)]          # mesh 0 tri 0 edge 0 (v0-v1, y=0) -> mesh 1 tri 0
        s = export.seam_rows(links, meshes, verts, tris)
        self.assertEqual(len(s), 1); self.assertAlmostEqual(s[0][1], 10.0, 3)
        links2 = [(0, 0, 0, 0, 0, 1, 0, 0, 0.0)]; verts2 = list(verts); verts2[3] = (100, 0, 0); verts2[4] = (200, 0, 0)
        self.assertEqual(export.seam_rows(links2, meshes, verts2, tris), [])                               # edges meet: no row
    def test_land_decode_and_file(self):
        import land, validate
        vh = struct.pack('<f', 10.0) + bytes([1] + [2] * 32) + bytes([3] + [0] * 32) + bytes(33 * 31) + b'\0\0\0'
        g = land.decode_vhgt(vh)
        self.assertEqual(g[0][0], (10 + 1) * 8.0); self.assertEqual(g[0][1], (10 + 1 + 2) * 8.0)          # row 0: offset + first byte, then cumulative
        self.assertEqual(g[1][0], (10 + 1 + 3) * 8.0); self.assertEqual(g[1][1], (10 + 1 + 3) * 8.0)      # next row starts from the previous row's first value
        self.assertAlmostEqual(land.height_at(g, 128.0, 0.0), g[0][1], 3)
        s = fmt.Strings(); pn = s.add('skyrim.esm')
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'l.mfoenv')
            fmt.write(p, dict(name='t'), [pn], s, [(1, 0, 0, 3, -2, 0, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1)], [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
                      [(0, 1, 2, -1, -1, -1, 0, 0)], [], [], [], None, [], [], [(3, -2, *[h for row in g for h in row])])
            e = fmt.read(p); self.assertEqual(len(e['land']), 1); self.assertEqual(e['land'][0][:2], (3, -2)); self.assertEqual(validate.run_file(p), 0)
    def test_moved_refs_flag_places(self):
        import library
        sc = dict(navm={}, navinfo={}, cells={5: dict(flags=1, persist=False, grid=None, world=0)}, movedfrom={5: 2}, _displaced=set(), _doubtful=set())
        self.assertEqual(library.place_flags(sc, [('i', 5), ('i', 6)]), {('i', 5): {'plugin_moved_refs'}, ('i', 6): set()})


class Anomalies(unittest.TestCase):
    def mesh(self, nv):
        import features as F
        return F.parse_np(nv)
    def test_counts(self):
        import anomaly as AN
        sq = [(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)]
        good = S.nvnm(sq, [(0, 1, 2, -1, -1, 1, 0, 0), (0, 2, 3, 0, -1, -1, 0, 0)], cell=1)
        r = AN.analyze_mesh(self.mesh(good), lambda raw: raw, lambda k: None)
        self.assertEqual((r['oneway'], r['notswapped'], r['stale'], r['kind3'], r['ownerless']), (0, 0, 0, 0, 0))
        oneway = S.nvnm(sq, [(0, 1, 2, -1, -1, 1, 0, 0), (0, 2, 3, -1, -1, -1, 0, 0)], cell=1)
        self.assertEqual(AN.analyze_mesh(self.mesh(oneway), lambda raw: raw, lambda k: None)['oneway'], 1)
        bad = S.nvnm(sq, [(0, 1, 2, 1, -1, -1, 0, 0), (0, 2, 3, -1, -1, 0, 0, 0)], cell=1)      # neighbours by index but the wrong edges
        self.assertEqual(AN.analyze_mesh(self.mesh(bad), lambda raw: raw, lambda k: None)['notswapped'] >= 1, True)
        links = S.nvnm(sq, [(0, 1, 2, 0, 1, 2, 7, 0)], links=[(3, 9, 0), (0, 9, 99), (2, 9, 0)], cell=1)   # three external edges: kind 3, stale target, kind 2 drop
        tgt = self.mesh(S.nvnm(sq, [(0, 1, 2, -1, -1, -1, 0, 0)], cell=1))
        r = AN.analyze_mesh(self.mesh(links), lambda raw: raw, lambda k: tgt)
        self.assertEqual((r['kind3'], r['stale'], r['dropLow']), (1, 1, 1))
    def test_components_and_graph(self):
        import anomaly as AN, features as F
        sq = [(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0)]
        a = F.parse_np(S.nvnm(sq, [(0, 1, 2, -1, -1, 1, 0, 0), (0, 2, 3, 0, -1, -1, 0, 0)], cell=1))
        far = F.parse_np(S.nvnm([(x + 5000, y, z) for x, y, z in sq], [(0, 1, 2, -1, -1, -1, 0, 0)] * 1, cell=1))
        share, isl, comp, off = AN.place_components([(1, a), (2, far)], {1: 0, 2: 1}, lambda k, raw: raw)
        self.assertAlmostEqual(share, 10000 / 15000.0, 2); self.assertEqual(isl, 0)        # the lone triangle is a small island (< 30 triangles)
        g = AN.coarse_graph_features([(1, a)], {1: 0}, lambda k, raw: raw)
        self.assertGreaterEqual(g['nodes'], 1)
    def test_reject_and_tags(self):
        import coverage as CV
        f = dict(interior=True, oneway=0, notswapped=0, stale=0, ownerless=0, kind3=0, mainShare=1.0)
        self.assertEqual(CV.reject_reasons(f, set(), 0.6), [])
        f2 = dict(f, oneway=2, kind3=1, mainShare=0.3)
        self.assertEqual(CV.reject_reasons(f2, {'displaced_mesh'}, 0.6), ['displaced_mesh', 'one_way_or_not_swapped_edges', 'kind3_portals', 'small_main_component'])
    def test_coverage_pick(self):
        import coverage as CV
        rng = random.Random(3); cands = {}; tagsets = {}
        names = CV.TAGNAMES
        for i in range(120):
            u = ('x', 60, i % 30, i // 30); cands[u] = {}
            tagsets[u] = frozenset(rng.sample(names, 5))
        pairs = CV.common_pairs(list(tagsets.values()), 0.02)
        ch, unmet = CV.pick(cands, tagsets, 2, 100, 7, 0.02)
        self.assertEqual(ch, CV.pick(cands, tagsets, 2, 100, 7, 0.02)[0])                  # deterministic
        for t in names:
            if any(t in tagsets[u] for u in cands): self.assertGreaterEqual(sum(1 for u in ch if t in tagsets[u]), 2) if not unmet else None
        ch2, _ = CV.pick(cands, tagsets, 2, 5, 7, 0.02); self.assertLessEqual(len(ch2), 5)  # the cap wins, the rest is reported unmet
        ch3, un3 = CV.pick(cands, tagsets, 2, 100, 7, 0.02, exclude=set(ch)); self.assertFalse(set(ch) & set(ch3))   # held-out never reuses a library place


class CppNewSections(unittest.TestCase):
    def test_old_v2_file_without_new_sections_loads_and_new_ones_fuzz(self):
        if not shutil.which('g++'): self.skipTest('no g++')
        import land
        with tempfile.TemporaryDirectory() as tmp:
            s = fmt.Strings(); pn = s.add('skyrim.esm'); g = land.decode_vhgt(struct.pack('<f', 1.0) + bytes(1092))
            kw = [(1, 0, 0, 3, -2, 0, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1)]
            p = os.path.join(tmp, 'n.mfoenv')
            fmt.write(p, dict(name='t'), [pn], s, kw, [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)], [(0, 1, 2, -1, -1, -1, 0, 0)], [], [], [], None, [], [], [(3, -2, *[h for row in g for h in row])])
            exe = os.path.join(tmp, 'chk'); r = subprocess.run(['g++', '-std=c++17', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', os.path.join(HERE, 'envfile_check.cpp'), '-o', exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[:1500])
            r = subprocess.run([exe, p, '1500'], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stdout + r.stderr[:1500])
            with open(p, 'rb') as fh: raw = bytearray(fh.read())
            n = struct.unpack_from('<I', raw, 12)[0]; struct.pack_into('<I', raw, 12, n - 3)      # an older v2 file: no DREF/SEAM/LAND entries
            q = os.path.join(tmp, 'old.mfoenv')
            with open(q, 'wb') as fh: fh.write(raw)
            r = subprocess.run([exe, q, '300'], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stdout + r.stderr[:1500])


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
