"""A uniform word is not read again while the update gap still covers it.

The gap is the lookup. The word is kept only while that gap covers it.
python -I -B tests/test_ub_memo.py
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
    source = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    if "ub_prune_gen" in source or "ub_tail_clear" in source:
        raise SystemExit("a uniform word keeps its own scan beside the gap")
    read = block(source, "bool ub_read(")
    if read.index("ub_word_hit(") > read.index("memory_read_as_submitted("):
        raise SystemExit("a uniform word is read before checking it was already known")
    if read.index("ub_gap_holds(") > read.index("for (size_t i = 0; i < lim; i++)"):
        raise SystemExit("ub_read walks the updates before checking the gap")
    if read.count("w.on = true") != 1 or read.index("w.on = true") > read.index("for (size_t i = 0; i < lim; i++)"):
        raise SystemExit("a uniform word is kept after an update has been copied onto it")
    hit = block(source, "bool ub_word_hit(")
    for needle in ("ub_gap_holds(", "memory_page_stamp(", "rec_value_now"):
        if needle not in hit:
            raise SystemExit(f"a remembered uniform word ignores {needle}")
    if "ub_updates" in hit:
        raise SystemExit("a remembered uniform word scans the update list itself")
    over = block(source, "uint64_t ub_overlay(")
    gap = over.index("ub_gap_holds(")
    copy = over.index("ub_copy_hit(")
    scan = over.index("for (size_t i = first;")
    if not (gap < copy < scan):
        raise SystemExit("a uniform copy is scanned before the gap, or kept in place of it")
    copied = block(source, "bool ub_copy_hit(")
    if "ub_list_epoch" not in copied or "stream_holds(" not in copied:
        raise SystemExit("a remembered uniform copy ignores a new update list or a reset stream")
    if "ub_updates" in copied:
        raise SystemExit("a remembered uniform copy scans the update list itself")
    if "ub_list_epoch" not in block(source, "void ub_copy_keep("):
        raise SystemExit("a uniform copy is kept after the update list has changed")
    print("PASS: a uniform word stays while the gap covers it")


if __name__ == "__main__":
    main()
