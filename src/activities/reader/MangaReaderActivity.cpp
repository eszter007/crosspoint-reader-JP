#include "MangaReaderActivity.h"

#include <Bitmap.h>
#include <DictIndex.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MangaWordLookupActivity.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MangaReaderActivity::onEnter() {
  Activity::onEnter();

  if (!book) return;

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  loadProgress();

  APP_STATE.openEpubPath = book->getFolder();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(book->getFolder(), book->getTitle(), "", "");

  loadCurrentPagePanels();
  requestUpdate();
}

void MangaReaderActivity::onExit() {
  saveProgress();
  panels.clear();
  book.reset();
  Activity::onExit();
}

void MangaReaderActivity::loadCurrentPagePanels() {
  panels.clear();
  panelsLoaded = false;
  if (book && currentPage < book->getPageCount()) {
    panelsLoaded = book->loadPagePanels(currentPage, panels);
  }
}

void MangaReaderActivity::nextPanel() {
  if (panels.empty()) {
    nextPage();
    return;
  }

  if (currentPanel < 0) {
    currentPanel = 0;
    viewMode = ViewMode::PanelZoom;
  } else if (currentPanel < static_cast<int>(panels.size()) - 1) {
    currentPanel++;
  } else {
    nextPage();
    return;
  }
  requestUpdate();
}

void MangaReaderActivity::prevPanel() {
  if (currentPanel > 0) {
    currentPanel--;
    requestUpdate();
  } else if (currentPanel == 0) {
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    requestUpdate();
  } else {
    prevPage();
  }
}

void MangaReaderActivity::nextPage() {
  if (!book) return;
  if (currentPage + 1 < book->getPageCount()) {
    currentPage++;
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    loadCurrentPagePanels();
    requestUpdate();
  }
}

void MangaReaderActivity::prevPage() {
  if (currentPage > 0) {
    currentPage--;
    currentPanel = -1;
    viewMode = ViewMode::FullPage;
    loadCurrentPagePanels();
    requestUpdate();
  }
}

void MangaReaderActivity::loop() {
  if (viewMode == ViewMode::TextOverlay) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      viewMode = ViewMode::PanelZoom;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchWordLookup();
      return;
    }
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(book ? book->getFolder() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (viewMode == ViewMode::PanelZoom) {
      currentPanel = -1;
      viewMode = ViewMode::FullPage;
      requestUpdate();
      return;
    }
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (viewMode == ViewMode::PanelZoom && currentPanel >= 0 &&
        currentPanel < static_cast<int>(panels.size())) {
      if (!panels[currentPanel].textBlocks.empty()) {
        if (DictIndex::isAvailable()) {
          launchWordLookup();
        } else {
          showTextOverlay = !showTextOverlay;
          viewMode = showTextOverlay ? ViewMode::TextOverlay : ViewMode::PanelZoom;
          requestUpdate();
        }
        return;
      }
    }
    if (viewMode == ViewMode::FullPage && !panels.empty()) {
      currentPanel = 0;
      viewMode = ViewMode::PanelZoom;
      requestUpdate();
      return;
    }
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) return;

  if (viewMode == ViewMode::PanelZoom) {
    if (nextTriggered) nextPanel();
    if (prevTriggered) prevPanel();
  } else {
    if (nextTriggered) {
      if (!panels.empty()) {
        currentPanel = 0;
        viewMode = ViewMode::PanelZoom;
        requestUpdate();
      } else {
        nextPage();
      }
    }
    if (prevTriggered) prevPage();
  }
}

void MangaReaderActivity::render(RenderLock&&) {
  if (!book) return;

  if (currentPage >= book->getPageCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  switch (viewMode) {
    case ViewMode::FullPage:
      renderFullPage();
      break;
    case ViewMode::PanelZoom:
      renderPanelZoom();
      break;
    case ViewMode::TextOverlay:
      renderTextOverlay();
      break;
  }

  saveProgress();
}

void MangaReaderActivity::renderFullPage() {
  renderer.clearScreen();

  std::string imgPath = book->getPageImagePath(currentPage);
  if (imgPath.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // Use the image decoder (supports JPG/PNG/BMP) for proper grayscale rendering.
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imgPath);
  if (!decoder) {
    renderer.drawCenteredText(UI_12_FONT_ID, screenH / 2, tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  ImageDimensions dims = {0, 0};
  if (!decoder->getDimensions(imgPath, dims) || dims.width <= 0 || dims.height <= 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, screenH / 2, tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  int x = 0, y = 0;
  float ratio = static_cast<float>(dims.width) / static_cast<float>(dims.height);
  float screenRatio = static_cast<float>(screenW) / static_cast<float>(screenH);
  if (dims.width > screenW || dims.height > screenH) {
    if (ratio > screenRatio) {
      y = static_cast<int>((screenH - screenW / ratio) / 2.0f);
    } else {
      x = static_cast<int>((screenW - screenH * ratio) / 2.0f);
    }
  } else {
    x = (screenW - dims.width) / 2;
    y = (screenH - dims.height) / 2;
  }

  // Cache path for decoded pixel data (avoids re-decoding JPG on grayscale passes)
  std::string cachePath = book->getCachePath() + "/page_" + std::to_string(currentPage) + ".2bp";

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = screenW;
  config.maxHeight = screenH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cachePath = cachePath;

  // BW pass — render to framebuffer but don't display yet
  decoder->decodeToFramebuffer(imgPath, renderer, config);

  // Draw panel highlights if panels are loaded
  if (panelsLoaded && !panels.empty()) {
    float scaleX = static_cast<float>(screenW) / static_cast<float>(dims.width);
    float scaleY = static_cast<float>(screenH) / static_cast<float>(dims.height);
    float scale = std::min(scaleX, scaleY);
    if (scale > 1.0f) scale = 1.0f;

    int imgDrawW = static_cast<int>(dims.width * scale);
    int imgDrawH = static_cast<int>(dims.height * scale);
    int imgX = (screenW - imgDrawW) / 2;
    int imgY = (screenH - imgDrawH) / 2;

    for (size_t i = 0; i < panels.size(); i++) {
      const auto& p = panels[i];
      int px = imgX + static_cast<int>(p.x * scale);
      int py = imgY + static_cast<int>(p.y * scale);
      int pw = static_cast<int>(p.w * scale);
      int ph = static_cast<int>(p.h * scale);
      renderer.drawRect(px, py, pw, ph, 2, true);
    }
  }

  // Status bar: page number
  char statusBuf[32];
  snprintf(statusBuf, sizeof(statusBuf), "%u/%u", currentPage + 1, book->getPageCount());
  int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusBuf);
  int statusX = screenW - statusW - 4;
  int statusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
  renderer.fillRect(statusX - 2, statusY - 1, statusW + 4,
                    renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
  renderer.drawText(SMALL_FONT_ID, statusX, statusY, statusBuf, true);

  // Display with grayscale: BW first, then LSB/MSB planes for 4-level gray.
  renderer.storeBwBuffer();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  decoder->decodeToFramebuffer(imgPath, renderer, config);
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  decoder->decodeToFramebuffer(imgPath, renderer, config);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

void MangaReaderActivity::renderPanelZoom() {
  renderer.clearScreen();

  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) {
    renderFullPage();
    return;
  }

  const auto& panel = panels[currentPanel];

  std::string imgPath = book->getPageImagePath(currentPage);
  if (imgPath.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2,
                              tr(STR_PAGE_LOAD_ERROR), true);
    renderer.displayBuffer();
    return;
  }

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  // Try to load a pre-cropped panel image (p<page>_<panel>.jpg)
  char panelFileName[64];
  snprintf(panelFileName, sizeof(panelFileName), "p%d_%d.jpg", currentPage, currentPanel);
  std::string panelImgPath = book->getFolder();
  if (panelImgPath.back() != '/') panelImgPath += '/';
  panelImgPath += panelFileName;

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(panelImgPath);
  if (!decoder || !Storage.exists(panelImgPath.c_str())) {
    // No pre-cropped panel image — fall back to full page view
    renderFullPage();
    return;
  }

  ImageDimensions panelDims = {0, 0};
  if (!decoder->getDimensions(panelImgPath, panelDims) || panelDims.width <= 0 || panelDims.height <= 0) {
    renderFullPage();
    return;
  }

  // Fit panel image to screen preserving aspect ratio
  int x = 0, y = 0;
  float ratio = static_cast<float>(panelDims.width) / static_cast<float>(panelDims.height);
  float screenRatio = static_cast<float>(screenW) / static_cast<float>(screenH);
  if (ratio > screenRatio) {
    y = static_cast<int>((screenH - screenW / ratio) / 2.0f);
  } else {
    x = static_cast<int>((screenW - screenH * ratio) / 2.0f);
  }

  std::string cachePath = book->getCachePath() + "/p" + std::to_string(currentPage)
                          + "_" + std::to_string(currentPanel) + ".2bp";
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = screenW;
  config.maxHeight = screenH;
  config.useGrayscale = true;
  config.useDithering = true;
  config.cachePath = cachePath;

  // BW pass
  decoder->decodeToFramebuffer(panelImgPath, renderer, config);

  // Panel indicator and status
  char statusBuf[48];
  snprintf(statusBuf, sizeof(statusBuf), "%d/%d  %u/%u",
           currentPanel + 1, (int)panels.size(),
           currentPage + 1, book->getPageCount());
  int statusW = renderer.getTextWidth(SMALL_FONT_ID, statusBuf);
  int statusX = screenW - statusW - 4;
  int statusY = screenH - renderer.getLineHeight(SMALL_FONT_ID) - 2;
  renderer.fillRect(statusX - 2, statusY - 1, statusW + 4,
                    renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
  renderer.drawText(SMALL_FONT_ID, statusX, statusY, statusBuf, true);

  if (!panels[currentPanel].textBlocks.empty()) {
    const char* hint = DictIndex::isAvailable() ? tr(STR_WORD_LOOKUP) : "Text";
    renderer.fillRect(2, statusY - 1, renderer.getTextWidth(SMALL_FONT_ID, hint) + 4,
                      renderer.getLineHeight(SMALL_FONT_ID) + 2, false);
    renderer.drawText(SMALL_FONT_ID, 4, statusY, hint, true);
  }

  // Display with grayscale
  renderer.storeBwBuffer();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  decoder->decodeToFramebuffer(panelImgPath, renderer, config);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
}

void MangaReaderActivity::renderTextOverlay() {
  renderer.clearScreen();

  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) {
    viewMode = ViewMode::PanelZoom;
    renderPanelZoom();
    return;
  }

  const auto& panel = panels[currentPanel];
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int jaFont = SETTINGS.getReaderFontId();
  const int lineH = renderer.getLineHeight(jaFont);
  int textY = screen.y + metrics.topPadding;

  // Header
  char headerBuf[32];
  snprintf(headerBuf, sizeof(headerBuf), "Panel %d/%d", currentPanel + 1, (int)panels.size());
  renderer.drawText(UI_12_FONT_ID, screen.x + metrics.contentSidePadding, textY, headerBuf, true,
                    EpdFontFamily::BOLD);
  textY += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

  // Draw text blocks
  int maxWidth = screen.width - metrics.contentSidePadding * 2;
  int textX = screen.x + metrics.contentSidePadding;
  int maxY = screen.y + screen.height - renderer.getLineHeight(SMALL_FONT_ID) - 4;

  for (const auto& tb : panel.textBlocks) {
    if (textY + lineH > maxY) break;
    if (tb.text.empty()) continue;

    // Word-wrap the text block
    std::string remaining = tb.text;
    while (!remaining.empty() && textY + lineH <= maxY) {
      if (renderer.getTextWidth(jaFont, remaining.c_str()) <= maxWidth) {
        renderer.drawText(jaFont, textX, textY, remaining.c_str(), true);
        textY += lineH;
        break;
      }

      // Find break point
      std::string accum;
      const char* p = remaining.c_str();
      while (*p) {
        size_t charLen = 1;
        auto c0 = static_cast<unsigned char>(*p);
        if (c0 >= 0xF0) charLen = 4;
        else if (c0 >= 0xE0) charLen = 3;
        else if (c0 >= 0xC0) charLen = 2;
        std::string test = accum + std::string(p, charLen);
        if (renderer.getTextWidth(jaFont, test.c_str()) > maxWidth) break;
        accum = test;
        p += charLen;
      }

      if (accum.empty()) {
        auto c0 = static_cast<unsigned char>(remaining[0]);
        size_t cl = 1;
        if (c0 >= 0xF0) cl = 4;
        else if (c0 >= 0xE0) cl = 3;
        else if (c0 >= 0xC0) cl = 2;
        accum = remaining.substr(0, cl);
        remaining = remaining.substr(cl);
      } else {
        remaining = remaining.substr(accum.size());
      }

      renderer.drawText(jaFont, textX, textY, accum.c_str(), true);
      textY += lineH;
    }

    textY += lineH / 2;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                            DictIndex::isAvailable() ? tr(STR_WORD_LOOKUP) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void MangaReaderActivity::launchWordLookup() {
  if (currentPanel < 0 || currentPanel >= static_cast<int>(panels.size())) return;
  if (!DictIndex::isAvailable()) return;

  const auto& panel = panels[currentPanel];
  if (panel.textBlocks.empty()) return;

  // Build a combined text string from all text blocks in this panel
  std::string combined;
  for (const auto& tb : panel.textBlocks) {
    if (!combined.empty()) combined += '\n';
    combined += tb.text;
  }

  if (combined.empty()) return;

  // Use the MangaWordLookup sub-activity with raw text
  startActivityForResult(
      std::make_unique<MangaWordLookupActivity>(renderer, mappedInput, std::move(combined)),
      [this](const ActivityResult&) {
        viewMode = ViewMode::PanelZoom;
        requestUpdate();
      });
}

void MangaReaderActivity::saveProgress() const {
  if (!book) return;
  std::string cachePath = book->getCachePath();

  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }

  uint8_t data[6];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  int16_t panelVal = static_cast<int16_t>(currentPanel);
  memcpy(data + 4, &panelVal, 2);

  ProgressFile::writeAtomic(cachePath, data, sizeof(data));
}

void MangaReaderActivity::loadProgress() {
  if (!book) return;
  std::string cachePath = book->getCachePath();

  HalFile f;
  if (Storage.openFileForRead("MNG", cachePath + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) == 6) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      int16_t panelVal;
      memcpy(&panelVal, data + 4, 2);
      currentPanel = panelVal;

      if (currentPage >= book->getPageCount()) {
        currentPage = 0;
        currentPanel = -1;
      }

      viewMode = (currentPanel >= 0) ? ViewMode::PanelZoom : ViewMode::FullPage;
    }
  }
}

void MangaReaderActivity::launchMenu() {
  // Placeholder for future menu (rotate, go home, etc.)
}

ScreenshotInfo MangaReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;  // reuse XTC type for now
  if (book) {
    snprintf(info.title, sizeof(info.title), "%s", book->getTitle().c_str());
    info.totalPages = static_cast<int>(book->getPageCount());
    info.currentPage = static_cast<int>(currentPage) + 1;
    info.progressPercent = book->getPageCount() > 0
        ? static_cast<int>((currentPage + 1) * 100 / book->getPageCount())
        : 0;
  }
  return info;
}
