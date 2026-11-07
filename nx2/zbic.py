"""ZBIC decompression (Switch firmware 22.0.0+'s zstd variant) through third_party/zbic, built to zbic.dll on first use.

    import zbic; plain = zbic.decompress(data)
"""
import ctypes
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit  # noqa: F401  (a64-kit on sys.path)
import toolchain

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "third_party", "zbic")
DLL = os.path.join(SRC, "zbic.dll" if os.name == "nt" else "zbic.so")
_lib = None


def _build():
    # (the one that came with the tools is used as it is. This compared file dates, which unpacking a zip can change: the source then
    # looked newer, and reading a .nup needed Microsoft's C library, which a player need not have at that point)
    if os.path.exists(DLL):
        return
    srcs = [os.path.join(SRC, "zstd.c"), os.path.join(SRC, "zbic_api.c")]
    cmd = [toolchain.need(toolchain.find("clang")), *toolchain.native_target(), *toolchain.native_link(),
           "-O2", "-shared", *([] if os.name == "nt" else ["-fPIC"]), "-DZSTD_ZBIC_SUPPORT=1", "-I", SRC, "-o", DLL, *srcs]
    print("building", os.path.basename(DLL), file=sys.stderr)
    subprocess.run(cmd, check=True)


def lib():
    global _lib
    if _lib is None:
        _build()
        _lib = ctypes.CDLL(DLL)
        _lib.zbic_content_size.restype = ctypes.c_longlong
        _lib.zbic_content_size.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        _lib.zbic_decompress.restype = ctypes.c_longlong
        _lib.zbic_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    return _lib


def is_zbic(data):
    return data[:4] == b"ZBIC"


def decompress(data, size=None):
    L = lib()
    if size is None:
        size = L.zbic_content_size(data, len(data))
        if size < 0:
            raise ValueError("ZBIC: no content size in the frame header")
    out = ctypes.create_string_buffer(size)
    n = L.zbic_decompress(out, size, data, len(data))
    if n != size:
        raise ValueError("ZBIC: decompression failed (%d of %d bytes)" % (n, size))
    return out.raw
