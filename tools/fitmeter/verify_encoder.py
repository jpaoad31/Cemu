#!/usr/bin/env python3
"""Encoder check against the game: the virtual meter's streams (from build/fitmeter_test --dump, i.e. the
C++ encoder through the IR link and the IRCU slave, XOR undone) decoded by Wii Fit U's own
SAMUProcess::SAMUDataSave (decoder_harness.py), compared with what activity.json says.

  python3 tools/fitmeter/verify_encoder.py [activity.json] [YYYY-MM-DDTHH:MM]
"""
import sys, os, json, subprocess, datetime, math
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import decoder_harness as H  # noqa: E402

act_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'sample_activity.json')
when = sys.argv[2] if len(sys.argv) > 2 else '2026-11-22T14:30'
out = os.path.join(ROOT, 'build', 'fitmeter_dump')
subprocess.run([os.path.join(ROOT, 'build', 'fitmeter_test'), '--dump', act_path, when, out], check=True)
streams = {b: open(os.path.join(out, 'b%d.bin' % b), 'rb').read() for b in (2, 3, 4, 5, 6)}
d, t = when.split('T')
now = tuple(map(int, d.split('-'))) + tuple(map(int, t.split(':')))
save, t32, calls, count, ctx = H.run(streams, now)
mem, call, proc = ctx

def ymd(d16):
    p = 0x0E100000
    mem.w16(p, d16)
    call('getCalendarTime__13RPSysCalendarSFPCUsRUiN22', p, p + 4, p + 8, p + 12)
    return datetime.date(mem.r32(p + 4), mem.r32(p + 8), mem.r32(p + 12))

decoded = {ymd(save.date[i]): i for i in range(50) if save.date[i]}
act = json.load(open(act_path))
today = datetime.date(*now[:3])
nowmin = now[3] * 60 + now[4]
fails = 0
checks = 0

def check(cond, msg):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print('FAIL', msg)

days = {datetime.date.fromisoformat(x['date']): x for x in act['days']}
oldest_steps = min(days)
for i in range(60):
    day = today - datetime.timedelta(days=i)
    if day < oldest_steps:
        break
    j = days.get(day, {})
    want_steps = min(j.get('steps', 0), 65529)
    want_kcal = j.get('kcal', round(j['steps'] * 0.04) if 'steps' in j else 0)
    want_kcal = min(want_kcal, 0xFFF9)
    slot = decoded.get(day)
    if i >= 50:
        continue
    check(slot is not None, '%s: no slot' % day)
    if slot is None:
        continue
    check(save.steps[slot] == want_steps, '%s steps %d want %d' % (day, save.steps[slot], want_steps))
    check(save.kcal[slot] == want_kcal, '%s kcal %d want %d' % (day, save.kcal[slot], want_kcal))

# per-minute expectations
def minute_arrays(j):
    mets = [None] * 1440; tag = [None] * 1440; climb = [0.0] * 1440; alt = [None] * 1440
    for s in j.get('segments', []):
        h, m = map(int, s['start'].split(':'))
        st = h * 60 + m
        n = s.get('minutes', 1)
        for k in range(st, st + n):
            if 'mets' in s: mets[k] = s['mets']
            if 'tag' in s: tag[k] = s['tag']
            if 'climb_m' in s: climb[k] += s['climb_m'] / n
    for k, v in enumerate(j.get('mets', [])): mets[k] = v
    for k, v in enumerate(j.get('altitude', [])): alt[k] = v
    for k, v in enumerate(j.get('tag', [])): tag[k] = v
    return mets, tag, climb, alt

def first_minute(pred):
    for day in sorted(days):
        arrs = minute_arrays(days[day])
        for k in range(1440):
            if pred(arrs, k):
                return (day, k)
    return None

# the three per-minute streams start together, at the first minute with any per-minute value
first_mets = first_tag = first_alt = first_minute(
    lambda a, k: a[0][k] is not None or a[1][k] is not None or a[3][k] is not None or a[2][k] != 0)
checked_minutes = 0
level = 0.0         # the expected altitude relative to the stream's first minute
got_level = 0       # the decoded one (sum of the per-minute changes)
ref = None
for day in sorted(days):
    if day > today:
        continue
    slot = decoded.get(day)
    mets, tag, climb, alt = minute_arrays(days[day])
    last = nowmin if day == today else 1439
    for k in range(last + 1):
        at = (day, k)
        if first_mets and at >= first_mets:
            wm = 0 if mets[k] is None else max(0, min(249, round(mets[k] * 10)))
            check(slot is not None and save.mets[slot][k] == wm, '%s %02d:%02d METs %s want %d' % (day, k // 60, k % 60, save.mets[slot][k] if slot is not None else None, wm))
        elif slot is not None:
            check(save.mets[slot][k] == 0xFF, '%s %02d:%02d METs before the data %d' % (day, k // 60, k % 60, save.mets[slot][k]))
        if first_tag and at >= first_tag:
            wt = tag[k] if tag[k] is not None else (act.get('default_tag', 0) if mets[k] is not None else 0)
            check(save.tag[slot][k] == wt, '%s %02d:%02d tag %d want %d' % (day, k // 60, k % 60, save.tag[slot][k], wt))
        if first_alt and at >= first_alt:
            if alt[k] is not None:
                if ref is None:
                    ref = round(alt[k]) - level
                level = round(alt[k]) - ref
            level += climb[k]
            if at != first_alt:  # the stream's first minute becomes the day's base, not a value
                v = save.height[slot][k]
                check(v != 0x7FFF, '%s %02d:%02d no altitude' % (day, k // 60, k % 60))
                got_level += v
                # a change too big for one minute (-122..+127) spills over: the minute is at the limit
                diff = level - got_level
                ok = abs(diff) <= 1 or (v == 127 and diff > 0) or (v == -122 and diff < 0)
                check(ok, '%s %02d:%02d altitude %d want %.1f' % (day, k // 60, k % 60, got_level, level))
        checked_minutes += 1
check(abs(got_level - level) <= 1, 'final altitude %d want %.1f' % (got_level, level))
print('decoded days:', ', '.join('%s:%d' % (k, save.steps[v]) for k, v in sorted(decoded.items())))
print('%d checks (%d minutes), %d failures; game decoder ran %d instructions' % (checks, checked_minutes, fails, count))
sys.exit(1 if fails else 0)
