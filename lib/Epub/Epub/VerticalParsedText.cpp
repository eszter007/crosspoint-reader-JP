#include "VerticalParsedText.h"

#include <algorithm>
#include <cmath>

#include "GfxRenderer.h"
#include "Kinsoku.h"

namespace {

// Minimal local UTF-8 decoder. Deliberately self-contained rather than
// depending on the project's internal utf8NextCodepoint() (used inside
// GfxRenderer.cpp) since that helper's visibility/signature wasn't
// confirmed against the exact checkout this lands on -- swap this out for
// the shared helper if/when it's exposed publicly, to avoid having two
// implementations to keep in sync.
uint32_t decodeUtf8At(const std::string& s, size_t i, size_t* bytesConsumed) {
  const unsigned char c0 = static_cast<unsigned char>(s[i]);
  if (c0 < 0x80) {
    *bytesConsumed = 1;
    return c0;
  }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    *bytesConsumed = 2;
    return ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    *bytesConsumed = 3;
    return ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3F);
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    *bytesConsumed = 4;
    return ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
  }
  // Malformed byte -- treat as a single replacement-ish char and move on
  // rather than getting stuck.
  *bytesConsumed = 1;
  return c0;
}

// REGULAR isn't a symbol we've confirmed exists by name in
// EpdFontFamily::Style for this checkout -- 0 is the bitwise-OR identity
// element for the style flags (BOLD | ITALIC | UNDERLINE are documented as
// being combined with bitwise OR), so this is safe regardless of what the
// "no styling" enumerator is actually called. Swap in the real symbol if
// one exists, for readability.
constexpr int kNoStyle = 0;

} // namespace

VerticalParsedText::VerticalParsedText(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                                        uint16_t viewportHeight)
    : renderer_(renderer), fontId_(fontId), viewportWidth_(viewportWidth), viewportHeight_(viewportHeight) {}

int VerticalParsedText::charAdvancePx() const {
  // getLineHeight() returns the font's advanceY, which is the right metric
  // for "how far apart are two stacked characters" -- CJK fonts are
  // designed so this is approximately equal to the character's em-square,
  // which is exactly the cell size tategaki layout wants.
  return renderer_.getLineHeight(fontId_);
}

void VerticalParsedText::addParagraph(const std::string& utf8Text) {
  const uint32_t paragraphIndex = static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size());
  paragraphBreaksBeforeIndex_.push_back(stream_.size());

  size_t i = 0;
  while (i < utf8Text.size()) {
    size_t consumed = 1;
    const uint32_t cp = decodeUtf8At(utf8Text, i, &consumed);
    // Skip raw newlines/tabs from source HTML -- paragraph breaks are
    // already tracked structurally via addParagraph() calls. Note: a
    // plain space is deliberately NOT skipped here even though CJK prose
    // itself never uses inter-word spaces, because Kinsoku::
    // isRotatedRunCharacter() now treats ' ' as part of a Latin run --
    // dropping it here would merge multi-word embedded English phrases
    // ("CrossPoint Reader") into one unreadable token
    // ("CrossPointReader"). A stray space between two CJK characters
    // (rare, but it happens in some EPUB markup) just renders as a
    // harmless near-invisible 1-character rotated "run".
    if (cp == '\n' || cp == '\r' || cp == '\t') {
      i += consumed;
      continue;
    }
    stream_.push_back(PendingChar{cp, paragraphIndex, static_cast<uint32_t>(i), {}});
    i += consumed;
  }
}

void VerticalParsedText::addAnnotatedParagraph(const std::vector<RubyRun>& runs) {
  const uint32_t paragraphIndex = static_cast<uint32_t>(paragraphBreaksBeforeIndex_.size());
  paragraphBreaksBeforeIndex_.push_back(stream_.size());

  for (const auto& run : runs) {
    if (run.baseText.empty()) continue;

    // Decode base text into codepoints, then distribute ruby across them.
    std::vector<size_t> baseOffsets;
    std::vector<uint32_t> baseCps;
    {
      size_t i = 0;
      while (i < run.baseText.size()) {
        size_t consumed = 1;
        const uint32_t cp = decodeUtf8At(run.baseText, i, &consumed);
        if (cp == '\n' || cp == '\r' || cp == '\t') {
          i += consumed;
          continue;
        }
        baseOffsets.push_back(i);
        baseCps.push_back(cp);
        i += consumed;
      }
    }

    if (baseCps.empty()) continue;

    if (run.rubyText.empty()) {
      for (size_t k = 0; k < baseCps.size(); k++) {
        stream_.push_back(
            PendingChar{baseCps[k], paragraphIndex, static_cast<uint32_t>(baseOffsets[k]), {}});
      }
    } else {
      // Decode ruby codepoints to distribute evenly across base characters.
      std::vector<uint32_t> rubyCps;
      {
        size_t ri = 0;
        while (ri < run.rubyText.size()) {
          size_t consumed = 1;
          rubyCps.push_back(decodeUtf8At(run.rubyText, ri, &consumed));
          ri += consumed;
        }
      }

      // Distribute ruby codepoints across base characters. Each base char
      // gets a roughly equal share of the annotation string, re-encoded
      // back to UTF-8.
      const size_t baseCount = baseCps.size();
      const size_t rubyCount = rubyCps.size();
      for (size_t k = 0; k < baseCount; k++) {
        const size_t rubyStart = rubyCount * k / baseCount;
        const size_t rubyEnd = rubyCount * (k + 1) / baseCount;
        std::string slice;
        for (size_t r = rubyStart; r < rubyEnd; r++) {
          const uint32_t rcp = rubyCps[r];
          if (rcp < 0x80) {
            slice.push_back(static_cast<char>(rcp));
          } else if (rcp < 0x800) {
            slice.push_back(static_cast<char>(0xC0 | (rcp >> 6)));
            slice.push_back(static_cast<char>(0x80 | (rcp & 0x3F)));
          } else if (rcp < 0x10000) {
            slice.push_back(static_cast<char>(0xE0 | (rcp >> 12)));
            slice.push_back(static_cast<char>(0x80 | ((rcp >> 6) & 0x3F)));
            slice.push_back(static_cast<char>(0x80 | (rcp & 0x3F)));
          } else {
            slice.push_back(static_cast<char>(0xF0 | (rcp >> 18)));
            slice.push_back(static_cast<char>(0x80 | ((rcp >> 12) & 0x3F)));
            slice.push_back(static_cast<char>(0x80 | ((rcp >> 6) & 0x3F)));
            slice.push_back(static_cast<char>(0x80 | (rcp & 0x3F)));
          }
        }
        stream_.push_back(PendingChar{baseCps[k], paragraphIndex,
                                       static_cast<uint32_t>(baseOffsets[k]), std::move(slice)});
      }
    }
  }
}

std::vector<VerticalPage> VerticalParsedText::layoutPages() {
  std::vector<VerticalPage> pages;
  if (stream_.empty()) return pages;

  const int cellPx = std::max(1, charAdvancePx());
  const int columnAdvancePx = cellPx + columnGapPx_;
  const uint16_t rowsPerColumn = static_cast<uint16_t>(std::max(1, viewportHeight_ / cellPx));
  const uint16_t columnsPerPage = static_cast<uint16_t>(std::max(1, viewportWidth_ / columnAdvancePx));
  const int ascender = renderer_.getFontAscenderSize(fontId_);

  // Index into paragraphBreaksBeforeIndex_ of the *next* paragraph start,
  // so we know when we've crossed into a new paragraph and should force a
  // fresh column.
  size_t nextParagraphBreakIdx = 1; // index 0 is the very first paragraph, already "started"

  VerticalPage page;
  page.columnCount = columnsPerPage;
  page.rowsPerColumn = rowsPerColumn;

  uint16_t column = 0;
  uint16_t row = 0;

  auto columnLeftX = [&](uint16_t col) -> int { return viewportWidth_ - cellPx - col * columnAdvancePx; };

  auto finalizePageIfNeeded = [&]() {
    if (column >= columnsPerPage) {
      pages.push_back(std::move(page));
      page = VerticalPage{};
      page.columnCount = columnsPerPage;
      page.rowsPerColumn = rowsPerColumn;
      column = 0;
      row = 0;
    }
  };

  auto placeUpright = [&](const PendingChar& pc) {
    VerticalGlyph g;
    g.codepoint = pc.codepoint;
    g.column = column;
    g.row = row;
    g.x = static_cast<uint16_t>(columnLeftX(column));
    g.y = static_cast<uint16_t>(row * cellPx + ascender);
    g.paragraphIndex = pc.paragraphIndex;
    g.byteOffset = pc.byteOffset;
    g.rotated = false;
    g.rubyText = pc.rubyText;
    page.glyphs.push_back(g);
  };

  size_t idx = 0;
  while (idx < stream_.size()) {
    const PendingChar& pc = stream_[idx];

    // Force a fresh column at the start of every paragraph after the
    // first, the same way horizontal layout starts a new line per
    // paragraph.
    if (nextParagraphBreakIdx < paragraphBreaksBeforeIndex_.size() &&
        idx == paragraphBreaksBeforeIndex_[nextParagraphBreakIdx]) {
      if (row != 0) {
        column++;
        row = 0;
        finalizePageIfNeeded();
      }
      nextParagraphBreakIdx++;
    }

    if (Kinsoku::isRotatedRunCharacter(pc.codepoint)) {
      // Gather the contiguous run of rotated-run characters (e.g. an
      // English word or a number) so it's laid out, and later rendered,
      // as a single sideways block instead of one cell per character.
      size_t runEnd = idx;
      std::string runUtf8;
      while (runEnd < stream_.size() && Kinsoku::isRotatedRunCharacter(stream_[runEnd].codepoint) &&
             stream_[runEnd].paragraphIndex == pc.paragraphIndex) {
        // Re-encode the codepoint back to UTF-8 for width measurement.
        // All rotated-run codepoints are ASCII by construction (see
        // Kinsoku::isRotatedRunCharacter), so this is a single byte.
        runUtf8.push_back(static_cast<char>(stream_[runEnd].codepoint));
        runEnd++;
        if (runEnd - idx > 64) break; // sanity cap on a single run
      }

      const int runWidthPx = renderer_.getTextAdvanceX(fontId_, runUtf8.c_str(), static_cast<EpdFontFamily::Style>(kNoStyle));
      const uint16_t rowsNeeded = static_cast<uint16_t>(std::max(1, static_cast<int>(std::ceil(static_cast<double>(runWidthPx) / cellPx))));

      // If the run doesn't fit in the remaining space of a column that
      // already has content, push the whole run to a fresh column rather
      // than splitting it mid-word. (If it doesn't fit even in an empty
      // column, let it render past the bottom edge -- a rare edge case for
      // a very long unbroken run of Latin/digits; revisit if it comes up
      // in practice.)
      if (row != 0 && row + rowsNeeded > rowsPerColumn) {
        column++;
        row = 0;
        finalizePageIfNeeded();
      }

      const int topY = row * cellPx;
      const int bottomY = topY + runWidthPx;
      // Reversing the run before handing it to drawTextRotated90CW is
      // required because that function advances *upward* (decreasing y)
      // as it consumes the string -- see GfxRenderer's documented
      // behaviour ("Cursor advances upward: yPos -= glyph->advanceX").
      // Starting from bottomY with the reversed string makes the run's
      // *first* character land at the top of its span and read downward
      // in step with the rest of the column, matching standard tategaki
      // convention for embedded Latin/number runs.
      // NOTE: the exact pixel offsets here (topY/bottomY/columnLeftX
      // alignment against ascender/descender) are derived from the
      // formulas in GfxRenderer's docs but have not been visually
      // verified on real glyph metrics -- expect to nudge this once you
      // can see it on-device.
      std::string reversedRun(runUtf8.rbegin(), runUtf8.rend());

      VerticalGlyph g;
      g.codepoint = 0; // unused for rotated entries; see rotatedRunText
      g.column = column;
      g.row = row;
      g.x = static_cast<uint16_t>(columnLeftX(column));
      g.y = static_cast<uint16_t>(bottomY);
      g.paragraphIndex = pc.paragraphIndex;
      g.byteOffset = pc.byteOffset;
      g.rotated = true;
      g.rotatedRunText = reversedRun;
      page.glyphs.push_back(g);

      row = static_cast<uint16_t>(row + rowsNeeded);
      if (row >= rowsPerColumn) {
        column++;
        row = 0;
        finalizePageIfNeeded();
      }
      idx = runEnd;
      continue;
    }

    // Single upright CJK/kana/punctuation character.
    bool startingNewColumn = (row == 0);
    if (startingNewColumn && Kinsoku::isLineStartProhibited(pc.codepoint) && !page.glyphs.empty()) {
      // Oikomi (追い込み): pull this character back into the previous
      // column as an extra row, rather than letting it start a new one.
      // We allow the previous column to grow by one cell beyond
      // rowsPerColumn -- visually this means that column is very slightly
      // taller than its neighbours, which is the standard, accepted
      // trade-off real typesetting software makes for this rule.
      VerticalGlyph& prev = page.glyphs.back();
      VerticalGlyph g;
      g.codepoint = pc.codepoint;
      g.column = prev.column;
      g.row = static_cast<uint16_t>(prev.row + 1);
      g.x = static_cast<uint16_t>(columnLeftX(prev.column));
      g.y = static_cast<uint16_t>(g.row * cellPx + ascender);
      g.paragraphIndex = pc.paragraphIndex;
      g.byteOffset = pc.byteOffset;
      g.rotated = false;
      g.rubyText = pc.rubyText;
      page.glyphs.push_back(g);
      idx++;
      continue;
    }

    bool endingColumn = (row == rowsPerColumn - 1);
    if (endingColumn && Kinsoku::isLineEndProhibited(pc.codepoint)) {
      // Oidashi (追い出し): push this character forward into a fresh
      // column instead of letting it end the current one.
      column++;
      row = 0;
      finalizePageIfNeeded();
    }

    placeUpright(pc);
    row++;
    if (row >= rowsPerColumn) {
      column++;
      row = 0;
      finalizePageIfNeeded();
    }
    idx++;
  }

  if (!page.glyphs.empty() || pages.empty()) {
    pages.push_back(std::move(page));
  }
  return pages;
}
