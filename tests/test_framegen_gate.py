"""Frame generation's copy and luma run only when the menu asks for made frames.

With the menu at Off, every present still copied the frame and reduced it to luma. Open areas paid for that
and were shown no extra frames.

python -I -B tests/test_framegen_gate.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8")
    gate = source[source.index("bool framegen_on("):source.index("bool framegen_on(") + 400]
    if "framegen_wanted(opts.framegen" not in gate:
        raise SystemExit("presents still run the interpolation while the menu has it off")
    fn = block(source, "bool framegen_wanted(")
    fixture = r'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
''' + fn + r'''
int main() {
    CHECK(!framegen_wanted(0, 1));   /* menu Off: no copy, no luma */
    CHECK(!framegen_wanted(1, 1));   /* the blank step between Off and 2x */
    CHECK(framegen_wanted(2, 1));
    CHECK(framegen_wanted(8, 1));
    CHECK(!framegen_wanted(2, 0));   /* RX_VK_FRAMEGEN=0 */
    std::printf("PASS: interpolation runs only from 2x up\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="framegen-gate-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
