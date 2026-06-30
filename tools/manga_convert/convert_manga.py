#!/usr/bin/env python3
"""Convert manga (image folder / CBZ / EPUB) into CrossPoint Reader format.

Replaces the Mokuro-based pipeline (tools/mokuro_convert/). Mokuro infers
panel boundaries by clustering OCR text-box positions, which only
approximates real panels and produces nothing for panels without text. This
tool instead:

  1. Detects actual panel RECTANGLES geometrically (white-gutter grid
     detection -- no ML model required).
  2. Crops each panel and sends it to Gemini (gemini-2.5-flash) asking what
     text/dialogue appears in it, as JSON.
  3. Writes the same panels.idx/panels.dat binary format the device already
     reads, plus page images renamed to a canonical page_NNNN.<ext> sequence
     so the device's natural-sort-based page scan can never misorder pages
     (no dependency on a distributor's arbitrary source filenames).

Page images are copied as-is (JPG/PNG) -- the device renders them directly,
no BMP conversion needed.

Usage:
    export GEMINI_API_KEY=$(cat ~/path/to/gemini.key)
    python3 convert_manga.py --input ./manga_pages/ --output-dir /path/to/sd/manga/Book/

    # Or pass the key file directly (key is read at runtime, never embedded):
    python3 convert_manga.py --input ./manga_pages/ --output-dir ./out/ \\
        --gemini-key-file ./gemini.key

    # Explicit page order, for sources whose filenames don't sort correctly
    # (one source filename per line, in the order pages should be read):
    python3 convert_manga.py --input ./manga_pages/ --output-dir ./out/ \\
        --page-order-file ./order.txt

    # Skip the Gemini OCR pass entirely (panels only, no text/lookup data):
    python3 convert_manga.py --input ./manga_pages/ --output-dir ./out/ --no-ocr

Output (in --output-dir):
    page_0000.jpg, page_0001.jpg, ...   canonical, trivially-sortable page
                                         images (device scans these directly)
    p<page>_<panel>.jpg                 cropped panel images for panel-zoom
    panels.idx / panels.dat             same binary format as before:

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
        uint16  x, y, w, h        panel bounding box (pixels)
        uint8   textCount         text blocks in this panel
        uint8   reserved
        uint16  translationLen    UTF-8 length of the panel's English translation
        bytes   translation[]     UTF-8 translation (translationLen bytes), empty if none
        Per text block (textCount entries):
            uint16  x, y, w, h    text block bounding box (pixels)
            uint16  textLen       UTF-8 text length
            bytes   text[]        UTF-8 text (textLen bytes, not null-terminated)
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

FORMAT_VERSION = 2  # v2 adds a per-panel translation string after the panel header

IDX_HEADER = "<II"  # version(4) + pageCount(4) = 8 bytes
IDX_RECORD = "<IIHH"  # dataOffset(4) + dataLength(4) + imgWidth(2) + imgHeight(2) = 12 bytes
PANEL_BOX = "<HHHHBBH"  # x(2)+y(2)+w(2)+h(2)+textCount(1)+pad(1)+translationLen(2) = 12 bytes
TEXT_BLOCK = "<HHHHH"  # x(2) + y(2) + w(2) + h(2) + textLen(2) = 10 bytes

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}

GEMINI_MODEL = "gemini-2.5-flash"
GEMINI_URL = f"https://generativelanguage.googleapis.com/v1beta/models/{GEMINI_MODEL}:generateContent"

PANEL_OCR_PROMPT = """This image is a single panel cropped from a Japanese manga page.
List every piece of text/dialogue visible in this panel, in the order a
reader would read them (top-to-bottom, right-to-left for manga). Then give
a single natural English translation of all of it combined, in the same
reading order, as it would read in an English localization of this manga.

Return ONLY a JSON object, no other text:
{"blocks": [{"text": "<the Japanese text, line breaks as \\n>",
             "bbox_2d": [ymin, xmin, ymax, xmax]}, ...],
 "translation": "<natural English translation of all the panel's text combined, in reading order>"}

bbox_2d is each text region's bounding box normalized to a 0-1000 scale
(0,0 = top-left of the panel image, 1000,1000 = bottom-right). If you
cannot determine a precise box, omit bbox_2d for that entry.
If there is no text in the panel, return {"blocks": [], "translation": ""}."""


# ── Page collection / ordering ───────────────────────────────────


def is_image(path: str) -> bool:
    return Path(path).suffix.lower() in IMAGE_EXTS


def _natural_sort_key(path: str):
    """Natural sort key matching FsHelpers::sortFileList() on the device.

    Case-insensitive; numeric substrings compared by integer value. Used
    only as the DEFAULT ordering for plain image folders/CBZ -- pass
    --page-order-file to override when a distributor's filenames don't
    sort into true reading order (e.g. cover/colophon/insert pages tagged
    with an unrelated product-code prefix).
    """
    name = os.path.basename(path)
    parts: list[tuple] = []
    i = 0
    while i < len(name):
        if name[i].isdigit():
            j = i
            while j < len(name) and name[j].isdigit():
                j += 1
            num_str = name[i:j].lstrip("0") or ""
            parts.append((0, len(num_str), num_str))
            i = j
        else:
            parts.append((1, 0, name[i].lower()))
            i += 1
    return parts


def collect_pages(input_path: str, work_dir: str, page_order_file: str | None) -> list[str]:
    """Return an ordered list of source page image paths."""
    p = Path(input_path)

    if p.is_dir():
        images = [str(f) for f in p.iterdir() if f.is_file() and is_image(str(f))]
    elif p.suffix.lower() in (".cbz", ".zip"):
        extract_dir = os.path.join(work_dir, "extracted")
        os.makedirs(extract_dir, exist_ok=True)
        with zipfile.ZipFile(str(p), "r") as zf:
            for info in zf.infolist():
                if info.is_dir() or not is_image(info.filename):
                    continue
                target = os.path.join(extract_dir, os.path.basename(info.filename))
                with zf.open(info) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
        images = [str(f) for f in Path(extract_dir).iterdir() if is_image(str(f))]
    elif p.suffix.lower() == ".epub":
        images = _extract_epub_pages(str(p), work_dir)
    else:
        print(f"Error: unsupported input: {p}", file=sys.stderr)
        sys.exit(1)

    if not images:
        print(f"Error: no image files found in {p}", file=sys.stderr)
        sys.exit(1)

    if page_order_file:
        with open(page_order_file, "r", encoding="utf-8") as f:
            order_names = [line.strip() for line in f if line.strip()]
        by_name = {os.path.basename(img): img for img in images}
        ordered = []
        for name in order_names:
            if name not in by_name:
                print(f"Warning: {name} from --page-order-file not found among extracted images", file=sys.stderr)
                continue
            ordered.append(by_name[name])
        missing = [img for img in images if os.path.basename(img) not in order_names]
        if missing:
            print(f"Warning: {len(missing)} images not listed in --page-order-file are dropped", file=sys.stderr)
        return ordered

    images.sort(key=_natural_sort_key)
    return images


def _extract_epub_pages(epub_path: str, work_dir: str) -> list[str]:
    """Extract page images from an EPUB in true spine reading order."""
    extract_dir = os.path.join(work_dir, "epub_extracted")
    os.makedirs(extract_dir, exist_ok=True)

    with zipfile.ZipFile(epub_path, "r") as zf:
        container = zf.read("META-INF/container.xml").decode("utf-8")
        m = re.search(r'full-path="([^"]+)"', container)
        if not m:
            print("Error: could not find OPF in EPUB container.xml", file=sys.stderr)
            sys.exit(1)
        opf_path = m.group(1)
        opf_dir = os.path.dirname(opf_path)
        opf = zf.read(opf_path).decode("utf-8")

        manifest = dict(re.findall(r'<item[^>]*id="([^"]+)"[^>]*href="([^"]+)"', opf))
        # href may appear before id -- also try the reverse attribute order
        manifest.update(dict((b, a) for a, b in re.findall(r'<item[^>]*href="([^"]+)"[^>]*id="([^"]+)"', opf)))
        spine_ids = re.findall(r'<itemref[^>]*idref="([^"]+)"', opf)

        images = []
        for idx, item_id in enumerate(spine_ids):
            href = manifest.get(item_id)
            if not href:
                continue
            full_href = os.path.normpath(os.path.join(opf_dir, href)) if opf_dir else href
            full_href = full_href.replace(os.sep, "/")
            if is_image(full_href):
                src_in_zip = full_href
            else:
                # Spine item is an XHTML wrapper page -- find the embedded image.
                try:
                    xhtml = zf.read(full_href).decode("utf-8", "ignore")
                except KeyError:
                    continue
                img_m = re.search(r'(?:src|xlink:href)="([^"]+)"', xhtml)
                if not img_m:
                    continue
                img_href = img_m.group(1)
                xhtml_dir = os.path.dirname(full_href)
                src_in_zip = os.path.normpath(os.path.join(xhtml_dir, img_href)).replace(os.sep, "/")

            try:
                data = zf.read(src_in_zip)
            except KeyError:
                print(f"Warning: image not found in EPUB: {src_in_zip}", file=sys.stderr)
                continue
            target = os.path.join(extract_dir, f"spine_{idx:04d}_{os.path.basename(src_in_zip)}")
            with open(target, "wb") as f:
                f.write(data)
            images.append(target)

    return images


# ── Panel detection ────────────────────────────────────────────
#
# Primary: a YOLO26-nano model fine-tuned on Manga109-s for panel/text
# detection (leoxs22/manga-panel-detector-yolo26n, mAP50 0.985 for panels).
# Falls back to a pure-Pillow white-gutter grid heuristic if `ultralytics`
# isn't installed, so this tool still runs with zero extra dependencies
# when ML deps aren't available -- just with lower detection quality.

_YOLO_MODEL = None
_YOLO_REPO = "leoxs22/manga-panel-detector-yolo26n"
_YOLO_FILENAME = "manga_panel_detector_fp32.pt"


def _load_yolo_model():
    global _YOLO_MODEL
    if _YOLO_MODEL is not None:
        return _YOLO_MODEL
    try:
        from huggingface_hub import hf_hub_download
        from ultralytics import YOLO
    except ImportError:
        return None
    try:
        weights_path = hf_hub_download(repo_id=_YOLO_REPO, filename=_YOLO_FILENAME)
        _YOLO_MODEL = YOLO(weights_path)
    except Exception as e:
        print(f"Warning: could not load YOLO panel detector ({e}); falling back to grid heuristic", file=sys.stderr)
        _YOLO_MODEL = False
    return _YOLO_MODEL if _YOLO_MODEL else None


def _box_area(b: list[int]) -> int:
    return max(0, b[2] - b[0]) * max(0, b[3] - b[1])


def _overlap_area(a: list[int], b: list[int]) -> int:
    ox1, oy1 = max(a[0], b[0]), max(a[1], b[1])
    ox2, oy2 = min(a[2], b[2]), min(a[3], b[3])
    return max(0, ox2 - ox1) * max(0, oy2 - oy1)


def _union_box(a: list[int], b: list[int]) -> list[int]:
    return [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]


def _dedupe_boxes(boxes_with_conf: list[tuple], overlap_thresh: float = 0.6) -> list[list[int]]:
    """Collapse boxes that substantially overlap into their union.

    The detector sometimes emits two overlapping boxes for the same panel
    region at different confidences/scales. Simply dropping the
    lower-confidence one can lose real page coverage when that box was
    actually larger and extended into area the kept box didn't cover (e.g.
    a small high-confidence box fully inside a larger, lower-confidence
    one that also reaches further) -- merging into the bounding union
    keeps all detected panel area while still collapsing the duplicate.

    Overlap is measured relative to the SMALLER of the two boxes so
    containment is caught regardless of which box gets processed first.
    """
    ordered = sorted(boxes_with_conf, key=lambda bc: -bc[1])
    kept: list[list[int]] = []
    for box, _conf in ordered:
        area = _box_area(box)
        if area == 0:
            continue
        merged_into = None
        for i, k in enumerate(kept):
            k_area = _box_area(k)
            if k_area > 0 and _overlap_area(box, k) / min(area, k_area) > overlap_thresh:
                merged_into = i
                break
        if merged_into is not None:
            kept[merged_into] = _union_box(kept[merged_into], box)
        else:
            kept.append(box)
    return kept


def is_sliver_panel(box: list[int], page_w: int, page_h: int) -> bool:
    """True for degenerate detections: a thin strip cropped from a panel
    edge/corner rather than a real panel. These are small relative to the
    page AND extremely elongated -- a legitimate small reaction-shot panel
    is usually close to square, while a false detection from a border line
    or torn-off panel edge is a long thin sliver."""
    w = max(1, box[2] - box[0])
    h = max(1, box[3] - box[1])
    area_frac = (w * h) / max(1, page_w * page_h)
    aspect = max(w / h, h / w)
    return area_frac < 0.025 and aspect > 4.0


def _detect_panels_yolo(img, conf: float = 0.4) -> list[list[int]] | None:
    """Detect panels with the YOLO26-nano Manga109 model. Returns None if
    the model isn't available (caller should fall back to the grid
    heuristic)."""
    model = _load_yolo_model()
    if model is None:
        return None

    results = model.predict(img, conf=conf, iou=0.5, verbose=False)
    boxes_with_conf = []
    for box in results[0].boxes:
        if int(box.cls) != 0:  # 0=panel, 1=text -- we only want panels here
            continue
        x1, y1, x2, y2 = box.xyxy[0].tolist()
        xy_box = [int(x1), int(y1), int(x2), int(y2)]
        if is_sliver_panel(xy_box, img.width, img.height):
            continue
        boxes_with_conf.append((xy_box, float(box.conf)))

    boxes = _dedupe_boxes(boxes_with_conf)
    if not boxes:
        return [[0, 0, img.width, img.height]]
    return boxes


def _snap_to_unclaimed_edges(boxes: list[list[int]], page_w: int, page_h: int,
                             max_gap_frac: float = 0.15) -> list[list[int]]:
    """Extend a panel's edge to the page boundary when it falls short by a
    plausible amount AND no other detected panel claims that space.

    The detector sometimes underestimates a panel's true extent near the
    page edge (e.g. missing a speech bubble that reaches close to the
    border), leaving a gap that should belong to that panel rather than
    being a deliberate gutter. Only snap small gaps (<15% of the page
    dimension) and only when nothing else occupies the overlapping range,
    so real gutters between adjacent panels are left alone.
    """
    original = [tuple(b) for b in boxes]
    result = [list(b) for b in boxes]

    def claimed_beyond(i: int, axis: str, beyond) -> bool:
        ox1, oy1, ox2, oy2 = original[i]
        for j, (jx1, jy1, jx2, jy2) in enumerate(original):
            if j == i:
                continue
            if axis == "x":
                overlaps = not (jy2 <= oy1 or jy1 >= oy2)
                if overlaps and beyond(jx1, jx2, ox1, ox2):
                    return True
            else:
                overlaps = not (jx2 <= ox1 or jx1 >= ox2)
                if overlaps and beyond(jy1, jy2, oy1, oy2):
                    return True
        return False

    for i, (x1, y1, x2, y2) in enumerate(original):
        if 0 < page_w - x2 < page_w * max_gap_frac and \
           not claimed_beyond(i, "x", lambda j1, j2, o1, o2: j2 > o2):
            result[i][2] = page_w
        if 0 < x1 < page_w * max_gap_frac and \
           not claimed_beyond(i, "x", lambda j1, j2, o1, o2: j1 < o1):
            result[i][0] = 0
        if 0 < page_h - y2 < page_h * max_gap_frac and \
           not claimed_beyond(i, "y", lambda j1, j2, o1, o2: j2 > o2):
            result[i][3] = page_h
        if 0 < y1 < page_h * max_gap_frac and \
           not claimed_beyond(i, "y", lambda j1, j2, o1, o2: j1 < o1):
            result[i][1] = 0

    return result


def _merge_small_gaps(splits: list[int], min_size: int) -> list[int]:
    """Collapse boundary points that would create a too-small segment.

    Walks left to right keeping a boundary only if it's far enough from the
    last kept one; a rejected boundary isn't a content drop -- the segment
    it would have started simply gets absorbed into its neighbor. The final
    page-edge boundary is always preserved.
    """
    if len(splits) <= 2:
        return splits
    merged = [splits[0]]
    for s in splits[1:]:
        if s - merged[-1] < min_size:
            continue
        merged.append(s)
    if merged[-1] != splits[-1]:
        merged[-1] = splits[-1]
    return merged


def _detect_panels_grid(img) -> list[list[int]]:
    """Detect panel rectangles by finding solid white gutter bands.

    Pure-Pillow grid detection: scans for rows/columns that are almost
    perfectly white (background gutters between panels), splitting the page
    into bands and then columns within each band. Requires near-total
    whiteness and a substantial minimum gutter/segment size so that
    incidental white space around a speech bubble -- which sits *inside* a
    panel, not between panels -- doesn't get mistaken for a panel boundary;
    candidate splits too close together are merged into their neighbor
    rather than dropped, so no page content is silently lost. Degrades
    gracefully (whole page as one panel) for free-form/borderless layouts.
    """
    gray = img.convert("L")
    w, h = gray.size
    pixels = gray.load()

    threshold = 215
    purity = 0.95
    min_gutter = max(6, int(h * 0.013))
    min_band_h = max(int(h * 0.05), 60)
    min_band_w = max(int(w * 0.06), 60)

    def is_white_row(y: int) -> bool:
        white = sum(1 for x in range(0, w, 2) if pixels[x, y] > threshold)
        return white > (w // 2) * purity

    h_splits = [0]
    in_gutter = False
    gutter_start = 0
    for y in range(h):
        white_row = is_white_row(y)
        if white_row and not in_gutter:
            in_gutter = True
            gutter_start = y
        elif not white_row and in_gutter:
            if y - gutter_start >= min_gutter:
                h_splits.append((gutter_start + y) // 2)
            in_gutter = False
    h_splits.append(h)
    h_splits = _merge_small_gaps(h_splits, min_band_h)

    panels = []
    for band_idx in range(len(h_splits) - 1):
        y1, y2 = h_splits[band_idx], h_splits[band_idx + 1]

        def is_white_col(x: int) -> bool:
            white = sum(1 for y in range(y1, y2, 2) if pixels[x, y] > threshold)
            return white > ((y2 - y1) // 2) * purity

        v_splits = [0]
        in_gutter = False
        gutter_start = 0
        for x in range(w):
            white_col = is_white_col(x)
            if white_col and not in_gutter:
                in_gutter = True
                gutter_start = x
            elif not white_col and in_gutter:
                if x - gutter_start >= min_gutter:
                    v_splits.append((gutter_start + x) // 2)
                in_gutter = False
        v_splits.append(w)
        v_splits = _merge_small_gaps(v_splits, min_band_w)

        for col_idx in range(len(v_splits) - 1):
            x1, x2 = v_splits[col_idx], v_splits[col_idx + 1]
            panels.append([x1, y1, x2, y2])

    if not panels:
        panels.append([0, 0, w, h])

    return panels


def detect_panels(img) -> list[list[int]]:
    """Detect panel rectangles -- YOLO model if available, else grid heuristic."""
    boxes = _detect_panels_yolo(img)
    if boxes is not None:
        return boxes
    return _detect_panels_grid(img)


def _y_overlap_frac(a: list[int], b: list[int]) -> float:
    """Fraction of the shorter panel's height that the two panels' vertical
    extents overlap. Used to decide whether two panels are in the same
    reading "tier" -- center-distance clustering breaks down when one tall
    panel spans the same vertical range as two shorter stacked panels (a
    very common manga layout); overlap is the geometrically correct test."""
    overlap = min(a[3], b[3]) - max(a[1], b[1])
    min_h = min(a[3] - a[1], b[3] - b[1])
    return max(0.0, overlap) / max(1, min_h)


def sort_panels_manga_order(panels: list[list[int]]) -> list[list[int]]:
    """Sort panel boxes in manga reading order via a "reads-before" graph,
    then a topological sort -- robust to mixed-size grids (e.g. one tall
    panel beside two stacked shorter ones), which simple row-clustering by
    Y-center gets wrong.

    For every pair of panels: if their vertical extents overlap
    substantially, they're in the same tier and read right-to-left; if not,
    whichever is higher up reads first (the other dimension doesn't matter
    once there's no vertical overlap). This produces a partial order;
    topological sort resolves the full reading sequence, with same-rank
    ties broken top-to-bottom then right-to-left.
    """
    n = len(panels)
    if n <= 1:
        return panels

    OVERLAP_THRESHOLD = 0.3
    edges: list[list[int]] = [[] for _ in range(n)]
    in_degree = [0] * n

    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            a, b = panels[i], panels[j]
            if _y_overlap_frac(a, b) > OVERLAP_THRESHOLD:
                a_cx, b_cx = (a[0] + a[2]) / 2, (b[0] + b[2]) / 2
                if a_cx > b_cx:  # same tier: right-to-left
                    edges[i].append(j)
                    in_degree[j] += 1
            else:
                a_cy, b_cy = (a[1] + a[3]) / 2, (b[1] + b[3]) / 2
                if a_cy < b_cy:  # different tiers: top-to-bottom
                    edges[i].append(j)
                    in_degree[j] += 1

    def tie_break_key(i: int):
        x1, y1, x2, y2 = panels[i]
        return ((y1 + y2) / 2, -(x1 + x2) / 2)

    available = [i for i in range(n) if in_degree[i] == 0]
    result: list[int] = []
    while available:
        available.sort(key=tie_break_key)
        node = available.pop(0)
        result.append(node)
        for j in edges[node]:
            in_degree[j] -= 1
            if in_degree[j] == 0:
                available.append(j)

    if len(result) != n:
        # Inconsistent/cyclic constraints (shouldn't happen with these two
        # simple rules, but don't silently drop panels if it does).
        return panels

    return [panels[i] for i in result]


# ── Gemini OCR (invoked via curl, key never embedded in code) ───


def call_gemini_panel_ocr(image_path: str, api_key: str, timeout: int = 60, retries: int = 3) -> dict:
    """Ask Gemini what text appears in a panel image, plus an English
    translation of it. Returns {"blocks": [{"text", "bbox_2d"}, ...],
    "translation": str}. Retries on transient errors (503/429/network) with
    exponential backoff; returns {"blocks": [], "translation": ""} if all
    attempts fail, so callers fall back to a panel with no text/translation
    rather than aborting the whole run.
    """
    for attempt in range(retries):
        result = _call_gemini_panel_ocr_once(image_path, api_key, timeout)
        if result is not None:
            return result
        if attempt < retries - 1:
            time.sleep(2 ** attempt)
    return {"blocks": [], "translation": ""}


def _call_gemini_panel_ocr_once(image_path: str, api_key: str, timeout: int) -> dict | None:
    """Single attempt. Returns None (not the empty dict) on a transient
    failure so the retry loop above can distinguish "retry" from "this
    panel genuinely has no text" (the latter is a successful empty result)."""
    with open(image_path, "rb") as f:
        image_b64 = base64.b64encode(f.read()).decode("ascii")

    mime = "image/png" if image_path.lower().endswith(".png") else "image/jpeg"

    payload = {
        "contents": [{
            "parts": [
                {"text": PANEL_OCR_PROMPT},
                {"inline_data": {"mime_type": mime, "data": image_b64}},
            ]
        }],
        "generationConfig": {"responseMimeType": "application/json"},
    }

    with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tf:
        json.dump(payload, tf)
        payload_path = tf.name

    try:
        result = subprocess.run(
            [
                "curl", "-s", "-X", "POST", GEMINI_URL,
                "-H", "Content-Type: application/json",
                "-H", f"x-goog-api-key: {api_key}",
                "-d", f"@{payload_path}",
            ],
            capture_output=True, text=True, timeout=timeout,
        )
    except (subprocess.TimeoutExpired, subprocess.SubprocessError, OSError):
        return None  # network-level failure -- retry
    finally:
        os.unlink(payload_path)

    if result.returncode != 0:
        return None  # network-level failure -- retry

    try:
        response = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None  # malformed response -- retry

    if "error" in response:
        status = response["error"].get("status", "")
        if status in ("UNAVAILABLE", "RESOURCE_EXHAUSTED", "DEADLINE_EXCEEDED", "INTERNAL"):
            return None  # transient API error -- retry
        print(f"  Warning: Gemini error: {response['error'].get('message', status)[:200]}", file=sys.stderr)
        return {"blocks": [], "translation": ""}  # non-transient -- give up on this panel

    try:
        text_out = response["candidates"][0]["content"]["parts"][0]["text"]
        parsed = json.loads(text_out)
        if not isinstance(parsed, dict):
            return {"blocks": [], "translation": ""}
        blocks = parsed.get("blocks", [])
        if not isinstance(blocks, list):
            blocks = []
        blocks = [b for b in blocks if isinstance(b, dict) and "text" in b]
        translation = parsed.get("translation", "")
        if not isinstance(translation, str):
            translation = ""
        return {"blocks": blocks, "translation": translation}
    except (KeyError, IndexError, json.JSONDecodeError) as e:
        snippet = result.stdout[:200]
        print(f"  Warning: could not parse Gemini response ({e}): {snippet}", file=sys.stderr)
        return {"blocks": [], "translation": ""}


# ── Binary output (same format the device already reads) ────────


def encode_page(panels_with_text: list[dict]) -> bytes:
    """Encode one page's panel+text data to binary."""
    buf = bytearray()
    panel_count = min(len(panels_with_text), 255)
    buf += struct.pack("BB", panel_count, 0)

    for panel in panels_with_text[:panel_count]:
        x1, y1, x2, y2 = panel["box"]
        w, h = x2 - x1, y2 - y1
        text_blocks = panel.get("text_blocks", [])
        text_count = min(len(text_blocks), 255)

        translation_bytes = panel.get("translation", "").encode("utf-8")
        if len(translation_bytes) > 0xFFFF:
            translation_bytes = translation_bytes[:0xFFFF]

        buf += struct.pack(
            PANEL_BOX, max(0, x1), max(0, y1), max(0, w), max(0, h), text_count, 0, len(translation_bytes)
        )
        buf += translation_bytes

        for tb in text_blocks[:text_count]:
            tx, ty, tw, th = tb["box"]
            text_bytes = tb["text"].encode("utf-8")
            if len(text_bytes) > 0xFFFF:
                text_bytes = text_bytes[:0xFFFF]
            buf += struct.pack(TEXT_BLOCK, max(0, tx), max(0, ty), max(0, tw), max(0, th), len(text_bytes))
            buf += text_bytes

    return bytes(buf)


def _write_panel_index(output_dir: str, idx_records: list[tuple], dat_chunks: list[bytes]):
    """Write panels.idx + panels.dat covering exactly the pages processed so
    far. Called after every page during conversion (not just once at the
    end) so a crash partway through never loses already-completed pages."""
    idx_path = os.path.join(output_dir, "panels.idx")
    dat_path = os.path.join(output_dir, "panels.dat")
    with open(idx_path, "wb") as f:
        f.write(struct.pack(IDX_HEADER, FORMAT_VERSION, len(idx_records)))
        for off, length, w, h in idx_records:
            f.write(struct.pack(IDX_RECORD, off, length, w, h))
    with open(dat_path, "wb") as f:
        for chunk in dat_chunks:
            f.write(chunk)


# ── Main pipeline ─────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description="Convert manga (image folder / CBZ / EPUB) into CrossPoint Reader format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--input", required=True, help="Image folder, .cbz/.zip, or .epub")
    parser.add_argument("--output-dir", required=True, help="Directory to write pages, panels, and panels.idx/dat")
    parser.add_argument("--page-order-file", help="Text file listing source filenames in reading order, one per line")
    parser.add_argument("--gemini-key-file", help="Path to a file containing the Gemini API key")
    parser.add_argument("--no-ocr", action="store_true", help="Skip Gemini OCR -- panel boxes only, no text")
    parser.add_argument("--panel-margin", type=int, default=10, help="Pixels of margin added around cropped panels")
    parser.add_argument("--max-pages", type=int, help="Only process the first N pages (for testing)")
    args = parser.parse_args()

    api_key = None
    if not args.no_ocr:
        if args.gemini_key_file:
            with open(args.gemini_key_file, "r", encoding="utf-8") as f:
                api_key = f.read().strip()
        else:
            api_key = os.environ.get("GEMINI_API_KEY")
        if not api_key:
            print(
                "Error: no Gemini API key. Pass --gemini-key-file or set GEMINI_API_KEY, "
                "or pass --no-ocr to skip text extraction.",
                file=sys.stderr,
            )
            sys.exit(1)

    from PIL import Image

    work_dir = tempfile.mkdtemp(prefix="manga_convert_")
    try:
        print(f"Collecting pages from: {args.input}")
        pages = collect_pages(args.input, work_dir, args.page_order_file)
        if args.max_pages:
            pages = pages[: args.max_pages]
        print(f"Found {len(pages)} pages")

        os.makedirs(args.output_dir, exist_ok=True)

        idx_records = []
        dat_chunks = []
        dat_offset = 0
        total_panels = 0
        total_text_blocks = 0

        for page_idx, src_path in enumerate(pages):
            print(f"[{page_idx + 1}/{len(pages)}] {os.path.basename(src_path)}")

            img = Image.open(src_path)
            img_w, img_h = img.size

            # Copy page to a canonical, trivially-sortable filename.
            ext = Path(src_path).suffix.lower()
            if ext not in (".jpg", ".jpeg", ".png"):
                ext = ".jpg"
                img.convert("RGB").save(os.path.join(args.output_dir, f"page_{page_idx:04d}{ext}"), "JPEG", quality=92)
            else:
                shutil.copy(src_path, os.path.join(args.output_dir, f"page_{page_idx:04d}{ext}"))

            boxes = detect_panels(img)
            boxes = sort_panels_manga_order(boxes)

            # Crop and save every panel first (fast, local) before dispatching
            # the slow network calls concurrently -- OCR is I/O-bound (network
            # latency dominated), so running a page's panels in parallel turns
            # ~N x call_latency into ~call_latency per page.
            panel_paths = []
            panel_rects = []
            for panel_idx, box in enumerate(boxes):
                x1, y1, x2, y2 = box
                mx1 = max(0, x1 - args.panel_margin)
                my1 = max(0, y1 - args.panel_margin)
                mx2 = min(img_w, x2 + args.panel_margin)
                my2 = min(img_h, y2 + args.panel_margin)

                # Always save a panel crop for panel-zoom view, even for a
                # whole-page single panel (len(boxes)==1) -- keeps panel
                # zoom available regardless of detection granularity.
                cropped = img.crop((mx1, my1, mx2, my2))
                panel_path = os.path.join(args.output_dir, f"p{page_idx}_{panel_idx}.jpg")
                cropped.convert("RGB").save(panel_path, "JPEG", quality=90)
                panel_paths.append(panel_path)
                panel_rects.append((mx1, my1, mx2, my2))

            if api_key:
                with ThreadPoolExecutor(max_workers=min(8, max(1, len(panel_paths)))) as pool:
                    ocr_results = list(pool.map(lambda p: call_gemini_panel_ocr(p, api_key), panel_paths))
            else:
                ocr_results = [{"blocks": [], "translation": ""} for _ in panel_paths]

            panels_with_text = []
            for panel_idx, box in enumerate(boxes):
                x1, y1, x2, y2 = box
                mx1, my1, mx2, my2 = panel_rects[panel_idx]
                ocr_result = ocr_results[panel_idx]
                translation = ocr_result.get("translation", "")
                panel_w, panel_h = mx2 - mx1, my2 - my1

                text_blocks = []
                for b in ocr_result.get("blocks", []):
                    text = b.get("text", "").strip()
                    if not text:
                        continue
                    bbox = b.get("bbox_2d")
                    if bbox and len(bbox) == 4:
                        ymin, xmin, ymax, xmax = bbox
                        tx1 = x1 + int(xmin / 1000 * panel_w)
                        ty1 = y1 + int(ymin / 1000 * panel_h)
                        tx2 = x1 + int(xmax / 1000 * panel_w)
                        ty2 = y1 + int(ymax / 1000 * panel_h)
                    else:
                        tx1, ty1, tx2, ty2 = x1, y1, x2, y2
                    text_blocks.append({"box": [tx1, ty1, tx2, ty2], "text": text})

                panels_with_text.append({"box": box, "text_blocks": text_blocks, "translation": translation})
                total_panels += 1
                total_text_blocks += len(text_blocks)

            page_data = encode_page(panels_with_text)
            idx_records.append((dat_offset, len(page_data), min(img_w, 0xFFFF), min(img_h, 0xFFFF)))
            dat_chunks.append(page_data)
            dat_offset += len(page_data)

            # Rewrite panels.idx/panels.dat after every page so a crash (or a
            # single panel's API call hanging) never loses already-completed
            # pages' work -- each rewrite is a small, self-consistent index
            # covering exactly the pages processed so far.
            _write_panel_index(args.output_dir, idx_records, dat_chunks)

        idx_path = os.path.join(args.output_dir, "panels.idx")
        dat_path = os.path.join(args.output_dir, "panels.dat")
        idx_size = os.path.getsize(idx_path)
        dat_size = os.path.getsize(dat_path)
        print(f"\nOutput in {args.output_dir}:")
        print(f"  {idx_path}: {idx_size:,} bytes ({len(pages)} pages)")
        print(f"  {dat_path}: {dat_size:,} bytes")
        print(f"  Total: {(idx_size + dat_size) / 1024:.1f} KB")
        print(f"  Panels: {total_panels} ({total_panels / max(len(pages), 1):.1f}/page avg)")
        print(f"  Text blocks: {total_text_blocks}")
        print("Done.")
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
