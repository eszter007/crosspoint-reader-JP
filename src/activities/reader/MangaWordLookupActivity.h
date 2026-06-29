#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class MangaWordLookupActivity final : public Activity {
 public:
  explicit MangaWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   std::string panelText);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct GlyphRef {
    uint32_t codepoint;
  };

  std::vector<GlyphRef> allGlyphs;
  std::vector<GlyphRef> selectableGlyphs;
  std::vector<size_t> selectToAllIdx;
  int cursorIndex = 0;

  bool hasResult = false;
  std::string resultHeadword;
  std::string resultDefinition;
  int resultMatchLen = 0;
  int scrollOffset = 0;
  int totalLines = 0;
  int maxScroll = 0;

  ButtonNavigator buttonNavigator;

  void moveCursor(int delta);
  void performLookup();
  std::string buildLookupText(size_t startIdx) const;
  void buildSelectableGlyphs();

  static void encodeUtf8(uint32_t cp, std::string& out);

  bool initialRenderDone = false;
  int fastRefreshCount = 0;
  static constexpr int kFullRefreshInterval = 10;
  static constexpr int kMaxLookupChars = 8;

  void renderContentArea(const Rect& screen, int contentTop);
};
