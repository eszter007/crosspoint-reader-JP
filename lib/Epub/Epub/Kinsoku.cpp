#include "Kinsoku.h"

namespace Kinsoku {

namespace {

// Closing brackets / quotation marks (both halfwidth-in-fullwidth-box JIS
// punctuation and the CJK bracket block).
bool isClosingBracket(uint32_t cp) {
  switch (cp) {
    case 0x3009: // 〉
    case 0x300B: // 》
    case 0x300D: // 」
    case 0x300F: // 』
    case 0x3011: // 】
    case 0x3015: // 〕
    case 0x3017: // 〗
    case 0x3019: // 〙
    case 0x301B: // 〛
    case 0xFF09: // ）
    case 0xFF3D: // ］
    case 0xFF5D: // ｝
    case 0x2019: // ’
    case 0x201D: // ”
      return true;
    default:
      return false;
  }
}

bool isOpeningBracket(uint32_t cp) {
  switch (cp) {
    case 0x3008: // 〈
    case 0x300A: // 《
    case 0x300C: // 「
    case 0x300E: // 『
    case 0x3010: // 【
    case 0x3014: // 〔
    case 0x3016: // 〖
    case 0x3018: // 〘
    case 0x301A: // 〚
    case 0xFF08: // （
    case 0xFF3B: // ［
    case 0xFF5B: // ｛
    case 0x2018: // ‘
    case 0x201C: // “
      return true;
    default:
      return false;
  }
}

// Small (yoon/sokuon) kana that cannot start a line, plus the
// chouonpu (long vowel mark) and iteration marks.
bool isSmallKanaOrMark(uint32_t cp) {
  switch (cp) {
    case 0x3041: // ぁ
    case 0x3043: // ぃ
    case 0x3045: // ぅ
    case 0x3047: // ぇ
    case 0x3049: // ぉ
    case 0x3063: // っ
    case 0x3083: // ゃ
    case 0x3085: // ゅ
    case 0x3087: // ょ
    case 0x308E: // ゎ
    case 0x30A1: // ァ
    case 0x30A3: // ィ
    case 0x30A5: // ゥ
    case 0x30A7: // ェ
    case 0x30A9: // ォ
    case 0x30C3: // ッ
    case 0x30E3: // ャ
    case 0x30E5: // ュ
    case 0x30E7: // ョ
    case 0x30EE: // ヮ
    case 0x30F5: // ヵ
    case 0x30F6: // ヶ
    case 0x30FC: // ー (chouonpu)
    case 0x3005: // 々
    case 0x309D: // ゝ
    case 0x309E: // ゞ
    case 0x30FD: // ヽ
    case 0x30FE: // ヾ
      return true;
    default:
      return false;
  }
}

// Ideographic / fullwidth punctuation that can't start a line: commas,
// fullstops, middle dot, question/exclamation marks, prolonged sound marks.
bool isLineStartPunctuation(uint32_t cp) {
  switch (cp) {
    case 0x3001: // 、
    case 0x3002: // 。
    case 0x30FB: // ・
    case 0xFF0C: // ，
    case 0xFF0E: // ．
    case 0xFF1A: // ：
    case 0xFF1B: // ；
    case 0xFF1F: // ？
    case 0xFF01: // ！
    case 0x2026: // … (ellipsis)
    case 0x2025: // ‥
      return true;
    default:
      return false;
  }
}

} // namespace

bool isLineStartProhibited(uint32_t codepoint) {
  return isClosingBracket(codepoint) || isSmallKanaOrMark(codepoint) || isLineStartPunctuation(codepoint);
}

bool isLineEndProhibited(uint32_t codepoint) {
  return isOpeningBracket(codepoint);
}

bool isAlwaysUpright(uint32_t codepoint) {
  // CJK ideographs, hiragana, katakana, and ideographic punctuation are
  // always drawn upright in tategaki, regardless of the kinsoku rules
  // above (kinsoku only governs *position*, not orientation).
  if (codepoint >= 0x3040 && codepoint <= 0x30FF) return true; // Hiragana + Katakana
  if (codepoint >= 0x3400 && codepoint <= 0x9FFF) return true; // CJK Unified + Ext A
  if (codepoint >= 0xF900 && codepoint <= 0xFAFF) return true; // CJK Compat Ideographs
  if (codepoint >= 0x3000 && codepoint <= 0x303F) return true; // CJK punctuation
  if (codepoint >= 0xFF00 && codepoint <= 0xFFEF) return true; // Fullwidth forms
  return false;
}

bool isRotatedRunCharacter(uint32_t codepoint) {
  // Basic Latin letters, digits, and a handful of common inline symbols.
  // Anything in this set gets batched together and rendered sideways via
  // drawTextRotated90CW so an embedded English word or number reads
  // left-to-right when the reader tilts their head (or the device),
  // matching standard tategaki convention.
  if (codepoint >= 'A' && codepoint <= 'Z') return true;
  if (codepoint >= 'a' && codepoint <= 'z') return true;
  if (codepoint >= '0' && codepoint <= '9') return true;
  if (codepoint == ' ') return true; // preserve spacing inside embedded Latin phrases/numbers
  switch (codepoint) {
    case '.':
    case ',':
    case '-':
    case '/':
    case ':':
    case '%':
    case '+':
    case '#':
    case '@':
      return true;
    default:
      return false;
  }
}

} // namespace Kinsoku
