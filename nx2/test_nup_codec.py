"""Game-free actual Nup constructor/copy_to regression; requires zstandard.
python -I -B nx2/test_nup_codec.py
"""
import argparse
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import zstandard

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
a = p.parse_args()
spec = importlib.util.spec_from_file_location('tested_nup', a.source/'nx2/nup.py')
nup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nup)


def package(name, logical, stored, codec, extent=None):
    """One title, one root directory, one file; no private content or other modules."""
    name = name.encode()
    file_at = 0x80 + 0x30 + 0x40 + 24
    strings_at = file_at + 80 + 4
    data_at = (strings_at + len(name) + 15) & ~15
    # The tail deliberately contains the rest of a truncated declared extent.
    blob = bytearray(data_at + len(stored) + len(logical) + 64)
    blob[:4] = b'NUP\x01'
    struct.pack_into('<Q', blob, 0x40, len(blob))
    blob[0x80:0x84] = b'TOC\x01'
    struct.pack_into('<IQQQQ', blob, 0x84, 1, 1, 1, strings_at-0x80, len(name))
    struct.pack_into('<QIIII', blob, 0xb0, 1, 0, 0, 0, 1)
    struct.pack_into('<4I', blob, file_at-24, 0, 0, 0, 1)
    struct.pack_into('<IIQQQ', blob, file_at, 0, len(name), len(logical), data_at,
                     len(stored) if extent is None else extent)
    blob[file_at+64] = codec
    struct.pack_into('<I', blob, file_at+80, 1)
    blob[strings_at:strings_at+len(name)] = name
    blob[data_at:data_at+len(stored)] = stored
    return blob


with tempfile.TemporaryDirectory(prefix='nup-codec-') as temporary:
    path = Path(temporary)/'fixture.nup'
    def copy(name, logical, stored, codec, extent=None):
        path.write_bytes(package(name, logical, stored, codec, extent))
        reader = nup.Nup(str(path))
        try:
            relative, record = next(reader.walk(0))
            assert relative == name
            output = io.BytesIO()
            reader.copy_to(record, output, relative)
            return output.getvalue()
        finally:
            reader.f.close()

    compressor = zstandard.ZstdCompressor(write_checksum=True)
    plain = bytes(32) + b'ordinary payload, not another Zstd frame'*100
    packed = compressor.compress(plain)
    assert copy('member.zs', plain, packed, 1) == plain, 'explicit outer codec ignored for .zs plaintext'
    game_zstd = compressor.compress(b'game asset content'*200)
    assert copy('asset.zs', game_zstd, game_zstd, 0) == game_zstd, 'raw game Zstd was decompressed'
    assert copy('legacy.bin', plain, packed, 0) == plain, 'legacy outer-frame detection changed'
    try:
        copy('truncated.zs', plain, packed, 1, len(packed)-1)
    except SystemExit:
        pass
    else:
        raise AssertionError('explicit extent was overread or incomplete frame accepted')
print('PASS: explicit .zs plaintext, raw game Zstd, legacy detection and truncated extent rejection')
