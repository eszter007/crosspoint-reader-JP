# Vertical (tategaki) Japanese text -- design notes

## Why this is its own pipeline, not a flag on the existing one

CrossPoint's existing layout engine (`ParsedText` + `ChapterHtmlSlimParser`)
is built around *word*-based horizontal layout: it collects words, measures
them, and runs a Knuth-Plass-style line-breaking pass to decide where to
wrap. None of that machinery is useful for Japanese: there are no spaces
between words, justification isn't done by stretching inter-word gaps, and
the unit of layout is the individual character, not the word.

Rather than bolting a `verticalMode` branch into `ParsedText`'s line-breaking
DP/greedy algorithms (which would make an already dense file harder to
follow for a feature that shares almost none of its logic), this adds a
parallel, much simpler layout engine purpose-built for column-fill CJK
text:

- `Kinsoku.{h,cpp}` -- character classification (line-start/line-end
  prohibitions, "always upright" CJK ranges, "needs sideways rotation"
  Latin/digit ranges).
- `VerticalParsedText.{h,cpp}` -- the layout engine. Takes plain UTF-8
  paragraphs in, produces a list of `VerticalPage` (one per screen) out.
- `blocks/VerticalTextBlock.{h,cpp}` -- renders one `VerticalPage` via the
  existing `GfxRenderer::drawText()` / `drawTextRotated90CW()` calls. No
  changes to `GfxRenderer` itself, no new pixel-level drawing code, no
  changes to the orientation/coordinate-rotation system documented in
  `lib/GfxRenderer/GfxRenderer.{h,cpp}` -- this only decides *where* to ask
  the existing renderer to put upright glyphs and rotated Latin runs.

This was a deliberate scope call: it gets a working vertical reading mode
in front of you to test against real books quickly, at the cost of a
second, smaller layout engine living alongside the first one instead of
one unified one. Unifying them under a shared `Block`/`Layout` interface
(so `Page` can hold either kind of block, and both flow through the same
`Section` cache machinery) is the natural follow-up once this is proven out
on real EPUBs and you've decided the column algorithm doesn't need more
changes -- doing that refactor *before* validating the algorithm risks
spending the effort twice.

## What's implemented

- Right-to-left, top-to-bottom column fill, one character per cell.
- Paragraph boundaries force a new column (mirrors how horizontal layout
  starts a new line per paragraph).
- Kinsoku shori (simplified rule set, see `Kinsoku.h`'s own doc comment for
  exactly what's covered and what isn't):
  - **Oikomi** (pull-back): a character that can't start a column (closing
    brackets, small kana, ideographic commas/periods, etc.) gets appended
    to the bottom of the *previous* column instead.
  - **Oidashi** (push-forward): an opening bracket that would otherwise be
    the last character in a column gets pushed to start the next column.
- Embedded Latin/digit runs (an English word, an acronym, a year) are
  detected, batched, and rendered as a single sideways block via the
  existing `drawTextRotated90CW()`, reversed so the run reads top-to-bottom
  in the same direction as the surrounding column instead of
  bottom-to-top (which is the direction that function was originally
  written for -- side-button labels).
- Per-character position tracking (`paragraphIndex` + `byteOffset` on every
  `VerticalGlyph`) -- this is the data jisho.org lookup will need in the
  next phase: given a tap at logical (x, y), find the nearest glyph, then
  walk byte offsets to find word boundaries before firing a lookup. That's
  *why* this field exists already even though nothing reads it yet.

A desktop-only sanity test (`dev/vertical_host_test/`) exercises the engine
against real Japanese sentences (including embedded English/numbers and
several kinsoku-relevant punctuation marks) using stub headers standing in
for `GfxRenderer`. It caught one real bug during development -- embedded
multi-word English phrases lost their spaces ("CrossPoint Reader" became
"CrossPointReader") because the original whitespace-skipping logic ran
before run-detection. Fixed in this version; see the test's output for a
worked example. **This stub test is not wired into the PlatformIO build and
should not be -- it's a development aid only, run with a plain
desktop `g++`.**

## What's explicitly deferred (don't be surprised it's missing)

- **Inline styling and images in vertical mode.** v1 operates on plain
  paragraph text extracted per chapter. Bold/italic/underline runs and
  embedded `<img>` are not consumed by `VerticalParsedText::addParagraph()`
  as written. Most Japanese fiction/light-novel content is lightly styled,
  so this is a reasonable v1 gap, but it does mean a heavily-styled EPUB
  will read fine but lose its emphasis formatting in vertical mode.
- **Punctuation positioning.** Real tategaki typesetting shifts commas,
  periods, and the small `っ/ゃ/ゅ/ょ` upward and to the side within their
  cell rather than centering them on the baseline like every other
  character. This renders them dead-center for now -- legible, not
  typographically correct. Fixing this needs per-glyph positional offsets
  that the current `.cpfont` binary format likely doesn't carry (it wasn't
  designed with vertical metrics in mind) -- worth checking before
  investing in this polish.
- **Ruby/furigana.** Not handled at all; `<rt>`/`<rb>` would currently just
  fall through the normal HTML tag handling untouched (or get stripped,
  depending on how `ChapterHtmlSlimParser` treats unrecognized tags --
  verify against the actual parser before assuming either way).
- **Full JIS X 4051 kinsoku.** The rule set in `Kinsoku.cpp` covers the
  punctuation/kana you'll hit constantly in real prose, not edge cases like
  prohibiting a break between a numeral and its counter suffix.
- **Pixel-perfect rotated-run alignment.** The math in
  `VerticalParsedText.cpp`'s rotated-run branch is derived from the
  formulas documented for `drawTextRotated90CW()`
  (`screenX = x + (ascender - top + glyphY)`, `screenY = yPos - left -
  glyphX`), but hasn't been visually verified against real glyph metrics on
  hardware. Expect to nudge the x-offset once you can see it next to
  upright text on an actual panel.

## Settings & caching (not yet wired -- see ../INTEGRATION.md)

The actual `CrossPointSettings`/`EpubReaderActivity`/cache-format wiring
needs hand-integration rather than a blind patch, because this PR didn't
have access to the literal current contents of those files (large,
actively-changing files; the project's own `CLAUDE.md` and recent PR
history show the maintainers are mid-refactor on font/CJK handling, so
guessing at exact current line numbers would be more likely to produce a
PR that doesn't apply than one that does). `../INTEGRATION.md` lays out
exactly what needs to change and why, for you (or whoever picks this up) to
wire against the real, current files.
