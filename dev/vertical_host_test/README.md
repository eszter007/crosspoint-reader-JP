# Vertical layout host test (desktop-only dev tool)

Not part of the firmware build. This is a quick way to exercise
`VerticalParsedText`/`VerticalTextBlock` against real Japanese text on your
laptop, without flashing a device, using stub headers in place of the real
`GfxRenderer`/`EpdFontFamily`.

The numbers in `stubs/GfxRenderer.h` (line height, ascender, average Latin
character advance) are rough placeholders, not measured from a real font --
good enough to drive the algorithm through realistic page/column counts and
catch logic bugs (infinite loops, off-by-ones, dropped characters), not to
judge visual correctness. This already caught one real bug during
development: see `docs/vertical-text-design.md`.

## Run it

From the repo root, after copying this patch's `lib/Epub/Epub/` files in:

```
cd dev/vertical_host_test
g++ -std=c++17 \
  -I stubs -I ../../lib/Epub/Epub \
  -o vtest \
  ../../lib/Epub/Epub/Kinsoku.cpp \
  ../../lib/Epub/Epub/VerticalParsedText.cpp \
  ../../lib/Epub/Epub/blocks/VerticalTextBlock.cpp \
  main_test.cpp
./vtest
```

It prints every `drawText`/`drawTextRotated90CW` call the layout would
make, column-by-column, plus a pass/fail line from a handful of structural
sanity assertions (no glyph outside its page's column/row bounds, etc.).
Edit the sample text in `main_test.cpp` to try your own paragraphs,
including punctuation near likely column boundaries to exercise the
kinsoku rules.
