"""Differential test of the Rust function builder (rs/hsr/src/lift/func.rs) against Unicorn: whole programs.

    python rs/tests/difftest_fn.py [--cases N] [--seed S] [--hsr rs/target/release/hsr.exe]

Each case is a random program of a few functions at CODE: integer instructions from difftest.py's groups, stack
loads and stores, forward conditional branches (B.cond, CBZ/CBNZ, TBZ/TBNZ), skipped dead code, counted loops (their
headers poll), BL and BLR calls, SVCs, jump tables (BR through a table the case lists) and function ends that return,
tail-call with B or tail-call indirectly with BR. Unicorn runs it from the first function until it returns to SENTINEL;
`hsr difftest-fn` lifts every function and calls the first. Registers, flags and the page must match.

The test runtime's SVC computes x0 = 3 * x0 + x1 + imm (fntest.rs); the Unicorn interrupt hook does the same.
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import difftest  # noqa: E402
from difftest import PAGE, PAGE_SIZE, CASE, unpredictable  # noqa: E402
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL, UC_HOOK_INTR  # noqa: E402
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X29, UC_ARM64_REG_X30,  # noqa: E402
                                 UC_ARM64_REG_SP, UC_ARM64_REG_NZCV, UC_ARM64_REG_PC, UC_CPU_ARM64_MAX)

CODE, CODE_SIZE = 0x20000, 0x2000
SENTINEL = 0x7FFF0
RESERVED = {16, 17, 20, 29, 30, 31}   # ADR/BR scratch, loop counter, frame, link, SP
ALU = ["addsub_imm", "logic_imm", "movewide", "bitfield", "extract", "logic_reg", "addsub_reg", "addsub_ext", "adc",
       "ccmp", "csel", "dp2", "dp1", "dp3"]
GROUP = {name: (mask, value) for name, mask, value in difftest.GROUPS}
M64 = 2**64 - 1

_probe = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=UC_CPU_ARM64_MAX)
_probe.mem_map(0, 0x1000, UC_PROT_ALL)
_legal = {}


def legal(w):
    if w not in _legal:
        _probe.mem_write(0, struct.pack("<I", w))
        try:
            _probe.emu_start(0, 4, count=1)
            _legal[w] = True
        except UcError:
            _legal[w] = False
    return _legal[w]


def alu_word(rng):
    while True:
        mask, value = GROUP[rng.choice(ALU)]
        w = (rng.getrandbits(32) & ~mask & 0xFFFFFFFF) | value
        if w & 31 in RESERVED and (w & 0x1FE00410) != 0x1A400000:   # (CCMP's low bits are NZCV, not a register)
            continue
        if not unpredictable(w) and legal(w):
            return w


def mem_word(rng):
    k = rng.randrange(0, 48)
    if rng.random() < 0.5:
        return 0xF90003E0 | k << 10 | rng.randrange(31)                    # STR Xt, [SP, #8k]
    return 0xF94003E0 | k << 10 | rng.choice([r for r in range(31) if r not in RESERVED])   # LDR Xt, [SP, #8k]


class Asm:
    """Instructions with label references, laid out after every function is generated"""

    def __init__(self):
        self.items = []   # (kind, arg): ("w", word) | ("label", name) | (branch kind, label, extra)
        self.n = 0

    def w(self, word):
        self.items.append(("w", word))

    def label(self, name):
        self.items.append(("label", name))

    def ref(self, kind, name, extra=0):
        self.items.append((kind, name, extra))

    def fresh(self):
        self.n += 1
        return "L%d" % self.n


def ops(a, rng, n):
    for _ in range(n):
        a.w(mem_word(rng) if rng.random() < 0.25 else alu_word(rng))


def gen_function(a, rng, i, nfun, jts):
    fn = "F%d" % i
    a.label(fn)
    callees = list(range(i + 1, nfun))
    nonleaf = bool(callees) and rng.random() < 0.8
    if nonleaf:
        a.w(0xA9BF7BFD)   # STP x29, x30, [SP, #-16]!
    for _ in range(rng.randrange(2, 7)):
        r = rng.random()
        if r < 0.25:
            ops(a, rng, rng.randrange(1, 5))
        elif r < 0.45:   # forward conditional branch over some instructions
            L = a.fresh()
            kind = rng.choice(["bcond", "cbz", "tbz"])
            a.ref(kind, L, rng.getrandbits(32))
            ops(a, rng, rng.randrange(1, 4))
            a.label(L)
        elif r < 0.52:   # dead code jumped over
            L = a.fresh()
            a.ref("b", L)
            ops(a, rng, rng.randrange(1, 3))
            a.label(L)
        elif r < 0.62:   # counted loop: its header polls
            L = a.fresh()
            a.w(0xD2800000 | rng.randrange(1, 5) << 5 | 20)   # MOVZ x20, #n
            a.label(L)
            ops(a, rng, rng.randrange(1, 4))
            if rng.random() < 0.3:
                a.w(0xD4000001 | rng.randrange(0x100) << 5)   # an SVC inside a loop
            a.w(0xD1000694)                                     # SUB x20, x20, #1
            a.ref("cbz", L, 1 << 31 | 1 << 24 | 20)            # CBNZ x20, L
        elif r < 0.72 and nonleaf:
            a.ref("bl", "F%d" % rng.choice(callees))
        elif r < 0.80 and nonleaf:
            a.ref("adr", "F%d" % rng.choice(callees))
            a.w(0xD63F0200)   # BLR x16
        elif r < 0.86:
            a.w(0xD4000001 | rng.randrange(0x10000) << 5)   # SVC #imm
        elif r < 0.95:   # jump table: BR x16 to one of k labels, each falling to a join
            k = rng.randrange(2, 5)
            labels = [a.fresh() for _ in range(k)]
            join = a.fresh()
            a.ref("adr", rng.choice(labels))
            br = a.fresh()
            a.label(br)
            a.w(0xD61F0200)   # BR x16
            jts.append((br, labels))
            for L in labels:
                a.label(L)
                ops(a, rng, rng.randrange(1, 3))
                a.ref("b", join)
            a.label(join)
    if nonleaf:
        a.w(0xA8C17BFD)   # LDP x29, x30, [SP], #16
    r = rng.random()
    if callees and r < 0.2:
        a.ref("b", "F%d" % rng.choice(callees))                 # tail call
    elif callees and r < 0.35:
        a.ref("adr", "F%d" % rng.choice(callees))
        a.w(0xD61F0200)                                          # indirect tail call (BR x16, no table)
    else:
        a.w(0xD65F03C0)                                          # RET


def assemble(a):
    addr, pc = {}, CODE
    for it in a.items:
        if it[0] == "label":
            addr[it[1]] = pc
        else:
            pc += 4
    words, pc = [], CODE
    for it in a.items:
        if it[0] == "label":
            continue
        kind = it[0]
        if kind == "w":
            w = it[1]
        else:
            off = (addr[it[1]] - pc) // 4
            x = it[2]
            if kind == "b":
                w = 0x14000000 | off & 0x3FFFFFF
            elif kind == "bl":
                w = 0x94000000 | off & 0x3FFFFFF
            elif kind == "bcond":
                w = 0x54000000 | (off & 0x7FFFF) << 5 | x & 15
            elif kind == "cbz":   # x: sf, nz, rt (random unless given)
                w = 0x34000000 | x & (1 << 31 | 1 << 24) | (off & 0x7FFFF) << 5 | x & 31
            elif kind == "tbz":
                w = 0x36000000 | x & (1 << 31 | 1 << 24 | 31 << 19) | (off & 0x3FFF) << 5 | x & 31
            elif kind == "adr":
                d = addr[it[1]] - pc
                w = 0x10000000 | (d & 3) << 29 | (d >> 2 & 0x7FFFF) << 5 | 16
            else:
                raise ValueError(kind)
        words.append(w)
        pc += 4
    return words, addr


def gen_program(rng):
    a, jts = Asm(), []
    nfun = rng.randrange(1, 5)
    for i in range(nfun):
        gen_function(a, rng, i, nfun, jts)
    words, addr = assemble(a)
    entries = [addr["F%d" % i] for i in range(nfun)]
    jt = [(addr[br], [addr[L] for L in labels]) for br, labels in jts]
    return words, entries, jt


def unicorn_run(words, nzcv, x, mem):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM, cpu=UC_CPU_ARM64_MAX)
    uc.mem_map(PAGE, PAGE_SIZE, UC_PROT_ALL)
    uc.mem_map(CODE, CODE_SIZE, UC_PROT_ALL)
    uc.mem_write(PAGE, bytes(mem))
    uc.mem_write(CODE, struct.pack("<%dI" % len(words), *words))

    def intr(uc, intno, _):
        pc = uc.reg_read(UC_ARM64_REG_PC)
        w = struct.unpack("<I", uc.mem_read(pc - 4, 4))[0]
        if intno != 2 or w & 0xFFE0001F != 0xD4000001:
            raise RuntimeError("interrupt %d at %#x" % (intno, pc))
        x0, x1 = uc.reg_read(UC_ARM64_REG_X0), uc.reg_read(UC_ARM64_REG_X1)
        uc.reg_write(UC_ARM64_REG_X0, (3 * x0 + x1 + (w >> 5 & 0xFFFF)) & M64)

    uc.hook_add(UC_HOOK_INTR, intr)
    for i in range(29):
        uc.reg_write(UC_ARM64_REG_X0 + i, x[i])
    uc.reg_write(UC_ARM64_REG_X29, x[29])
    uc.reg_write(UC_ARM64_REG_X30, SENTINEL)
    uc.reg_write(UC_ARM64_REG_SP, x[31])
    uc.reg_write(UC_ARM64_REG_NZCV, nzcv)
    uc.emu_start(CODE, SENTINEL, count=100000)
    if uc.reg_read(UC_ARM64_REG_PC) != SENTINEL:
        raise RuntimeError("did not return")
    out = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(29)]
    out += [uc.reg_read(UC_ARM64_REG_X29), uc.reg_read(UC_ARM64_REG_X30), uc.reg_read(UC_ARM64_REG_SP)]
    return uc.reg_read(UC_ARM64_REG_NZCV) & 0xF0000000, out, bytes(uc.mem_read(PAGE, PAGE_SIZE))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--hsr", default=os.path.join(os.path.dirname(__file__), "..", "target", "release", "hsr.exe"))
    a = ap.parse_args()
    rng = random.Random(a.seed)
    cases, refused = [], 0
    while len(cases) < a.cases:
        words, entries, jt = gen_program(rng)
        nzcv = rng.getrandbits(4) << 28
        x = difftest.regs_for(0, rng)
        x[31] = PAGE + 0x800
        x[30] = SENTINEL
        mem = bytes(rng.getrandbits(8) for _ in range(PAGE_SIZE))
        try:
            exp = unicorn_run(words, nzcv, x, mem)
        except (UcError, RuntimeError):
            refused += 1
            continue
        cases.append((words, entries, jt, nzcv, x, mem, exp))
    with tempfile.TemporaryDirectory() as d:
        inp, outp = os.path.join(d, "in.bin"), os.path.join(d, "out.bin")
        with open(inp, "wb") as f:
            for words, entries, jt, nzcv, x, mem, _ in cases:
                body = CASE.pack(0, nzcv, entries[0], *x, 0, 0, bytes(512)) + mem
                body += struct.pack("<I%dQ" % len(entries), len(entries), *entries)
                body += struct.pack("<I%dI" % len(words), len(words), *words)
                body += struct.pack("<I", len(jt))
                for br, ts in jt:
                    body += struct.pack("<QI%dQ" % len(ts), br, len(ts), *ts)
                f.write(struct.pack("<I", len(body)) + body)
        r = subprocess.run([a.hsr, "difftest-fn", inp, outp], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            return 1
        print(r.stdout.strip())
        got = open(outp, "rb").read()
    step = 4 + CASE.size + PAGE_SIZE
    ok = wrong = untr = trapped = 0
    shown = 0
    for i, (words, entries, jt, nzcv, x, mem, (enzcv, ex, emem)) in enumerate(cases):
        rec = got[i * step:(i + 1) * step]
        status = struct.unpack_from("<I", rec)[0]
        if status == 1:
            untr += 1
            continue
        _, gnzcv, gpc, *gx, _, _, _ = CASE.unpack_from(rec, 4)
        bad = ["trap at %#x" % gpc] if status == 2 else []
        if gnzcv != enzcv:
            bad.append("nzcv %x != %x" % (gnzcv >> 28, enzcv >> 28))
        bad += ["%s %#x != %#x" % ("sp" if j == 31 else "x%d" % j, gx[j], ex[j]) for j in range(32) if gx[j] != ex[j]]
        gmem = rec[4 + CASE.size:]
        if gmem != emem:
            k = next(j for j in range(PAGE_SIZE) if gmem[j] != emem[j])
            bad.append("mem +%#x got %s want %s" % (k, gmem[k:k + 8].hex(), emem[k:k + 8].hex()))
        if bad:
            wrong += 1
            trapped += status == 2
            if shown < int(os.environ.get("SHOW", "3")):
                shown += 1
                print("WRONG case %d (%d words, entries %s): %s" % (i, len(words), " ".join("%#x" % e for e in entries),
                                                                  "; ".join(bad[:6])))
        else:
            ok += 1
    print("programs: %d ok, %d wrong (%d trapped), %d not translated, %d refused by Unicorn" % (
        ok, wrong, trapped, untr, refused))
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
