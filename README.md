# CrossPoint Reader — Japanese Language Learning Fork

A fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) e-reader firmware for the Xteink X4, focused on **reading Japanese books** with built-in learning tools. Read native Japanese novels and texts with instant dictionary lookup, verb deinflection, grammar references, and AI-powered page translation — all on an e-ink device.

This fork is fully compatible with upstream CrossPoint and can be flashed onto any supported Xteink X4 device.
It can be tested in an emulator before flashing: https://github.com/eszter007/Crosspoint-Emulator

---

## What's Different from Upstream CrossPoint

This fork adds a complete Japanese reading toolkit on top of the base e-reader:

### Vertical Japanese Text (Tategaki)

Japanese books are automatically detected from EPUB metadata (`<dc:language>ja</dc:language>`) and rendered in vertical text layout — no manual setting needed. The vertical text engine handles:

- Right-to-left column flow with proper line breaking (kinsoku rules)
- Font-adaptive punctuation, bracket, and dash positioning (works with UDDigiKyokasho, Noto Serif, Noto Sans)
- Bold, italic, and emphasis marks (sesame dots ﹅)
- A per-book "Vertical Text: ON/OFF" toggle in the reader menu to override auto-detection (persists across reopens)

### Dictionary & Word Lookup

Open the reader menu and select **Word Lookup** to look up any word on the current page. Works in both vertical and horizontal reading modes. Only shown for Japanese books.

- **JMdict vocabulary** — Full JMdict/Jitendex dictionary with readings, part-of-speech tags, definitions, and example sentences
- **Verb deinflection** — Conjugated forms resolve to their dictionary base automatically:
  - te-form: 読んで → 読む
  - masu: 食べます → 食べる
  - negative: ありません → ある
  - past: 食べませんでした → 食べる
  - volitional: 眠ろう → 眠る
  - passive: 読まれた → 読む
  - causative: 食べさせる → 食べる
  - compound auxiliaries: 読んでいる, 食べてしまう, etc.
- **Grammar dictionary** — Integrated grammar reference (e.g. "Dictionary of Japanese Grammar" in Yomitan format) surfaces grammar patterns alongside vocabulary
- **Name dictionary (JMnedict)** — Japanese names recognized and grouped with honorifics (根岸さん, 和樹くん shown as one unit)
- **Smart word boundaries** — Pre-scans the page to find dictionary-matchable positions. Filters out single-character particles and conjugation fragments from results. Handles compound words and bound suffixes (設計士).
- **Digit + counter grouping** — Numbers with counters (2年, 15人) shown together with the counter's reading
- **Multiple readings** — Kanji with multiple dictionary entries show all readings sorted by frequency
- **Scrollable definitions** — Long entries scroll with Up/Down. Word navigation with Left/Right

### Page Translation

Select **Translate Page** from the reader menu to translate the current page from Japanese to English via Google's Gemini 2.5 Flash API. Available for all books (not just Japanese). Requires a Gemini API key on the SD card.

### Furigana (Ruby Text)

Reading aids rendered above (horizontal) or beside (vertical) kanji, with positioning adjustments so dense furigana doesn't overlap base characters. Can be toggled on/off per-book from the reader menu.

### Manga Panel Reader (Mokuro)

Read manga with OCR text overlays and per-panel dictionary lookup. Uses [Mokuro](https://github.com/kha-white/mokuro) for OCR, then a conversion tool packs the panel/text data into a compact binary format the device reads natively.

- **Full-page view** — Displays the manga page BMP scaled to screen with panel highlight rectangles
- **Panel-by-panel zoom** — Navigate panels in reading order with page turn buttons. Each panel is scaled to fill the screen.
- **Text overlay** — View OCR'd Japanese text from the current panel, word-wrapped with the reader font
- **Word lookup** — Press Confirm on a zoomed panel to open dictionary lookup for the panel's text. Same dictionary, deinflection, and grammar features as EPUB word lookup.
- **Progress saving** — Current page and panel position saved automatically

### Image Handling

- Dedicated full-page images with aspect-aware rotation
- No blank pages between consecutive images
- Status bar respected for rotated images
- SVG `<image>` elements (Calibre-generated cover pages) now render correctly

### Home Screen

- Reading progress percentage shown below the author on the "Continue Reading" card
- Progress uses spine-aware calculation (matches the Library view)

### Library

The home menu's **Library** has two tabs:

- **Books** — All books on the SD card as a 3-column cover grid, sorted by recency (recently opened first, then alphabetical). Covers and titles are auto-generated from EPUB metadata on first visit. A peek row hints at more content below the button bar.
- **Shelves** — Folders on the SD card that contain books, shown as a list with a cover thumbnail, folder name, book count, and a chevron. Tap a shelf to see all books in that folder as a cover grid with progress.

Tab switching uses the same pattern as Settings: Confirm cycles tabs when the tab row is focused, hold Up/Down to switch tabs from anywhere.

### Insights

The home menu shows an **Insights** entry (between File Transfer and Settings) that tracks your reading activity:

- **Streak widget** — flame icon with current streak count, weekly minutes, and a Mon–Sun day grid with checkmark circles for days you read
- **Stat cards** (2x2 grid):
  - Books finished
  - Days read
  - Total reading time
  - Longest streak
- **Monthly calendar** — navigate months with Left/Right buttons. Days you read are shown as filled black circles. Today is shown with an outline circle. A "X days read" subtitle summarizes each month.

Reading time is recorded automatically when you close a book (minimum 1 minute to count). Books finished are counted once per book (no double-counting on re-open). Stats persist in `/system/reading_stats.bin` on the SD card root — unaffected by cache clears or firmware updates.

### Font Selection

The reader uses whatever font is selected in Settings (built-in Noto Serif/Sans or SD card fonts like UDDigiKyokasho). No font is auto-overridden — the user's choice is always respected.

---

## Setup

### Flash the Firmware

Flash this fork's firmware to your Xteink X4 using the standard CrossPoint flashing process. See the [upstream documentation](https://github.com/crosspoint-reader/crosspoint-reader) for flashing instructions.

### Install Dictionaries

The word lookup feature requires dictionary files on the SD card. Three dictionaries are supported:

#### 1. Vocabulary Dictionary (required)

Download [Jitendex](https://github.com/stephenmk/Jitendex) in Yomitan format, then convert:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jitendex-yomitan.zip \
  --output-dir /path/to/sd/dict/
```

This produces `dict/jmdict.idx` and `dict/jmdict.dat` on the SD card.

#### 2. Name Dictionary (optional, recommended)

Download [JMnedict](https://github.com/JMdictProject) in Yomitan format:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jmnedict-yomitan.zip \
  --output-dir /path/to/sd/dict/
```

Rename the output files to `jmnedict.idx` and `jmnedict.dat` in the `dict/` folder.

#### 3. Grammar Dictionary (optional)

Convert a grammar reference (e.g. "Dictionary of Japanese Grammar") in Yomitan format:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input grammar-dict-yomitan.zip \
  --output-dir /path/to/sd/dict/
```

Rename the output files to `grammar.idx` and `grammar.dat` in the `dict/` folder.

#### SD Card Layout

After setup, your SD card `dict/` folder should contain:

```
/dict/
  jmdict.idx        # Vocabulary index (required)
  jmdict.dat        # Vocabulary definitions (required)
  jmnedict.idx      # Name dictionary index (optional)
  jmnedict.dat      # Name dictionary definitions (optional)
  grammar.idx       # Grammar reference index (optional)
  grammar.dat       # Grammar reference definitions (optional)
```

### Install Japanese Fonts (optional)

For the best vertical text experience, install Japanese `.cpfont` font files on the SD card. Place them in `/.fonts/` organized by family:

```
/.fonts/
  UDDigiKyokasho/
    regular.cpfont
    bold.cpfont
```

UDDigiKyokasho is auto-selected as the default when available. Other supported fonts include Noto Serif JP and Noto Sans JP — the vertical text engine adapts positioning to each font's metrics.

Without SD card fonts, the built-in Noto Serif/Sans fonts work fine for both horizontal and vertical Japanese text.

### Set Up Translation (optional)

To use the "Translate Page" feature:

1. Get a free API key from [Google AI Studio](https://aistudio.google.com/apikey)
2. Create a file `gemini.key` in the `/system/` folder on the SD card containing just the API key:
   ```
   AIzaSyYOUR_KEY_HERE
   ```

The device needs WiFi access for translation. The emulator uses libcurl instead (no WiFi needed on desktop).

---

## Usage

### Reading a Japanese Book

1. Copy a Japanese EPUB to the SD card
2. Open it from My Library — vertical text mode activates automatically
3. Press **Confirm/Enter** to open the reader menu

### Word Lookup

1. Reader menu → **Word Lookup**
2. **Left/Right** — navigate between matched words on the page
3. **Up/Down** — scroll within a long definition
4. **Back** — return to reading

The position counter (e.g. 10/35) appears in the header. Words are pre-scanned so you only land on positions with actual dictionary matches.

### Translation

1. Reader menu → **Translate Page**
2. Wait for "Translating..." to complete
3. **Up/Down** — scroll the translation
4. **Back** — return to reading

### Reading Manga

1. Prepare your manga using the mokuro conversion tools (see below)
2. Copy the output folder to the SD card — it contains BMP page images plus `panels.idx` and `panels.dat`
3. Open the folder from the file browser — the reader detects it as a manga book automatically
4. **Page turn buttons** — In full-page view, enters panel zoom on the first panel. In panel zoom, cycles through panels then advances to the next page.
5. **Back** — Returns from panel zoom to full-page view, or from full-page to the file browser
6. **Confirm** — In panel zoom, opens word lookup for the current panel's text (requires dictionaries)
7. Long-press **Back** — Jump to the file browser from anywhere

### Toggling Vertical Text

For Japanese books, the reader menu shows **Vertical Text: ON/OFF** and **Furigana: ON/OFF**. Toggle either in-place without leaving the menu. Both settings are per-book and persist across reopens.

---

## Building from Source

This fork uses the same PlatformIO build system as upstream CrossPoint:

```bash
# Clone
git clone https://github.com/eszter007/crosspoint-reader-JP.git
cd crosspoint-reader-JP

# Build
pio run

# Flash
pio run -t upload
```

### Building for the Desktop Emulator

This fork works with the [Crosspoint Emulator](https://github.com/eszter007/Crosspoint-Emulator) for desktop testing. See the emulator's README for setup instructions.

---

## Dictionary Converter

The `tools/dict_convert/convert_jmdict.py` script converts dictionary sources to the binary format the device reads. Supported input formats:

| Format | Extension | Source |
|--------|-----------|--------|
| Yomitan/Yomichan | `.zip` | Jitendex, JMnedict, grammar dicts |
| JMdict JSON | `.json` / `.tgz` | jmdict-simplified |
| MDict | `.mdx` | Any MDict dictionary (requires `pip install readmdict`) |

The converter handles:
- Structured content flattening (Yomitan's nested HTML → plain text with formatting)
- Redirect resolution (variant spellings → canonical entry with full definition)
- Frequency-based priority sorting
- Reading/headword cross-indexing

Output: binary `.idx` (sorted 40-byte records) + `.dat` (UTF-8 definitions) files optimized for the device's constrained memory (binary search, no full-file loading).

---

## Manga Conversion Tools

The `tools/mokuro_convert/` directory contains scripts to prepare manga for the device.

### Quick Start (pre-processed Mokuro)

If you already have Mokuro JSON output from a manga volume:

```bash
python3 tools/mokuro_convert/prepare_panels.py \
  --input /path/to/mokuro_output/ \
  --output /path/to/sd/MangaTitle/
```

This converts Mokuro's per-page JSON (panel coordinates + OCR text) and page images into the binary format the device reads: `panels.idx`, `panels.dat`, and BMP page images.

### Full Pipeline (from raw manga images)

If you have raw manga page images and want to run OCR:

```bash
python3 tools/mokuro_convert/run_mokuro.py \
  --input /path/to/manga_pages/ \
  --output /path/to/sd/MangaTitle/
```

This runs Mokuro OCR (requires `pip install mokuro`), then converts the output. GPU recommended for OCR speed.

### SD Card Layout

```
/MangaTitle/
  001.bmp           # Page images (BMP format)
  002.bmp
  ...
  panels.idx        # Panel index (binary, auto-generated)
  panels.dat        # Panel data with OCR text (binary, auto-generated)
```

The device detects any folder containing `panels.idx` as a manga book.

---

## Compatibility with Upstream

This fork tracks upstream CrossPoint and can merge new releases. The Japanese features are additive — they don't modify the base reading experience for non-Japanese books. English and other-language EPUBs work identically to upstream.

Key integration points:
- `lib/Dict/` — Dictionary lookup, deinflection (new library, no upstream conflicts)
- `lib/MangaPanel/` — Manga panel binary format parser (new library)
- `lib/Epub/Epub/VerticalSection.*`, `VerticalParsedText.*` — Vertical text engine (new files)
- `src/activities/reader/EpubReaderWordLookupActivity.*` — Word lookup UI (new activity)
- `src/activities/reader/EpubReaderTranslationActivity.*` — Translation UI (new activity)
- `src/activities/reader/MangaReaderActivity.*` — Manga panel reader (new activity)
- `src/activities/reader/MangaWordLookupActivity.*` — Manga word lookup (new activity)
- `src/activities/reader/EpubReaderActivity.cpp` — Auto-detection and menu wiring (minimal changes to existing code)
- `src/activities/reader/ReaderActivity.cpp` — Manga folder routing (minimal changes)
- `src/activities/home/FileBrowserActivity.cpp` — Manga folder detection (minimal changes)
- `lib/I18n/translations/english.yaml` — New UI strings (additive)

---

## Credits

Built on top of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) — open-source e-reader firmware, community-built, fully hackable, free forever.

Dictionary data from [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and [Jitendex](https://github.com/stephenmk/Jitendex), used under their respective licenses.

Icons by [Tabler Icons](https://tabler.io/icons) (MIT license).
