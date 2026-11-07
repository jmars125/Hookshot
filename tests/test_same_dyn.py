"""A draw does not rebuild dynamic state that matches the draw before it.

python -I -B tests/test_same_dyn.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    fn = block(source, "bool dyn_same(")
    pack = block(source, "int dyn_pack(")
    for needle in ("s.viewport", "s.scissor", "s.chmask", "s.blend", "s.ds", "cur_program", "s.poly"):
        if needle not in pack:
            raise SystemExit(f"dynamic state is remembered without {needle}")
    body = block(source, "void dynamic_state(")
    if body.index("dyn_same(") > body.index("std::memset(&d, 0"):
        raise SystemExit("dynamic state is built before checking the draw before it")
    guard = body[:body.index("dyn_same(")]
    if "last_dyn_valid" not in guard:
        raise SystemExit("a clear's dynamic state is left unset")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    float fp[2]{{1.f, 2.f}}, cfp[2]{{1.f, 2.f}};
    uint32_t up[2]{{3, 4}}, cup[2]{{3, 4}};
    uint8_t raw[2]{{5, 6}}, craw[2]{{5, 6}};
    CHECK(dyn_same(fp, 2, up, 2, raw, 2, cfp, cup, craw));
    cfp[1] = 9.f;
    CHECK(!dyn_same(fp, 2, up, 2, raw, 2, cfp, cup, craw));
    cfp[1] = 2.f;
    cup[0] = 8;
    CHECK(!dyn_same(fp, 2, up, 2, raw, 2, cfp, cup, craw));
    cup[0] = 3;
    craw[1] = 7;
    CHECK(!dyn_same(fp, 2, up, 2, raw, 2, cfp, cup, craw));
    CHECK(!dyn_same(fp, 0, up, 2, raw, 2, cfp, cup, craw));
    std::printf("PASS: the same dynamic state is not built again\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="same-dyn-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
