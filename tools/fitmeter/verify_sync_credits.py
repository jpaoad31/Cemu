#!/usr/bin/env python3
"""What the game credits per sync (Fit Meter Challenge steps, height; the step records it compares against),
over a series of syncs with the virtual meter's demo, through the game's own SAMUProcess::SAMUDataSave with its
real per-date step records (RPHealthSaveManager OtherMeasureData*, get/setStep).

  python3 tools/fitmeter/verify_sync_credits.py --since 2026-11-23T00:48 2026-11-23T00:48 2026-11-23T00:49 ...
"""
import sys, os, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import decoder_harness as H  # noqa: E402

argv = sys.argv[1:]
since = None
if '--since' in argv:
    k = argv.index('--since'); since = argv[k + 1]; del argv[k:k + 2]
expect_zero = '--expect-zero-resync' in argv
argv = [a for a in argv if not a.startswith('--')]
out = os.path.join(ROOT, 'build', 'fitmeter_credits')
save = None
prev_n = 0
fails = 0
results = []
import datetime, struct
last = None
for when in argv:
    # SAMUReceiveProcessOnStageWait: banks 02-04 are asked for min(10080, minutes since the transfer + 10) bytes
    t = datetime.datetime.fromisoformat(when)
    size = 10080 if last is None else min(10080, int((t - last).total_seconds() // 60) + 10)
    last = t
    subprocess.run([os.path.join(ROOT, 'build', 'fitmeter_test'), '--dump', 'demo', when, out, '--minute-bytes', str(size)]
                   + (['--since', since] if since else []), check=True, stdout=subprocess.DEVNULL)
    streams = {b: open(os.path.join(out, 'b%d.bin' % b), 'rb').read() for b in (2, 3, 4, 5, 6)}
    d, t = when.split('T')
    now = tuple(map(int, d.split('-'))) + tuple(map(int, t.split(':')))
    save, t32, calls, count, ctx = H.run(streams, now, save)
    new = save.credits[prev_n:]
    prev_n = len(save.credits)
    steps = sum(v for n, v in new if n == 'addFMCUnsettledStep')
    height = sum(v for n, v in new if n == 'addFMCUnsettledHeight')
    sent = 0
    import struct
    for i in range(60):
        v = struct.unpack('>H', streams[6][2 * i:2 * i + 2])[0]
        if v == 0xFFFF:
            break
        sent += v
    results.append((when, steps, height, sent))
    print('%s: asked %5d minute bytes; the meter sent %6d steps in bank 06; the game credited %6d steps, height %d' % (when, size, sent, steps, height))
# what the save holds per day after the last sync: steps, and the minutes with METs > 0 / altitude / a tag
mem, call, proc = ctx
for i in sorted(range(50), key=lambda i: -save.date[i]):
    if not save.date[i]:
        continue
    p = 0x0E100000
    mem.w16(p, save.date[i])
    call('getCalendarTime__13RPSysCalendarSFPCUsRUiN22', p, p + 4, p + 8, p + 12)
    nm = sum(1 for v in save.mets[i] if v != 0xFF)
    na = sum(1 for v in save.mets[i] if v not in (0xFF, 0))
    nh = sum(1 for v in save.height[i] if v != 0x7FFF)
    up = sum(v for v in save.height[i] if v != 0x7FFF and v > 0)
    nt = sum(1 for v in save.tag[i] if v != 0xFF)
    print('  %04d-%02d-%02d: %6d steps, %5d kcal; minutes: METs %4d (%4d moving), altitude %4d (+%d m), tag %4d' % (
        mem.r32(p + 4), mem.r32(p + 8), mem.r32(p + 12), save.steps[i], save.kcal[i], nm, na, nh, up, nt))
# a re-sync right after a sync credits nothing new
for (w0, s0, h0, x0), (w1, s1, h1, x1) in zip(results, results[1:]):
    if w0.split('T')[0] == w1.split('T')[0] and x1 == x0 and s1 != 0:
        print('FAIL: %s re-sync credited %d steps' % (w1, s1)); fails += 1
print('%d failures' % fails)
sys.exit(1 if fails else 0)
