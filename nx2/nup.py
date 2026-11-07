"""Read a .nup package (a decrypted Switch 2 dump: titles, each with an exefs/ and a romfs/ tree) and extract it.

    python -I nx2/nup.py <file.nup>                      list the titles and their trees
    python -I nx2/nup.py <file.nup> --extract <dir> [--title <n|version>] [--only exefs|romfs]

The layout (worked out from the file; little-endian):
    0x000  "NUP\\x01" header, u64 file size at +0x40
    0x080  "TOC\\x01", u32 titles, u64 dirs, u64 files, u64 strings offset (from 0x80), u64 strings size
    0x0b0  titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version, ...,
           u64 offset + u64 size of the title's metadata blob at +0x20
    then   dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 children, u64 0
    then   files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 size again,
           32-byte hash, 16 bytes 0
    then   the child list, u32 each: a node index; below the dir count a dir, else (index - dirs) a file
    then   the name strings (not 0-terminated)
The data of a file is stored as is at its data offset, or, in .nup files from newer dumpers, as a zstd frame whose content is the
file: its size field is then the size of the file, not of the frame (seen on rtld: 10524 bytes in the listing, frame content 10524).
A file that is itself zstd in the game (the romfs .zs files) is not one of those: its frame's content is larger than the file.
"""
import argparse
import os
import shutil
import stat
import struct
import sys


ZSTD = b"\x28\xb5\x2f\xfd"
WINDOWS_DEVICES = {"CON", "CONIN$", "CONOUT$", "PRN", "AUX", "NUL"} | {p + n for p in ("COM", "LPT") for n in "123456789¹²³"}


def frame_content_size(head):
    """The content size a zstd frame header states, None when it states none"""
    if head[:4] != ZSTD or len(head) < 18:
        return None
    fhd = head[4]
    single = (fhd >> 5) & 1
    at = 5 + (0 if single else 1) + (0, 1, 2, 4)[fhd & 3]
    n = (1 if single else 0, 2, 4, 8)[fhd >> 6]
    if not n:
        return None
    size = int.from_bytes(head[at:at + n], "little")
    return size + 256 if n == 2 else size


class Nup:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0x80)
        if h[:4] != b"NUP\x01":
            raise SystemExit("%s: not a NUP package (magic %r)" % (path, h[:4]))
        self.file_size = struct.unpack_from("<Q", h, 0x40)[0]
        # (a package that ends early (a download or copy that did not finish) failed 30 minutes in as "short read in the
        # package": said now, with the sizes)
        have = os.fstat(self.f.fileno()).st_size
        if self.file_size and have < self.file_size:
            raise SystemExit("the .nup file is incomplete: it is %d MB, its header says %d MB. Download or copy it again "
                             "(wait for the download to finish) and check that the drive had room" % (have >> 20, self.file_size >> 20))
        t = self.f.read(0x30)
        if t[:4] != b"TOC\x01":
            raise SystemExit("%s: no TOC at 0x80" % path)
        nt, nd, nf, soff, ssz = struct.unpack_from("<IQQQQ", t, 4)
        self.nd, self.nf = nd, nf
        dirs_at = 0x80 + 0x30 + 0x40 * nt
        files_at = dirs_at + 24 * nd
        kids_at = files_at + 80 * nf
        strings_at = 0x80 + soff
        self.f.seek(0)
        meta = self.f.read(strings_at + ssz)
        self.strings = meta[strings_at:strings_at + ssz]
        self.titles = []
        for i in range(nt):
            o = 0xb0 + 0x40 * i
            tid, root, exefs, romfs, ver = struct.unpack_from("<QIIII", meta, o)
            self.titles.append(dict(index=i, id=tid, root=root, exefs=exefs, romfs=romfs, version=ver))
        self.dirs = [struct.unpack_from("<4I", meta, dirs_at + 24 * i) for i in range(nd)]
        self.files = []
        for i in range(nf):
            no, nl, size, off, size2 = struct.unpack_from("<IIQQQ", meta, files_at + 80 * i)
            self.files.append((no, nl, size, off, size2))
        nk = (strings_at - kids_at) // 4
        self.kids = struct.unpack_from("<%dI" % nk, meta, kids_at)

    def name(self, off, n):
        return self.strings[off:off + n].decode("utf-8")

    def walk(self, d, prefix=""):
        """(relative path, file record) for every file under dir d."""
        no, nl, first, n = self.dirs[d]
        for k in self.kids[first:first + n]:
            if k < self.nd:
                cno, cnl = self.dirs[k][:2]
                yield from self.walk(k, prefix + self.name(cno, cnl) + "/")
            else:
                r = self.files[k - self.nd]
                yield prefix + self.name(r[0], r[1]), r

    def inner_is_zstd(self, r):
        """Whether what this frame holds is itself a zstd frame"""
        import zstandard
        self.f.seek(r[3])
        try:
            return zstandard.ZstdDecompressor().decompressobj().decompress(self.f.read(min(r[2], 1 << 20)))[:4] == ZSTD
        except zstandard.ZstdError:   # (made with one of the game's dictionaries: the game's own file)
            return False

    def is_packed(self, r, name=""):
        """A file the package keeps compressed. A frame that states no size is tried: it is the file when it unpacks to the listed size.
        A .zs file is zstd in the game too, and three of them happen to hold exactly as much as they take: that one is packed only if
        what it holds is zstd again"""
        self.f.seek(r[3])
        head = self.f.read(18)
        if head[:4] != ZSTD:
            return False
        size = frame_content_size(head)
        if name.endswith(".zs") and (size is None or size == r[2]):
            return size == r[2] and self.inner_is_zstd(r)
        if size is not None:
            return size == r[2]
        import zstandard
        self.f.seek(r[3])
        d = zstandard.ZstdDecompressor().decompressobj()
        got = 0
        left = r[2]
        try:
            while left and got <= r[2] and not d.eof:
                b = self.f.read(min(left, 1 << 20))
                if not b:
                    break
                left -= len(b)
                got += len(d.decompress(b))
        except zstandard.ZstdError:
            return False
        return got == r[2]

    def copy_to(self, r, g, name=""):
        """The file, as itself, into the open file g"""
        packed = self.is_packed(r, name)
        self.f.seek(r[3])
        left = r[2]
        if packed:
            left += 1 << 16   # (a frame of something that does not compress is a little larger than what it holds)
            import zstandard
            d = zstandard.ZstdDecompressor().decompressobj()
            got = 0
            while left and not getattr(d, "eof", False):
                b = self.f.read(min(left, 8 << 20))
                if not b:
                    break
                left -= len(b)
                try:
                    out = d.decompress(b)
                except zstandard.ZstdError:   # (the end of the frame, and the next file's bytes after it)
                    break
                g.write(out)
                got += len(out)
            if got != r[2]:
                raise SystemExit("a compressed file came out as %d bytes, %d expected" % (got, r[2]))
            return
        while left:
            b = self.f.read(min(left, 8 << 20))
            if not b:
                raise SystemExit("the .nup file ends early (short read at %d of %d MB): it is incomplete. Download or copy it again"
                                 % (self.f.tell() >> 20, os.fstat(self.f.fileno()).st_size >> 20))
            g.write(b)
            left -= len(b)

    def read(self, r, n=None):
        import io
        b = io.BytesIO()
        self.copy_to(r, b)
        return b.getvalue() if n is None else b.getvalue()[:n]


def pick_title(nup, want):
    if want is None:
        return max(nup.titles, key=lambda t: t["version"])   # (the newest: the update over the base)
    for t in nup.titles:
        if str(t["index"]) == want or str(t["version"]) == want or "%016x" % t["id"] == want.lower():
            return t
    raise SystemExit("no title %s" % want)


def extraction_path(out, part, rel, temporary_suffix=".part"):
    """A package path stays under its part, without following existing output links."""
    parts = rel.split("/")
    if (not rel or "\\" in rel or any(not p or p in (".", "..") or p[-1] in " ." or
            any(c in '<>:"|?*' or ord(c) < 32 for c in p) or p.split(".")[0].rstrip(" ").upper() in WINDOWS_DEVICES
            for p in parts)):
        raise SystemExit("unsafe path in the package: %r" % rel)
    root = os.path.realpath(os.path.abspath(out))
    base = os.path.join(root, part)
    dst = os.path.join(base, *parts)
    if os.path.commonpath((os.path.normcase(base), os.path.normcase(os.path.abspath(dst)))) != os.path.normcase(base):
        raise SystemExit("path leaves the extraction folder: %r" % rel)
    at = root
    paths = [at]
    for p in (part, *parts):
        at = os.path.join(at, p)
        paths.append(at)
    temporary = dst + temporary_suffix
    paths.append(temporary)
    for path in paths:
        try:
            info = os.lstat(path)
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
            raise SystemExit("extraction path is a link or reparse point: %s" % path)
        if path == temporary and info.st_nlink > 1:
            raise SystemExit("extraction temporary file has other hard links: %s" % path)
    return dst


def extract(nup, t, out, only=None, quiet=False):
    total = n = 0
    for part in ("exefs", "romfs"):
        if only and part != only:
            continue
        for rel, r in nup.walk(t[part]):
            dst = extraction_path(out, part, rel)
            if os.path.exists(dst) and os.path.getsize(dst) == r[2]:
                continue          # (re-runs pick up where they stopped)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst + ".part", "wb") as g:
                nup.copy_to(r, g, rel)
            os.replace(dst + ".part", dst)
            total += r[2]
            n += 1
            if not quiet and n % 5000 == 0:
                print("  %d files, %.1f GB" % (n, total / 1e9), flush=True)
    return n, total


def heal(nup, t, out, names):
    """An update's exefs need not hold every module whole: what is not an NSO there is taken from the package's other titles"""
    for m in names:
        p = extraction_path(out, "exefs", m)
        have = b""
        if os.path.exists(p):
            with open(p, "rb") as f:
                have = f.read(16)
        if have[:4] == b"NSO0":
            continue
        for other in reversed(nup.titles):
            if other["index"] == t["index"]:
                continue
            for rel, r in nup.walk(other["exefs"]):
                if rel == m and nup.read(r, 4) == b"NSO0":
                    with open(p + ".part", "wb") as g:
                        g.write(nup.read(r))
                    os.replace(p + ".part", p)
                    print("  %s: taken from title %d (v%d)" % (m, other["index"], other["version"]))
                    break
            else:
                continue
            break
        else:
            if have:
                print("  %s is not a game module in any title: it starts %s" % (m, have.hex()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("nup")
    ap.add_argument("--extract", metavar="DIR")
    ap.add_argument("--title", help="index, version or title id (default: the newest)")
    ap.add_argument("--only", choices=("exefs", "romfs"))
    a = ap.parse_args()
    nup = Nup(a.nup)
    print("%s: %.2f GB, %d titles, %d dirs, %d files" % (os.path.basename(a.nup), nup.file_size / 1e9,
                                                          len(nup.titles), nup.nd, nup.nf))
    for t in nup.titles:
        ex = list(nup.walk(t["exefs"]))
        ro = list(nup.walk(t["romfs"]))
        print("title %d: %016x v%d  exefs %d files (%s)  romfs %d files, %.2f GB" % (
            t["index"], t["id"], t["version"], len(ex), ", ".join("%s %d" % (p, r[2]) for p, r in ex),
            len(ro), sum(r[2] for _, r in ro) / 1e9))
        same = sum(1 for _, r in ro if r[2] != r[4])
        if same:
            print("  (%d romfs files with two different sizes)" % same)
    if a.extract:
        t = pick_title(nup, a.title)
        free = shutil.disk_usage(os.path.dirname(os.path.abspath(a.extract)) or ".").free
        print("extracting title %d (v%d) to %s (%.0f GB free)" % (t["index"], t["version"], a.extract, free / 1e9))
        n, total = extract(nup, t, a.extract, a.only)
        print("done: %d files written, %.2f GB" % (n, total / 1e9))
        if a.only != "romfs":   # (main is ZBIC-compressed on firmware 22+: made a plain NSO for the tools)
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import unpack
            heal(nup, t, a.extract, unpack.NAMES)
            for m in unpack.NAMES:
                p = extraction_path(a.extract, "exefs", m, temporary_suffix=".plain")
                if os.path.exists(p) and unpack.unpack_nso(p):
                    print("  %s: ZBIC -> plain NSO" % m)


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):   # (-I ignores PYTHONIOENCODING: a .nup's or a file's name in other letters crashed a print)
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    sys.exit(main())
