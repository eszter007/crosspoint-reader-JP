#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <ctime>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "components/icons/flame.h"
#include "components/icons/stats_icons.h"
#include "fontIds.h"

namespace {
struct Today {
  uint16_t year;
  uint8_t month, day;
  int dow;  // 0=Mon..6=Sun (ISO week)
};

Today getToday() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  // Convert C dow (0=Sun) to ISO (0=Mon..6=Sun)
  int isoDow = (t->tm_wday + 6) % 7;
  return {static_cast<uint16_t>(t->tm_year + 1900), static_cast<uint8_t>(t->tm_mon + 1),
          static_cast<uint8_t>(t->tm_mday), isoDow};
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  READING_STATS.loadFromFile();
  requestUpdate();
}

void ReadingStatsActivity::onExit() { Activity::onExit(); }

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_STATS));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const Today today = getToday();
  const int streak = READING_STATS.getStreak(today.year, today.month, today.day);
  const uint16_t weekMinutes = READING_STATS.getMinutesThisWeek(today.year, today.month, today.day);
  bool weekDays[7] = {};
  READING_STATS.getWeekStatus(today.year, today.month, today.day, today.dow, weekDays);

  // --- Card dimensions ---
  const int cardMargin = 20;
  const int cardX = screen.x + cardMargin;
  const int cardW = screen.width - 2 * cardMargin;
  const int cardPad = 16;
  const int cardRadius = 12;

  const int iconSize = 32;
  const int streakLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int circleSize = 24;
  const int cardH = cardPad + iconSize + 10 + smallLineH + 16 + smallLineH + 8 + circleSize + cardPad;

  const int cardY = contentTop + 10;

  // Draw card border
  renderer.drawRoundedRect(cardX, cardY, cardW, cardH, 2, cardRadius, true);

  // --- Row 1: Flame icon + "X day streak" centered ---
  char streakBuf[32];
  snprintf(streakBuf, sizeof(streakBuf), "%d day streak", streak);
  const int streakTextW = renderer.getTextWidth(UI_12_FONT_ID, streakBuf, EpdFontFamily::BOLD);
  const int row1TotalW = iconSize + 8 + streakTextW;
  const int row1X = cardX + (cardW - row1TotalW) / 2;
  const int row1Y = cardY + cardPad;

  renderer.drawIcon(FlameIcon, row1X, row1Y, iconSize, iconSize);
  renderer.drawText(UI_12_FONT_ID, row1X + iconSize + 8,
                    row1Y + (iconSize - streakLineH) / 2, streakBuf, true, EpdFontFamily::BOLD);

  // --- Row 2: "X minutes read this week" centered ---
  char weekBuf[48];
  snprintf(weekBuf, sizeof(weekBuf), "%d minutes read this week", weekMinutes);
  const int weekTextW = renderer.getTextWidth(SMALL_FONT_ID, weekBuf);
  const int row2Y = row1Y + iconSize + 4;
  renderer.drawText(SMALL_FONT_ID, cardX + (cardW - weekTextW) / 2, row2Y, weekBuf, true);

  // --- Separator line ---
  const int sepY = row2Y + smallLineH + 8;
  renderer.drawLine(cardX + cardPad, sepY, cardX + cardW - cardPad, sepY, true);

  // --- Row 3: Day labels (Mon-Sun) ---
  static const char* dayLabels[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  const int dayAreaW = cardW - 2 * cardPad;
  const int daySpacing = dayAreaW / 7;
  const int labelsY = sepY + 12;

  for (int i = 0; i < 7; i++) {
    const int cx = cardX + cardPad + daySpacing / 2 + i * daySpacing;
    const bool isToday = (i == today.dow);

    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, dayLabels[i],
                                              isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, cx - labelW / 2, labelsY, dayLabels[i], true,
                      isToday ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  // --- Row 4: Circle icons (24x24 pre-rendered from SVG) ---
  const int circlesY = labelsY + smallLineH + 6;

  for (int i = 0; i < 7; i++) {
    const int cx = cardX + cardPad + daySpacing / 2 + i * daySpacing;
    const int ix = cx - circleSize / 2;

    if (weekDays[i]) {
      renderer.drawIcon(CircleCheckIcon, ix, circlesY, circleSize, circleSize);
    } else {
      renderer.drawIcon(CircleEmptyIcon, ix, circlesY, circleSize, circleSize);
    }
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
