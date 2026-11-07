"""A draw keeps the last program and pipeline when the material has not changed.

python -I -B tests/test_same_draw.py [--cxx g++]
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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    shader = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    prog = block(shader, "bool prog_same(")
    pipe = block(draw, "bool pipe_same(")
    choose = block(shader, "const Program* program_for(")
    if choose.index("prog_same(") > choose.index("picks[") :
        raise SystemExit("program_for hashes the inputs before checking the last answer")
    pipes = block(draw, "PipeEntry* graphics_pipeline(")
    if pipes.index("pipe_same(") > pipes.index("mine.find("):
        raise SystemExit("graphics_pipeline hashes the key before checking the last pipeline")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{prog}
{pipe}
int main() {{
    uint32_t va[16]{{}}, vb[16]{{}};
    uint8_t ct[8]{{}}, cu[8]{{}};
    uint32_t fm[8]{{}}, fn[8]{{}};
    uint32_t dv[16]{{}}, dw[16]{{}};
    uint64_t bl[8]{{}}, bm[8]{{}};
    va[3] = 7; vb[3] = 7; ct[1] = 2; cu[1] = 2; fm[0] = 9; fn[0] = 9; dv[1] = 4; dw[1] = 4; bl[0] = 11; bm[0] = 11;
    CHECK(prog_same(1, 5, 9, 3, 4, 8, 2, va, ct, 1, 5, 9, 3, 4, 8, 2, vb, cu));
    CHECK(!prog_same(1, 5, 9, 3, 4, 8, 2, va, ct, 1, 5, 9, 99, 4, 8, 2, vb, cu));
    CHECK(!prog_same(1, 5, 9, 3, 4, 8, 2, va, ct, 1, 5, 9, 3, 4, 1, 2, vb, cu));
    cu[1] = 3;
    CHECK(!prog_same(1, 5, 9, 3, 4, 8, 2, va, ct, 1, 5, 9, 3, 4, 8, 2, vb, cu));
    CHECK(pipe_same(1, 2, 3, 4, 1, 5, fm, 6, va, dv, 7, bl, 8, 9, 0, 1, 1, 2, 3, 4, 1, 5, fn, 6, vb, dw, 7, bm, 8, 9, 0, 1));
    CHECK(!pipe_same(1, 2, 3, 4, 1, 5, fm, 6, va, dv, 7, bl, 8, 9, 0, 1, 1, 99, 3, 4, 1, 5, fn, 6, vb, dw, 7, bm, 8, 9, 0, 1));
    fn[0] = 10;
    CHECK(!pipe_same(1, 2, 3, 4, 1, 5, fm, 6, va, dv, 7, bl, 8, 9, 0, 1, 1, 2, 3, 4, 1, 5, fn, 6, vb, dw, 7, bm, 8, 9, 0, 1));
    std::printf("PASS: the same material keeps its program and pipeline\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="same-draw-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
