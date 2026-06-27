#pragma once

#include <string>

struct VerticalPage;
class Page;

namespace PageTextExtractor {

// Extract plain UTF-8 text from a vertical page's glyph stream.
// Inserts newlines at paragraph boundaries. Skips rotated-run duplicate glyphs.
std::string fromVerticalPage(const VerticalPage& page);

// Extract plain UTF-8 text from a horizontal page's element tree.
std::string fromHorizontalPage(const Page& page);

}  // namespace PageTextExtractor
