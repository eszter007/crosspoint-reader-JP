#include <cstdio>
#include <string>

#include "GfxRenderer.h"
#include "VerticalParsedText.h"
#include "blocks/VerticalTextBlock.h"

int main() {
  GfxRenderer renderer;
  // A 480x800 portrait viewport minus rough margins, similar to what the
  // real EpubReaderActivity would pass in for the X4's Portrait
  // orientation.
  const uint16_t viewportWidth = 440;
  const uint16_t viewportHeight = 760;
  const int fontId = 1016; // arbitrary stand-in id, unused by the stub

  VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);

  // Paragraph 1: plain prose with kinsoku-relevant punctuation
  // (closing bracket, ideographic comma/period) deliberately placed near
  // likely column boundaries by repetition.
  layout.addParagraph(
      "吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。"
      "何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。");

  // Paragraph 2: includes an embedded English word and a number, to
  // exercise the rotated-run path.
  layout.addParagraph(
      "彼はCrossPoint Readerという端末で2024年に日本語の小説を読んでいた。「これは良い」と思った。");

  std::vector<VerticalPage> pages = layout.layoutPages();
  std::printf("Laid out %zu paragraph(s) into %zu page(s).\n", size_t(2), pages.size());

  for (size_t p = 0; p < pages.size(); p++) {
    const VerticalPage& page = pages[p];
    std::printf("\n=== Page %zu: %zu glyphs, %u columns x %u rows ===\n", p, page.glyphs.size(), page.columnCount,
                page.rowsPerColumn);
    VerticalTextBlock block(page);
    block.render(renderer, fontId);
  }

  // Basic sanity assertions (not a full test framework -- just enough to
  // catch obvious regressions like infinite loops, empty output, or
  // out-of-range column/row indices).
  bool ok = true;
  for (const auto& page : pages) {
    for (const auto& g : page.glyphs) {
      if (g.column >= page.columnCount) {
        std::printf("FAIL: glyph column %u >= columnCount %u\n", g.column, page.columnCount);
        ok = false;
      }
      if (!g.rotated && g.row > page.rowsPerColumn) {
        std::printf("FAIL: upright glyph row %u > rowsPerColumn %u\n", g.row, page.rowsPerColumn);
        ok = false;
      }
    }
  }
  std::printf("\n%s\n", ok ? "SANITY CHECKS PASSED" : "SANITY CHECKS FAILED");
  return ok ? 0 : 1;
}
