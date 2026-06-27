#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "VerticalParsedText.h"

class GfxRenderer;

class VerticalSection {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  std::vector<VerticalPage> pages;

  bool extractParagraphsAndLayout(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveToCache(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool loadFromCache(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit VerticalSection(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/vsections/" + std::to_string(spineIndex) + ".bin") {}

  bool loadSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool createSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight);
  bool clearCache() const;
  const VerticalPage* getPage() const;
  const VerticalPage* getPage(int pageIndex) const;
};
