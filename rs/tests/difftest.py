"""Differential test of the Rust lifter's instruction translation (rs/hsr/src/lift/a64.rs) against Unicorn.

    python rs/tests/difftest.py [--cases N] [--seed S] [--hsr rs/target/release/hsr.exe] [--group NAME]

Random instruction words from each encoding group run once in Unicorn and once through `hsr difftest` (each word lifted
alone into a Cranelift function and run on a context). Registers, flags and the 4 KB guest page must match. Words
Unicorn refuses or faults on are dropped; words hsr does not translate are counted per group. Needs Python with
`unicorn` (pip install "unicorn>=2.1,<2.2").
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys
import tempfile

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X29, UC_ARM64_REG_X30, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_NZCV, UC_ARM64_REG_TPIDR_EL0)

PAGE, PAGE_SIZE = 0x10000, 0x1000
PC = PAGE + 0x800
CASE = struct.Struct("<IIQ32Q")

# (name, fixed bits mask, fixed bits value): the other bits are random
GROUPS = [
    ("pcrel", 0x1F000000, 0x10000000),
    ("addsub_imm", 0x1F800000, 0x11000000),
    ("logic_imm", 0x1F800000, 0x12000000),
    ("movewide", 0x1F800000, 0x12800000),
    ("bitfield", 0x1F800000, 0x13000000),
    ("extract", 0x1F800000, 0x13800000),
    ("logic_reg", 0x1F000000, 0x0A000000),
    ("addsub_reg", 0x1F200000, 0x0B000000),
    ("addsub_ext", 0x1F200000, 0x0B200000),
    ("adc", 0x1FE0FC00, 0x1A000000),
    ("ccmp", 0x1FE00410, 0x1A400000),
    ("csel", 0x1FE00000, 0x1A800000),
    ("dp2", 0x5FE0C000, 0x1AC00000),
    ("dp1", 0x5FFFF000, 0x5AC00000),
    ("dp3", 0x1F000000, 0x1B000000),
    ("ldst_uimm", 0x3F000000, 0x39000000),
    ("ldst_imm9", 0x3F200000, 0x38000000),
    ("ldst_reg", 0x3F200C00, 0x38200800),
    ("ldst_pair", 0x3E000000, 0x28000000),
    ("ldr_lit", 0x3F000000, 0x18000000),
]
SYSTEM = [0xD503201F, 0xD503203F, 0xD503233F, 0xD50323BF, 0xD5033BBF, 0xD5033B9F, 0xD50339BF]
SYS_RW = [0xD53B4200, 0xD51B4200, 0xD53BD040, 0xD51BD040]   # MRS/MSR NZCV, MRS/MSR TPIDR_EL0 (Rt added)


def unpredictable(word):
    """Encodings whose result the architecture leaves open (CONSTRAINED UNPREDICTABLE): not compared"""
    rt, rn, rt2 = word & 31, (word >> 5) & 31, (word >> 10) & 31
    if word & 0x3A000000 == 0x28000000:   # load/store pair
        load, wb = (word >> 22) & 1, (word >> 23) & 3 in (1, 3)
        return (load and rt == rt2) or (wb and rn != 31 and rn in (rt, rt2))
    if word & 0x3B200000 == 0x38000000 and (word >> 10) & 3 in (1, 3):   # pre/post-indexed
        return rn != 31 and rn == rt
    return False


def regs_for(word, rng):
    """Random registers, with the base register of a load/store (bits 9:5) pointing into the page"""
    x = [rng.choice((rng.getrandbits(64), rng.getrandbits(32), rng.randrange(-64, 64) & (2**64 - 1), 0, 1,
                     0x8000000000000000, 0x7FFFFFFF)) for _ in range(32)]
    base = PAGE + 0x100 + rng.randrange(0, 0xD00) // 16 * 16
    rn = (word >> 5) & 31
    x[rn] = base
    x[31] = base if rn == 31 else (PAGE + 0x400) & ~15   # SP in the page as well
    m = (word >> 16) & 31   # a register offset stays small
    if m != rn and rng.random() < 0.7:
        x[m] = rng.randrange(-0x40, 0x40) & (2**64 - 1)
    return x


def unicorn_run(word, nzcv, x, mem):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(PAGE, PAGE_SIZE, UC_PROT_ALL)
    uc.mem_write(PAGE, bytes(mem))
    for i in range(29):
        uc.reg_write(UC_ARM64_REG_X0 + i, x[i])
    uc.reg_write(UC_ARM64_REG_X29, x[29])
    uc.reg_write(UC_ARM64_REG_X30, x[30])
    uc.reg_write(UC_ARM64_REG_SP, x[31])
    uc.reg_write(UC_ARM64_REG_NZCV, nzcv)
    uc.reg_write(UC_ARM64_REG_TPIDR_EL0, 0)
    uc.emu_start(PC, PC + 4, count=1)
    out = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(29)]
    out += [uc.reg_read(UC_ARM64_REG_X29), uc.reg_read(UC_ARM64_REG_X30), uc.reg_read(UC_ARM64_REG_SP)]
    return uc.reg_read(UC_ARM64_REG_NZCV) & 0xF0000000, out, bytes(uc.mem_read(PAGE, PAGE_SIZE))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=400, help="per group")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--group")
    ap.add_argument("--hsr", default=os.path.join(os.path.dirname(__file__), "..", "target", "release", "hsr.exe"))
    a = ap.parse_args()
    rng = random.Random(a.seed)
    cases = []   # (group, word, nzcv, x, mem, expected)
    refused = collections.Counter()
    groups = [g for g in GROUPS if not a.group or g[0] == a.group]
    words = [(name, (rng.getrandbits(32) & ~mask) | value) for name, mask, value in groups for _ in range(a.cases)]
    if not a.group or a.group == "system":
        words += [("system", w) for w in SYSTEM] + [("system", w | rng.randrange(31)) for w in SYS_RW for _ in range(8)]
    for name, word in words:
        if unpredictable(word):
            continue
        nzcv = rng.getrandbits(4) << 28
        x = regs_for(word, rng)
        mem = bytearray(rng.getrandbits(8) for _ in range(PAGE_SIZE))
        mem[PC - PAGE:PC - PAGE + 4] = struct.pack("<I", word)
        try:
            exp = unicorn_run(word, nzcv, x, mem)
        except UcError:
            refused[name] += 1
            continue
        cases.append((name, word, nzcv, x, bytes(mem), exp))
    with tempfile.TemporaryDirectory() as d:
        inp, outp = os.path.join(d, "in.bin"), os.path.join(d, "out.bin")
        with open(inp, "wb") as f:
            for _, word, nzcv, x, mem, _ in cases:
                f.write(CASE.pack(word, nzcv, PC, *x) + mem)
        r = subprocess.run([a.hsr, "difftest", inp, outp], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 1
        print(r.stdout.strip())
        got = open(outp, "rb").read()
    step = 4 + CASE.size + PAGE_SIZE
    stats = collections.defaultdict(collections.Counter)
    shown = collections.Counter()
    for i, (name, word, nzcv, x, mem, (enzcv, ex, emem)) in enumerate(cases):
        rec = got[i * step:(i + 1) * step]
        status = struct.unpack_from("<I", rec)[0]
        if status:
            stats[name]["untranslated"] += 1
            continue
        _, gnzcv, _, *gx = CASE.unpack_from(rec, 4)
        gmem = rec[4 + CASE.size:]
        bad = []
        if gnzcv != enzcv:
            bad.append("nzcv %x != %x" % (gnzcv >> 28, enzcv >> 28))
        bad += ["%s %#x != %#x" % ("sp" if j == 31 else "x%d" % j, gx[j], ex[j]) for j in range(32) if gx[j] != ex[j]]
        if gmem != emem:
            k = next(j for j in range(PAGE_SIZE) if gmem[j] != emem[j])
            bad.append("mem +%#x" % k)
        if bad:
            stats[name]["wrong"] += 1
            if shown[name] < 3:
                shown[name] += 1
                print("WRONG %-11s %08x: %s" % (name, word, "; ".join(bad[:4])))
        else:
            stats[name]["ok"] += 1
    print("%-12s %6s %6s %6s %6s" % ("group", "ok", "wrong", "untr.", "refused"))
    total = collections.Counter()
    for name in [g[0] for g in groups] + (["system"] if not a.group or a.group == "system" else []):
        s = stats[name]
        total.update(s)
        print("%-12s %6d %6d %6d %6d" % (name, s["ok"], s["wrong"], s["untranslated"], refused[name]))
    print("%-12s %6d %6d %6d %6d" % ("all", total["ok"], total["wrong"], total["untranslated"], sum(refused.values())))
    return 1 if total["wrong"] else 0


if __name__ == "__main__":
    sys.exit(main())
