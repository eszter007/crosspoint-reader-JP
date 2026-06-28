#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr int COVER_ASPECT_NUM = 3;
constexpr int COVER_ASPECT_DEN = 4;
}  // namespace

int RecentBooksActivity::getCellHeight(int cellWidth) const {
  int coverWidth = cellWidth - 2 * COVER_PADDING;
  int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
  int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  return COVER_PADDING + coverHeight + CELL_TEXT_GAP + lineHeight + COVER_PADDING;
}

int RecentBooksActivity::getVisibleRows(int cellHeight, int contentHeight) const {
  if (cellHeight <= 0) return 1;
  return std::max(1, contentHeight / cellHeight);
}

void RecentBooksActivity::loadRecentBooks() { recentBooks = RECENT_BOOKS.getBooks(); }

void RecentBooksActivity::loadBookProgress() {
  bookProgress.clear();
  bookProgress.reserve(recentBooks.size());
  for (const auto& book : recentBooks) {
    BookProgress bp;
    bp.percent = readProgressPercent(book.path);
    bookProgress.push_back(bp);
  }
}

int RecentBooksActivity::readProgressPercent(const std::string& bookPath) const {
  std::string cachePath;
  if (FsHelpers::hasEpubExtension(bookPath)) {
    cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(bookPath));
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    cachePath = "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
  } else {
    return -1;
  }

  HalFile f;
  if (!Storage.openFileForRead("LIB", cachePath + "/progress.bin", f)) {
    return 0;
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    uint8_t data[6];
    if (f.read(data, 6) != 6) return 0;
    uint16_t spineIndex = data[0] | (data[1] << 8);
    uint16_t currentPage = data[2] | (data[3] << 8);
    uint16_t pageCount = data[4] | (data[5] << 8);
    if (pageCount == 0) return 0;

    // Try to get spine count from book.bin for a book-level estimate
    HalFile bookFile;
    if (Storage.openFileForRead("LIB", cachePath + "/book.bin", bookFile)) {
      uint8_t version;
      uint32_t lutOffset;
      uint16_t spineCount;
      if (bookFile.read(&version, 1) == 1) {
        bookFile.read(reinterpret_cast<uint8_t*>(&lutOffset), 4);
        bookFile.read(reinterpret_cast<uint8_t*>(&spineCount), 2);
        if (spineCount > 0) {
          float sectionProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
          float overallProgress = (static_cast<float>(spineIndex) + sectionProgress) / static_cast<float>(spineCount);
          int pct = static_cast<int>(overallProgress * 100.0f + 0.5f);
          return std::clamp(pct, 0, 100);
        }
      }
    }

    // Fallback: section-level progress
    int pct = static_cast<int>((static_cast<float>(currentPage) / static_cast<float>(pageCount)) * 100.0f + 0.5f);
    return std::clamp(pct, 0, 100);
  }

  // XTC: 4-byte currentPage, then we need total from somewhere
  // XTC progress.bin is just currentPage (4 bytes), no total stored
  // Return -1 to indicate unknown
  return -1;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  loadRecentBooks();
  loadBookProgress();

  selectorIndex = 0;
  scrollRow = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookProgress.clear();
}

void RecentBooksActivity::loop() {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  if (selectorIndex < recentBooks.size() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int gridWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int cellWidth = gridWidth / GRID_COLS;
  const int cellHeight = getCellHeight(cellWidth);
  const int visibleRows = getVisibleRows(cellHeight, contentHeight);
  auto updateScroll = [&]() {
    int selectedRow = static_cast<int>(selectorIndex) / GRID_COLS;
    if (selectedRow < scrollRow) scrollRow = selectedRow;
    if (selectedRow >= scrollRow + visibleRows) scrollRow = selectedRow - visibleRows + 1;
    requestUpdate();
  };

  // Left/Right: linear navigation
  buttonNavigator.onNext([this, bookCount, &updateScroll] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), bookCount);
    updateScroll();
  });

  buttonNavigator.onPrevious([this, bookCount, &updateScroll] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), bookCount);
    updateScroll();
  });
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      loadRecentBooks();
      loadBookProgress();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= recentBooks.size()) {
        selectorIndex = recentBooks.size() - 1;
      }
      requestUpdate(true);
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    const int gridWidth = pageWidth - 2 * metrics.contentSidePadding;
    const int cellWidth = gridWidth / GRID_COLS;
    const int coverWidth = cellWidth - 2 * COVER_PADDING;
    const int coverHeight = coverWidth * COVER_ASPECT_DEN / COVER_ASPECT_NUM;
    const int thumbHeight = metrics.homeCoverHeight;
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int cellHeight = getCellHeight(cellWidth);
    const int visibleRows = getVisibleRows(cellHeight, contentHeight);
    const int totalRows = (static_cast<int>(recentBooks.size()) + GRID_COLS - 1) / GRID_COLS;

    for (int row = scrollRow; row < std::min(scrollRow + visibleRows, totalRows); row++) {
      for (int col = 0; col < GRID_COLS; col++) {
        const int idx = row * GRID_COLS + col;
        if (idx >= static_cast<int>(recentBooks.size())) break;

        const auto& book = recentBooks[idx];
        const int cellX = metrics.contentSidePadding + col * cellWidth;
        const int cellY = contentTop + (row - scrollRow) * cellHeight;
        const int coverX = cellX + COVER_PADDING;
        const int coverY = cellY + COVER_PADDING;
        const bool selected = (idx == static_cast<int>(selectorIndex));

        if (selected) {
          renderer.fillRoundedRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4, SELECTION_RADIUS,
                                   Color::LightGray);
        }

        bool hasCover = false;
        if (!book.coverBmpPath.empty()) {
          const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
          HalFile file;
          if (Storage.openFileForRead("LIB", coverPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              const float bmpW = static_cast<float>(bitmap.getWidth());
              const float bmpH = static_cast<float>(bitmap.getHeight());
              const float bmpRatio = bmpW / bmpH;
              const float cellRatio = static_cast<float>(coverWidth) / static_cast<float>(coverHeight);
              const float cropX = std::max(0.0f, 1.0f - (cellRatio / bmpRatio));
              renderer.drawBitmap(bitmap, coverX, coverY, coverWidth, coverHeight, cropX);
              hasCover = true;
            }
            file.close();
          }
        }

        renderer.drawRect(coverX, coverY, coverWidth, coverHeight, true);

        if (!hasCover) {
          renderer.drawIcon(CoverIcon, coverX + (coverWidth - 32) / 2, coverY + (coverHeight - 32) / 2, 32, 32);
          auto titleLines = renderer.wrappedText(SMALL_FONT_ID, book.title.c_str(), coverWidth - 8, 3);
          int textY = coverY + (coverHeight - 32) / 2 + 36;
          for (const auto& line : titleLines) {
            if (textY + lineHeight > coverY + coverHeight) break;
            const int textW = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
            const int textX = coverX + (coverWidth - textW) / 2;
            renderer.drawText(SMALL_FONT_ID, textX, textY, line.c_str(), true);
            textY += lineHeight;
          }
        }

        const int labelY = coverY + coverHeight + CELL_TEXT_GAP;
        if (idx < static_cast<int>(bookProgress.size()) && bookProgress[idx].percent >= 0) {
          char pctBuf[8];
          snprintf(pctBuf, sizeof(pctBuf), "%d%%", bookProgress[idx].percent);
          const int pctW = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
          renderer.drawText(SMALL_FONT_ID, coverX + (coverWidth - pctW) / 2, labelY, pctBuf, true);
        }
      }
    }

    if (totalRows > visibleRows) {
      const int scrollBarHeight = contentHeight;
      const int barX = pageWidth - metrics.scrollBarRightOffset - metrics.scrollBarWidth;
      const int barY = contentTop;
      const int thumbH = std::max(10, scrollBarHeight * visibleRows / totalRows);
      const int thumbY = barY + (scrollBarHeight - thumbH) * scrollRow / (totalRows - visibleRows);
      renderer.fillRect(barX, thumbY, metrics.scrollBarWidth, thumbH, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
