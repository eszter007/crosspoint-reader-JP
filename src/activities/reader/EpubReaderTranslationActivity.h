#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderTranslationActivity final : public Activity {
 public:
  // preTranslatedText: if non-empty, the activity shows it directly without
  // any network call (used when a translation was already extracted offline
  // during manga conversion and stored alongside the page data).
  explicit EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::string sourceText, std::string preTranslatedText = "");

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
  bool hasPreTranslation = false;

  int scrollOffset = 0;
  int maxScrollOffset = 0;

  ButtonNavigator buttonNavigator;

  bool readApiKey(std::string& keyOut);
  bool callGeminiApi(const std::string& apiKey);

  void onWifiComplete(bool success);
};
