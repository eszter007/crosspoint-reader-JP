#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int scrollOffset = 0;
  // Calendar month navigation
  uint16_t calYear;
  uint8_t calMonth;

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
