#!/usr/bin/env python3
"""Runs Wii Fit U's own Fit Meter decoder (SAMUProcess::SAMUDataSave, v81 step.elf) on log streams.

The streams are what the virtual meter sends for SAMU banks 02..06 (after the XOR is undone).
The game's RPHealthSaveManager get/setSAMU* accessors are replaced by a Python model of
RPStepSAMUData (50 day slots); everything between is the original PowerPC code, run in the decomp's
interpreter (tools/emu/ppc.py).

Usage:
  decoder_harness.py --now 2026-11-22T14:30 [--b2 f] [--b3 f] [--b4 f] [--b5 f] [--b6 f] [--json out.json]
Each --bN is a raw stream file (the meter's bytes, un-XORed).
"""
import sys, os, struct, json, argparse, datetime

DECOMP = os.path.expanduser(os.environ.get('WFU_DECOMP', '~/github/jpaoad31/wii-fit-u'))
sys.path.insert(0, DECOMP + '/tools/emu')
sys.path.insert(0, DECOMP + '/tools/split')
import ppc, rpxelf  # noqa: E402
from ppc import M32  # noqa: E402

ELF = DECOMP + '/build/rpx81/step.elf'
TIMER = 248625000 // 4  # the Wii U's OS timer (ticks per second)
IMPORT_BASE = {'.fimport_coreinit': 0x2b58000}  # the bl targets in the original text (step.dis)

SAVEMGR_PTR = 0x1014DB50       # RPHealthSaveManager::sInstance (lbl -9392(4117<<16))
PARAM_PTR = 0x1014DB54         # RPHealthParameter::sInstance (-9388)
BINS = 0x11C3F444              # jet::SAMUSeq's BinContainer list
BIN_COUNT = 0x11C3FA20


class SamuSave:
    """RPStepSAMUData as the accessors see it (50 days)."""
    def __init__(self):
        self.date = [0] * 50
        self.steps = [0] * 50
        self.kcal = [0] * 50
        self.mets = [[0xFF] * 1440 for _ in range(50)]
        self.height = [[0x7FFF] * 1440 for _ in range(50)]
        self.tag = [[0xFF] * 1440 for _ in range(50)]
        self.flag = [0] * 50
        self.base = [0] * 50
        self.trans = 0


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def run(streams, now, save=None, trace=False, night_owl=False, state=None):
    save = save or SamuSave()
    e = rpxelf.Elf(ELF)
    S = {}
    for y in e.symbols:
        if y.name and y.value:
            S.setdefault(y.name, y.value)
    img = ppc.Image()
    for s in e.sections:
        if s.addr and s.data and s.type != rpxelf.SHT_NOBITS and s.addr < 0xC0000000:
            img.add(s.addr, s.data)
    mem = ppc.Mem(img, 0, zero=[(0, 1 << 32)])
    # import names by their original stub address
    imports = {}
    secs = {s.name: s for s in e.sections if s.name.startswith('.fimport')}
    cs = secs['.fimport_coreinit']
    for y in e.symbols:
        if cs.addr <= y.value < cs.addr + cs.size and y.name and not y.name.startswith('.'):
            imports[0x2b58000 + y.value - cs.addr] = y.name
    heap = [0x40000000]

    def bump(n, al=64):
        a = (heap[0] + al - 1) & ~(al - 1)
        heap[0] = a + max(n, 4)
        return a

    stubs = {}
    calls = {}
    credits = save.__dict__.setdefault('credits', [])

    def mk(fn):
        def f(c):
            r = fn(c)
            if r is not None:
                c.r[3] = r & M32
            return c.lr
        return f

    def stub(name, fn):
        stubs[S[name]] = mk(fn)

    R = lambda c, i: c.r[i]  # noqa: E731
    # the save model
    stub('getSAMUDate__19RPHealthSaveManagerCFi', lambda c: save.date[R(c, 4)])
    stub('setSAMUDate__19RPHealthSaveManagerFUsi', lambda c: save.date.__setitem__(R(c, 5), R(c, 4) & 0xFFFF))
    stub('getSAMUSteps__19RPHealthSaveManagerCFi', lambda c: save.steps[R(c, 4)])
    stub('setSAMUSteps__19RPHealthSaveManagerFUii', lambda c: save.steps.__setitem__(R(c, 5), R(c, 4)))
    stub('getSAMUCalorie__19RPHealthSaveManagerCFi', lambda c: save.kcal[R(c, 4)])
    stub('setSAMUCalorie__19RPHealthSaveManagerFUii', lambda c: save.kcal.__setitem__(R(c, 5), R(c, 4)))
    stub('getSAMUMETs__19RPHealthSaveManagerCFiT1', lambda c: save.mets[R(c, 4)][R(c, 5)])
    stub('setSAMUMETs__19RPHealthSaveManagerFUciT2', lambda c: save.mets[R(c, 5)].__setitem__(R(c, 6), R(c, 4) & 0xFF))
    stub('getSAMUHeight__19RPHealthSaveManagerCFiT1', lambda c: save.height[R(c, 4)][R(c, 5)] & 0xFFFF)
    stub('setSAMUHeight__19RPHealthSaveManagerFsiT2', lambda c: save.height[R(c, 5)].__setitem__(R(c, 6), s16(R(c, 4))))
    stub('getSAMUActivityTag__19RPHealthSaveManagerCFiT1', lambda c: save.tag[R(c, 4)][R(c, 5)])
    stub('setSAMUActivityTag__19RPHealthSaveManagerFUciT2', lambda c: save.tag[R(c, 5)].__setitem__(R(c, 6), R(c, 4) & 0xFF))
    stub('getSAMUFlag__19RPHealthSaveManagerFi', lambda c: save.flag[R(c, 4)])
    stub('setSAMUFlag__19RPHealthSaveManagerFUci', lambda c: save.flag.__setitem__(R(c, 5), R(c, 4) & 0xFF))
    stub('setSAMUComment__19RPHealthSaveManagerFPCwi', lambda c: 0)
    stub('setSAMUTransDate__19RPHealthSaveManagerFUi', lambda c: setattr(save, 'trans', R(c, 4)))
    stub('getSAMUTransDate__19RPHealthSaveManagerCFv', lambda c: save.trans)
    for nm in ('addFMCUnsettledStep__19RPHealthSaveManagerFUi', 'addFMCUnsettledHeight__19RPHealthSaveManagerFUi',
               'addFMCUnsettledDistance__19RPHealthSaveManagerFUi'):
        stub(nm, (lambda n: lambda c: credits.append((n.split('__')[0], R(c, 4))))(nm))
    stub('Alloc__Q3_3jet6memory9AllocatorFUiT1', lambda c: bump(R(c, 4), R(c, 5) or 4))
    stub('Free__Q3_3jet6memory9AllocatorFPv', lambda c: 0)
    for nm in ('getSAMUBaseHeight__19RPHealthSaveManagerCFi',):
        if nm in S:
            stub(nm, lambda c: save.base[R(c, 4)])
    for nm in ('setSAMUBaseHeight__19RPHealthSaveManagerFsi',):
        if nm in S:
            stub(nm, lambda c: save.base.__setitem__(R(c, 5), s16(R(c, 4))))
    # code that is game code but outside the decoder: everything of these classes returns 0
    real_prefixes = ('getCalendar', 'isLeapYear', 'getDaysOfMonth', 'getDayOfWeek')
    stub_classes = ('19RPHealthSaveManager', '17RPHealthParameter', '17RPStepProfileData', '16RPStepSystemData',
                    '12RPHealthBank', '13RPSndAudioMgr', '19RPStepSaveDataMgr', '16RPHealthAreaMgr')
    by_addr = {}
    # the per-date step records (the Fit Meter Challenge credit compares against them): the game's own code
    REAL_SAVE = ('OtherMeasure', 'getStep__19RPHealthSaveManager', 'setStep__19RPHealthSaveManager', 'OtherDaySearch',
                 'DateSub', 'DateAdd')
    for nm, a in S.items():
        if any(r in nm for r in REAL_SAVE):
            continue
        if any(cl in nm for cl in stub_classes) and a not in stubs and 0x02000000 <= a < 0x10000000:
            by_addr[a] = nm

    def generic(nm):
        def f(c):
            calls[nm] = calls.get(nm, 0) + 1
            c.r[3] = 0
            return c.lr
        return f

    def imp(nm):
        def f(c):
            calls[nm] = calls.get(nm, 0) + 1
            if nm in ('memcpy', 'memmove', 'OSBlockMove'):
                d, s_, n = c.r[3], c.r[4], c.r[5]
                if n:
                    mem.write(d, mem.read(s_, n))
                return c.lr
            if nm in ('memset', 'OSBlockSet'):
                d, v, n = c.r[3], c.r[4], c.r[5]
                if n:
                    mem.write(d, bytes([v & 0xFF]) * n)
                return c.lr
            if nm == 'OSCalendarTimeToTicks':
                # OSCalendarTime: sec, min, hour, mday, mon (0-11), year, wday, yday, msec, usec (s32 each)
                a = c.r[3]
                f_ = [struct.unpack('>i', mem.read(a + 4 * k, 4))[0] for k in range(10)]
                y_, mo_ = f_[5] + f_[4] // 12, f_[4] % 12  # the OS normalises out-of-range fields
                dt = datetime.datetime(y_, mo_ + 1, 1) + datetime.timedelta(days=f_[3] - 1, hours=f_[2], minutes=f_[1], seconds=f_[0])
                t = int((dt - datetime.datetime(2000, 1, 1)).total_seconds()) * TIMER + (f_[8] * TIMER) // 1000
                c.r[3] = (t >> 32) & M32
                c.r[4] = t & M32
                return c.lr
            if nm == 'OSTicksToCalendarTime':
                t = (c.r[3] << 32) | c.r[4]
                if t & (1 << 63):
                    t -= 1 << 64
                secs, rem = divmod(t, TIMER)
                dt = datetime.datetime(2000, 1, 1) + datetime.timedelta(seconds=secs)
                vals = [dt.second, dt.minute, dt.hour, dt.day, dt.month - 1, dt.year, (dt.weekday() + 1) % 7,
                        dt.timetuple().tm_yday - 1, rem * 1000 // TIMER, 0]
                for k, v in enumerate(vals):
                    mem.write(c.r[5] + 4 * k, struct.pack('>i', v))
                return c.lr
            c.r[3] = 0
            return c.lr
        return f

    def policy(pc):
        if pc in stubs:
            return stubs[pc]
        if pc in imports:
            return imp(imports[pc])
        if pc in by_addr:
            return generic(by_addr[pc])
        if 0x2b58000 <= pc < 0x2b62000:
            return imp('import_%x' % pc)
        return None

    cpu = ppc.Cpu(mem, policy)

    def call(fn, *args, budget=2_000_000_000):
        cpu.r = [0] * 32
        cpu.r[1] = 0x0E0F0000
        cpu.r[2] = 0x10000000  # unused (GHS doesn't use r2/r13 small data here)
        for i, a in enumerate(args):
            cpu.r[3 + i] = a & M32
        cpu.lr = ppc.RET_SENTINEL
        cpu.run(S[fn] if isinstance(fn, str) else fn, budget)
        return cpu.r[3]

    # singletons: zeroed objects
    savemgr = bump(0x80000)
    param = bump(0x80000)
    if getattr(save, 'savemgr_mem', None) is not None:
        mem.write(savemgr, save.savemgr_mem)
    mem.w32(SAVEMGR_PTR, savemgr)
    mem.w32(PARAM_PTR, param)
    if night_owl:
        mem.w8(param + 0x5CC5B, 1)  # RPHealthParameter mDayStartMode (SaveU16Data_: day starts at 3:00)
    y, mo, d, h, mi = now
    t32 = call('getCalendarTime32__13RPSysCalendarSFUiN41', y, mo, d, h, mi)
    proc = bump(0x2000)
    mem.w32(proc + 212, t32)
    mem.w32(0x11C3FA30, t32)
    # the receive containers
    n = 0
    for bank in (2, 3, 4, 5, 6):
        data = streams.get(bank)
        if data is None:
            continue
        a = bump(len(data))
        mem.write(a, data)
        b = BINS + 20 * n
        mem.w32(b + 0, a)
        mem.w32(b + 4, len(data))
        mem.w32(b + 8, bank)
        mem.w16(b + 12, 0)
        mem.w32(b + 16, 0)
        n += 1
    mem.w32(BIN_COUNT, n)
    call('SAMUDataSave__11SAMUProcessFv', proc)
    save.savemgr_mem = mem.read(savemgr, 0x80000)
    return save, t32, calls, cpu.count, (mem, call, proc)


def date16_to_ymd(call, d16):
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--now', required=True, help='meter time YYYY-MM-DDTHH:MM')
    for b in (2, 3, 4, 5, 6):
        ap.add_argument('--b%d' % b)
    ap.add_argument('--json')
    a = ap.parse_args()
    dt, tm = a.now.split('T')
    y, mo, d = map(int, dt.split('-'))
    h, mi = map(int, tm.split(':'))
    streams = {}
    for b in (2, 3, 4, 5, 6):
        f = getattr(a, 'b%d' % b)
        if f:
            streams[b] = open(f, 'rb').read()
    save, t32, calls, count, ctx = run(streams, (y, mo, d, h, mi))
    mem, call, proc = ctx
    days = []
    for i in range(50):
        if save.date[i] == 0:
            continue
        p = 0x0E100000
        call('getCalendarTime__13RPSysCalendarSFPCUsRUiN22', 0, 0, 0, 0) if False else None
        mem.w16(p, save.date[i])
        call('getCalendarTime__13RPSysCalendarSFPCUsRUiN22', p, p + 4, p + 8, p + 12)
        yy, mm, dd = mem.r32(p + 4), mem.r32(p + 8), mem.r32(p + 12)
        mets = save.mets[i]
        hts = save.height[i]
        tags = save.tag[i]
        days.append({'slot': i, 'date16': save.date[i], 'date': '%04d-%02d-%02d' % (yy, mm, dd),
                     'steps': save.steps[i], 'kcal': save.kcal[i], 'flag': save.flag[i],
                     'mets': mets, 'height': hts, 'tag': tags})
    out = {'now32': t32, 'trans': save.trans, 'instructions': count, 'stubbed_calls': calls, 'days': days}
    if a.json:
        json.dump(out, open(a.json, 'w'))
    for dd in days:
        nm = sum(1 for v in dd['mets'] if v != 0xFF)
        nh = sum(1 for v in dd['height'] if v != 0x7FFF)
        nt = sum(1 for v in dd['tag'] if v != 0xFF)
        print('%s slot %2d steps %6d kcal %5d flag %d  METs %4d min  height %4d min  tag %4d min' % (
            dd['date'], dd['slot'], dd['steps'], dd['kcal'], dd['flag'], nm, nh, nt))
    print('instructions', count, 'stubbed', calls)


if __name__ == '__main__':
    main()
