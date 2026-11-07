"""Descriptor timing is off unless RX_VK_SLOW_DESC is set.

build_descriptors used to read the clock before every uniform, storage buffer, and
written-buffer stamp, then again inside a log that prints at most 40 lines.
python -I -B tests/test_desc_clock.py
"""
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
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    if "bool slow_desc_timing()" not in source or "RX_VK_SLOW_DESC" not in source:
        raise SystemExit("descriptor timing has no off switch")
    fn = block(source, "void build_descriptors(")
    clocks = [line.strip() for line in fn.splitlines() if "steady_clock::now()" in line]
    logs = [line.strip() for line in fn.splitlines() if "slow_descriptor(" in line]
    if not clocks or not logs:
        raise SystemExit("build_descriptors no longer times descriptors")
    bare = [line for line in clocks + logs if "slow_desc_timing()" not in line]
    if bare:
        raise SystemExit("unguarded descriptor clock:\n" + "\n".join(bare))
    print(f"PASS: {len(clocks)} descriptor clocks and {len(logs)} slow logs stay behind RX_VK_SLOW_DESC")


if __name__ == "__main__":
    main()
