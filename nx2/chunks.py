"""Stable chunk boundaries for the lifted C (lift.py --write).

a64-kit's write() closes a chunk once it reaches CHUNK_BYTES of text, so a function that grows (a jump table found,
a fix in one form) moves every boundary after it, and build.py recompiles every chunk from there on: a fix to two
functions cost a full 45-minute rebuild. Here the boundaries are guest addresses, kept in work/image/chunks.txt:
a chunk ends after the last function below the next boundary, so only the chunks holding changed functions differ.
Without the file (or with --rechunk) the boundaries come from the sizes, as before, and are saved.

write()'s test is `size >= chunk_bytes`. chunk_bytes is a Boundaries instance: an int whose reflected comparison
(__le__, which Python tries first for a subclass on the right) decides from the function just emitted.
"""
import bisect
import os
import re

import a64_lift

FILE = "chunks.txt"


class Boundaries(int):
    def __new__(cls, size, starts, funcs):
        self = super().__new__(cls, size)
        self.starts = sorted(starts)
        self.recorded = []   # (without saved boundaries) where the size rule started a chunk
        self.current = None
        self.after = None    # the function emitted after `current`
        order = sorted(funcs)
        self.ends = set()
        for b in self.starts:
            i = bisect.bisect_left(order, b)
            if 0 < i < len(order):
                self.ends.add(order[i - 1])
        self.next_of = {a: b for a, b in zip(order, order[1:])}
        return self

    def __le__(self, size):   # `size >= self`
        e = self.current
        if self.starts:
            return e in self.ends
        if size >= int(self):
            nxt = self.next_of.get(e)
            if nxt is not None:
                self.recorded.append(nxt)
            return True
        return False


def load(img_dir):
    path = os.path.join(img_dir, FILE)
    if not os.path.exists(path):
        return []
    return [int(l, 16) for l in open(path) if l.strip() and not l.startswith("#")]


def save(img_dir, starts):
    with open(os.path.join(img_dir, FILE), "w") as f:
        f.write("# first guest address of each lifted C chunk after the first (nx2/chunks.py)\n")
        f.write("".join("%08x\n" % s for s in sorted(starts)))


def seed_from_gen(gen_dir):
    """The boundaries of an existing size-chunked lift: the first function defined in each code_NNN.c after the
    first"""
    starts = []
    pat = re.compile(r"^void f_([0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{")
    n = 1
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):
        for line in open(os.path.join(gen_dir, "code_%03d.c" % n)):
            m = pat.match(line)
            if m:
                starts.append(int(m.group(1), 16))
                break
        n += 1
    return starts


def write(L, gen_dir, img_dir, rechunk=False):
    """L.write(gen_dir) with stable boundaries; returns its files"""
    starts = [] if rechunk else load(img_dir)
    if not starts and not rechunk:
        starts = seed_from_gen(gen_dir)
    b = Boundaries(a64_lift.CHUNK_BYTES, starts, L.funcs)
    base_emit = L.emit_function

    def emit_function(e, texts):
        b.current = e
        return base_emit(e, texts)
    L.emit_function = emit_function
    try:
        files = L.write(gen_dir, chunk_bytes=b)
    finally:
        del L.emit_function
    # (the numbered chunks only: files has code_hot.c too, and counting it skipped the first stale chunk, linked twice)
    n = sum(1 for f in files if re.fullmatch(r"code_\d{3}\.c", os.path.basename(f)))
    while os.path.exists(os.path.join(gen_dir, "code_%03d.c" % n)):   # (fewer chunks than the last lift)
        os.remove(os.path.join(gen_dir, "code_%03d.c" % n))
        n += 1
    save(img_dir, starts or b.recorded)
    print("chunks: %d, boundaries %s" % (len(files), "kept" if starts else "from sizes (saved)"))
    return files
