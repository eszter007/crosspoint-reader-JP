#include "EpubReaderTranslationActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int HTTP_BUF_SIZE = 2048;
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;
constexpr const char* API_KEY_PATH = "/system/gemini.key";
constexpr const char* GEMINI_MODEL = "gemini-2.5-flash";

struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    int newCap = capacity == 0 ? 1024 : capacity;
    while (newCap < size) newCap *= 2;
    char* newData = static_cast<char*>(realloc(data, newCap));
    if (!newData) return false;
    data = newData;
    capacity = newCap;
    return true;
  }
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("XLAT", "Response buffer OOM (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

}  // namespace

EpubReaderTranslationActivity::EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::string sourceText, std::string preTranslatedText)
    : Activity("Translation", renderer, mappedInput), sourceText(std::move(sourceText)) {
  if (!preTranslatedText.empty()) {
    translatedText = std::move(preTranslatedText);
    hasPreTranslation = true;
    state = SHOWING_RESULT;
  }
}

void EpubReaderTranslationActivity::onEnter() {
  Activity::onEnter();

  if (hasPreTranslation) {
    requestUpdate();
    return;
  }

  WiFi.mode(WIFI_STA);

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void EpubReaderTranslationActivity::onExit() {
  Activity::onExit();

  if (!hasPreTranslation && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

bool EpubReaderTranslationActivity::readApiKey(std::string& keyOut) {
  char buf[128];
  size_t len = Storage.readFileToBuffer(API_KEY_PATH, buf, sizeof(buf));
  if (len == 0) return false;

  // Trim trailing whitespace/newlines
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
    len--;
  }
  if (len == 0) return false;

  keyOut.assign(buf, len);
  return true;
}

bool EpubReaderTranslationActivity::callGeminiApi(const std::string& apiKey) {
  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += GEMINI_MODEL;
  url += ":generateContent?key=";
  url += apiKey;

  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("XLAT", "Calling Gemini (heap: %u)", static_cast<unsigned>(freeHeap));
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("XLAT", "Insufficient heap for TLS: %u < %u", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(MIN_HEAP_FOR_TLS));
    errorMessage = tr(STR_TRANSLATION_LOW_MEMORY);
    return false;
  }

  JsonDocument reqDoc;
  auto contents = reqDoc["contents"].to<JsonArray>();
  auto part = contents.add<JsonObject>();
  auto parts = part["parts"].to<JsonArray>();
  auto textPart = parts.add<JsonObject>();
  textPart["text"] =
      std::string("Translate the following Japanese text to English. "
                   "Return only the translation, no commentary.\n\n") +
      sourceText;

  auto config = reqDoc["generationConfig"].to<JsonObject>();
  config["temperature"] = 0.3;
  config["maxOutputTokens"] = 2048;

  std::string body;
  serializeJson(reqDoc, body);

  ResponseBuffer responseBuf;

  esp_http_client_config_t httpConfig = {};
  httpConfig.url = url.c_str();
  httpConfig.event_handler = httpEventHandler;
  httpConfig.user_data = &responseBuf;
  httpConfig.method = HTTP_METHOD_POST;
  httpConfig.timeout_ms = 30000;
  httpConfig.buffer_size = HTTP_BUF_SIZE;
  httpConfig.buffer_size_tx = HTTP_BUF_SIZE;
  httpConfig.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&httpConfig);
  if (!client) {
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  bool headerOk =
      esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK;
  if (headerOk) {
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.length()));
  }

  if (!headerOk) {
    esp_http_client_cleanup(client);
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  LOG_DBG("XLAT", "Gemini response: HTTP %d (err: %d)", httpCode, err);

  if (err != ESP_OK || httpCode != 200 || !responseBuf.data) {
    LOG_ERR("XLAT", "API call failed: err=%d http=%d", err, httpCode);
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  JsonDocument respDoc;
  DeserializationError jsonErr = deserializeJson(respDoc, responseBuf.data);
  if (jsonErr) {
    LOG_ERR("XLAT", "JSON parse error: %s", jsonErr.c_str());
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  const char* text = respDoc["candidates"][0]["content"]["parts"][0]["text"];
  if (!text) {
    LOG_ERR("XLAT", "No text in Gemini response");
    errorMessage = tr(STR_TRANSLATION_FAILED);
    return false;
  }

  translatedText = text;
  return true;
}

void EpubReaderTranslationActivity::onWifiComplete(bool success) {
  if (!success) {
    errorMessage = tr(STR_TRANSLATION_WIFI_FAILED);
    state = ERROR;
    requestUpdate();
    return;
  }

  std::string apiKey;
  if (!readApiKey(apiKey)) {
    errorMessage = tr(STR_TRANSLATION_NO_API_KEY);
    state = ERROR;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = TRANSLATING;
  }
  requestUpdateAndWait();

  if (callGeminiApi(apiKey)) {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
  } else {
    RenderLock lock(*this);
    state = ERROR;
  }
  requestUpdate();
}

void EpubReaderTranslationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (state == SHOWING_RESULT) {
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
      if (scrollOffset < maxScrollOffset) {
        scrollOffset++;
        requestUpdate();
      }
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
      if (scrollOffset > 0) {
        scrollOffset--;
        requestUpdate();
      }
    });
  }
}

void EpubReaderTranslationActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_TRANSLATE_PAGE));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - footerHeight;
  const int maxWidth = screen.width - metrics.horizontalPadding * 2;
  const int textX = screen.x + metrics.horizontalPadding;

  if (state == TRANSLATING) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, screen.y + screen.height / 2, tr(STR_TRANSLATING), true);
  } else if (state == ERROR) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, screen.y + screen.height / 2, errorMessage.c_str(),
                              true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SHOWING_RESULT) {
    const int fontId = UI_12_FONT_ID;
    const int lineHeight = renderer.getLineHeight(fontId);

    auto lines = renderer.wrappedText(fontId, translatedText.c_str(), maxWidth, 64);

    maxScrollOffset = static_cast<int>(lines.size()) - (contentBottom - contentTop) / lineHeight;
    if (maxScrollOffset < 0) maxScrollOffset = 0;
    if (scrollOffset > maxScrollOffset) scrollOffset = maxScrollOffset;

    int y = contentTop;
    for (int i = scrollOffset; i < static_cast<int>(lines.size()) && y + lineHeight <= contentBottom; i++) {
      renderer.drawText(fontId, textX, y, lines[i].c_str(), true);
      y += lineHeight;
    }

    if (maxScrollOffset > 0) {
      std::string scrollInfo =
          std::to_string(scrollOffset + 1) + "/" + std::to_string(maxScrollOffset + 1);
      renderer.drawText(SMALL_FONT_ID, screen.x + screen.width - metrics.horizontalPadding - 40,
                        contentBottom + 2, scrollInfo.c_str(), true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
