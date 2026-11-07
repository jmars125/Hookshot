"""Rebinding the same program, buffers, or handles does not drop the draw's descriptors.

python -I -B tests/test_same_bind.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def before(text, start, end, early, late):
    part = text[text.index(start):text.index(end)]
    if part.index(early) > part.index(late):
        raise SystemExit(f"{start} bumps the bindings before checking they changed")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    handles = block(source, "bool handles_unchanged(")
    bufs = block(source, "bool buf_unchanged(")
    before(source, "case NOP_BIND_TEX:", "case NOP_STENCIL:", "handles_unchanged(", "s.bind_gen++")
    before(source, "case NOP_BIND_UBO:", "case NOP_BIND_TEX:", "buf_unchanged(", "s.bind_gen++")
    before(source, "case NOP_BIND_BUFS:", "case NOP_BIND_UBO:", "buf_unchanged(", "s.bind_gen++")
    before(source, "case NOP_BIND_PROGRAM:", "case NOP_BIND_STATE:", "std::memcmp(&s.prog", "s.bind_gen++")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{handles}
{bufs}
int main() {{
    const uint64_t have[] = {{1, 2, 3}};
    const uint64_t next[] = {{1, 2, 4}};
    CHECK(handles_unchanged(have, have, 3));
    CHECK(handles_unchanged(have, next, 0));
    CHECK(!handles_unchanged(have, next, 3));
    CHECK(!handles_unchanged(have, next, -1));
    CHECK(buf_unchanged(1, 2, 0x1000, 64, 1, 2, 0x1000, 64));
    CHECK(!buf_unchanged(1, 2, 0x1000, 64, 1, 2, 0x2000, 64));
    CHECK(!buf_unchanged(1, 2, 0x1000, 64, 1, 3, 0x1000, 64));
    std::printf("PASS: the same binding does not count as a new one\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="same-bind-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
