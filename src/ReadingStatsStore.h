#pragma once
#include <cstdint>
#include <string>

struct DailyReading {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint16_t minutesRead;
};

class ReadingStatsStore {
  static ReadingStatsStore instance;

  static constexpr int MAX_DAYS = 60;
  DailyReading days[MAX_DAYS] = {};
  int dayCount = 0;

 public:
  static ReadingStatsStore& getInstance() { return instance; }

  void addMinutes(uint16_t year, uint8_t month, uint8_t day, uint16_t minutes);

  int getStreak(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;

  uint16_t getMinutesForDay(uint16_t year, uint8_t month, uint8_t day) const;

  uint16_t getMinutesThisWeek(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay) const;

  bool hasReadToday(uint16_t year, uint8_t month, uint8_t day) const;

  // Get reading status for each day of the current week (Sun=0..Sat=6).
  // Returns true in readDays[i] if minutes > 0 for that day.
  void getWeekStatus(uint16_t todayYear, uint8_t todayMonth, uint8_t todayDay,
                     int todayDow, bool readDays[7]) const;

  bool saveToFile() const;
  bool loadFromFile();
};

#define READING_STATS ReadingStatsStore::getInstance()
