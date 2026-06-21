#include "EpubReaderWordLookupActivity.h"

#include <DictIndex.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WordLookup.h>

#include "CrossPointSettings.h"
#include "Epub/Kinsoku.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMaxLookupChars = 8;

bool isLookupableChar(uint32_t cp) {
  if (cp < 0x30) return false;
  if (Kinsoku::needsVerticalRotation(cp)) return false;
  if (Kinsoku::verticalShiftType(cp) != 0) return false;
  if (Kinsoku::isSmallKana(cp)) return false;
  if (cp == 0xFE45 || cp == 0xFE46) return false;
  if (cp == 0x30FC) return false;
  if (cp == 0x30FB) return false;
  if (cp == 0x2026 || cp == 0x2025) return false;
  if (cp >= 0x3040 && cp <= 0x309F) return true;  // Hiragana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;  // Katakana
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;  // CJK Unified
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;  // CJK Ext A
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;  // CJK Compat
  return cp >= 0x80;
}

bool isKatakana(uint32_t cp) {
  return (cp >= 0x30A0 && cp <= 0x30FF) || cp == 0x30FC ||
         (Kinsoku::isSmallKana(cp) && cp >= 0x30A0);
}

bool isHiragana(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x309F) ||
         (Kinsoku::isSmallKana(cp) && cp < 0x30A0);
}

bool isCJK(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0xF900 && cp <= 0xFAFF);
}
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                            const VerticalPage& page)
    : Activity("WordLookup", renderer, mappedInput) {
  allGlyphs.reserve(page.glyphs.size());
  selectableGlyphs.reserve(page.glyphs.size());
  for (const auto& g : page.glyphs) {
    if (g.renderKind == VerticalGlyph::RotatedRun) continue;
    // Include all single characters (even rotated punct like ー) in allGlyphs
    // for lookup text building, but only lookupable chars in selectableGlyphs.
    GlyphRef ref{g.x, g.y, g.column, g.row, g.codepoint, g.paragraphIndex, false};
    allGlyphs.push_back(ref);
    if (isLookupableChar(g.codepoint)) {
      selectToAllIdx.push_back(allGlyphs.size() - 1);
      selectableGlyphs.push_back(ref);
    }
  }
}

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderWordLookupActivity::onExit() { Activity::onExit(); }

void EpubReaderWordLookupActivity::moveCursor(int delta) {
  if (selectableGlyphs.empty()) return;
  int prev = cursorIndex;
  // When moving forward and we have a match, jump past the matched word
  if (delta > 0 && hasResult && resultMatchLen > 1) {
    cursorIndex += resultMatchLen;
  } else {
    cursorIndex += delta;
  }
  if (cursorIndex < 0) cursorIndex = 0;
  if (cursorIndex >= static_cast<int>(selectableGlyphs.size())) {
    cursorIndex = static_cast<int>(selectableGlyphs.size()) - 1;
  }
  if (cursorIndex == prev) return;
  performLookup();
}

void EpubReaderWordLookupActivity::encodeUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string EpubReaderWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= selectableGlyphs.size() || startIdx >= selectToAllIdx.size()) return text;

  const size_t allStart = selectToAllIdx[startIdx];
  const uint32_t paraIdx = allGlyphs[allStart].paragraphIndex;
  int charCount = 0;

  for (size_t i = allStart; i < allGlyphs.size() && charCount < kMaxLookupChars; i++) {
    const auto& g = allGlyphs[i];
    if (g.paragraphIndex != paraIdx) break;
    encodeUtf8(g.codepoint, text);
    charCount++;
  }
  return text;
}

void EpubReaderWordLookupActivity::performLookup() {
  hasResult = false;
  resultHeadword.clear();
  resultDefinition.clear();
  resultMatchLen = 0;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    hasResult = true;
    resultHeadword = std::move(result.entry.headword);
    resultDefinition = std::move(result.entry.definition);
    resultMatchLen = static_cast<int>(result.matchLength);
  }

  requestUpdate();
}

void EpubReaderWordLookupActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    performLookup();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveCursor(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveCursor(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveCursor(10); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveCursor(-10); });
}

void EpubReaderWordLookupActivity::renderContentArea(const Rect& screen, int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  const int jaFont = SETTINGS.getReaderFontId();

  if (selectableGlyphs.empty()) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, tr(STR_NO_MATCH), true);
  } else if (hasResult) {
    const int maxWidth = screen.width - metrics.contentSidePadding * 2;
    const int textX = screen.x + metrics.contentSidePadding;

    std::string posText = std::to_string(cursorIndex + 1) + "/" + std::to_string(selectableGlyphs.size());
    renderer.drawText(SMALL_FONT_ID, textX, contentTop, posText.c_str(), true);

    int headY = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 2;
    renderer.drawText(jaFont, textX, headY, resultHeadword.c_str(), true, EpdFontFamily::BOLD);

    int defY = headY + renderer.getLineHeight(jaFont) + metrics.verticalSpacing;

    const int defFont = SMALL_FONT_ID;
    const int defLineH = renderer.getLineHeight(defFont);
    // Split on newlines first, then wrap each line individually
    int linesDrawn = 0;
    constexpr int kMaxDefLines = 20;
    std::string defText = resultDefinition;
    size_t nlPos = 0;
    while (nlPos <= defText.size() && linesDrawn < kMaxDefLines) {
      size_t nextNl = defText.find('\n', nlPos);
      std::string paragraph = (nextNl == std::string::npos)
          ? defText.substr(nlPos) : defText.substr(nlPos, nextNl - nlPos);
      nlPos = (nextNl == std::string::npos) ? defText.size() + 1 : nextNl + 1;

      if (paragraph.empty()) {
        defY += defLineH / 2;
        continue;
      }
      // Try space-based wrapping first (for Latin text), then fall back to
      // character-level wrapping (for CJK text without spaces).
      std::string rem = paragraph;
      while (!rem.empty() && linesDrawn < kMaxDefLines) {
        if (renderer.getTextWidth(defFont, rem.c_str()) <= maxWidth) {
          renderer.drawText(defFont, textX, defY, rem.c_str(), true);
          defY += defLineH;
          linesDrawn++;
          break;
        }
        // Try to break at the last space that fits
        std::string bestLine;
        size_t lastSpaceBreak = std::string::npos;
        std::string accum;
        const char* p = rem.c_str();
        while (*p) {
          size_t charLen = 1;
          auto c0 = static_cast<unsigned char>(*p);
          if (c0 >= 0xF0) charLen = 4;
          else if (c0 >= 0xE0) charLen = 3;
          else if (c0 >= 0xC0) charLen = 2;
          std::string test = accum + std::string(p, charLen);
          if (renderer.getTextWidth(defFont, test.c_str()) > maxWidth) {
            // Never orphan sentence-ending punctuation on its own line
            uint32_t nextCp = 0;
            if (charLen == 3) nextCp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) | (static_cast<unsigned char>(p[2]) & 0x3F);
            else if (charLen == 1) nextCp = c0;
            if (nextCp == 0x3002 || nextCp == 0x3001 || nextCp == 0xFF01 || nextCp == 0xFF1F ||
                nextCp == '.' || nextCp == ',' || nextCp == '!' || nextCp == '?') {
              accum = test;
              p += charLen;
            }
            break;
          }
          accum = test;
          if (*p == ' ') lastSpaceBreak = accum.size();
          p += charLen;
        }
        if (accum.empty()) {
          // Single char wider than maxWidth — force it
          auto c0 = static_cast<unsigned char>(rem[0]);
          size_t cl = 1;
          if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
          accum = rem.substr(0, cl);
          rem = rem.substr(cl);
        } else if (lastSpaceBreak != std::string::npos && lastSpaceBreak > 0) {
          // Break at last space to keep Latin words intact
          std::string line = accum.substr(0, lastSpaceBreak);
          rem = rem.substr(lastSpaceBreak);
          // Skip leading space
          if (!rem.empty() && rem[0] == ' ') rem = rem.substr(1);
          accum = line;
        } else {
          // No space found — break at character boundary (CJK text)
          rem = rem.substr(accum.size());
        }
        renderer.drawText(defFont, textX, defY, accum.c_str(), true);
        defY += defLineH;
        linesDrawn++;
      }
    }

    // Attribution is shown in the header instead
  } else {
    std::string preview;
    encodeUtf8(selectableGlyphs[cursorIndex].codepoint, preview);

    std::string posText = std::to_string(cursorIndex + 1) + "/" + std::to_string(selectableGlyphs.size());
    UITheme::drawCenteredText(renderer, screen, jaFont, contentTop, preview.c_str(), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, SMALL_FONT_ID,
                              contentTop + renderer.getLineHeight(jaFont) + 4, posText.c_str(), true);

    std::string windowPreview = buildLookupText(static_cast<size_t>(cursorIndex));
    if (!windowPreview.empty()) {
      UITheme::drawCenteredText(renderer, screen, jaFont,
                                contentTop + renderer.getLineHeight(jaFont) + 30, windowPreview.c_str(),
                                true);
    }
  }
}

void EpubReaderWordLookupActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - footerHeight;

  if (!initialRenderDone) {
    renderer.clearScreen();

    GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                   tr(STR_WORD_LOOKUP));

    renderContentArea(screen, contentTop);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    initialRenderDone = true;
    fastRefreshCount = 0;
  } else {
    renderer.fillRect(screen.x, contentTop, screen.width, contentBottom - contentTop, false);

    renderContentArea(screen, contentTop);

    fastRefreshCount++;
    if (fastRefreshCount >= kFullRefreshInterval) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      fastRefreshCount = 0;
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }
}
