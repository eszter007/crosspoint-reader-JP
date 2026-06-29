#include "MangaPanel.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace manga {

static constexpr size_t IDX_HEADER_SIZE = 8;
static constexpr size_t IDX_RECORD_SIZE = 12;
static constexpr size_t PANEL_HEADER_SIZE = 10;
static constexpr size_t TEXT_HEADER_SIZE = 10;

static uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool MangaBook::isMangaFolder(const std::string& folderPath) {
  std::string idxPath = folderPath;
  if (idxPath.back() != '/') idxPath += '/';
  idxPath += "panels.idx";
  return Storage.exists(idxPath.c_str());
}

std::string MangaBook::getTitle() const {
  auto pos = folderPath.find_last_of('/');
  if (pos != std::string::npos && pos + 1 < folderPath.size()) {
    return folderPath.substr(pos + 1);
  }
  return folderPath;
}

std::string MangaBook::getCachePath() const {
  size_t hash = std::hash<std::string>{}(folderPath);
  char buf[64];
  snprintf(buf, sizeof(buf), "/.crosspoint/manga_%zu", hash);
  return std::string(buf);
}

bool MangaBook::load() {
  if (!loadIndex()) return false;
  scanImages();
  return true;
}

bool MangaBook::loadIndex() {
  std::string idxPath = folderPath;
  if (idxPath.back() != '/') idxPath += '/';
  idxPath += "panels.idx";

  HalFile f;
  if (!Storage.openFileForRead("MNG", idxPath, f)) {
    LOG_ERR("MNG", "Cannot open panels.idx: %s", idxPath.c_str());
    return false;
  }

  uint8_t header[IDX_HEADER_SIZE];
  if (f.read(header, IDX_HEADER_SIZE) != static_cast<int>(IDX_HEADER_SIZE)) {
    LOG_ERR("MNG", "Short read on idx header");
    return false;
  }

  uint32_t version = readU32(header);
  if (version != FORMAT_VERSION) {
    LOG_ERR("MNG", "Unsupported panel format version: %u", version);
    return false;
  }

  pageCount = readU32(header + 4);
  if (pageCount == 0 || pageCount > 10000) {
    LOG_ERR("MNG", "Invalid page count: %u", pageCount);
    return false;
  }

  pageIndex.clear();
  pageIndex.reserve(pageCount);

  uint8_t rec[IDX_RECORD_SIZE];
  for (uint32_t i = 0; i < pageCount; i++) {
    if (f.read(rec, IDX_RECORD_SIZE) != static_cast<int>(IDX_RECORD_SIZE)) {
      LOG_ERR("MNG", "Short read on idx record %u", i);
      return false;
    }
    PageInfo pi;
    pi.dataOffset = readU32(rec);
    pi.dataLength = readU32(rec + 4);
    pi.imgWidth = readU16(rec + 8);
    pi.imgHeight = readU16(rec + 10);
    pageIndex.push_back(pi);
  }

  LOG_DBG("MNG", "Loaded panels.idx: %u pages", pageCount);
  return true;
}

bool MangaBook::scanImages() {
  imageFiles.clear();

  std::string dirPath = folderPath;
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    LOG_ERR("MNG", "Cannot open manga folder: %s", dirPath.c_str());
    return false;
  }

  char name[200];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string_view sv(name);
        if (FsHelpers::hasBmpExtension(sv)) {
          imageFiles.emplace_back(name);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(imageFiles);
  LOG_DBG("MNG", "Found %u BMP images in %s", (unsigned)imageFiles.size(), dirPath.c_str());
  return true;
}

std::string MangaBook::getPageImagePath(uint32_t pageIdx) const {
  if (pageIdx >= imageFiles.size()) return "";
  std::string path = folderPath;
  if (path.back() != '/') path += '/';
  path += imageFiles[pageIdx];
  return path;
}

uint16_t MangaBook::getPageImgWidth(uint32_t pageIdx) const {
  if (pageIdx >= pageIndex.size()) return 0;
  return pageIndex[pageIdx].imgWidth;
}

uint16_t MangaBook::getPageImgHeight(uint32_t pageIdx) const {
  if (pageIdx >= pageIndex.size()) return 0;
  return pageIndex[pageIdx].imgHeight;
}

bool MangaBook::loadPagePanels(uint32_t pageIdx, std::vector<Panel>& panels) const {
  panels.clear();
  if (pageIdx >= pageIndex.size()) return false;

  const auto& pi = pageIndex[pageIdx];
  if (pi.dataLength == 0) return true;

  std::string datPath = folderPath;
  if (datPath.back() != '/') datPath += '/';
  datPath += "panels.dat";

  HalFile f;
  if (!Storage.openFileForRead("MNG", datPath, f)) {
    LOG_ERR("MNG", "Cannot open panels.dat");
    return false;
  }

  if (!f.seekSet(pi.dataOffset)) {
    LOG_ERR("MNG", "Seek failed to offset %u", pi.dataOffset);
    return false;
  }

  if (pi.dataLength > 32768) {
    LOG_ERR("MNG", "Page data too large: %u bytes", pi.dataLength);
    return false;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(pi.dataLength);
  if (!buf) {
    LOG_ERR("MNG", "OOM: %u bytes for page data", pi.dataLength);
    return false;
  }

  if (f.read(buf.get(), pi.dataLength) != static_cast<int>(pi.dataLength)) {
    LOG_ERR("MNG", "Short read on page data");
    return false;
  }

  size_t pos = 0;
  if (pos + 2 > pi.dataLength) return false;
  uint8_t panelCount = buf[pos];
  pos += 2;

  panels.reserve(panelCount);

  for (uint8_t p = 0; p < panelCount; p++) {
    if (pos + PANEL_HEADER_SIZE > pi.dataLength) {
      LOG_ERR("MNG", "Panel header overrun at panel %u", p);
      return false;
    }

    Panel panel;
    panel.x = readU16(buf.get() + pos);
    panel.y = readU16(buf.get() + pos + 2);
    panel.w = readU16(buf.get() + pos + 4);
    panel.h = readU16(buf.get() + pos + 6);
    uint8_t textCount = buf[pos + 8];
    pos += PANEL_HEADER_SIZE;

    panel.textBlocks.reserve(textCount);

    for (uint8_t t = 0; t < textCount; t++) {
      if (pos + TEXT_HEADER_SIZE > pi.dataLength) {
        LOG_ERR("MNG", "Text header overrun at text %u", t);
        return false;
      }

      TextBlock tb;
      tb.x = readU16(buf.get() + pos);
      tb.y = readU16(buf.get() + pos + 2);
      tb.w = readU16(buf.get() + pos + 4);
      tb.h = readU16(buf.get() + pos + 6);
      uint16_t textLen = readU16(buf.get() + pos + 8);
      pos += TEXT_HEADER_SIZE;

      if (pos + textLen > pi.dataLength) {
        LOG_ERR("MNG", "Text data overrun: need %u bytes", textLen);
        return false;
      }

      tb.text.assign(reinterpret_cast<const char*>(buf.get() + pos), textLen);
      pos += textLen;
      panel.textBlocks.push_back(std::move(tb));
    }

    panels.push_back(std::move(panel));
  }

  return true;
}

}  // namespace manga
