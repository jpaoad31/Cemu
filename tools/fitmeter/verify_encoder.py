#!/usr/bin/env python3
"""Encoder check against the game: the virtual meter's streams (from build/fitmeter_test --dump, i.e. the
C++ encoder through the IR link and the IRCU slave, XOR undone) decoded by Wii Fit U's own
SAMUProcess::SAMUDataSave (decoder_harness.py), compared with what activity.json says.

  python3 tools/fitmeter/verify_encoder.py [activity.json|demo] [YYYY-MM-DDTHH:MM] [--night-owl]

"demo" checks the built-in demo source (the reference is the activity_used.json the dump writes).
--night-owl sets the meter's and the game's day start to 3:00.
"""
import sys, os, json, subprocess, datetime, math, struct


def rnd(x):
    # the encoder's rounding: half away from zero, in float (lroundf)
    x = struct.unpack('f', struct.pack('f', x))[0]
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import decoder_harness as H  # noqa: E402

argv = sys.argv[1:]
since = None
if '--since' in argv:
    k = argv.index('--since'); since = argv[k + 1]; del argv[k:k + 2]
args = [a for a in argv if not a.startswith('--')]
night_owl = '--night-owl' in argv
act_path = args[0] if len(args) > 0 else os.path.join(HERE, 'sample_activity.json')
when = args[1] if len(args) > 1 else '2026-11-22T14:30'
out = os.path.join(ROOT, 'build', 'fitmeter_dump')
subprocess.run([os.path.join(ROOT, 'build', 'fitmeter_test'), '--dump', act_path, when, out] + (['--night-owl'] if night_owl else [])
               + (['--since', since] if since else []),
               check=True, stdout=subprocess.DEVNULL)
act_path = os.path.join(out, 'activity_used.json')
streams = {b: open(os.path.join(out, 'b%d.bin' % b), 'rb').read() for b in (2, 3, 4, 5, 6)}
d, t = when.split('T')
now = tuple(map(int, d.split('-'))) + tuple(map(int, t.split(':')))
save, t32, calls, count, ctx = H.run(streams, now, night_owl=night_owl)
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
# the per-day banks' today: before 3:00 with night owl it is still yesterday (SaveU16Data_)
day_today = today - datetime.timedelta(days=1) if night_owl and now[3] < 3 else today
fails = 0
checks = 0

def check(cond, msg):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print('FAIL', msg)

days = {datetime.date.fromisoformat(x['date']): x for x in act['days']}
oldest_steps = min(d for d, j in days.items() if 'steps' in j or 'kcal' in j)
for i in range(60):
    day = day_today - datetime.timedelta(days=i)
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
    for k, v in enumerate(j.get('climb', [])): climb[k] += v or 0.0
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
# the game asks 10080 bytes: older minutes may not fit. The stream's own length says where it starts.
b2 = streams[2]
covered, i = 0, 0
while i < len(b2) and b2[i] != 0xFF:
    if i + 1 < len(b2) and b2[i + 1] == 0:
        covered += b2[i]; i += 2
    else:
        covered += 1; i += 1
if covered:
    start_abs = datetime.datetime(*now) - datetime.timedelta(minutes=covered - 1)
    cov = (start_abs.date(), start_abs.hour * 60 + start_abs.minute)
    if first_mets is None or cov > first_mets:
        first_mets = first_tag = first_alt = cov
print('per-minute streams cover %d minutes from %s %02d:%02d' % (covered, first_mets[0], first_mets[1] // 60, first_mets[1] % 60) if covered else 'no per-minute data')
checked_minutes = 0
level = 0.0         # the expected altitude relative to the stream's first minute
got_level = 0       # the decoded one (sum of the per-minute changes)
ref = None
for day in sorted(days):
    if day > today:
        continue
    slot = decoded.get(day)
    if slot is None:
        # the save has 50 day slots: an older day can be pushed out
        check(day < min(day_today, today) - datetime.timedelta(days=48), '%s: no slot' % day)
        continue
    mets, tag, climb, alt = minute_arrays(days[day])
    last = nowmin if day == today else 1439
    for k in range(last + 1):
        at = (day, k)
        if first_mets and at >= first_mets:
            wm = 0 if mets[k] is None else max(0, min(249, rnd(struct.unpack('f', struct.pack('f', mets[k]))[0] * 10)))
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
# the dates: every day slot the game filled must be a day the source has data for, within today and the
# meter's memory (its registration), and none after today
expected_days = {d for d, j in days.items() if 'steps' in j or 'kcal' in j or any(
    v is not None for v in (j.get('mets') or [])) or any(v not in (None, 0, 0.0) for v in (j.get('climb') or []))}
first_day = min(expected_days) if expected_days else today
for d in decoded:
    # days between the oldest day with data and today are sent (as 0 when the source has none)
    check(first_day <= d, 'the game filled %s, before the meter has data (%s)' % (d, first_day))
    check(d <= today, 'the game filled %s, after today' % d)
    if since:
        sd = datetime.date.fromisoformat(since.split('T')[0])
        check(d >= sd - datetime.timedelta(days=1), 'the game filled %s, before the registration %s' % (d, since))
print('decoded days:', ', '.join('%s:%d' % (k, save.steps[v]) for k, v in sorted(decoded.items())))
print('%d checks (%d minutes), %d failures; game decoder ran %d instructions' % (checks, checked_minutes, fails, count))
sys.exit(1 if fails else 0)
