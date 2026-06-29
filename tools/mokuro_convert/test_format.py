#!/usr/bin/env python3
"""Verify the binary panel format by round-tripping sample data."""

import os
import struct
import sys
import tempfile

# Add parent dir to path so we can import the module
sys.path.insert(0, os.path.dirname(__file__))
from prepare_panels import (
    FORMAT_VERSION,
    IDX_HEADER,
    IDX_RECORD,
    PANEL_BOX,
    TEXT_BLOCK,
    cluster_into_panels,
    sort_panels_manga_order,
    write_binary,
)

SAMPLE_PAGES = [
    {
        "img_width": 1654,
        "img_height": 2339,
        "blocks": [
            {
                "box": [1200, 100, 1400, 500],
                "lines": ["おはよう", "ございます"],
                "vertical": True,
            },
            {
                "box": [1100, 120, 1190, 480],
                "lines": ["今日は"],
                "vertical": True,
            },
            {
                "box": [200, 100, 500, 600],
                "lines": ["左のパネル"],
                "vertical": True,
            },
            {
                "box": [300, 1200, 700, 1800],
                "lines": ["下のパネル", "テキスト"],
                "vertical": True,
            },
        ],
    },
    {
        "img_width": 1654,
        "img_height": 2339,
        "blocks": [],
    },
    {
        "img_width": 1654,
        "img_height": 2339,
        "blocks": [
            {
                "box": [800, 500, 1200, 1000],
                "lines": ["一つだけ"],
                "vertical": True,
            },
        ],
    },
]


def read_binary(output_dir: str) -> list[dict]:
    """Read back the binary format and decode it."""
    idx_path = os.path.join(output_dir, "panels.idx")
    dat_path = os.path.join(output_dir, "panels.dat")

    with open(idx_path, "rb") as f:
        header = f.read(struct.calcsize(IDX_HEADER))
        version, page_count = struct.unpack(IDX_HEADER, header)

        records = []
        rec_size = struct.calcsize(IDX_RECORD)
        for _ in range(page_count):
            rec = f.read(rec_size)
            offset, length, w, h = struct.unpack(IDX_RECORD, rec)
            records.append((offset, length, w, h))

    with open(dat_path, "rb") as f:
        dat = f.read()

    pages = []
    for offset, length, img_w, img_h in records:
        page_data = dat[offset : offset + length]
        pos = 0
        panel_count, _ = struct.unpack_from("BB", page_data, pos)
        pos += 2

        panels = []
        for _ in range(panel_count):
            px, py, pw, ph, text_count, _ = struct.unpack_from(
                PANEL_BOX, page_data, pos
            )
            pos += struct.calcsize(PANEL_BOX)

            text_blocks = []
            for _ in range(text_count):
                tx, ty, tw, th, text_len = struct.unpack_from(
                    TEXT_BLOCK, page_data, pos
                )
                pos += struct.calcsize(TEXT_BLOCK)
                text = page_data[pos : pos + text_len].decode("utf-8")
                pos += text_len
                text_blocks.append(
                    {"box": [tx, ty, tx + tw, ty + th], "text": text}
                )

            panels.append(
                {
                    "box": [px, py, px + pw, py + ph],
                    "text_blocks": text_blocks,
                }
            )

        pages.append(
            {
                "img_width": img_w,
                "img_height": img_h,
                "panels": panels,
            }
        )

    return version, pages


def test_format():
    with tempfile.TemporaryDirectory() as tmpdir:
        write_binary(SAMPLE_PAGES, tmpdir)

        version, pages = read_binary(tmpdir)

        assert version == FORMAT_VERSION, f"Version mismatch: {version} != {FORMAT_VERSION}"
        assert len(pages) == len(SAMPLE_PAGES), f"Page count: {len(pages)} != {len(SAMPLE_PAGES)}"

        # Page 0: 4 text blocks should cluster into panels
        p0 = pages[0]
        assert p0["img_width"] == 1654
        assert p0["img_height"] == 2339
        assert len(p0["panels"]) > 0, "Page 0 should have panels"

        # Verify text is preserved
        all_text = []
        for panel in p0["panels"]:
            for tb in panel["text_blocks"]:
                all_text.append(tb["text"])
        assert any("おはよう" in t for t in all_text), f"Japanese text not found: {all_text}"
        assert any("左のパネル" in t for t in all_text), f"Left panel text not found: {all_text}"

        # Page 1: no text blocks → single full-page panel
        p1 = pages[1]
        assert len(p1["panels"]) == 1, f"Empty page should have 1 fallback panel, got {len(p1['panels'])}"

        # Page 2: single text block → one panel
        p2 = pages[2]
        assert len(p2["panels"]) >= 1
        found = False
        for panel in p2["panels"]:
            for tb in panel["text_blocks"]:
                if "一つだけ" in tb["text"]:
                    found = True
        assert found, "Single block text not found"

        # Verify file sizes are reasonable
        idx_size = os.path.getsize(os.path.join(tmpdir, "panels.idx"))
        dat_size = os.path.getsize(os.path.join(tmpdir, "panels.dat"))
        expected_idx = struct.calcsize(IDX_HEADER) + len(SAMPLE_PAGES) * struct.calcsize(IDX_RECORD)
        assert idx_size == expected_idx, f"idx size: {idx_size} != expected {expected_idx}"
        assert dat_size > 0, "dat file should not be empty"

        print(f"All tests passed!")
        print(f"  Pages: {len(pages)}")
        print(f"  idx size: {idx_size} bytes (header {struct.calcsize(IDX_HEADER)} + {len(SAMPLE_PAGES)} * {struct.calcsize(IDX_RECORD)})")
        print(f"  dat size: {dat_size} bytes")
        for i, p in enumerate(pages):
            panel_count = len(p["panels"])
            text_count = sum(len(pnl["text_blocks"]) for pnl in p["panels"])
            print(f"  Page {i}: {p['img_width']}x{p['img_height']}, {panel_count} panels, {text_count} text blocks")


def test_clustering():
    """Test the panel clustering logic."""
    blocks = [
        {"box": [1200, 100, 1400, 500], "lines": ["A"], "vertical": True},
        {"box": [1100, 120, 1190, 480], "lines": ["B"], "vertical": True},
        {"box": [200, 100, 500, 600], "lines": ["C"], "vertical": True},
    ]

    panels = cluster_into_panels(blocks, 1654, 2339)
    # A and B are close → should merge into one panel
    # C is far away → separate panel
    assert len(panels) == 2, f"Expected 2 panels from clustering, got {len(panels)}"
    print("Clustering test passed!")

    # Test manga reading order (right-to-left)
    sorted_panels = sort_panels_manga_order(panels)
    # The right-side panel (A+B, center ~1200) should come before left panel (C, center ~350)
    first_cx = (sorted_panels[0]["box"][0] + sorted_panels[0]["box"][2]) / 2
    second_cx = (sorted_panels[1]["box"][0] + sorted_panels[1]["box"][2]) / 2
    assert first_cx > second_cx, f"Manga order wrong: first panel center {first_cx} should be > {second_cx}"
    print("Manga reading order test passed!")


def test_empty():
    """Test with no text blocks."""
    panels = cluster_into_panels([], 800, 1200)
    assert len(panels) == 1, "Empty blocks should produce one full-page panel"
    assert panels[0]["box"] == [0, 0, 800, 1200]
    print("Empty page test passed!")


if __name__ == "__main__":
    test_empty()
    test_clustering()
    test_format()
    print("\nAll tests passed!")
