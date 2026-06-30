#pragma once

#include <MangaPanel.h>

#include <memory>
#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MangaReaderActivity final : public Activity {
 public:
  explicit MangaReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::unique_ptr<manga::MangaBook> book)
      : Activity("MangaReader", renderer, mappedInput), book(std::move(book)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;

 private:
  std::unique_ptr<manga::MangaBook> book;

  uint32_t currentPage = 0;
  int currentPanel = -1;  // -1 = full page view, 0+ = zoomed panel
  int pagesUntilFullRefresh = 0;

  std::vector<manga::Panel> panels;
  bool panelsLoaded = false;

  bool showTextOverlay = false;
  bool pendingScreenshot = false;
  bool ignoreNextConfirmRelease = false;
  unsigned long readingSessionStartMs = 0;

  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  unsigned long pageTurnDuration = 0;

  ButtonNavigator buttonNavigator;

  enum class ViewMode { FullPage, PanelZoom, TextOverlay };
  ViewMode viewMode = ViewMode::FullPage;

  void loadCurrentPagePanels();
  void renderFullPage();
  void renderPanelZoom();
  void renderTextOverlay();

  void nextPanel();
  void prevPanel();
  void nextPage();
  void prevPage();

  void saveProgress() const;
  void loadProgress();

  void launchWordLookup();
  void launchMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
};
