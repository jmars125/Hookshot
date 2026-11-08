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
                                 UC_ARM64_REG_NZCV, UC_ARM64_REG_TPIDR_EL0, UC_ARM64_REG_FPCR, UC_ARM64_REG_FPSR,
                                 UC_ARM64_REG_Q0, UC_CPU_ARM64_MAX)

PAGE, PAGE_SIZE = 0x10000, 0x1000
PC = PAGE + 0x800
# word, nzcv, pc, x0..x30 + sp, fpcr, fpsr, then 32 V registers of 16 bytes
CASE = struct.Struct("<IIQ32QII512s")
FP = 1 << 40   # GROUPS flag: the case gets FP-shaped V registers and FPCR / FPSR

# Floating-point bit patterns worth hitting: zeros, ones, infinities, quiet and signalling NaNs, subnormals, the
# extremes of the normal range, integers near the conversion limits
S_SPECIAL = [0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFA00000,
             0x00000001, 0x807FFFFF, 0x00800000, 0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x5F000000, 0x3F000000,
             0x4B000001, 0x40490FDB]
D_SPECIAL = [0, 1 << 63, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
             0x7FF8000000000000, 0x7FF0000000000001, 0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0010000000000000,
             0x7FEFFFFFFFFFFFFF, 0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000, 0x3FE0000000000000,
             0x41DFFFFFFFC00000, 0x400921FB54442D18, 0x3FF0000000000001]


def rand_s(rng):
    r = rng.random()
    if r < 0.35:
        return rng.choice(S_SPECIAL)
    if r < 0.8:   # an ordinary value of moderate size
        return (rng.getrandbits(1) << 31) | (rng.randrange(0x70, 0x90) << 23) | rng.getrandbits(23)
    return rng.getrandbits(32)


def rand_d(rng):
    r = rng.random()
    if r < 0.35:
        return rng.choice(D_SPECIAL)
    if r < 0.8:
        return (rng.getrandbits(1) << 63) | (rng.randrange(0x3C0, 0x440) << 52) | rng.getrandbits(52)
    return rng.getrandbits(64)


def fp_state(rng):
    """(fpcr, fpsr, V registers as bytes): FPCR mostly default, with FZ, a rounding mode or DN some of the time;
    FPSR.IXC usually already set, as it is in a running game"""
    fpcr = 0
    if rng.random() < 0.25:
        fpcr |= 1 << 24
    if rng.random() < 0.15:
        fpcr |= rng.randrange(1, 4) << 22
    if rng.random() < 0.05:
        fpcr |= 1 << 25
    fpsr = rng.choice((0x10, 0x10, 0x10, 0, 0x9F))
    v = b""
    for _ in range(32):
        lo = rand_d(rng) if rng.random() < 0.5 else rand_s(rng) | (rand_s(rng) << 32)
        v += struct.pack("<QQ", lo, rng.getrandbits(64))
    return fpcr, fpsr, v

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
    ("exclusive", 0x3F000000, 0x08000000),
    ("atomic", 0x3F200C00, 0x38200000),
    ("ldapr", 0x3FFFFC00, 0x38BFC000),
    ("crc32", 0x7FE0F000, 0x1AC04000),
    ("v_uimm", 0x3F000000, 0x3D000000),
    ("v_imm9", 0x3F200000, 0x3C000000),
    ("v_reg", 0x3F200C00, 0x3C200800),
    ("v_pair", 0x3E000000, 0x2C000000),
    ("v_lit", 0x3F000000, 0x1C000000),
    ("fp_dp1", FP | 0xFFA07C00, 0x1E204000),
    ("fp_dp2", FP | 0xFFA00C00, 0x1E200800),
    ("fp_cmp", FP | 0xFFA0FC07, 0x1E202000),
    ("fp_csel", FP | 0xFFA00C00, 0x1E200C00),
    ("fp_imm", FP | 0xFFA01FE0, 0x1E201000),
    ("fp_fma", FP | 0xFF800000, 0x1F000000),
    ("fp_fcvt", FP | 0xFFBF7C00, 0x1E224000),
    ("fp_cvt", FP | 0x7F20FC00, 0x1E200000),
    # Advanced SIMD: vector forms, SIMD-scalar forms, structure loads and stores
    ("n_copy", FP | 0x9FE08400, 0x0E000400),
    ("n_3same", FP | 0x9F200400, 0x0E200400),
    ("n_3diff", FP | 0x9F200C00, 0x0E200000),
    ("n_2misc", FP | 0x9F3E0C00, 0x0E200800),
    ("n_across", FP | 0x9F3E0C00, 0x0E300800),
    ("n_perm", FP | 0xBF208C00, 0x0E000800),
    ("n_ext", FP | 0xBFE08400, 0x2E000000),
    ("n_modimm", FP | 0x9FF80400, 0x0F000400),
    ("n_shift", FP | 0x9F800400, 0x0F000400),
    ("n_elem", FP | 0x9F000400, 0x0F000000),
    ("s_3same", FP | 0xDF200400, 0x5E200400),
    ("s_2misc", FP | 0xDF3E0C00, 0x5E200800),
    ("s_pair", FP | 0xDF3E0C00, 0x5E300800),
    ("s_shift", FP | 0xDF800400, 0x5F000400),
    ("s_elem", FP | 0xDF000400, 0x5F000000),
    ("s_dup", FP | 0xFFE0FC00, 0x5E000400),
    ("v_mult", 0xBFBF0000, 0x0C000000),
    ("v_mult_post", 0xBFA00000, 0x0C800000),
    ("v_single", 0xBF9F0000, 0x0D000000),
    ("v_single_post", 0xBF800000, 0x0D800000),
]
SYSTEM = [0xD503201F, 0xD503203F, 0xD503233F, 0xD50323BF, 0xD5033BBF, 0xD5033B9F, 0xD50339BF]
SYS_RW = [0xD53B4200, 0xD51B4200, 0xD53BD040, 0xD51BD040]   # MRS/MSR NZCV, MRS/MSR TPIDR_EL0 (Rt added)


def unpredictable(word):
    """Encodings whose result the architecture leaves open (CONSTRAINED UNPREDICTABLE): not compared"""
    rt, rn, rt2 = word & 31, (word >> 5) & 31, (word >> 10) & 31
    if word & 0x3A000000 == 0x28000000:   # load/store pair
        load, wb = (word >> 22) & 1, (word >> 23) & 3 in (1, 3)
        simd = (word >> 26) & 1   # SIMD&FP registers are not the base's register file
        return (load and rt == rt2) or (wb and not simd and rn != 31 and rn in (rt, rt2))
    if word & 0x3F200000 == 0x38000000 and (word >> 10) & 3 in (1, 3):   # pre/post-indexed, general registers
        return rn != 31 and rn == rt
    return False


def lse_signed_model(word, nzcv, x, mem, fpcr, fpsr, v):
    """LDSMAX / LDSMIN by the Arm ARM. Unicorn 2.1 gets them wrong (32-bit LDSMIN came out as an unsigned minimum),
    so these words are checked against this model instead"""
    size, rs, rn, rt, opc = word >> 30, (word >> 16) & 31, (word >> 5) & 31, word & 31, (word >> 12) & 7
    n = 1 << size
    a = (x[rn] if rn != 31 else x[31]) - PAGE
    if not 0 <= a <= PAGE_SIZE - n:
        raise UcError(0)
    bits = 8 * n
    old = int.from_bytes(mem[a:a + n], "little")
    s = (x[rs] if rs != 31 else 0) & ((1 << bits) - 1)
    sg = lambda v: v - (1 << bits) if v >> (bits - 1) else v
    new = (max if opc == 4 else min)(sg(old), sg(s)) & ((1 << bits) - 1)
    out = list(x)
    m = bytearray(mem)
    m[a:a + n] = new.to_bytes(n, "little")
    if rt != 31:
        out[rt] = old
    return nzcv, out, bytes(m), fpcr, fpsr, v


def is_lse_signed(word):
    return word & 0x3F20FC00 in (0x38204000, 0x38205000) and not word & 0x04000000


def regs_for(word, rng, fp=False):
    """Random registers, with the base register of a load/store (bits 9:5) pointing into the page (not for FP data
    processing, whose bits 9:5 name a V register or an integer source)"""
    x = [rng.choice((rng.getrandbits(64), rng.getrandbits(32), rng.randrange(-64, 64) & (2**64 - 1), 0, 1,
                     0x8000000000000000, 0x7FFFFFFF, 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFF, 1 << 53 | 1, 1 << 24 | 1))
         for _ in range(32)]
    if fp:
        x[31] = (PAGE + 0x400) & ~15
        return x
    base = PAGE + 0x100 + rng.randrange(0, 0xD00) // 16 * 16
    rn = (word >> 5) & 31
    x[rn] = base
    x[31] = base if rn == 31 else (PAGE + 0x400) & ~15   # SP in the page as well
    m = (word >> 16) & 31   # a register offset stays small
    if m != rn and rng.random() < 0.7:
        x[m] = rng.randrange(-0x40, 0x40) & (2**64 - 1)
    return x


def unicorn_run(word, nzcv, x, mem, fpcr, fpsr, v):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=UC_CPU_ARM64_MAX)
    uc.mem_map(PAGE, PAGE_SIZE, UC_PROT_ALL)
    uc.mem_write(PAGE, bytes(mem))
    uc.reg_write(UC_ARM64_REG_FPCR, fpcr)
    uc.reg_write(UC_ARM64_REG_FPSR, fpsr)
    for i in range(32):
        lo, hi = struct.unpack_from("<QQ", v, 16 * i)
        uc.reg_write(UC_ARM64_REG_Q0 + i, lo | hi << 64)
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
    vout = b"".join(struct.pack("<QQ", q & (2**64 - 1), q >> 64)
                    for q in (uc.reg_read(UC_ARM64_REG_Q0 + i) for i in range(32)))
    return (uc.reg_read(UC_ARM64_REG_NZCV) & 0xF0000000, out, bytes(uc.mem_read(PAGE, PAGE_SIZE)),
            uc.reg_read(UC_ARM64_REG_FPCR), uc.reg_read(UC_ARM64_REG_FPSR), vout)


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
    fp_groups = {name for name, mask, _ in groups if mask & FP}
    words = [(name, (rng.getrandbits(32) & ~mask & 0xFFFFFFFF) | value)
             for name, mask, value in groups for _ in range(a.cases)]
    if not a.group or a.group == "system":
        words += [("system", w) for w in SYSTEM] + [("system", w | rng.randrange(31)) for w in SYS_RW for _ in range(8)]
    for name, word in words:
        if unpredictable(word):
            continue
        nzcv = rng.getrandbits(4) << 28
        x = regs_for(word, rng, name in fp_groups)
        fpcr, fpsr, v = fp_state(rng)
        mem = bytearray(rng.getrandbits(8) for _ in range(PAGE_SIZE))
        mem[PC - PAGE:PC - PAGE + 4] = struct.pack("<I", word)
        try:
            exp = (lse_signed_model if is_lse_signed(word) else unicorn_run)(word, nzcv, x, mem, fpcr, fpsr, v)
        except UcError:
            refused[name] += 1
            continue
        cases.append((name, word, nzcv, x, bytes(mem), fpcr, fpsr, v, exp))
    with tempfile.TemporaryDirectory() as d:
        inp, outp = os.path.join(d, "in.bin"), os.path.join(d, "out.bin")
        with open(inp, "wb") as f:
            for _, word, nzcv, x, mem, fpcr, fpsr, v, _ in cases:
                f.write(CASE.pack(word, nzcv, PC, *x, fpcr, fpsr, v) + mem)
        r = subprocess.run([a.hsr, "difftest", inp, outp], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 1
        print(r.stdout.strip())
        got = open(outp, "rb").read()
    step = 4 + CASE.size + PAGE_SIZE
    stats = collections.defaultdict(collections.Counter)
    shown = collections.Counter()
    for i, (name, word, nzcv, x, mem, fpcr, fpsr, v, (enzcv, ex, emem, efpcr, efpsr, ev)) in enumerate(cases):
        rec = got[i * step:(i + 1) * step]
        status = struct.unpack_from("<I", rec)[0]
        if status:
            stats[name]["untranslated"] += 1
            continue
        _, gnzcv, _, *gx, gfpcr, gfpsr, gv = CASE.unpack_from(rec, 4)
        gmem = rec[4 + CASE.size:]
        bad = []
        if gnzcv != enzcv:
            bad.append("nzcv %x != %x" % (gnzcv >> 28, enzcv >> 28))
        bad += ["%s %#x != %#x" % ("sp" if j == 31 else "x%d" % j, gx[j], ex[j]) for j in range(32) if gx[j] != ex[j]]
        if gfpcr != efpcr:
            bad.append("fpcr %#x != %#x" % (gfpcr, efpcr))
        if gfpsr != efpsr:
            bad.append("fpsr %#x != %#x (in %#x, fpcr %#x)" % (gfpsr, efpsr, fpsr, fpcr))
        for j in range(32):
            if gv[16 * j:16 * j + 16] != ev[16 * j:16 * j + 16]:
                bad.append("v%d %s != %s (in n %s m %s)" % (j, gv[16 * j:16 * j + 16][::-1].hex(),
                           ev[16 * j:16 * j + 16][::-1].hex(), v[16 * ((word >> 5) & 31):][:8][::-1].hex(),
                           v[16 * ((word >> 16) & 31):][:8][::-1].hex()))
        if gmem != emem:
            k = next(j for j in range(PAGE_SIZE) if gmem[j] != emem[j])
            bad.append("mem +%#x got %s want %s" % (k, gmem[k:k + 8].hex(), emem[k:k + 8].hex()))
        if bad:
            stats[name]["wrong"] += 1
            if shown[name] < int(os.environ.get("SHOW", "3")):
                shown[name] += 1
                print("WRONG %-11s %08x: %s" % (name, word, "; ".join(bad[:5])))
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
