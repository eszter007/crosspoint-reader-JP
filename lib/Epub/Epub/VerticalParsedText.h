#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

// A single positioned glyph cell within a vertically-laid-out page.
// `paragraphIndex` + `byteOffset` identify exactly where this character
// came from in the source text -- this is the hook point for phase 2
// (tap-to-select word lookup against jisho.org): given a tap at logical
// (x, y), find the nearest VerticalGlyph, then walk byteOffset backwards/
// forwards to find word boundaries before firing off a lookup.
struct VerticalGlyph {
  // How this glyph is drawn in vertical layout:
  //   Upright          - normal CJK/kana, drawn at the baseline anchor (x,y).
  //   RotatedRun       - sideways Latin/number run rotated 90° CCW; x,y is the
  //                      rotation baseline anchor; rotatedRunText holds text.
  //   UprightRun       - short upright inline run (e.g. 2-digit tate-chu-yoko)
  //                      drawn unrotated; x,y is the baseline anchor.
  //   RotatedPunct     - a single bracket/dash rotated 90° CCW and aligned in
  //                      its cell using glyph metrics; x,y is the cell top-left
  //                      and codepoint holds the character.
  //   SmallKanaCorner  - small (yoon/sokuon) kana placed toward the cell's
  //                      top-right corner; x,y is the cell's top-left and
  //                      codepoint holds the character.
  enum RenderKind : uint8_t { Upright = 0, RotatedRun = 1, RotatedPunct = 2, SmallKanaCorner = 3, UprightRun = 4 };

  uint32_t codepoint = 0;
  uint16_t column = 0;      // 0 = rightmost column on the page
  uint16_t row = 0;          // 0 = topmost cell in the column
  uint16_t x = 0;            // logical screen-space draw position
  uint16_t y = 0;            // logical screen-space draw position
  uint32_t paragraphIndex = 0;
  uint32_t byteOffset = 0;   // UTF-8 byte offset into that paragraph's text
  uint8_t renderKind = Upright;
  uint8_t style = 0;           // EpdFontFamily::Style flags (BOLD, ITALIC, etc.)
  // Populated for run render kinds (RotatedRun/UprightRun).
  std::string rotatedRunText;
  // Furigana/ruby annotation for this glyph (UTF-8). Rendered in a smaller
  // font to the right of the base character in vertical layout.
  std::string rubyText;
};

// One screen's worth of vertically laid out text, ready to hand to
// VerticalTextBlock::render(). Most fields are fixed-size and serialize
// trivially; the only variable-length piece is VerticalGlyph::rotatedRunText
// on entries where rotated == true, which needs a length-prefixed write/read
// (see docs/vertical-text-design.md for the proposed vsections/*.bin layout)
// rather than a flat memcpy of the vector.
struct VerticalPage {
  std::vector<VerticalGlyph> glyphs;
  uint16_t columnCount = 0;
  uint16_t rowsPerColumn = 0;
};

// Lays out one or more paragraphs of Japanese (or any CJK) text into
// right-to-left, top-to-bottom columns, following simplified kinsoku shori
// rules (see Kinsoku.h) and batching embedded Latin/number runs into
// sideways-rotated blocks.
//
// Deliberately does NOT attempt word-wrap, hyphenation, or the
// Knuth-Plass-style "badness" minimization that ParsedText uses for
// horizontal Latin script -- none of that applies to CJK text, where the
// unit of layout is the individual character, not the word. This makes the
// vertical engine considerably simpler than ParsedText despite doing a
// conceptually similar job.
//
// v1 scope / known limitations (see docs/vertical-text-design.md):
//   - Operates on plain paragraph text; does not currently consume
//     per-run bold/italic/underline styling or inline images.
//   - Punctuation (、 。 etc.) is shifted toward the upper-right of its cell
//     for tategaki, but the offset is an approximation (half cellPx).
class VerticalParsedText {
 public:
  VerticalParsedText(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth, uint16_t viewportHeight);

  // Adds one paragraph's worth of text (UTF-8). Call once per <p> (or
  // equivalent block) in source order; paragraphIndex is just this call's
  // ordinal position and is what VerticalGlyph::paragraphIndex refers back
  // to, so the caller is responsible for keeping its own paragraph-index
  // -> original-text mapping if it needs to resolve lookups later.
  void addParagraph(const std::string& utf8Text);

  // A single run of base text optionally annotated with ruby (furigana).
  // For <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>, this produces two
  // RubyRun entries: {"漢", "かん"} and {"字", "じ"}.
  // Unannotated text has empty ruby.
  struct RubyRun {
    std::string baseText;
    std::string rubyText;
    uint8_t style = 0;
  };

  void addAnnotatedParagraph(const std::vector<RubyRun>& runs);

  // Runs the column-fill layout algorithm over everything added so far and
  // returns one VerticalPage per screen's worth of content. Call this once
  // after all paragraphs for a chapter have been added via addParagraph().
  std::vector<VerticalPage> layoutPages();

  // Column-to-column gap in pixels, added on top of the character cell
  // size when advancing to a new column. Mirrors the role
  // SETTINGS.lineCompression plays for horizontal text; exposed as a
  // setter so EpubReaderActivity can wire it to a reader setting instead
  // of a hardcoded constant.
  void setColumnGapPx(int gapPx) { columnGapPx_ = gapPx; }
  // Extra right-side padding (in pixels) reserved for vertical ruby so it
  // doesn't clip against the right edge.
  void setRightPaddingPx(int padPx) { rightPaddingPx_ = (padPx < 0) ? 0 : padPx; }

 private:
  const GfxRenderer& renderer_;
  int fontId_;
  uint16_t viewportWidth_;
  uint16_t viewportHeight_;
  int columnGapPx_ = 0;
  int rightPaddingPx_ = 0;

  struct PendingChar {
    uint32_t codepoint;
    uint32_t paragraphIndex;
    uint32_t byteOffset;
    uint8_t style;
    std::string rubyText;
  };

  // Flattened, paragraph-tagged codepoint stream built up by addParagraph()
  // and consumed by layoutPages(). Paragraph boundaries are recorded as a
  // forced column break (a new paragraph always starts at the top of a
  // fresh column, matching how horizontal layout starts a new line).
  std::vector<PendingChar> stream_;
  std::vector<size_t> paragraphBreaksBeforeIndex_;

  int charAdvancePx() const;
};
