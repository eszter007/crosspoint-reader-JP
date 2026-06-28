#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  int scrollRow = 0;

  bool longPressFired = false;

  std::vector<RecentBook> recentBooks;

  struct BookProgress {
    int percent = -1;
  };
  std::vector<BookProgress> bookProgress;

  static constexpr int GRID_COLS = 3;
  static constexpr int COVER_PADDING = 6;
  static constexpr int CELL_TEXT_GAP = 4;
  static constexpr int SELECTION_RADIUS = 6;

  int getVisibleRows(int cellHeight, int contentHeight) const;
  int getCellHeight(int cellWidth) const;

  void loadRecentBooks();
  void loadBookProgress();
  int readProgressPercent(const std::string& bookPath) const;

  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
