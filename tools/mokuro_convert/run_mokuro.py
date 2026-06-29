#!/usr/bin/env python3
"""Run Mokuro OCR on manga images and produce binary panel data for CrossPoint Reader.

Full pipeline: extracts images from a manga source (CBZ/ZIP, image folder, or PDF),
runs Mokuro for OCR, detects panels via comic-text-detector, and outputs compact
binary files the device reads for panel-by-panel manga navigation.

If you already have Mokuro output, use prepare_panels.py instead (no ML deps needed).

Requirements:
    pip install mokuro manga-ocr comic-text-detector Pillow

Usage:
    # From a CBZ/ZIP archive
    python3 run_mokuro.py --input manga.cbz --output-dir /path/to/sd/panels/

    # From an image folder
    python3 run_mokuro.py --input ./manga_pages/ --output-dir /path/to/sd/panels/

    # Skip OCR, only detect panels from images (no text overlay)
    python3 run_mokuro.py --input ./manga_pages/ --output-dir ./out/ --panels-only

    # Also save Mokuro JSON for later reuse with prepare_panels.py
    python3 run_mokuro.py --input manga.cbz --output-dir ./out/ --save-mokuro ./mokuro_out/

Output:
    panels.idx  -- 8-byte header + 12-byte fixed records (one per page)
    panels.dat  -- variable-length panel + text data per page

See prepare_panels.py docstring for the binary format specification.
"""

import argparse
import json
import os
import shutil
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

FORMAT_VERSION = 1

IDX_HEADER = "<II"
IDX_RECORD = "<IIHH"
PANEL_BOX = "<HHHHBB"
TEXT_BLOCK = "<HHHHH"

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tiff", ".tif"}


def is_image(path: str) -> bool:
    return Path(path).suffix.lower() in IMAGE_EXTS


# ── Image extraction ────────────────────────────────────────────


def extract_images(input_path: str, work_dir: str) -> list[str]:
    """Extract or locate manga page images, return sorted paths."""
    p = Path(input_path)

    if p.is_dir():
        images = sorted(
            str(f) for f in p.iterdir() if f.is_file() and is_image(str(f))
        )
        if not images:
            print(f"Error: no image files found in {p}", file=sys.stderr)
            sys.exit(1)
        return images

    if p.suffix.lower() in (".cbz", ".zip"):
        extract_dir = os.path.join(work_dir, "images")
        os.makedirs(extract_dir, exist_ok=True)
        with zipfile.ZipFile(str(p), "r") as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                if is_image(info.filename):
                    # Flatten directory structure
                    basename = os.path.basename(info.filename)
                    target = os.path.join(extract_dir, basename)
                    with zf.open(info) as src, open(target, "wb") as dst:
                        shutil.copyfileobj(src, dst)
        images = sorted(
            str(f) for f in Path(extract_dir).iterdir() if is_image(str(f))
        )
        if not images:
            print(f"Error: no image files found in archive {p}", file=sys.stderr)
            sys.exit(1)
        return images

    print(f"Error: unsupported input format: {p}", file=sys.stderr)
    sys.exit(1)


# ── OCR via Mokuro ──────────────────────────────────────────────


def run_ocr(image_paths: list[str], save_mokuro_dir: str | None = None) -> list[dict]:
    """Run manga-ocr on each page image, return per-page text block data."""
    try:
        from manga_ocr import MangaOcr
    except ImportError:
        print(
            "Error: manga-ocr not installed. Run: pip install manga-ocr",
            file=sys.stderr,
        )
        sys.exit(1)

    try:
        from PIL import Image
    except ImportError:
        print(
            "Error: Pillow not installed. Run: pip install Pillow",
            file=sys.stderr,
        )
        sys.exit(1)

    print("Loading manga-ocr model...")
    mocr = MangaOcr()

    pages = []
    for i, img_path in enumerate(image_paths):
        print(f"  OCR page {i + 1}/{len(image_paths)}: {os.path.basename(img_path)}")
        img = Image.open(img_path)
        w, h = img.size

        # manga-ocr processes the full image; for per-block OCR we need
        # text detection first. If comic-text-detector is available, we
        # use its bubble boxes. Otherwise fall back to full-page OCR.
        text_blocks = detect_text_blocks(img_path, img, mocr)

        page_data = {
            "img_width": w,
            "img_height": h,
            "blocks": text_blocks,
        }
        pages.append(page_data)

        if save_mokuro_dir:
            os.makedirs(save_mokuro_dir, exist_ok=True)
            json_path = os.path.join(
                save_mokuro_dir, f"{Path(img_path).stem}.json"
            )
            with open(json_path, "w", encoding="utf-8") as f:
                json.dump(page_data, f, ensure_ascii=False, indent=2)

    return pages


def detect_text_blocks(img_path: str, img, mocr) -> list[dict]:
    """Detect text regions and OCR each one."""
    try:
        from comic_text_detector.inference import TextDetector
    except ImportError:
        # Fallback: OCR the entire page as one block
        text = mocr(img)
        w, h = img.size
        if text.strip():
            return [
                {
                    "box": [0, 0, w, h],
                    "lines": text.strip().split("\n"),
                    "vertical": True,
                }
            ]
        return []

    # Use comic-text-detector for text bubble detection
    try:
        detector = getattr(detect_text_blocks, "_detector", None)
        if detector is None:
            print("  Loading text detector model...")
            detector = TextDetector()
            detect_text_blocks._detector = detector

        result = detector(img_path)
        blocks = []
        for bbox in result.get("blk_list", []):
            x1, y1, x2, y2 = (
                int(bbox["xyxy"][0]),
                int(bbox["xyxy"][1]),
                int(bbox["xyxy"][2]),
                int(bbox["xyxy"][3]),
            )
            # Crop and OCR this region
            crop = img.crop((x1, y1, x2, y2))
            text = mocr(crop)
            if text.strip():
                blocks.append(
                    {
                        "box": [x1, y1, x2, y2],
                        "lines": text.strip().split("\n"),
                        "vertical": bbox.get("vertical", True),
                    }
                )
        return blocks
    except Exception as e:
        print(f"  Warning: text detection failed ({e}), falling back to full-page OCR")
        text = mocr(img)
        w, h = img.size
        if text.strip():
            return [
                {
                    "box": [0, 0, w, h],
                    "lines": text.strip().split("\n"),
                    "vertical": True,
                }
            ]
        return []


# ── Panel detection ─────────────────────────────────────────────


def detect_panels_from_images(image_paths: list[str]) -> list[list[list[int]]]:
    """Detect panel bounding boxes from source images.

    Uses comic-text-detector if available, otherwise falls back to simple
    image-based detection (finding rectangular regions via contour analysis).
    Returns a list of panel box lists, one per page.
    """
    try:
        return _detect_panels_ctd(image_paths)
    except ImportError:
        return _detect_panels_contour(image_paths)


def _detect_panels_ctd(image_paths: list[str]) -> list[list[list[int]]]:
    """Panel detection via comic-text-detector."""
    from comic_text_detector.inference import TextDetector

    detector = TextDetector()
    all_panels = []

    for i, img_path in enumerate(image_paths):
        print(
            f"  Panel detection {i + 1}/{len(image_paths)}: {os.path.basename(img_path)}"
        )
        result = detector(img_path)
        panels = []
        for panel in result.get("panel_list", []):
            x1, y1, x2, y2 = (
                int(panel["xyxy"][0]),
                int(panel["xyxy"][1]),
                int(panel["xyxy"][2]),
                int(panel["xyxy"][3]),
            )
            panels.append([x1, y1, x2, y2])
        all_panels.append(panels)

    return all_panels


def _detect_panels_contour(image_paths: list[str]) -> list[list[list[int]]]:
    """Fallback panel detection using basic image analysis.

    Finds large rectangular contours that likely represent panel borders.
    Requires Pillow only (no ML).
    """
    try:
        from PIL import Image, ImageFilter
    except ImportError:
        print(
            "Error: Pillow not installed. Run: pip install Pillow",
            file=sys.stderr,
        )
        sys.exit(1)

    all_panels = []

    for i, img_path in enumerate(image_paths):
        print(
            f"  Panel detection (contour) {i + 1}/{len(image_paths)}: {os.path.basename(img_path)}"
        )
        img = Image.open(img_path).convert("L")
        w, h = img.size

        # Simple approach: divide page into a grid based on white gutters
        # Convert to binary (white = gutter, dark = content)
        threshold = 230
        pixels = img.load()

        # Scan for horizontal gutters (rows that are mostly white)
        h_splits = [0]
        min_gutter = h // 50  # minimum gutter height
        in_gutter = False
        gutter_start = 0

        for y in range(h):
            white_count = sum(1 for x in range(w) if pixels[x, y] > threshold)
            is_white_row = white_count > w * 0.85

            if is_white_row and not in_gutter:
                in_gutter = True
                gutter_start = y
            elif not is_white_row and in_gutter:
                if y - gutter_start >= min_gutter:
                    h_splits.append((gutter_start + y) // 2)
                in_gutter = False

        h_splits.append(h)

        # For each horizontal band, scan for vertical gutters
        panels = []
        for band_idx in range(len(h_splits) - 1):
            y1 = h_splits[band_idx]
            y2 = h_splits[band_idx + 1]
            if y2 - y1 < h // 10:
                continue

            v_splits = [0]
            in_gutter = False

            for x in range(w):
                white_count = sum(
                    1 for y in range(y1, y2) if pixels[x, y] > threshold
                )
                band_h = y2 - y1
                is_white_col = white_count > band_h * 0.85

                if is_white_col and not in_gutter:
                    in_gutter = True
                    gutter_start = x
                elif not is_white_col and in_gutter:
                    if x - gutter_start >= min_gutter:
                        v_splits.append((gutter_start + x) // 2)
                    in_gutter = False

            v_splits.append(w)

            for col_idx in range(len(v_splits) - 1):
                x1 = v_splits[col_idx]
                x2 = v_splits[col_idx + 1]
                if x2 - x1 < w // 10:
                    continue
                panels.append([x1, y1, x2, y2])

        if not panels:
            panels.append([0, 0, w, h])

        all_panels.append(panels)

    return all_panels


# ── Manga reading order sort ───────────────────────────────────


def sort_panels_manga_order(panels: list[list[int]], img_width: int) -> list[list[int]]:
    """Sort panel boxes in manga reading order (right-to-left, top-to-bottom)."""
    if len(panels) <= 1:
        return panels

    centers = [((b[0] + b[2]) / 2, (b[1] + b[3]) / 2) for b in panels]
    max_cx = max(c[0] for c in centers)
    min_cx = min(c[0] for c in centers)
    span = max_cx - min_cx if max_cx > min_cx else 1
    col_tol = span * 0.15

    indexed = list(zip(range(len(panels)), centers))
    indexed.sort(key=lambda ic: (-ic[1][0], ic[1][1]))

    columns: list[list] = []
    for item in indexed:
        placed = False
        for col in columns:
            col_x = sum(c[1][0] for c in col) / len(col)
            if abs(item[1][0] - col_x) < col_tol:
                col.append(item)
                placed = True
                break
        if not placed:
            columns.append([item])

    columns.sort(key=lambda col: -sum(c[1][0] for c in col) / len(col))
    result = []
    for col in columns:
        col.sort(key=lambda ic: ic[1][1])
        result.extend(panels[ic[0]] for ic in col)

    return result


# ── Merge OCR text into detected panels ─────────────────────────


def assign_text_to_panels(
    panels: list[list[int]], text_blocks: list[dict]
) -> list[dict]:
    """Assign OCR text blocks to their containing panels.

    Each text block is assigned to the panel whose box contains its center.
    Unassigned text blocks go to the nearest panel.
    """
    result = [{"box": p, "text_blocks": []} for p in panels]

    for tb in text_blocks:
        tx1, ty1, tx2, ty2 = tb["box"]
        tcx = (tx1 + tx2) / 2
        tcy = (ty1 + ty2) / 2

        best_idx = -1
        best_dist = float("inf")

        for i, panel in enumerate(panels):
            px1, py1, px2, py2 = panel
            if px1 <= tcx <= px2 and py1 <= tcy <= py2:
                best_idx = i
                break
            # Distance to panel center
            pcx = (px1 + px2) / 2
            pcy = (py1 + py2) / 2
            dist = (tcx - pcx) ** 2 + (tcy - pcy) ** 2
            if dist < best_dist:
                best_dist = dist
                best_idx = i

        if best_idx >= 0:
            result[best_idx]["text_blocks"].append(tb)

    # Sort text blocks within each panel (right-to-left for vertical manga)
    for panel in result:
        panel["text_blocks"].sort(key=lambda tb: (-tb["box"][0], tb["box"][1]))

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

        buf += struct.pack(
            PANEL_BOX, max(0, x1), max(0, y1), max(0, w), max(0, h), text_count, 0
        )

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


def write_binary(
    image_paths: list[str],
    pages_ocr: list[dict] | None,
    panels_per_page: list[list[list[int]]] | None,
    output_dir: str,
):
    """Write panels.idx + panels.dat."""
    try:
        from PIL import Image
    except ImportError:
        print("Error: Pillow not installed. Run: pip install Pillow", file=sys.stderr)
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)
    idx_path = os.path.join(output_dir, "panels.idx")
    dat_path = os.path.join(output_dir, "panels.dat")

    page_count = len(image_paths)
    dat_offset = 0
    idx_records = []
    dat_chunks = []

    for i, img_path in enumerate(image_paths):
        img = Image.open(img_path)
        img_w, img_h = img.size
        img.close()

        # Get panel boxes
        if panels_per_page and i < len(panels_per_page):
            panel_boxes = panels_per_page[i]
        else:
            panel_boxes = [[0, 0, img_w, img_h]]

        panel_boxes = sort_panels_manga_order(panel_boxes, img_w)

        # Get text blocks and assign to panels
        if pages_ocr and i < len(pages_ocr):
            text_blocks = pages_ocr[i].get("blocks", [])
            panels = assign_text_to_panels(panel_boxes, text_blocks)
        else:
            panels = [{"box": p, "text_blocks": []} for p in panel_boxes]

        page_data = encode_page(panels)
        data_len = len(page_data)

        idx_records.append(
            (dat_offset, data_len, min(img_w, 0xFFFF), min(img_h, 0xFFFF))
        )
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
        len(p) if panels_per_page and i < len(panels_per_page) else 1
        for i, p in enumerate(panels_per_page or [[]])
    )
    total_text = (
        sum(len(p.get("blocks", [])) for p in pages_ocr) if pages_ocr else 0
    )

    print(f"\nOutput:")
    print(f"  {idx_path}: {idx_size:,} bytes ({page_count} pages)")
    print(f"  {dat_path}: {dat_size:,} bytes")
    print(f"  Total: {(idx_size + dat_size) / 1024:.1f} KB")
    print(f"  Panels: ~{total_panels}")
    print(f"  Text blocks: {total_text}")


def crop_panel_images(
    image_paths: list[str],
    pages_ocr: list[dict] | None,
    panels_per_page: list[list[list[int]]] | None,
    output_dir: str,
):
    """Crop individual panels from source images and save as separate JPGs.

    Each panel is saved as p<page>_<panel>.jpg. The manga reader loads
    these directly for panel zoom view.
    """
    from PIL import Image

    os.makedirs(output_dir, exist_ok=True)
    total = 0
    for i, img_path in enumerate(image_paths):
        img = Image.open(img_path)

        if panels_per_page and i < len(panels_per_page):
            panel_boxes = panels_per_page[i]
        else:
            continue

        panel_boxes = sort_panels_manga_order(panel_boxes, img.width)

        if len(panel_boxes) <= 1:
            img.close()
            continue

        margin = 10
        for pi, box in enumerate(panel_boxes):
            x1 = max(0, box[0] - margin)
            y1 = max(0, box[1] - margin)
            x2 = min(img.width, box[2] + margin)
            y2 = min(img.height, box[3] + margin)
            cropped = img.crop((x1, y1, x2, y2))
            panel_path = os.path.join(output_dir, f"p{i}_{pi}.jpg")
            cropped.save(panel_path, "JPEG", quality=90)
            total += 1

        img.close()
        if (i + 1) % 50 == 0:
            print(f"  {i + 1}/{len(image_paths)} pages cropped")

    print(f"  Cropped {total} panel images")


# ── CLI ─────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description="Run Mokuro OCR + panel detection on manga, output binary panel data."
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Manga source: CBZ/ZIP archive or directory of images",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory to write panels.idx and panels.dat",
    )
    parser.add_argument(
        "--panels-only",
        action="store_true",
        help="Skip OCR, only detect panels (no text overlay data)",
    )
    parser.add_argument(
        "--save-mokuro",
        metavar="DIR",
        help="Also save Mokuro JSON files to this directory for later reuse",
    )

    args = parser.parse_args()

    work_dir = tempfile.mkdtemp(prefix="mokuro_")
    try:
        print(f"Extracting images from: {args.input}")
        image_paths = extract_images(args.input, work_dir)
        print(f"Found {len(image_paths)} pages")

        pages_ocr = None
        if not args.panels_only:
            print("\nRunning OCR...")
            pages_ocr = run_ocr(image_paths, args.save_mokuro)

        print("\nDetecting panels...")
        panels_per_page = detect_panels_from_images(image_paths)

        print("\nWriting binary output...")
        write_binary(image_paths, pages_ocr, panels_per_page, args.output_dir)

        print("\nCropping panel images...")
        crop_panel_images(image_paths, pages_ocr, panels_per_page, args.output_dir)
        print("Done.")
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
