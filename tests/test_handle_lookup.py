"""A texture handle is looked up once per use.

The hit counters used to search the map on their own, then the draw searched it
again. The counters run only while RX_VK_LOG_HITS is set.

python -I -B tests/test_handle_lookup.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    start = source.index("VkImageView texture_view_of(")
    body = source[start:source.index("struct BankCache", start)]
    finds = body.count("handle_hits.find")
    if finds != 1:
        raise SystemExit(f"texture_view_of searches the handle map {finds} times")
    if "RX_VK_LOG_HITS" not in body:
        raise SystemExit("the hit counters are gone")
    stat = body.index("st_new++")
    guard = body.rfind("if (log_hits)", 0, stat)
    if guard < 0:
        raise SystemExit("the hit counters run on every lookup")
    print("PASS: one handle lookup, and the counters stay behind RX_VK_LOG_HITS")


if __name__ == "__main__":
    main()
