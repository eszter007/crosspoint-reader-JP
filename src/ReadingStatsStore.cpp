#include "ReadingStatsStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

ReadingStatsStore ReadingStatsStore::instance;

static constexpr const char* STATS_PATH = "/.crosspoint/reading_stats.bin";
static constexpr uint8_t STATS_VERSION = 1;

namespace {
int daysSinceEpoch(uint16_t y, uint8_t m, uint8_t d) {
  int yy = y, mm = m;
  if (mm <= 2) { yy--; mm += 12; }
  return 365 * yy + yy / 4 - yy / 100 + yy / 400 + (153 * (mm - 3) + 2) / 5 + d - 306;
}

// day-of-week: 0=Sun .. 6=Sat
int dowFromDate(uint16_t y, uint8_t m, uint8_t d) {
  return (daysSinceEpoch(y, m, d) + 1) % 7;
}

void subtractDays(uint16_t& y, uint8_t& m, uint8_t& d, int n) {
  int epoch = daysSinceEpoch(y, m, d) - n;
  // Inverse: convert epoch days back to y/m/d (good enough for recent dates)
  int a = epoch + 306;
  int yy = (4 * a + 3) / 1461;
  int doy = a - (365 * yy + yy / 4 - yy / 100 + yy / 400);
  int mm = (5 * doy + 2) / 153;
  d = static_cast<uint8_t>(doy - (153 * mm + 2) / 5 + 1);
  mm += 3;
  if (mm > 12) { mm -= 12; yy++; }
  y = static_cast<uint16_t>(yy);
  m = static_cast<uint8_t>(mm);
}
}  // namespace

void ReadingStatsStore::addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes) {
  for (int i = 0; i < dayCount; i++) {
    if (days[i].year == year && days[i].month == month && days[i].day == day) {
      days[i].minutesRead += minutes;
      return;
    }
  }
  if (dayCount >= MAX_DAYS) {
    // Drop oldest
    memmove(&days[0], &days[1], (MAX_DAYS - 1) * sizeof(DailyReading));
    dayCount = MAX_DAYS - 1;
  }
  days[dayCount++] = {year, month, day, minutes};
}

uint16_t ReadingStatsStore::getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const {
  for (int i = 0; i < dayCount; i++) {
    if (days[i].year == year && days[i].month == month && days[i].day == day) {
      return days[i].minutesRead;
    }
  }
  return 0;
}

bool ReadingStatsStore::hasReadToday(uint16_t year, uint8_t month, uint8_t day) const {
  return getMinutesForDay(year, month, day) > 0;
}

int ReadingStatsStore::getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  int streak = 0;
  uint16_t y = todayYear;
  uint8_t m = todayMonth, d = todayDay;

  // Check today first
  if (getMinutesForDay(y, m, d) > 0) {
    streak = 1;
  } else {
    return 0;
  }

  // Check consecutive previous days
  for (int i = 1; i < MAX_DAYS; i++) {
    uint16_t py = todayYear;
    uint8_t pm = todayMonth, pd = todayDay;
    subtractDays(py, pm, pd, i);
    if (getMinutesForDay(py, pm, pd) > 0) {
      streak++;
    } else {
      break;
    }
  }
  return streak;
}

uint16_t ReadingStatsStore::getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const {
  // ISO week: Mon=0..Sun=6
  int dow = (dowFromDate(todayYear, todayMonth, todayDay) + 6) % 7;
  uint16_t total = 0;
  for (int i = 0; i <= dow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, dow - i);
    total += getMinutesForDay(y, m, d);
  }
  return total;
}

void ReadingStatsStore::getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay,
                                      int todayDow, bool readDays[7]) const {
  for (int i = 0; i < 7; i++) readDays[i] = false;
  for (int i = 0; i <= todayDow; i++) {
    uint16_t y = todayYear;
    uint8_t m = todayMonth, d = todayDay;
    subtractDays(y, m, d, todayDow - i);
    readDays[i] = getMinutesForDay(y, m, d) > 0;
  }
}

bool ReadingStatsStore::saveToFile() const {
  HalFile f;
  if (!Storage.openFileForWrite("STAT", STATS_PATH, f)) return false;
  f.write(&STATS_VERSION, 1);
  uint16_t count = static_cast<uint16_t>(dayCount);
  f.write(reinterpret_cast<const uint8_t*>(&count), 2);
  for (int i = 0; i < dayCount; i++) {
    f.write(reinterpret_cast<const uint8_t*>(&days[i]), sizeof(DailyReading));
  }
  f.close();
  return true;
}

bool ReadingStatsStore::loadFromFile() {
  HalFile f;
  if (!Storage.openFileForRead("STAT", STATS_PATH, f)) return false;
  uint8_t version;
  if (f.read(&version, 1) != 1 || version != STATS_VERSION) { f.close(); return false; }
  uint16_t count;
  if (f.read(reinterpret_cast<uint8_t*>(&count), 2) != 2) { f.close(); return false; }
  if (count > MAX_DAYS) count = MAX_DAYS;
  dayCount = 0;
  for (int i = 0; i < count; i++) {
    DailyReading dr;
    if (f.read(reinterpret_cast<uint8_t*>(&dr), sizeof(DailyReading)) != sizeof(DailyReading)) break;
    days[dayCount++] = dr;
  }
  f.close();
  return true;
}
