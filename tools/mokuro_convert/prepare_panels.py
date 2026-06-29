#!/usr/bin/env python3
"""Convert Mokuro OCR output to binary panel data for CrossPoint Reader.

Takes existing Mokuro output (per-page JSON files from the _ocr/ directory,
or a v0.2 HTML file) and produces compact binary files the device reads for
panel-by-panel manga navigation with text overlay.

Since Mokuro output contains text blocks but not panel bounding boxes, this
tool clusters nearby text blocks into panels using spatial heuristics, then
sorts everything into manga reading order (right-to-left, top-to-bottom).

Output:
    panels.idx  -- 8-byte header + 12-byte fixed records (one per page)
    panels.dat  -- variable-length panel + text data per page

Usage:
    # From Mokuro _ocr/ directory (per-page JSON files)
    python3 prepare_panels.py --input ./manga_ocr/ --output-dir /path/to/sd/panels/

    # From Mokuro v0.2 HTML file
    python3 prepare_panels.py --input ./manga.html --output-dir /path/to/sd/panels/

    # Provide source images for full-page panel fallback
    python3 prepare_panels.py --input ./manga_ocr/ --images ./manga_images/ --output-dir ./out/

Binary format (panels.idx):
    Header:
        uint32  version         (currently 1)
        uint32  pageCount

    Per page (pageCount records, 12 bytes each):
        uint32  dataOffset      byte offset into panels.dat
        uint32  dataLength      byte length of this page's data
        uint16  imgWidth        source image width (pixels)
        uint16  imgHeight       source image height (pixels)

Binary format (panels.dat, per page at dataOffset):
    uint8   panelCount
    uint8   reserved

    Per panel (panelCount entries):
        uint16  x, y, w, h     panel bounding box (pixels)
        uint8   textCount       text blocks in this panel
        uint8   reserved

        Per text block (textCount entries):
            uint16  x, y, w, h text block bounding box (pixels)
            uint16  textLen     UTF-8 text length
            bytes   text[]      UTF-8 text (textLen bytes, not null-terminated)
"""

import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path

FORMAT_VERSION = 1

IDX_HEADER = "<II"  # version(4) + pageCount(4) = 8 bytes
IDX_RECORD = "<IIHI"  # dataOffset(4) + dataLength(4) + imgWidth(2) + imgHeight(2) = 12 bytes
# Note: imgWidth and imgHeight are uint16, but struct packing makes this <IIHI
# Actually let's fix this - we want uint32+uint32+uint16+uint16 = 12 bytes
IDX_RECORD = "<IIHH"  # dataOffset(4) + dataLength(4) + imgWidth(2) + imgHeight(2) = 12 bytes

PANEL_BOX = "<HHHHBB"  # x(2) + y(2) + w(2) + h(2) + textCount(1) + pad(1) = 10 bytes
TEXT_BLOCK = "<HHHHH"  # x(2) + y(2) + w(2) + h(2) + textLen(2) = 10 bytes


# ── Mokuro parsing ──────────────────────────────────────────────


def parse_ocr_json(path: str) -> dict:
    """Parse a single Mokuro _ocr/*.json file."""
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    return {
        "img_width": data.get("img_width", 0),
        "img_height": data.get("img_height", 0),
        "blocks": [
            {
                "box": b["box"],  # [x1, y1, x2, y2]
                "lines": b.get("lines", []),
                "vertical": b.get("vertical", True),
            }
            for b in data.get("blocks", [])
        ],
    }


def _parse_css_int(style: str, prop: str) -> int:
    """Extract an integer CSS property value from a style string."""
    m = re.search(rf'{prop}\s*:\s*(\d+)', style)
    return int(m.group(1)) if m else 0


def parse_mokuro_html(path: str) -> list[dict]:
    """Parse a Mokuro HTML file, extracting per-page data from CSS-positioned divs.

    Mokuro generates HTML with:
      - div.pageContainer with style="width:W; height:H; background-image:url(...)"
      - div.textBox children with style="left:X; top:Y; width:W; height:H; writing-mode:..."
        containing <p> elements with OCR text lines.
    """
    with open(path, "r", encoding="utf-8") as f:
        html = f.read()

    pages = []

    # Match each pageContainer div and its content (up to the next pageContainer or end)
    page_pattern = re.compile(
        r'<div[^>]*class="pageContainer"[^>]*style="([^"]*)"[^>]*>(.*?)(?=<div[^>]*class="pageContainer"|$)',
        re.DOTALL,
    )

    textbox_pattern = re.compile(
        r'<div[^>]*class="textBox"[^>]*style="([^"]*)"[^>]*>(.*?)</div>',
        re.DOTALL,
    )

    p_pattern = re.compile(r'<p>(.*?)</p>', re.DOTALL)

    for page_match in page_pattern.finditer(html):
        page_style = page_match.group(1)
        page_content = page_match.group(2)

        img_w = _parse_css_int(page_style, 'width')
        img_h = _parse_css_int(page_style, 'height')

        if img_w == 0 or img_h == 0:
            continue

        blocks = []
        for tb_match in textbox_pattern.finditer(page_content):
            tb_style = tb_match.group(1)
            tb_content = tb_match.group(2)

            left = _parse_css_int(tb_style, 'left')
            top = _parse_css_int(tb_style, 'top')
            width = _parse_css_int(tb_style, 'width')
            height = _parse_css_int(tb_style, 'height')
            vertical = 'vertical' in tb_style

            # Extract text lines from <p> elements
            lines = [
                re.sub(r'<[^>]+>', '', p.group(1)).strip()
                for p in p_pattern.finditer(tb_content)
            ]
            lines = [l for l in lines if l]

            blocks.append({
                "box": [left, top, left + width, top + height],
                "lines": lines,
                "vertical": vertical,
            })

        pages.append({
            "img_width": img_w,
            "img_height": img_h,
            "blocks": blocks,
        })

    return pages


def load_pages(input_path: str) -> list[dict]:
    """Load page data from Mokuro output (directory of JSONs or HTML file)."""
    p = Path(input_path)

    if p.is_file() and p.suffix.lower() in (".html", ".htm"):
        pages = parse_mokuro_html(str(p))
        if not pages:
            print(f"Error: no page data found in HTML file: {p}", file=sys.stderr)
            sys.exit(1)
        return pages

    if p.is_dir():
        json_files = sorted(p.glob("*.json"))
        if not json_files:
            print(f"Error: no JSON files found in: {p}", file=sys.stderr)
            sys.exit(1)
        return [parse_ocr_json(str(f)) for f in json_files]

    if p.is_file() and p.suffix.lower() == ".json":
        return [parse_ocr_json(str(p))]

    print(f"Error: unsupported input: {p}", file=sys.stderr)
    sys.exit(1)


# ── Panel detection (text-block clustering) ─────────────────────


def cluster_into_panels(
    blocks: list[dict], img_width: int, img_height: int, merge_margin: float = 0.05
) -> list[dict]:
    """Cluster text blocks into panels using spatial proximity.

    Expands each block's bounding box by merge_margin (fraction of page size),
    then merges overlapping boxes into panel groups. Each panel's bounding box
    is the union of its constituent text blocks with padding.

    If no text blocks exist, returns a single panel covering the full page.
    """
    if not blocks:
        return [
            {
                "box": [0, 0, img_width, img_height],
                "text_blocks": [],
            }
        ]

    margin_x = int(img_width * merge_margin)
    margin_y = int(img_height * merge_margin)

    # Build expanded boxes for overlap detection
    expanded = []
    for i, b in enumerate(blocks):
        x1, y1, x2, y2 = b["box"]
        expanded.append(
            (
                max(0, x1 - margin_x),
                max(0, y1 - margin_y),
                min(img_width, x2 + margin_x),
                min(img_height, y2 + margin_y),
                i,
            )
        )

    # Union-find for merging overlapping expanded boxes
    parent = list(range(len(blocks)))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for i in range(len(expanded)):
        for j in range(i + 1, len(expanded)):
            ax1, ay1, ax2, ay2, _ = expanded[i]
            bx1, by1, bx2, by2, _ = expanded[j]
            if ax1 <= bx2 and ax2 >= bx1 and ay1 <= by2 and ay2 >= by1:
                union(i, j)

    # Group blocks by cluster
    groups: dict[int, list[int]] = {}
    for i in range(len(blocks)):
        root = find(i)
        groups.setdefault(root, []).append(i)

    # Build panel for each group
    panels = []
    padding = int(min(img_width, img_height) * 0.02)

    for indices in groups.values():
        min_x = min(blocks[i]["box"][0] for i in indices)
        min_y = min(blocks[i]["box"][1] for i in indices)
        max_x = max(blocks[i]["box"][2] for i in indices)
        max_y = max(blocks[i]["box"][3] for i in indices)

        panel_box = [
            max(0, min_x - padding),
            max(0, min_y - padding),
            min(img_width, max_x + padding),
            min(img_height, max_y + padding),
        ]

        text_blocks = [blocks[i] for i in indices]
        # Sort text blocks within panel: right-to-left for vertical manga
        text_blocks.sort(key=lambda tb: (-tb["box"][0], tb["box"][1]))

        panels.append({"box": panel_box, "text_blocks": text_blocks})

    return panels


def sort_panels_manga_order(panels: list[dict]) -> list[dict]:
    """Sort panels in manga reading order: right-to-left, top-to-bottom.

    Uses a column-based approach: panels whose horizontal centers fall in the
    same column band are grouped together and sorted top-to-bottom within the
    column, then columns are ordered right-to-left.
    """
    if len(panels) <= 1:
        return panels

    # Compute horizontal centers
    centers = []
    for p in panels:
        cx = (p["box"][0] + p["box"][2]) / 2
        cy = (p["box"][1] + p["box"][3]) / 2
        centers.append((cx, cy))

    # Sort right-to-left first, then top-to-bottom within similar x positions
    # Use a tolerance band for "same column" (~15% of page width)
    max_x = max(c[0] for c in centers)
    min_x = min(c[0] for c in centers)
    page_width = max_x - min_x if max_x > min_x else 1
    col_tolerance = page_width * 0.15

    indexed = list(zip(range(len(panels)), centers))
    # Sort primarily by x descending (right to left), secondarily by y ascending
    indexed.sort(key=lambda ic: (-ic[1][0], ic[1][1]))

    # Group into columns
    columns: list[list[tuple]] = []
    for item in indexed:
        placed = False
        for col in columns:
            col_x = sum(c[1][0] for c in col) / len(col)
            if abs(item[1][0] - col_x) < col_tolerance:
                col.append(item)
                placed = True
                break
        if not placed:
            columns.append([item])

    # Sort columns right-to-left by average x
    columns.sort(key=lambda col: -sum(c[1][0] for c in col) / len(col))

    # Within each column, sort top-to-bottom
    result = []
    for col in columns:
        col.sort(key=lambda ic: ic[1][1])
        result.extend(panels[ic[0]] for ic in col)

    return result


# ── Binary output ───────────────────────────────────────────────


def encode_page(panels: list[dict]) -> bytes:
    """Encode one page's panel+text data to binary."""
    buf = bytearray()
    panel_count = min(len(panels), 255)
    buf += struct.pack("BB", panel_count, 0)

    for panel in panels[:panel_count]:
        x1, y1, x2, y2 = panel["box"]
        w = x2 - x1
        h = y2 - y1
        text_blocks = panel.get("text_blocks", [])
        text_count = min(len(text_blocks), 255)

        buf += struct.pack(PANEL_BOX, max(0, x1), max(0, y1), max(0, w), max(0, h), text_count, 0)

        for tb in text_blocks[:text_count]:
            tx1, ty1, tx2, ty2 = tb["box"]
            tw = tx2 - tx1
            th = ty2 - ty1
            text = "\n".join(tb.get("lines", []))
            text_bytes = text.encode("utf-8")
            if len(text_bytes) > 0xFFFF:
                text_bytes = text_bytes[:0xFFFF]

            buf += struct.pack(
                TEXT_BLOCK,
                max(0, tx1),
                max(0, ty1),
                max(0, tw),
                max(0, th),
                len(text_bytes),
            )
            buf += text_bytes

    return bytes(buf)


def write_binary(pages: list[dict], output_dir: str, target_height: int = 0):
    """Write panels.idx + panels.dat from processed page data.

    If target_height > 0, all coordinates and image dimensions are scaled
    to match resized BMPs (preserving aspect ratio from the original).
    """
    os.makedirs(output_dir, exist_ok=True)
    idx_path = os.path.join(output_dir, "panels.idx")
    dat_path = os.path.join(output_dir, "panels.dat")

    page_count = len(pages)
    idx_header_size = struct.calcsize(IDX_HEADER)
    idx_record_size = struct.calcsize(IDX_RECORD)

    dat_offset = 0
    idx_records = []
    dat_chunks = []

    for page in pages:
        orig_w = page.get("img_width", 0)
        orig_h = page.get("img_height", 0)

        # Compute scale factor if images were resized
        if target_height > 0 and orig_h > target_height:
            scale = target_height / orig_h
        else:
            scale = 1.0

        img_w = min(int(orig_w * scale), 0xFFFF)
        img_h = min(int(orig_h * scale), 0xFFFF)

        # Scale text block coordinates
        blocks = page.get("blocks", [])
        if scale != 1.0:
            scaled_blocks = []
            for b in blocks:
                box = b["box"]
                scaled_blocks.append({
                    "box": [int(box[0] * scale), int(box[1] * scale),
                            int(box[2] * scale), int(box[3] * scale)],
                    "lines": b.get("lines", []),
                    "vertical": b.get("vertical", True),
                })
            blocks = scaled_blocks

        panels = cluster_into_panels(blocks, img_w, img_h)
        panels = sort_panels_manga_order(panels)

        page_data = encode_page(panels)
        data_len = len(page_data)

        idx_records.append((dat_offset, data_len, img_w, img_h))
        dat_chunks.append(page_data)
        dat_offset += data_len

    with open(idx_path, "wb") as f:
        f.write(struct.pack(IDX_HEADER, FORMAT_VERSION, page_count))
        for offset, length, w, h in idx_records:
            f.write(struct.pack(IDX_RECORD, offset, length, w, h))

    with open(dat_path, "wb") as f:
        for chunk in dat_chunks:
            f.write(chunk)

    idx_size = os.path.getsize(idx_path)
    dat_size = os.path.getsize(dat_path)
    total_panels = sum(
        len(cluster_into_panels(p.get("blocks", []), p.get("img_width", 0), p.get("img_height", 0)))
        for p in pages
    )
    total_text_blocks = sum(len(p.get("blocks", [])) for p in pages)

    print(f"Output:")
    print(f"  {idx_path}: {idx_size:,} bytes ({page_count} pages)")
    print(f"  {dat_path}: {dat_size:,} bytes")
    print(f"  Total: {(idx_size + dat_size) / 1024:.1f} KB")
    print(f"  Panels: {total_panels} ({total_panels / max(page_count, 1):.1f}/page avg)")
    print(f"  Text blocks: {total_text_blocks}")


# ── CLI ─────────────────────────────────────────────────────────


IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".tiff", ".tif"}


def convert_images_to_bmp(images_dir: str, output_dir: str):
    """Convert manga page images (JPG/PNG) to 1-bit BMP for the device.

    The CrossPoint manga reader expects BMP files. This converts source images
    to 1-bit (monochrome) BMPs with Floyd-Steinberg dithering, sorted by
    filename to match the panel index page order.
    """
    try:
        from PIL import Image
    except ImportError:
        print("Error: Pillow not installed. Run: pip install Pillow", file=sys.stderr)
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    files = sorted(
        f for f in os.listdir(images_dir)
        if os.path.splitext(f)[1].lower() in IMAGE_EXTS
    )

    if not files:
        print(f"Warning: no images found in {images_dir}")
        return

    print(f"Converting {len(files)} images to 1-bit BMP...")
    for i, fname in enumerate(files):
        src = os.path.join(images_dir, fname)
        base = os.path.splitext(fname)[0]
        dst = os.path.join(output_dir, base + ".bmp")

        if os.path.exists(dst):
            continue

        img = Image.open(src).convert("L")  # grayscale
        # Scale to device screen size (high-quality Lanczos in grayscale)
        TARGET_H = 800
        if img.height > TARGET_H:
            ratio = TARGET_H / img.height
            target_w = int(img.width * ratio)
            img = img.resize((target_w, TARGET_H), Image.LANCZOS)
        from PIL import ImageEnhance
        img = ImageEnhance.Contrast(img).enhance(1.2)
        bw.save(dst)

        if (i + 1) % 50 == 0 or i + 1 == len(files):
            print(f"  {i + 1}/{len(files)} converted")

    print(f"  Images saved to {output_dir}")


def crop_panels(pages: list[dict], images_dir: str, output_dir: str):
    """Crop individual panels from source images and save as separate files.

    Each panel is saved as p<page>_<panel>.jpg in the output directory.
    The manga reader loads these directly for panel zoom, avoiding
    complex cropping at render time.
    """
    try:
        from PIL import Image
    except ImportError:
        print("Error: Pillow not installed. Run: pip install Pillow", file=sys.stderr)
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    # Get sorted image files from the source directory
    image_files = sorted(
        f for f in os.listdir(images_dir)
        if os.path.splitext(f)[1].lower() in IMAGE_EXTS
    )

    if len(image_files) != len(pages):
        print(f"Warning: {len(image_files)} images but {len(pages)} pages in panel data")

    total_panels = 0
    for page_idx, page in enumerate(pages):
        if page_idx >= len(image_files):
            break

        src_path = os.path.join(images_dir, image_files[page_idx])
        img_w = page.get("img_width", 0)
        img_h = page.get("img_height", 0)
        if img_w == 0 or img_h == 0:
            continue

        blocks = page.get("blocks", [])
        panels = cluster_into_panels(blocks, img_w, img_h)
        panels = sort_panels_manga_order(panels)

        if len(panels) <= 1:
            continue

        img = Image.open(src_path)

        for panel_idx, panel in enumerate(panels):
            x1, y1, x2, y2 = panel["box"]
            # Add small margin around the panel
            margin = 10
            x1 = max(0, x1 - margin)
            y1 = max(0, y1 - margin)
            x2 = min(img.width, x2 + margin)
            y2 = min(img.height, y2 + margin)

            cropped = img.crop((x1, y1, x2, y2))
            panel_path = os.path.join(output_dir, f"p{page_idx}_{panel_idx}.jpg")
            cropped.save(panel_path, "JPEG", quality=90)
            total_panels += 1

        img.close()

        if (page_idx + 1) % 50 == 0:
            print(f"  {page_idx + 1}/{len(pages)} pages cropped")

    print(f"Cropped {total_panels} panel images")


def main():
    parser = argparse.ArgumentParser(
        description="Convert Mokuro OCR output to binary panel data for CrossPoint Reader."
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Mokuro output: directory of per-page JSON files, or a v0.2+ HTML file",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory to write panels.idx and panels.dat",
    )
    parser.add_argument(
        "--merge-margin",
        type=float,
        default=0.05,
        help="Fraction of page size used as margin when clustering text blocks into panels (default: 0.05)",
    )

    parser.add_argument(
        "--images",
        help="Directory containing source manga images (JPG/PNG). "
             "Images are converted to 1-bit BMP and copied to --output-dir "
             "so the device can render them.",
    )

    args = parser.parse_args()

    print(f"Loading Mokuro data from: {args.input}")
    pages = load_pages(args.input)
    print(f"Loaded {len(pages)} pages")

    if not pages:
        print("Error: no pages found", file=sys.stderr)
        sys.exit(1)

    write_binary(pages, args.output_dir)

    if args.images:
        crop_panels(pages, args.images, args.output_dir)

    print("Done.")


if __name__ == "__main__":
    main()
