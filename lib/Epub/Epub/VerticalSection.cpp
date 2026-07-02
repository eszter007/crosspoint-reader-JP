#include "VerticalSection.h"

#include <Arduino.h>
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

// v38: bumped past every cache written during this session's debugging (v37 covers several
// low-memory bugs, since fixed, that silently dropped glyphs into the cache on save -- a device
// that already wrote a v37 cache for this book must not reuse it) -- see cache format comment below
constexpr uint8_t VSECTION_FILE_VERSION = 38;
constexpr size_t PARSE_BUFFER_SIZE = 1024;

using RubyRun = VerticalParsedText::RubyRun;

// Receives extraction output in document order, one paragraph or image at a time. The extractor
// deliberately has no whole-chapter storage: a real Japanese chapter's text plus its laid-out
// glyph pages runs to megabytes, which can never fit in the ESP32-C3's ~220KB heap (the previous
// accumulate-everything design only worked in the desktop emulator's 8MB heap). See
// VerticalSection.h for the full memory model.
struct ParagraphSink {
  virtual ~ParagraphSink() = default;
  // Takes ownership of the runs; the extractor's buffer is cleared after the call.
  virtual void onParagraph(std::vector<RubyRun>&& runs) = 0;
  virtual void onImage(const std::string& src) = 0;
};

struct TextExtractor {
  // Each paragraph is a sequence of RubyRun entries. Unannotated text has
  // empty ruby; annotated text (<ruby>base<rt>reading</rt></ruby>) maps
  // base -> rubyText.
  ParagraphSink* sink = nullptr;

  std::vector<RubyRun> currentRuns;
  std::string currentText;
  int blockDepth = 0;
  int skipDepth = -1;

  // Pathological books put an entire chapter in one <p>/<div>. currentText/currentRuns are the
  // only unbounded-by-markup buffers left in extraction, so force a paragraph split once the
  // accumulated text passes this size -- the split hands the text to the sink (which lays out and
  // flushes pages to SD), keeping extraction O(bounded-paragraph) instead of O(chapter). A forced
  // split starts a new column mid-paragraph; harmless compared to the alternative (OOM).
  static constexpr size_t MAX_PARAGRAPH_BYTES = 16 * 1024;

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
      if (sink) sink->onParagraph(std::move(currentRuns));
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
        // Complete the paragraph built so far, then emit the image in document order. (For the
        // rare mid-paragraph image this places the partial text before the image, where the old
        // accumulate-then-interleave code placed the image before the whole paragraph; identical
        // for the usual block-level images.)
        self->flushParagraph();
        if (self->sink) self->sink->onImage(std::string(src));
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
        self->currentRuns.push_back(
            RubyRun{std::move(self->rubyBase), std::move(self->rubyAnnotation), self->currentStyle(), self->hasEmphasis()});
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
      // Forced split for markup-less mega-paragraphs; see MAX_PARAGRAPH_BYTES. (Not applied
      // inside <ruby> -- ruby runs are a handful of characters by nature.)
      if (self->currentText.size() + static_cast<size_t>(len) > MAX_PARAGRAPH_BYTES) {
        self->flushParagraph();
      }
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

namespace {

// ---- Page (de)serialization (cache format v37) -----------------------------------------------
// File layout:
//   header: u8 version, i32 fontId, u16 viewportWidth, u16 viewportHeight,
//           u16 pageCount, u32 indexOffset          (pageCount/indexOffset patched post-stream)
//   page records (variable length, written as pages are laid out)
//   footer at indexOffset: pageCount x u32 file offset of each page record
// The footer lets loadSectionFile() open a chapter by reading only the header + 4 bytes/page,
// and getPage() seek straight to one page -- pages are never all resident in RAM.

bool writePage(HalFile& file, const VerticalPage& page) {
  const bool isImg = page.isImagePage();
  serialization::writePod(file, isImg);
  if (isImg) {
    serialization::writeString(file, page.imagePath);
    serialization::writePod(file, page.imageWidth);
    serialization::writePod(file, page.imageHeight);
    serialization::writePod(file, page.imageRotated);
    return true;
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
  return true;
}

bool readPage(HalFile& file, VerticalPage& page) {
  page.glyphs.clear();
  page.imagePath.clear();

  bool isImg = false;
  serialization::readPod(file, isImg);
  if (isImg) {
    serialization::readString(file, page.imagePath);
    serialization::readPod(file, page.imageWidth);
    serialization::readPod(file, page.imageHeight);
    serialization::readPod(file, page.imageRotated);
    return !page.imagePath.empty();
  }

  uint32_t glyphCount;
  serialization::readPod(file, glyphCount);
  serialization::readPod(file, page.columnCount);
  serialization::readPod(file, page.rowsPerColumn);

  // One page is bounded by screen geometry (a few hundred cells); a corrupt count must not
  // drive a huge reserve on a heap that can't take it.
  constexpr uint32_t MAX_GLYPHS_PER_PAGE = 4096;
  if (glyphCount > MAX_GLYPHS_PER_PAGE) {
    LOG_ERR("VSC", "Corrupt page record: %u glyphs", glyphCount);
    return false;
  }
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
  return true;
}

// Streams the temp HTML once looking for "<rt", to decide ruby layout geometry (column gap /
// right padding) before the first paragraph is laid out. The old design scanned the fully
// accumulated paragraph list for ruby runs; a streaming pipeline has to know up front. A false
// positive (e.g. "<rt" inside a comment) merely pads columns slightly -- harmless.
bool fileContainsRubyTag(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("VSC", path, f)) return false;
  uint8_t buf[512];
  int state = 0;  // matched prefix length of "<rt"
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    for (size_t i = 0; i < n; i++) {
      const char c = static_cast<char>(buf[i]);
      if (state == 0) {
        state = (c == '<') ? 1 : 0;
      } else if (state == 1) {
        state = (c == 'r') ? 2 : (c == '<' ? 1 : 0);
      } else {
        if (c == 't') {
          f.close();
          return true;
        }
        state = (c == '<') ? 1 : 0;
      }
    }
  }
  f.close();
  return false;
}

// Concrete sink: feeds each extracted paragraph straight into the column layout, and whenever a
// batch of characters is buffered (or an image forces a boundary), lays out the batch and writes
// each resulting page to the cache file immediately. Nothing here is O(chapter): the layout
// stream, the produced pages, and the paragraph being extracted are all O(batch).
struct LayoutPageSink final : ParagraphSink {
  VerticalParsedText& layout;
  HalFile& out;
  std::vector<uint32_t>& pageOffsets;
  Epub& epub;
  const std::string& chapterDir;
  const std::string& imageBasePath;
  const uint16_t viewportWidth;
  const uint16_t viewportHeight;
  size_t imgIdx = 0;
  bool failed = false;

  // ~1-2 screens of text per layout batch. A batch boundary lands between paragraphs, which
  // already force a fresh column, so the only observable effect is an occasional page that ends
  // at a paragraph boundary instead of mid-paragraph -- same behavior as an image boundary.
  static constexpr size_t BATCH_CHARS = 640;

  LayoutPageSink(VerticalParsedText& layout, HalFile& out, std::vector<uint32_t>& pageOffsets, Epub& epub,
                 const std::string& chapterDir, const std::string& imageBasePath, uint16_t viewportWidth,
                 uint16_t viewportHeight)
      : layout(layout),
        out(out),
        pageOffsets(pageOffsets),
        epub(epub),
        chapterDir(chapterDir),
        imageBasePath(imageBasePath),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight) {}

  void onParagraph(std::vector<RubyRun>&& runs) override {
    if (failed) return;
    layout.addAnnotatedParagraph(runs);
    runs.clear();  // free this paragraph's text now -- layout owns its own copy in the stream
    if (layout.pendingCount() >= BATCH_CHARS) flushText();
  }

  void onImage(const std::string& src) override {
    if (failed) return;
    flushText();
    writeOne(makeImagePage(src));
  }

  void flushText() {
    if (failed || layout.pendingCount() == 0) return;
    auto pages = layout.layoutPages();
    layout.reset();
    for (const auto& p : pages) writeOne(p);
  }

  void writeOne(const VerticalPage& p) {
    pageOffsets.push_back(static_cast<uint32_t>(out.position()));
    if (!writePage(out, p)) {
      LOG_ERR("VSC", "Failed to write page %zu to cache", pageOffsets.size() - 1);
      failed = true;
    }
  }

  VerticalPage makeImagePage(const std::string& src) {
    std::string resolvedSrc = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(chapterDir + src));

    // Determine extension and cached path
    std::string ext;
    const size_t extPos = resolvedSrc.rfind('.');
    if (extPos != std::string::npos) ext = resolvedSrc.substr(extPos);
    const std::string cachedPath = imageBasePath + std::to_string(imgIdx++) + ext;

    // Extract image from EPUB to cache if not already present
    if (!Storage.exists(cachedPath.c_str())) {
      HalFile cachedFile;
      if (Storage.openFileForWrite("VSC", cachedPath, cachedFile)) {
        epub.readItemContentsToStream(resolvedSrc, cachedFile, 4096);
        cachedFile.flush();
        cachedFile.close();
      }
    }

    // Get actual image dimensions. Store natural (unrotated) dimensions --
    // ImageBlock::render handles rotation, scaling, and centering itself.
    int displayW = viewportWidth;
    int displayH = viewportHeight;
    bool rotated = false;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedPath);
    if (decoder) {
      ImageDimensions dims = {0, 0};
      if (decoder->getDimensions(cachedPath, dims) && dims.width > 0 && dims.height > 0) {
        const bool viewportIsPortrait = (viewportHeight > viewportWidth);
        const bool imageIsLandscape = (dims.width > dims.height);
        rotated = (viewportIsPortrait == imageIsLandscape);
        displayW = dims.width;
        displayH = dims.height;
      }
    }

    VerticalPage page;
    page.imagePath = cachedPath;
    page.imageWidth = static_cast<int16_t>(displayW);
    page.imageHeight = static_cast<int16_t>(displayH);
    page.imageRotated = rotated;
    return page;
  }
};

// Byte offset of the pageCount field in the header: u8 version + i32 fontId + 2x u16 viewport.
constexpr size_t HEADER_PAGECOUNT_OFFSET = 1 + sizeof(int) + 2 * sizeof(uint16_t);

}  // namespace

bool VerticalSection::streamParseAndLayout(HalFile& out, const int fontId, const uint16_t viewportWidth,
                                           const uint16_t viewportHeight) {
  LOG_INF("VSC", "streamParseAndLayout start spine=%d free=%u", spineIndex, ESP.getFreeHeap());
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

  const bool hasRuby = fileContainsRubyTag(tmpHtmlPath);

  // Resolve image paths relative to the chapter's directory in the EPUB.
  const auto& spineItem = epub->getSpineItem(spineIndex);
  std::string chapterDir;
  {
    const size_t slash = spineItem.href.rfind('/');
    if (slash != std::string::npos) chapterDir = spineItem.href.substr(0, slash + 1);
  }
  const std::string imageBasePath = epub->getCachePath() + "/img_v" + std::to_string(spineIndex) + "_";

  VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
  const int lineH = renderer.getLineHeight(fontId);
  layout.setColumnGapPx((lineH / 3) < 4 ? 4 : (lineH / 3));
  if (hasRuby) {
    layout.setColumnGapPx(lineH * 2 / 3);
    layout.setRightPaddingPx((lineH / 2) < 2 ? 2 : (lineH / 2));
  }

  LayoutPageSink sink(layout, out, pageOffsets_, *epub, chapterDir, imageBasePath, viewportWidth, viewportHeight);

  TextExtractor extractor;
  extractor.sink = &sink;

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
  sink.flushText();

  if (sink.failed) return false;

  LOG_INF("VSC", "streamParseAndLayout end spine=%d pages=%zu free=%u", spineIndex, pageOffsets_.size(),
          ESP.getFreeHeap());
  return true;
}

bool VerticalSection::createSectionFile(const int fontId, const uint16_t viewportWidth,
                                         const uint16_t viewportHeight) {
  const auto vsectionsDir = epub->getCachePath() + "/vsections";
  Storage.mkdir(vsectionsDir.c_str());

  pageOffsets_.clear();
  loadedPageIndex_ = -1;
  pageCount = 0;

  HalFile file;
  if (!Storage.openFileForWrite("VSC", filePath, file)) {
    return false;
  }

  // Header with placeholders; pageCount and the offset-table location aren't known until all
  // pages have been streamed out, so they're patched by the seek-back below. The write mode is
  // O_RDWR (not append), so the seek-back write lands in place.
  serialization::writePod(file, VSECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  const uint16_t pageCountPlaceholder = 0;
  const uint32_t indexOffsetPlaceholder = 0;
  serialization::writePod(file, pageCountPlaceholder);
  serialization::writePod(file, indexOffsetPlaceholder);

  if (!streamParseAndLayout(file, fontId, viewportWidth, viewportHeight)) {
    file.close();
    Storage.remove(filePath.c_str());
    pageOffsets_.clear();
    return false;
  }

  const auto indexOffset = static_cast<uint32_t>(file.position());
  for (const uint32_t off : pageOffsets_) {
    serialization::writePod(file, off);
  }

  pageCount = static_cast<uint16_t>(pageOffsets_.size());
  if (!file.seek(HEADER_PAGECOUNT_OFFSET)) {
    file.close();
    Storage.remove(filePath.c_str());
    pageOffsets_.clear();
    pageCount = 0;
    return false;
  }
  serialization::writePod(file, pageCount);
  serialization::writePod(file, indexOffset);
  file.close();

  LOG_DBG("VSC", "Cached %u vertical pages (streamed)", pageCount);
  return true;
}

bool VerticalSection::loadSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
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
  uint32_t indexOffset;
  serialization::readPod(file, cachedPageCount);
  serialization::readPod(file, indexOffset);

  pageOffsets_.clear();
  loadedPageIndex_ = -1;

  if (cachedPageCount > 0) {
    if (indexOffset == 0 || !file.seek(indexOffset)) {
      file.close();
      LOG_ERR("VSC", "Bad page index offset in cache");
      clearCache();
      return false;
    }
    pageOffsets_.resize(cachedPageCount);
    const size_t want = static_cast<size_t>(cachedPageCount) * sizeof(uint32_t);
    const size_t got = file.read(reinterpret_cast<uint8_t*>(pageOffsets_.data()), want);
    if (got != want) {
      pageOffsets_.clear();
      file.close();
      LOG_ERR("VSC", "Truncated page index in cache");
      clearCache();
      return false;
    }
  }

  file.close();
  pageCount = cachedPageCount;
  LOG_DBG("VSC", "Opened cache: %u vertical pages (index only, %u bytes resident)", pageCount,
          static_cast<unsigned>(pageOffsets_.size() * sizeof(uint32_t)));
  return true;
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

const VerticalPage* VerticalSection::getPage() const { return getPage(currentPage); }

const VerticalPage* VerticalSection::getPage(int pageIndex) const {
  if (pageIndex < 0 || pageIndex >= static_cast<int>(pageOffsets_.size())) {
    return nullptr;
  }
  if (pageIndex == loadedPageIndex_) {
    return &loadedPage_;
  }

  // Fault the page in from the SD cache. The previous pointer returned by getPage() is
  // invalidated here -- all callers fetch-and-render one page at a time.
  loadedPageIndex_ = -1;
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) {
    return nullptr;
  }
  if (!file.seek(pageOffsets_[static_cast<size_t>(pageIndex)])) {
    file.close();
    return nullptr;
  }
  const bool ok = readPage(file, loadedPage_);
  file.close();
  if (!ok) {
    LOG_ERR("VSC", "Failed to read page %d from cache", pageIndex);
    return nullptr;
  }
  loadedPageIndex_ = pageIndex;
  return &loadedPage_;
}
