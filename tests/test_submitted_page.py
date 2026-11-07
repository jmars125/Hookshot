"""A replay read reuses the last answer for that page until a stamp or a new capture.

Once a capture exists, every uniform word took the pool lock to ask whether its page
was in one. python -I -B tests/test_submitted_page.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    fn = block(source, "bool submitted_page_reuse(")
    read = block(source, "bool memory_read_as_submitted(")
    reuse_at = read.index("submitted_page_reuse(")
    lock_at = read.index("std::lock_guard lk(pools_mu)")
    if reuse_at > lock_at:
        raise SystemExit("the page is looked up under the pool lock before the cache")
    for name in ("void memory_stamp(", "void memory_collect_written(", "bool memory_take_capture("):
        body = block(source, name)
        if "cap_view_bump()" not in body:
            raise SystemExit(f"{name} changes page state without dropping the cached read")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    CHECK(submitted_page_reuse(9, 4, 7, 9, 4, 7));
    CHECK(!submitted_page_reuse(9, 4, 8, 9, 4, 7));
    CHECK(!submitted_page_reuse(9, 5, 7, 9, 4, 7));
    CHECK(!submitted_page_reuse(10, 4, 7, 9, 4, 7));
    CHECK(!submitted_page_reuse(9, 4, 7, 9, 4, 0));
    std::printf("PASS: a captured page is reused only for that generation\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="submitted-page-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
