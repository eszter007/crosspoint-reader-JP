#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

namespace manga {

static constexpr uint32_t FORMAT_VERSION = 2;  // v2 adds per-panel translation string

struct TextBlock {
  uint16_t x, y, w, h;
  std::string text;
};

struct Panel {
  uint16_t x, y, w, h;
  std::vector<TextBlock> textBlocks;
  std::string translation;  // pre-extracted English translation, may be empty
};

struct PageInfo {
  uint32_t dataOffset;
  uint32_t dataLength;
  uint16_t imgWidth;
  uint16_t imgHeight;
};

class MangaBook {
 public:
  explicit MangaBook(std::string folderPath) : folderPath(std::move(folderPath)) {}

  bool load();

  uint32_t getPageCount() const { return pageCount; }
  const std::string& getFolder() const { return folderPath; }
  std::string getTitle() const;

  bool loadPagePanels(uint32_t pageIndex, std::vector<Panel>& panels) const;
  uint16_t getPageImgWidth(uint32_t pageIndex) const;
  uint16_t getPageImgHeight(uint32_t pageIndex) const;

  std::string getPageImagePath(uint32_t pageIndex) const;

  std::string getCachePath() const;

  static bool isMangaFolder(const std::string& folderPath);

 private:
  std::string folderPath;
  uint32_t pageCount = 0;
  std::vector<PageInfo> pageIndex;
  std::vector<std::string> imageFiles;

  bool loadIndex();
  bool scanImages();
};

}  // namespace manga
