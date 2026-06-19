#include "VerticalTextBlock.h"

#include "GfxRenderer.h"

namespace {
// See VerticalParsedText.cpp's kNoStyle comment -- same reasoning applies
// here: 0 is the bitwise-OR identity for the style flags regardless of
// what the "no styling applied" enumerator is actually named in this
// checkout's EpdFontFamily::Style.
constexpr int kNoStyle = 0;
} // namespace

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int offsetX, int offsetY, bool black) const {
  for (const VerticalGlyph& g : page_.glyphs) {
    const int dx = g.x + offsetX;
    const int dy = g.y + offsetY;

    if (g.rotated) {
      renderer.drawTextRotated90CW(fontId, dx, dy, g.rotatedRunText.c_str(), black,
                                    static_cast<EpdFontFamily::Style>(kNoStyle));
      continue;
    }

    std::string utf8Char;
    const uint32_t cp = g.codepoint;
    if (cp < 0x80) {
      utf8Char.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      utf8Char.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      utf8Char.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      utf8Char.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      utf8Char.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      utf8Char.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      utf8Char.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      utf8Char.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      utf8Char.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      utf8Char.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }

    renderer.drawText(fontId, dx, dy, utf8Char.c_str(), black, static_cast<EpdFontFamily::Style>(kNoStyle));
  }
}
