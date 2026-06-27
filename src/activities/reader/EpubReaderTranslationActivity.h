#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderTranslationActivity final : public Activity {
 public:
  explicit EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string sourceText);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == WIFI_SELECTION || state == TRANSLATING; }

 private:
  enum State {
    WIFI_SELECTION,
    TRANSLATING,
    SHOWING_RESULT,
    ERROR,
  };

  State state = WIFI_SELECTION;
  std::string sourceText;
  std::string translatedText;
  std::string errorMessage;

  int scrollOffset = 0;
  int maxScrollOffset = 0;

  ButtonNavigator buttonNavigator;

  bool readApiKey(std::string& keyOut);
  bool callGeminiApi(const std::string& apiKey);

  void onWifiComplete(bool success);
};
