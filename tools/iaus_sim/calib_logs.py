#!/usr/bin/env python3
"""iaus_sim/calib_logs.py -- real fight shapes out of saved MFO field logs, for calibrating the sim (v2).

READ-ONLY. usage: calib_logs.py <dir-or-MFO.log> [...]      (default: _evidence/*/MFO.log of the main checkout)

What the logs of today CAN give (passive lines that already exist):
  [hb]     worker ticks over wall time             -> the pump cadence
  [bleed]  DOWN / UP (alive after N s ...)         -> bleedout length, time alive before a down
  [sense]  foes=, hpLoss=%/s                       -> incoming damage rate while foes are present, foe counts
  [cast]   "... CAST <spell> ... AI-fired"         -> the ENGINE's own cast cadence per actor
  [cast]   gambit casts                            -> MFO's cast cadence per actor
  [target-pin] PIN                                 -> target switches per fight minute
A FIGHT is a run of [sense] lines with foes>0 for one follower (gap < 90 s). What they can NOT give is in the doc's
probe spec (swing cadence, damage per hit, time to kill).
"""
import glob, os, re, statistics as st, sys, collections

TS = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')


def secs(line):
    m = TS.match(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0 if m else None


def q(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')


def summ(name, v, unit=''):
    if not v: print('  %-44s no samples' % name); return
    print('  %-44s n=%-5d p10=%.2f p50=%.2f p90=%.2f mean=%.2f %s' % (name, len(v), q(v, .1), q(v, .5), q(v, .9), st.fmean(v), unit))


def main():
    args = sys.argv[1:] or sorted(glob.glob('/mnt/gaming/modlists/Projects/marth-follower-overhaul/_evidence/*/MFO.log'))
    files = []
    for a in args: files += sorted(glob.glob(os.path.join(a, '**', 'MFO.log'), recursive=True)) if os.path.isdir(a) else [a]
    pump, down_len, alive_before, hploss, foes, ai_gap, mfo_gap, pins_per_min, fight_len, conf = [], [], [], [], [], [], [], [], [], []
    ench_gap, spell_gap, potion_gap = [], [], []; last_kind = {}
    for f in files:
        hb = []; last_cast = {}; last_mfo = {}; sense = collections.defaultdict(list); pins = collections.defaultdict(list)
        with open(f, 'rb') as fh:
            for raw in fh:
                line = raw.decode('utf-8', 'replace'); t = secs(line)
                if t is None: continue
                if '[hb] ' in line:
                    m = re.search(r'worker ticks=(\d+)', line)
                    if m: hb.append((t, int(m.group(1))))
                elif '[bleed]' in line:
                    m = re.search(r'UP \(alive after ([\d.]+)s', line)
                    if m: down_len.append(float(m.group(1)))
                    m = re.search(r'DOWN \(.*was alive for ([\d.]+)s', line)
                    if m: alive_before.append(float(m.group(1)))
                elif '[sense]' in line:
                    m = re.search(r'\[sense\] (\w+): own=(\d+) foes=(\d+) load=([\d.]+) hpLoss=([\d.\-]+)%/s .*confidence=([\d.]+)', line)
                    if m:
                        sense[m.group(1)].append((t, int(m.group(3))))
                        if int(m.group(3)) > 0: hploss.append(float(m.group(5))); foes.append(int(m.group(3))); conf.append(float(m.group(6)))
                elif '[cast]' in line:
                    m = re.search(r'\[cast\] (\w+) .*? CAST .*formType=(\d+) .*AI-fired', line)
                    if m:
                        a = m.group(1); kk = (a, m.group(2)); dst = {'21': ench_gap, '22': spell_gap, '46': potion_gap}.get(m.group(2))
                        if dst is not None and kk in last_kind and 0.05 < t - last_kind[kk] < 20: dst.append(t - last_kind[kk])
                        last_kind[kk] = t
                        if a in last_cast and 0 < t - last_cast[a] < 20: ai_gap.append(t - last_cast[a])
                        last_cast[a] = t
                    else:
                        m = re.search(r'\[cast\] (\w+) ', line)
                        if m and ('claim filed' in line or 'FIRED' in line or 'direct' in line.lower()):
                            a = m.group(1)
                            if a in last_mfo and 0 < t - last_mfo[a] < 30: mfo_gap.append(t - last_mfo[a])
                            last_mfo[a] = t
                elif '[target-pin]' in line and ' PIN ' in line:
                    m = re.search(r'\[target-pin\] (\w+): PIN', line)
                    if m: pins[m.group(1)].append(t)
        for (t0, n0), (t1, n1) in zip(hb, hb[1:]):
            if 4 < t1 - t0 < 400 and n1 > n0: pump.append(1000.0 * (t1 - t0) / (n1 - n0))
        last_kind.clear()
        for a, rows in sense.items():   # fights: runs of foes>0
            start = None; last = None
            for t, n in rows:
                if n > 0:
                    if start is None or t - last > 90:
                        if start is not None and last - start > 0: fight_len.append(last - start)
                        start = t
                    last = t
                elif start is not None:
                    fight_len.append(max(last - start, 0.0));
                    k = [p for p in pins.get(a, []) if start <= p <= last + 5]
                    if last - start >= 20: pins_per_min.append(60.0 * len(k) / (last - start))
                    start = None
    print('%d log(s)' % len(files))
    summ('pump period (ms per worker tick)', pump, 'ms')
    summ('bleedout length (s down before UP)', down_len, 's')
    summ('time alive before a DOWN', alive_before, 's')
    summ('incoming damage while foes present (hp %/s)', hploss, '%/s')
    summ('  ... of those, the non-zero ones', [x for x in hploss if x > 0], '%/s')
    summ('foes on a follower ([sense] foes>0)', foes)
    summ('confidence while foes present', conf)
    summ('engine AI cast gap, same actor (<20 s)', ai_gap, 's')
    summ('  weapon-enchant procs (formType 21) = HIT cadence', ench_gap, 's')
    summ('  ... of those under 6 s (consecutive swings)', [x for x in ench_gap if x < 6], 's')
    summ('  engine spell casts (formType 22)', spell_gap, 's')
    summ('  ... of those under 8 s', [x for x in spell_gap if x < 8], 's')
    summ('  engine potion / food use (formType 46)', potion_gap, 's')
    summ('MFO cast gap, same actor (<30 s)', mfo_gap, 's')
    summ('fight span seen by [sense] (>=1 sample)', [x for x in fight_len if x > 0], 's')
    summ('target pins per fight minute', pins_per_min, '/min')


if __name__ == '__main__':
    main()
