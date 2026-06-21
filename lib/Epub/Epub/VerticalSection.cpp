#include "VerticalSection.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <FsHelpers.h>

#include <cstring>
#include <string>

#include "Epub/converters/ImageDecoderFactory.h"
#include "GfxRenderer.h"

namespace {

constexpr uint8_t VSECTION_FILE_VERSION = 36;
constexpr size_t PARSE_BUFFER_SIZE = 1024;

using RubyRun = VerticalParsedText::RubyRun;

struct TextExtractor {
  // Each paragraph is a sequence of RubyRun entries. Unannotated text has
  // empty ruby; annotated text (<ruby>base<rt>reading</rt></ruby>) maps
  // base -> rubyText.
  struct ImageEntry {
    std::string src;
    size_t insertBeforeParagraph;
  };
  std::vector<ImageEntry> images;

  std::vector<std::vector<RubyRun>> paragraphs;
  std::vector<RubyRun> currentRuns;
  std::string currentText;
  int blockDepth = 0;
  int skipDepth = -1;

  // Ruby parsing state
  bool inRuby = false;
  bool inRt = false;
  bool inRp = false;
  std::string rubyBase;
  std::string rubyAnnotation;

  // Style tracking — each entry records the elementDepth at which
  // bold/italic was activated. On endElement, if we're leaving that
  // depth, pop and flush.
  int boldDepth = 0;
  int italicDepth = 0;
  int elementDepth = 0;
  static constexpr int MAX_STYLE_STACK = 8;
  int boldOpenedAtDepth[MAX_STYLE_STACK] = {};
  int boldStackSize = 0;
  int italicOpenedAtDepth[MAX_STYLE_STACK] = {};
  int italicStackSize = 0;
  int emphasisDepth = 0;
  int emphasisOpenedAtDepth[MAX_STYLE_STACK] = {};
  int emphasisStackSize = 0;

  bool hasEmphasis() const { return emphasisDepth > 0; }

  static bool isBoldTag(const char* name) {
    return strcasecmp(name, "b") == 0 || strcasecmp(name, "strong") == 0;
  }
  static bool isItalicTag(const char* name) {
    return strcasecmp(name, "i") == 0 || strcasecmp(name, "em") == 0;
  }
  uint8_t currentStyle() const {
    uint8_t s = 0;
    if (boldDepth > 0) s |= 1;    // EpdFontFamily::BOLD
    if (italicDepth > 0) s |= 2;  // EpdFontFamily::ITALIC
    return s;
  }

  static bool isSkipTag(const char* name) {
    return strcasecmp(name, "head") == 0 || strcasecmp(name, "style") == 0 || strcasecmp(name, "script") == 0;
  }

  void flushCurrentText() {
    if (!currentText.empty()) {
      currentRuns.push_back(RubyRun{std::move(currentText), {}, currentStyle(), hasEmphasis()});
      currentText.clear();
    }
  }

  void flushParagraph() {
    flushCurrentText();
    if (!currentRuns.empty()) {
      paragraphs.push_back(std::move(currentRuns));
      currentRuns.clear();
    }
  }

  static bool isBlockTag(const char* name) {
    static constexpr const char* blockTags[] = {"p",  "div", "h1", "h2",   "h3",  "h4",
                                                "h5", "h6",  "li", "blockquote", "section", "article"};
    for (const auto* tag : blockTags) {
      if (strcasecmp(name, tag) == 0) return true;
    }
    return false;
  }

  static bool hasClass(const char** atts, const char* cls) {
    if (!atts) return false;
    for (int i = 0; atts[i]; i += 2) {
      if (strcasecmp(atts[i], "class") == 0 && atts[i + 1]) {
        const char* val = atts[i + 1];
        const size_t clsLen = strlen(cls);
        while (*val) {
          while (*val == ' ') val++;
          if (strncasecmp(val, cls, clsLen) == 0 && (val[clsLen] == ' ' || val[clsLen] == '\0'))
            return true;
          while (*val && *val != ' ') val++;
        }
      }
    }
    return false;
  }

  static void XMLCALL startElement(void* userData, const char* name, const char** atts) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->elementDepth++;
    if (self->skipDepth >= 0) {
      self->skipDepth++;
      return;
    }
    if (isSkipTag(name)) {
      self->skipDepth = 1;
      return;
    }
    if (isBlockTag(name)) {
      if (self->blockDepth == 0) {
        self->flushParagraph();
      }
      self->blockDepth++;
    }
    if (strcasecmp(name, "ruby") == 0) {
      self->flushCurrentText();
      self->inRuby = true;
      self->rubyBase.clear();
      self->rubyAnnotation.clear();
    } else if (strcasecmp(name, "rt") == 0) {
      self->inRt = true;
      self->rubyAnnotation.clear();
    } else if (strcasecmp(name, "rp") == 0) {
      self->inRp = true;
    }
    if (isBoldTag(name) || hasClass(atts, "bold")) {
      self->flushCurrentText();
      self->boldDepth++;
      if (self->boldStackSize < MAX_STYLE_STACK)
        self->boldOpenedAtDepth[self->boldStackSize++] = self->elementDepth;
    }
    if (isItalicTag(name) || hasClass(atts, "italic")) {
      self->flushCurrentText();
      self->italicDepth++;
      if (self->italicStackSize < MAX_STYLE_STACK)
        self->italicOpenedAtDepth[self->italicStackSize++] = self->elementDepth;
    }
    if (hasClass(atts, "em-sesame") || hasClass(atts, "em-dot") || hasClass(atts, "em-circle") ||
        hasClass(atts, "em-sesame-open") || hasClass(atts, "em-dot-open") || hasClass(atts, "em-circle-open") ||
        hasClass(atts, "em-triangle") || hasClass(atts, "em-double-circle")) {
      self->flushCurrentText();
      self->emphasisDepth++;
      if (self->emphasisStackSize < MAX_STYLE_STACK)
        self->emphasisOpenedAtDepth[self->emphasisStackSize++] = self->elementDepth;
    }
    if (strcasecmp(name, "img") == 0 || strcasecmp(name, "image") == 0) {
      const char* src = nullptr;
      if (atts) {
        for (int i = 0; atts[i]; i += 2) {
          if (strcasecmp(atts[i], "src") == 0 || strcasecmp(atts[i], "xlink:href") == 0) {
            src = atts[i + 1];
            break;
          }
        }
      }
      if (src && src[0] != '\0') {
        self->flushParagraph();
        self->images.push_back(ImageEntry{std::string(src), self->paragraphs.size()});
      }
    }
    if (strcasecmp(name, "br") == 0 || strcasecmp(name, "br/") == 0) {
      if (!self->inRuby) {
        self->currentText.push_back('\n');
      }
    }
  }

  static void XMLCALL endElement(void* userData, const char* name) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->elementDepth--;
    if (self->skipDepth > 0) {
      self->skipDepth--;
      if (self->skipDepth == 0) self->skipDepth = -1;
      return;
    }
    if (strcasecmp(name, "rp") == 0) {
      self->inRp = false;
      return;
    }
    if (strcasecmp(name, "rt") == 0) {
      self->inRt = false;
      // Emit a RubyRun for the base text accumulated so far with this annotation.
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), std::move(self->rubyAnnotation), self->currentStyle(), self->hasEmphasis()});
        self->rubyBase.clear();
      }
      self->rubyAnnotation.clear();
      return;
    }
    if (strcasecmp(name, "ruby") == 0) {
      // Flush any remaining base text that had no <rt> (malformed markup).
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), {}, self->currentStyle(), self->hasEmphasis()});
        self->rubyBase.clear();
      }
      self->inRuby = false;
      return;
    }
    if (self->boldStackSize > 0 && self->boldOpenedAtDepth[self->boldStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->boldDepth--;
      self->boldStackSize--;
    }
    if (self->italicStackSize > 0 && self->italicOpenedAtDepth[self->italicStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->italicDepth--;
      self->italicStackSize--;
    }
    if (self->emphasisStackSize > 0 && self->emphasisOpenedAtDepth[self->emphasisStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->emphasisDepth--;
      self->emphasisStackSize--;
    }
    if (isBlockTag(name)) {
      self->blockDepth--;
      if (self->blockDepth <= 0) {
        self->blockDepth = 0;
        self->flushParagraph();
      }
    }
  }

  static void XMLCALL characterData(void* userData, const char* s, int len) {
    auto* self = static_cast<TextExtractor*>(userData);
    if (self->skipDepth >= 0) return;
    if (self->inRp) return;
    if (self->inRt) {
      self->rubyAnnotation.append(s, static_cast<size_t>(len));
    } else if (self->inRuby) {
      self->rubyBase.append(s, static_cast<size_t>(len));
    } else {
      self->currentText.append(s, static_cast<size_t>(len));
    }
  }

  static void XMLCALL defaultHandler(void* userData, const char* s, int len) {
    if (len >= 4 && s[0] == '&') {
      auto* self = static_cast<TextExtractor*>(userData);
      std::string entity(s, static_cast<size_t>(len));
      std::string resolved;
      if (entity == "&nbsp;") {
        resolved = " ";
      } else if (entity == "&mdash;") {
        resolved = "\xe2\x80\x94";
      } else if (entity == "&ndash;") {
        resolved = "\xe2\x80\x93";
      } else if (entity == "&hellip;") {
        resolved = "\xe2\x80\xa6";
      } else if (entity == "&amp;") {
        resolved = "&";
      } else if (entity == "&lt;") {
        resolved = "<";
      } else if (entity == "&gt;") {
        resolved = ">";
      } else if (entity == "&quot;") {
        resolved = "\"";
      } else if (entity == "&apos;") {
        resolved = "'";
      } else {
        return;
      }

      if (self->inRp) return;
      if (self->inRt) {
        self->rubyAnnotation.append(resolved);
      } else if (self->inRuby) {
        self->rubyBase.append(resolved);
      } else {
        self->currentText.append(resolved);
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

  extractor.flushParagraph();

  if (extractor.paragraphs.empty() && extractor.images.empty()) {
    pages.clear();
    pageCount = 0;
    return true;
  }

  // Resolve image paths relative to the chapter's directory in the EPUB.
  const auto& spineItem = epub->getSpineItem(spineIndex);
  std::string chapterDir;
  {
    const size_t slash = spineItem.href.rfind('/');
    if (slash != std::string::npos) chapterDir = spineItem.href.substr(0, slash + 1);
  }

  const std::string imageBasePath = epub->getCachePath() + "/img_v" + std::to_string(spineIndex) + "_";

  auto makeImagePage = [&](const TextExtractor::ImageEntry& img, size_t imgIdx) -> VerticalPage {
    std::string resolvedSrc = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(chapterDir + img.src));

    // Determine extension and cached path
    std::string ext;
    const size_t extPos = resolvedSrc.rfind('.');
    if (extPos != std::string::npos) ext = resolvedSrc.substr(extPos);
    const std::string cachedPath = imageBasePath + std::to_string(imgIdx) + ext;

    // Extract image from EPUB to cache if not already present
    if (!Storage.exists(cachedPath.c_str())) {
      HalFile cachedFile;
      if (Storage.openFileForWrite("VSC", cachedPath, cachedFile)) {
        epub->readItemContentsToStream(resolvedSrc, cachedFile, 4096);
        cachedFile.flush();
        cachedFile.close();
      }
    }

    // Get actual image dimensions and scale to fit viewport preserving aspect ratio.
    // Landscape images (wider than tall) are rotated 90° CW to fill the portrait screen.
    int displayW = viewportWidth;
    int displayH = viewportHeight;
    bool rotated = false;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedPath);
    if (decoder) {
      ImageDimensions dims = {0, 0};
      if (decoder->getDimensions(cachedPath, dims) && dims.width > 0 && dims.height > 0) {
        // Rotate when the image orientation doesn't match the viewport:
        // portrait viewport + landscape image, or landscape viewport + portrait image.
        const bool viewportIsPortrait = (viewportHeight > viewportWidth);
        const bool imageIsLandscape = (dims.width > dims.height);
        if (viewportIsPortrait == imageIsLandscape) {
          // Image doesn't match viewport — rotate 90° CW to fill screen better.
          const float scaleX = static_cast<float>(viewportWidth) / dims.height;
          const float scaleY = static_cast<float>(viewportHeight) / dims.width;
          const float scale = (scaleX < scaleY) ? scaleX : scaleY;
          displayW = static_cast<int>(dims.height * scale + 0.5f);
          displayH = static_cast<int>(dims.width * scale + 0.5f);
          rotated = true;
        } else {
          const float scaleX = static_cast<float>(viewportWidth) / dims.width;
          const float scaleY = static_cast<float>(viewportHeight) / dims.height;
          const float scale = (scaleX < scaleY) ? scaleX : scaleY;
          displayW = static_cast<int>(dims.width * scale + 0.5f);
          displayH = static_cast<int>(dims.height * scale + 0.5f);
        }
        if (displayW < 1) displayW = 1;
        if (displayH < 1) displayH = 1;
      }
    }

    VerticalPage page;
    page.imagePath = cachedPath;
    page.imageWidth = static_cast<int16_t>(displayW);
    page.imageHeight = static_cast<int16_t>(displayH);
    page.imageRotated = rotated;
    return page;
  };

  VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
  const int lineH = renderer.getLineHeight(fontId);
  layout.setColumnGapPx((lineH / 3) < 4 ? 4 : (lineH / 3));
  bool hasRuby = false;
  for (const auto& para : extractor.paragraphs) {
    for (const auto& run : para) {
      if (!run.rubyText.empty()) { hasRuby = true; break; }
    }
    if (hasRuby) break;
  }
  if (hasRuby) {
    layout.setColumnGapPx(lineH * 2 / 3);
    layout.setRightPaddingPx((lineH / 2) < 2 ? 2 : (lineH / 2));
  }

  // Layout text paragraphs, inserting image pages at the right positions.
  size_t nextImageIdx = 0;
  for (size_t paraIdx = 0; paraIdx <= extractor.paragraphs.size(); paraIdx++) {
    // Insert any images that belong before this paragraph
    while (nextImageIdx < extractor.images.size() &&
           extractor.images[nextImageIdx].insertBeforeParagraph <= paraIdx) {
      // Flush any pending text pages before inserting the image
      {
        auto textPages = layout.layoutPages();
        pages.insert(pages.end(), std::make_move_iterator(textPages.begin()),
                     std::make_move_iterator(textPages.end()));
        layout.reset();
      }
      pages.push_back(makeImagePage(extractor.images[nextImageIdx], nextImageIdx));
      nextImageIdx++;
    }
    if (paraIdx < extractor.paragraphs.size()) {
      layout.addAnnotatedParagraph(extractor.paragraphs[paraIdx]);
    }
  }
  // Flush remaining text pages
  auto remainingPages = layout.layoutPages();
  pages.insert(pages.end(), std::make_move_iterator(remainingPages.begin()),
               std::make_move_iterator(remainingPages.end()));

  pageCount = static_cast<uint16_t>(pages.size());
  LOG_DBG("VSC", "Laid out %zu paragraphs + %zu images into %u pages",
          extractor.paragraphs.size(), extractor.images.size(), pageCount);
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
    const bool isImg = page.isImagePage();
    serialization::writePod(file, isImg);
    if (isImg) {
      serialization::writeString(file, page.imagePath);
      serialization::writePod(file, page.imageWidth);
      serialization::writePod(file, page.imageHeight);
      serialization::writePod(file, page.imageRotated);
      continue;
    }
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
      serialization::writePod(file, g.renderKind);
      serialization::writePod(file, g.style);
      serialization::writePod(file, g.emphasis);

      if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
        const auto runLen = static_cast<uint16_t>(g.rotatedRunText.size());
        serialization::writePod(file, runLen);
        if (runLen > 0) {
          file.write(reinterpret_cast<const uint8_t*>(g.rotatedRunText.data()), runLen);
        }
      }

      const auto rubyLen = static_cast<uint16_t>(g.rubyText.size());
      serialization::writePod(file, rubyLen);
      if (rubyLen > 0) {
        file.write(reinterpret_cast<const uint8_t*>(g.rubyText.data()), rubyLen);
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
    bool isImg = false;
    serialization::readPod(file, isImg);
    if (isImg) {
      serialization::readString(file, page.imagePath);
      serialization::readPod(file, page.imageWidth);
      serialization::readPod(file, page.imageHeight);
      serialization::readPod(file, page.imageRotated);
      pages.push_back(std::move(page));
      continue;
    }
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
      serialization::readPod(file, g.renderKind);
      serialization::readPod(file, g.style);
      serialization::readPod(file, g.emphasis);

      if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
        uint16_t runLen;
        serialization::readPod(file, runLen);
        if (runLen > 0) {
          g.rotatedRunText.resize(runLen);
          file.read(reinterpret_cast<uint8_t*>(g.rotatedRunText.data()), runLen);
        }
      }

      uint16_t rubyLen;
      serialization::readPod(file, rubyLen);
      if (rubyLen > 0) {
        g.rubyText.resize(rubyLen);
        file.read(reinterpret_cast<uint8_t*>(g.rubyText.data()), rubyLen);
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
