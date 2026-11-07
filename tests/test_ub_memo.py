"""A uniform word is not read again while later updates miss it.

python -I -B tests/test_ub_memo.py [--cxx g++]
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
    source = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    fn = block(source, "bool ub_tail_clear(")
    read = block(source, "bool ub_read(")
    if read.index("ub_word_hit(") > read.index("memory_read_as_submitted("):
        raise SystemExit("a uniform word is read before checking it was already known")
    hit = block(source, "bool ub_word_hit(")
    for needle in ("ub_tail_clear(", "memory_page_stamp(", "ub_prune_gen", "rec_value_now"):
        if needle not in hit:
            raise SystemExit(f"a remembered uniform word ignores {needle}")
    over = block(source, "uint64_t ub_overlay(")
    if over.index("ub_copy_hit(") > over.index("for (size_t i = first;"):
        raise SystemExit("a uniform copy is scanned before checking the last one still stands")
    if "ub_tail_clear(" not in block(source, "bool ub_copy_hit("):
        raise SystemExit("a remembered uniform copy ignores updates after it")
    if "ub_prune_gen" not in block(source, "void ub_prune("):
        raise SystemExit("dropping an update leaves a remembered word in place")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
{fn}
int main() {{
    uint64_t dst[4]{{100, 200, 300, 400}};
    uint64_t len[4]{{16, 16, 16, 16}};
    CHECK(ub_tail_clear(2, 2, 1000, 4, dst, len, 4));
    CHECK(ub_tail_clear(2, 4, 1000, 4, dst, len, 4));
    CHECK(!ub_tail_clear(0, 4, 204, 8, dst, len, 4));
    CHECK(!ub_tail_clear(1, 0, 1000, 4, dst, len, 4));
    CHECK(!ub_tail_clear(0, 5, 1000, 4, dst, len, 4));
    CHECK(!ub_tail_clear(0, 1, 100, 4, dst, len, 0));
    std::printf("PASS: a uniform word stays until an update lands on it\\n");
}}
'''
    with tempfile.TemporaryDirectory(prefix="ub-memo-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
