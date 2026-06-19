# Wiring vertical text into the rest of the app

The files under `lib/Epub/Epub/` in this patch are new and self-contained
-- they don't touch anything that exists today, so dropping them into your
checkout can't break the current (horizontal) reading experience by
itself. What's below is the remaining glue to actually reach them from a
running book. None of this was written as a blind diff against guessed
line numbers in files I didn't have literal access to (`CrossPointSettings.
{h,cpp}`, `EpubReaderActivity.cpp`, `SettingsActivity.cpp`) -- treat each
step as a worked instruction, not a patch to apply mechanically.

## 1. Add a persisted setting

In `CrossPointSettings.h`, add a field alongside the other reader settings
(`fontFamily`, `lineCompression`, etc. -- see the `Settings System` section
of the project's DeepWiki for where these live):

```cpp
bool verticalTextMode = false;
```

Add a getter/setter pair matching the existing convention for the other
boolean toggles in that class, and bump whatever version/size constant
guards the binary `settings.bin` format -- the settings system in this
project persists a fixed-layout struct, so adding a field without
bumping that will misread old settings files on existing installs.

## 2. Expose it in the Settings UI

In `SettingsActivity.cpp`, add a `TOGGLE` setting entry (see the
`SettingInfo` structure documented for this activity) under whatever
section currently holds `embeddedStyle`/`hyphenationEnabled`-style reading
toggles, with a label along the lines of "Vertical text (Japanese)".

## 3. Branch the chapter-load path

In `EpubReaderActivity.cpp`, wherever the activity currently asks
`ChapterHtmlSlimParser`/`Section` to produce pages for the current spine
item, branch on `SETTINGS.verticalTextMode`:

```cpp
if (SETTINGS.verticalTextMode) {
  // 1. Extract plain paragraph text for this chapter. The simplest
  //    starting point is NOT to reuse ChapterHtmlSlimParser as-is (it's
  //    built to emit styled word-runs for the horizontal engine) -- write
  //    a small helper that walks the chapter HTML and calls
  //    VerticalParsedText::addParagraph() once per block-level element's
  //    text content, dropping markup. A from-scratch Expat handler with
  //    ~5 callbacks (start tag, end tag, char data) covers this; you do
  //    not need the styling/CSS/image machinery the existing parser
  //    carries for v1 vertical mode (see docs/vertical-text-design.md's
  //    "explicitly deferred" section).
  // 2. VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
  //    layout.addParagraph(...) per block; auto pages = layout.layoutPages();
  // 3. Cache `pages` the same way the existing pipeline caches Page
  //    objects, but under a separate cache namespace -- e.g.
  //    `.crosspoint/epub_<hash>/vsections/<N>.bin` rather than
  //    `sections/<N>.bin` -- so toggling vertical mode on/off doesn't
  //    require explaining a mixed-format cache file, and so an existing
  //    horizontal cache isn't invalidated/overwritten by enabling this.
  //    Include `verticalTextMode` itself in whatever validation header
  //    the cache file already stores (font ID, viewport dims, line
  //    compression, etc.) so toggling the setting invalidates correctly.
} else {
  // existing horizontal path, unchanged
}
```

## 4. Branch the render call

Wherever the activity currently does `textBlock->render(renderer, ...)`
for a `Page`'s blocks, branch the same way and call
`VerticalTextBlock::render(renderer, fontId)` instead when in vertical
mode.

## 5. Page-turn semantics

No changes needed to the page-turn input handling itself (`loop()`'s
button mapping) -- pages remain the unit of navigation either way; only
the column order *within* a page is right-to-left instead of the
horizontal engine's top-to-bottom line order. Forward/back still means
"next/previous page in the chapter's page list."

## 6. Orientation

Vertical Japanese reading conventionally wants a tall (portrait) viewport,
which is already CrossPoint's default orientation -- no orientation system
changes needed. If you want to *force* portrait whenever vertical mode is
on (overriding a user's landscape preference, since vertical+landscape
would produce extremely short, extremely numerous columns), that's a small
addition in `EpubReaderActivity::onEnter()`'s existing
`applyReaderOrientation()` call, not a change to the orientation system
itself.

## 7. Build

These are plain `.cpp`/`.h` files under `lib/Epub/Epub/`, the same
directory `ParsedText.cpp` already lives in -- PlatformIO's default
recursive lib scanning should pick them up without any `platformio.ini`
changes. Confirm with:

```
pio run -e default
```

## Suggested commit/PR shape

Given the size of this, it's worth landing as two PRs rather than one:

1. This patch as-is (new files, unused/unwired) plus the settings toggle
   from steps 1-2, with the toggle's `EpubReaderActivity` branch initially
   just falling through to a "not implemented yet" message -- gets the
   plumbing and naming bikeshedding reviewed early without blocking on
   the harder integration work.
2. Steps 3-6, once you've validated against a real Japanese EPUB on
   hardware.
