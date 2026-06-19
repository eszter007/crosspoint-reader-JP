#include "VerticalSection.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <cstring>
#include <string>

#include "GfxRenderer.h"

namespace {

constexpr uint8_t VSECTION_FILE_VERSION = 1;
constexpr size_t PARSE_BUFFER_SIZE = 1024;

struct TextExtractor {
  std::vector<std::string> paragraphs;
  std::string currentText;
  int blockDepth = 0;

  static bool isBlockTag(const char* name) {
    static constexpr const char* blockTags[] = {"p",  "div", "h1", "h2",   "h3",  "h4",
                                                "h5", "h6",  "li", "blockquote", "section", "article"};
    for (const auto* tag : blockTags) {
      if (strcasecmp(name, tag) == 0) return true;
    }
    return false;
  }

  static void XMLCALL startElement(void* userData, const char* name, const char** /*atts*/) {
    auto* self = static_cast<TextExtractor*>(userData);
    if (isBlockTag(name)) {
      if (self->blockDepth == 0 && !self->currentText.empty()) {
        self->paragraphs.push_back(std::move(self->currentText));
        self->currentText.clear();
      }
      self->blockDepth++;
    }
    if (strcasecmp(name, "br") == 0 || strcasecmp(name, "br/") == 0) {
      self->currentText.push_back('\n');
    }
  }

  static void XMLCALL endElement(void* userData, const char* name) {
    auto* self = static_cast<TextExtractor*>(userData);
    if (isBlockTag(name)) {
      self->blockDepth--;
      if (self->blockDepth <= 0) {
        self->blockDepth = 0;
        if (!self->currentText.empty()) {
          self->paragraphs.push_back(std::move(self->currentText));
          self->currentText.clear();
        }
      }
    }
  }

  static void XMLCALL characterData(void* userData, const char* s, int len) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->currentText.append(s, static_cast<size_t>(len));
  }

  static void XMLCALL defaultHandler(void* userData, const char* s, int len) {
    // Handle &nbsp; and similar HTML entities that expat can't resolve on its own.
    if (len >= 4 && s[0] == '&') {
      auto* self = static_cast<TextExtractor*>(userData);
      std::string entity(s, static_cast<size_t>(len));
      if (entity == "&nbsp;") {
        self->currentText.push_back(' ');
      } else if (entity == "&mdash;") {
        self->currentText.append("\xe2\x80\x94");
      } else if (entity == "&ndash;") {
        self->currentText.append("\xe2\x80\x93");
      } else if (entity == "&hellip;") {
        self->currentText.append("\xe2\x80\xa6");
      } else if (entity == "&amp;") {
        self->currentText.push_back('&');
      } else if (entity == "&lt;") {
        self->currentText.push_back('<');
      } else if (entity == "&gt;") {
        self->currentText.push_back('>');
      } else if (entity == "&quot;") {
        self->currentText.push_back('"');
      } else if (entity == "&apos;") {
        self->currentText.push_back('\'');
      }
    }
  }
};

}  // namespace

bool VerticalSection::extractParagraphsAndLayout(const int fontId, const uint16_t viewportWidth,
                                                  const uint16_t viewportHeight) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_v" + std::to_string(spineIndex) + ".html";

  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      delay(50);
    }
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
    HalFile tmpHtml;
    if (!Storage.openFileForWrite("VSC", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, PARSE_BUFFER_SIZE);
    tmpHtml.close();
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  }

  if (!success) {
    LOG_ERR("VSC", "Failed to stream chapter HTML");
    return false;
  }

  TextExtractor extractor;
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("VSC", "OOM: XML parser");
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  XML_SetDefaultHandlerExpand(parser, TextExtractor::defaultHandler);
  XML_SetUserData(parser, &extractor);
  XML_SetElementHandler(parser, TextExtractor::startElement, TextExtractor::endElement);
  XML_SetCharacterDataHandler(parser, TextExtractor::characterData);

  HalFile htmlFile;
  if (!Storage.openFileForRead("VSC", tmpHtmlPath, htmlFile)) {
    destroyXmlParser(parser);
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  bool parseOk = true;
  int done;
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("VSC", "OOM: parse buffer");
      parseOk = false;
      break;
    }
    const size_t len = htmlFile.read(buf, PARSE_BUFFER_SIZE);
    if (len == 0 && htmlFile.available() > 0) {
      LOG_ERR("VSC", "File read error");
      parseOk = false;
      break;
    }
    done = htmlFile.available() == 0;
    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("VSC", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
      break;
    }
  } while (!done);

  htmlFile.close();
  destroyXmlParser(parser);
  Storage.remove(tmpHtmlPath.c_str());

  if (!parseOk) return false;

  if (!extractor.currentText.empty()) {
    extractor.paragraphs.push_back(std::move(extractor.currentText));
  }

  if (extractor.paragraphs.empty()) {
    pages.clear();
    pageCount = 0;
    return true;
  }

  VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
  for (const auto& para : extractor.paragraphs) {
    layout.addParagraph(para);
  }
  pages = layout.layoutPages();
  pageCount = static_cast<uint16_t>(pages.size());
  LOG_DBG("VSC", "Laid out %zu paragraphs into %u pages", extractor.paragraphs.size(), pageCount);
  return true;
}

bool VerticalSection::saveToCache(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  const auto vsectionsDir = epub->getCachePath() + "/vsections";
  Storage.mkdir(vsectionsDir.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("VSC", filePath, file)) {
    return false;
  }

  serialization::writePod(file, VSECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, pageCount);

  for (const auto& page : pages) {
    const auto glyphCount = static_cast<uint32_t>(page.glyphs.size());
    serialization::writePod(file, glyphCount);
    serialization::writePod(file, page.columnCount);
    serialization::writePod(file, page.rowsPerColumn);

    for (const auto& g : page.glyphs) {
      serialization::writePod(file, g.codepoint);
      serialization::writePod(file, g.column);
      serialization::writePod(file, g.row);
      serialization::writePod(file, g.x);
      serialization::writePod(file, g.y);
      serialization::writePod(file, g.paragraphIndex);
      serialization::writePod(file, g.byteOffset);
      serialization::writePod(file, g.rotated);

      if (g.rotated) {
        const auto runLen = static_cast<uint16_t>(g.rotatedRunText.size());
        serialization::writePod(file, runLen);
        if (runLen > 0) {
          file.write(reinterpret_cast<const uint8_t*>(g.rotatedRunText.data()), runLen);
        }
      }
    }
  }

  file.close();
  LOG_DBG("VSC", "Cached %u vertical pages", pageCount);
  return true;
}

bool VerticalSection::loadFromCache(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != VSECTION_FILE_VERSION) {
    file.close();
    LOG_DBG("VSC", "Version mismatch: %u vs %u", version, VSECTION_FILE_VERSION);
    clearCache();
    return false;
  }

  int cachedFontId;
  uint16_t cachedWidth, cachedHeight;
  serialization::readPod(file, cachedFontId);
  serialization::readPod(file, cachedWidth);
  serialization::readPod(file, cachedHeight);

  if (cachedFontId != fontId || cachedWidth != viewportWidth || cachedHeight != viewportHeight) {
    file.close();
    LOG_DBG("VSC", "Parameter mismatch, clearing cache");
    clearCache();
    return false;
  }

  uint16_t cachedPageCount;
  serialization::readPod(file, cachedPageCount);

  pages.clear();
  pages.reserve(cachedPageCount);

  for (uint16_t p = 0; p < cachedPageCount; p++) {
    VerticalPage page;
    uint32_t glyphCount;
    serialization::readPod(file, glyphCount);
    serialization::readPod(file, page.columnCount);
    serialization::readPod(file, page.rowsPerColumn);

    page.glyphs.reserve(glyphCount);
    for (uint32_t gi = 0; gi < glyphCount; gi++) {
      VerticalGlyph g;
      serialization::readPod(file, g.codepoint);
      serialization::readPod(file, g.column);
      serialization::readPod(file, g.row);
      serialization::readPod(file, g.x);
      serialization::readPod(file, g.y);
      serialization::readPod(file, g.paragraphIndex);
      serialization::readPod(file, g.byteOffset);
      serialization::readPod(file, g.rotated);

      if (g.rotated) {
        uint16_t runLen;
        serialization::readPod(file, runLen);
        if (runLen > 0) {
          g.rotatedRunText.resize(runLen);
          file.read(reinterpret_cast<uint8_t*>(g.rotatedRunText.data()), runLen);
        }
      }
      page.glyphs.push_back(std::move(g));
    }
    pages.push_back(std::move(page));
  }

  file.close();
  pageCount = cachedPageCount;
  LOG_DBG("VSC", "Loaded %u vertical pages from cache", pageCount);
  return true;
}

bool VerticalSection::loadSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  return loadFromCache(fontId, viewportWidth, viewportHeight);
}

bool VerticalSection::createSectionFile(const int fontId, const uint16_t viewportWidth,
                                         const uint16_t viewportHeight) {
  if (!extractParagraphsAndLayout(fontId, viewportWidth, viewportHeight)) {
    return false;
  }
  return saveToCache(fontId, viewportWidth, viewportHeight);
}

bool VerticalSection::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }
  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("VSC", "Failed to clear cache");
    return false;
  }
  LOG_DBG("VSC", "Cache cleared");
  return true;
}

const VerticalPage* VerticalSection::getPage() const {
  if (currentPage < 0 || currentPage >= static_cast<int>(pages.size())) {
    return nullptr;
  }
  return &pages[currentPage];
}
