"""The shader census finds the game's compute programs: Effect/*.vsetb and the agl shader archives (.sharcb).

Without them the seed had no compute program at all, and each was translated and built the first time the game met it,
2-18 s with the picture frozen on an RX 7700S. Synthetic data only, no game files. python -I -B tests/test_shader_census.py
"""
import collections
import os
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import shader_census as sc  # noqa: E402

CONTROL_SIZE = 0xD8


def control(stage, length=0x40, c1o=0, end=0x100):
    c = bytearray(CONTROL_SIZE)
    c[0:4] = sc.CONTROL_MAGIC
    struct.pack_into("<I", c, 0x38, length)
    struct.pack_into("<II", c, 0x40, c1o, end)
    struct.pack_into("<H", c, 0x56, stage)
    return c


def sharc(entries, code_base=0x1000, size=0x4000):
    """A BAHS blob: each entry (control offset, stage, code offset from the code region, code end)"""
    data = bytearray(size)
    data[0:4] = b"BAHS"
    struct.pack_into("<I", data, 0x1C, code_base)
    for at, stage, code_off, end in entries:
        data[at:at + CONTROL_SIZE] = control(stage, end=end)
        struct.pack_into("<I", data, at - 0x68, code_off)
        code = code_base + code_off
        if code + end <= size:
            data[code:code + end] = bytes((at + i) & 0xFF for i in range(end))
    return bytes(data)


def check_sharc():
    data = sharc([(0x200, 5, 0x100, 0x100),     # a compute program
                  (0x400, 1, 0x300, 0x100),     # a fragment stage: not a program of its own here
                  (0x600, 5, 0x3000, 0x100)])   # its code past the end of the archive
    stats = collections.Counter()
    found = list(sc.sharc_computes(data, stats))
    if len(found) != 1 or len(found[0]) != 1:
        raise SystemExit("expected one compute program, got %r" % [[s[0] for s in p] for p in found])
    stage, ctl, code = found[0][0]
    if stage != 5 or ctl != data[0x200:0x200 + CONTROL_SIZE] or code != data[0x1100:0x1200]:
        raise SystemExit("the compute program's control section or code is not the archive's")
    if stats["sharc, stage 5"] != 1 or stats["sharc, code out of range"] != 1:
        raise SystemExit("unexpected counts: %r" % dict(stats))


def check_archives():
    with tempfile.TemporaryDirectory() as romfs:
        for folder, name in (("Shader", "Model.Product.bfsha.zs"), ("Shader", "ApplicationPackage.release.sarc.zs"),
                             ("Effect", "Animal_Cow.Nin_Ounce_NVN2.vsetb.byml.zs"), ("Effect", "notes.txt"),
                             ("Layout2Shader", "MergeShader.bfsha.zs")):
            os.makedirs(os.path.join(romfs, folder), exist_ok=True)
            open(os.path.join(romfs, folder, name), "wb").close()
        names = sorted(os.path.basename(p) for p in sc.archives(romfs))
    want = sorted(["Model.Product.bfsha.zs", "ApplicationPackage.release.sarc.zs", "Animal_Cow.Nin_Ounce_NVN2.vsetb.byml.zs",
                   "MergeShader.bfsha.zs"])
    if names != want:
        raise SystemExit("archives: %r, expected %r" % (names, want))


def check_main_reads_sharc():
    source = (ROOT / "nx2" / "shader_census.py").read_text(encoding="utf-8")
    if 'data[:4] == b"BAHS"' not in source or "sharc_computes(data, stats)" not in source:
        raise SystemExit("main() does not hand BAHS archives to sharc_computes")


def main():
    check_sharc()
    check_archives()
    check_main_reads_sharc()
    print("shader census: compute programs from .vsetb and .sharcb ok")


if __name__ == "__main__":
    main()
