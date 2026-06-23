#include "EpubReaderWordLookupActivity.h"

#include <DictIndex.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
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
  // Digits — not useful alone, but included in allGlyphs for compounds like ７歳
  if (cp >= '0' && cp <= '9') return false;
  if (cp >= 0xFF10 && cp <= 0xFF19) return false;
  // Skip common particles/grammar — N5 level, every learner knows these.
  // They clutter the lookup with unhelpful single-char matches.
  switch (cp) {
    case 0x306F: // は
    case 0x304C: // が
    case 0x306E: // の
    case 0x306B: // に
    case 0x3067: // で
    case 0x3092: // を
    case 0x3082: // も
    case 0x3068: // と
    case 0x304B: // か
    case 0x306A: // な
    case 0x3078: // へ
    case 0x3088: // よ
    case 0x306D: // ね
    case 0x308F: // わ
    case 0x3066: // て
    case 0x3060: // だ
    case 0x305F: // た
    case 0x308B: // る
    return false;
    default:
      break;
  }
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
  for (const auto& g : page.glyphs) {
    if (g.renderKind == VerticalGlyph::RotatedRun) continue;
    GlyphRef ref{g.x, g.y, g.column, g.row, g.codepoint, g.paragraphIndex, false};
    allGlyphs.push_back(ref);
  }

  // Pre-scan: walk through all characters, do dictionary lookups to find
  // word boundaries. Characters inside a matched word are skipped.
  // Filtered particles (は、が etc.) are still checked — if they start a
  // multi-char match (e.g. という, ことになる), they become selectable.
  selectableGlyphs.reserve(allGlyphs.size());
  size_t skipUntil = 0;
  for (size_t i = 0; i < allGlyphs.size(); i++) {
    const auto& g = allGlyphs[i];
    if (i < skipUntil) continue;

    const bool lookupable = isLookupableChar(g.codepoint);

    // Build lookup text from this position
    std::string text;
    const uint32_t paraIdx = g.paragraphIndex;
    int charCount = 0;
    for (size_t j = i; j < allGlyphs.size() && charCount < kMaxLookupChars; j++) {
      if (allGlyphs[j].paragraphIndex != paraIdx) break;
      encodeUtf8(allGlyphs[j].codepoint, text);
      charCount++;
    }

    // Try dictionary lookup to find match length
    WordLookupResult result;
    bool hasMatch = !text.empty() && WordLookup::lookup(text, 0, result);
    int matchChars = 0;
    if (hasMatch) {
      size_t pos = 0;
      while (pos < result.matchLength && pos < text.size()) {
        auto c = static_cast<unsigned char>(text[pos]);
        if (c < 0x80) pos += 1;
        else if ((c & 0xE0) == 0xC0) pos += 2;
        else if ((c & 0xF0) == 0xE0) pos += 3;
        else pos += 4;
        matchChars++;
      }
      if (matchChars > 1) {
        skipUntil = i + matchChars;
        // If a name match is followed by an honorific, extend skip to include it
        size_t afterMatch = i + matchChars;
        if (afterMatch < allGlyphs.size() && allGlyphs[afterMatch].paragraphIndex == paraIdx) {
          uint32_t nextCp = allGlyphs[afterMatch].codepoint;
          // さん、くん、ちゃん、さま、氏、様
          if (nextCp == 0x3055) { // さ → check for さん、さま
            if (afterMatch + 1 < allGlyphs.size()) {
              uint32_t nn = allGlyphs[afterMatch + 1].codepoint;
              if (nn == 0x3093) skipUntil = afterMatch + 2; // さん
              if (nn == 0x307E && afterMatch + 2 < allGlyphs.size() && allGlyphs[afterMatch + 2].paragraphIndex == paraIdx) skipUntil = afterMatch + 2; // さま
            }
          } else if (nextCp == 0x304F) { // く → くん
            if (afterMatch + 1 < allGlyphs.size() && allGlyphs[afterMatch + 1].codepoint == 0x3093)
              skipUntil = afterMatch + 2;
          } else if (nextCp == 0x3061) { // ち → ちゃん
            if (afterMatch + 2 < allGlyphs.size() && allGlyphs[afterMatch + 1].codepoint == 0x3083 && allGlyphs[afterMatch + 2].codepoint == 0x3093)
              skipUntil = afterMatch + 3;
          } else if (nextCp == 0x6C0F || nextCp == 0x69D8) { // 氏、様
            skipUntil = afterMatch + 1;
          }
        }
      }
    }

    // For filtered particles, only promote if a grammar dictionary match exists
    bool grammarPromoted = false;
    if (!lookupable && charCount >= 2 && Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
      for (int wLen = std::min(charCount, 10); wLen >= 2; wLen--) {
        size_t byteEnd = 0;
        int cnt = 0;
        for (size_t b = 0; b < text.size() && cnt < wLen; cnt++) {
          auto c = static_cast<unsigned char>(text[b]);
          if (c < 0x80) b += 1;
          else if ((c & 0xE0) == 0xC0) b += 2;
          else if ((c & 0xF0) == 0xE0) b += 3;
          else b += 4;
          byteEnd = b;
        }
        std::string window = text.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          grammarPromoted = true;
          if (wLen > 1) skipUntil = i + wLen;
          break;
        }
      }
    }

    // Only include positions that actually have a dictionary match
    if ((lookupable && hasMatch) || grammarPromoted) {
      selectToAllIdx.push_back(i);
      selectableGlyphs.push_back(g);
    }
  }
}

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
  // Find first position with a match
  const int maxIdx = static_cast<int>(selectableGlyphs.size()) - 1;
  for (cursorIndex = 0; cursorIndex <= maxIdx; cursorIndex++) {
    performLookup();
    if (hasResult) break;
  }
  if (cursorIndex > maxIdx) cursorIndex = 0;
  requestUpdate();
}

void EpubReaderWordLookupActivity::onExit() { Activity::onExit(); }

void EpubReaderWordLookupActivity::moveCursor(int delta) {
  if (selectableGlyphs.empty()) return;
  const int maxIdx = static_cast<int>(selectableGlyphs.size()) - 1;
  const int step = (delta > 0) ? 1 : -1;
  const int startIdx = cursorIndex;

  for (int attempts = 0; attempts < 30; attempts++) {
    cursorIndex += (attempts == 0) ? delta : step;
    if (cursorIndex < 0) { cursorIndex = 0; performLookup(); return; }
    if (cursorIndex > maxIdx) { cursorIndex = maxIdx; performLookup(); return; }
    if (cursorIndex == startIdx) { performLookup(); return; }
    performLookup();
    if (hasResult) return;
  }
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
  scrollOffset = 0;
  totalLines = 9999;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    hasResult = true;
    resultHeadword = std::move(result.entry.headword);
    resultDefinition = std::move(result.entry.definition);
    int chars = 0;
    size_t pos = 0;
    while (pos < result.matchLength && pos < text.size()) {
      auto c = static_cast<unsigned char>(text[pos]);
      if (c < 0x80) pos += 1;
      else if ((c & 0xE0) == 0xC0) pos += 2;
      else if ((c & 0xF0) == 0xE0) pos += 3;
      else pos += 4;
      chars++;
    }
    resultMatchLen = chars;

    // For short hiragana-only matches (≤3 chars), check if the grammar dict
    // has a better entry and promote it to the main result. Functional words
    // like こと, もの, よう get unhelpful JMdict hits ("ancient capital").
    if (chars <= 3 && Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
      bool allHiragana = true;
      for (size_t b = 0; b < result.matchLength && b < text.size();) {
        auto c = static_cast<unsigned char>(text[b]);
        uint32_t cp = 0;
        if (c < 0x80) { cp = c; b += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (text[b+1] & 0x3F); b += 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((text[b+1] & 0x3F) << 6) | (text[b+2] & 0x3F); b += 3; }
        else { b += 4; }
        if (cp < 0x3040 || cp > 0x309F) { allHiragana = false; break; }
      }
      if (allHiragana) {
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(resultHeadword.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          resultDefinition = std::move(gramEntry.definition);
        }
      }
    }
  }

  // Grammar scan: search for grammar patterns in a window around the cursor.
  // Try starting from a few characters BEFORE the cursor (to catch patterns
  // like ことになる when cursor is on こと) and also from the cursor itself.
  hasGrammar = false;
  grammarHeadword.clear();
  grammarDefinition.clear();
  if (Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
    const size_t allStart = selectToAllIdx[static_cast<size_t>(cursorIndex)];
    const uint32_t paraIdx = allGlyphs[allStart].paragraphIndex;

    // Try starting positions: cursor-3, cursor-2, cursor-1, cursor
    int bestGramLen = 0;
    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b = 0; b < backoff && scanStart > 0; b++) {
        scanStart--;
        if (allGlyphs[scanStart].paragraphIndex != paraIdx) { scanStart++; break; }
      }

      std::string gramText;
      int gCharCount = 0;
      for (size_t j = scanStart; j < allGlyphs.size() && gCharCount < 12; j++) {
        if (allGlyphs[j].paragraphIndex != paraIdx) break;
        encodeUtf8(allGlyphs[j].codepoint, gramText);
        gCharCount++;
      }

      for (int wLen = std::min(gCharCount, 10); wLen >= 2; wLen--) {
        size_t byteEnd = 0;
        int cnt = 0;
        for (size_t b = 0; b < gramText.size() && cnt < wLen; cnt++) {
          auto c = static_cast<unsigned char>(gramText[b]);
          if (c < 0x80) b += 1;
          else if ((c & 0xE0) == 0xC0) b += 2;
          else if ((c & 0xF0) == 0xE0) b += 3;
          else b += 4;
          byteEnd = b;
        }
        std::string window = gramText.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          if (gramEntry.headword != resultHeadword && wLen > bestGramLen) {
            bestGramLen = wLen;
            hasGrammar = true;
            grammarHeadword = std::move(gramEntry.headword);
            grammarDefinition = std::move(gramEntry.definition);
          }
          break;
        }
      }
    }
  }

  // Merge grammar into the definition so the single scroll-aware render loop
  // handles it (gets maxDefY clamping and scroll offset for free).
  if (hasGrammar) {
    resultDefinition += "\n\n— Grammar: " + grammarHeadword + " —\n" + grammarDefinition;
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
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] {
    if (hasResult && scrollOffset < maxScroll) { scrollOffset = std::min(maxScroll, scrollOffset + 5); requestUpdate(); }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] {
    if (scrollOffset > 0) { scrollOffset = std::max(0, scrollOffset - 5); requestUpdate(); }
  });
}

void EpubReaderWordLookupActivity::renderContentArea(const Rect& screen, int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  const int jaFont = SETTINGS.getReaderFontId();

  if (selectableGlyphs.empty() || !hasResult) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, tr(STR_NO_MATCH), true);
  } else {
    const int maxWidth = screen.width - metrics.contentSidePadding * 2;
    const int textX = screen.x + metrics.contentSidePadding;

    std::string posText = std::to_string(cursorIndex + 1) + "/" + std::to_string(selectableGlyphs.size());
    int defY;

    if (scrollOffset == 0) {
      renderer.drawText(SMALL_FONT_ID, textX, contentTop, posText.c_str(), true);
      int headY = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 2;
      renderer.drawText(jaFont, textX, headY, resultHeadword.c_str(), true, EpdFontFamily::BOLD);
      defY = headY + renderer.getLineHeight(jaFont) + metrics.verticalSpacing;
    } else {
      // When scrolled, skip the header to show more definition
      std::string scrollInfo = resultHeadword + " (" + posText + ") \xe2\x96\xb2";
      renderer.drawText(SMALL_FONT_ID, textX, contentTop, scrollInfo.c_str(), true);
      defY = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 4;
    }

    const int defFont = SMALL_FONT_ID;
    const int defLineH = renderer.getLineHeight(defFont);
    int linesDrawn = 0;
    int lineIndex = 0;
    // screen.height already excludes the button-hints band, so its bottom edge
    // is the top of the buttons; stay a hair above it.
    const int maxDefY = screen.y + screen.height - 2;
    const int firstDefY = defY;
    const int kMaxDefLines = 999;
    std::string defText = resultDefinition;
    size_t nlPos = 0;
    while (nlPos <= defText.size() && linesDrawn < kMaxDefLines) {
      size_t nextNl = defText.find('\n', nlPos);
      std::string paragraph = (nextNl == std::string::npos)
          ? defText.substr(nlPos) : defText.substr(nlPos, nextNl - nlPos);
      nlPos = (nextNl == std::string::npos) ? defText.size() + 1 : nextNl + 1;

      if (paragraph.empty()) {
        lineIndex++;
        if (lineIndex > scrollOffset) defY += defLineH / 2;
        continue;
      }
      // Try space-based wrapping first (for Latin text), then fall back to
      // character-level wrapping (for CJK text without spaces).
      std::string rem = paragraph;
      while (!rem.empty() && linesDrawn < kMaxDefLines) {
        if (renderer.getTextWidth(defFont, rem.c_str()) <= maxWidth) {
          lineIndex++;
          if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
            renderer.drawText(defFont, textX, defY, rem.c_str(), true);
            defY += defLineH;
            linesDrawn++;
          }
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
        lineIndex++;
        if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
          renderer.drawText(defFont, textX, defY, accum.c_str(), true);
          defY += defLineH;
          linesDrawn++;
        }
      }
    }

    totalLines = lineIndex;
    // Leave at least a screenful visible: max scroll = total - capacity
    const int visibleCapacity = (maxDefY - firstDefY) / defLineH;
    maxScroll = std::max(0, totalLines - visibleCapacity);
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
    // Clear from content top all the way to the physical bottom (including the
    // button-hint band margins, which screen.height excludes), then redraw hints.
    const int physBottom = renderer.getScreenHeight();
    renderer.fillRect(0, contentTop, renderer.getScreenWidth(), physBottom - contentTop, false);
    const auto labels2 = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels2.btn1, labels2.btn2, labels2.btn3, labels2.btn4);

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
