#pragma once
#include "Arduino.h"
enum timeStatus_t { timeNotSet, timeNeedsSync, timeSet };
extern timeStatus_t g_time_status;
inline timeStatus_t timeStatus() { return g_time_status; }
inline void events() {}
inline void updateNTP() {}
enum ezDebugLevel_t { NONE, ERROR, INFO, DEBUG };
inline void setDebug(ezDebugLevel_t) {}
inline void setServer(const String&) {}
inline void setInterval(unsigned) {}
enum ezMonth_t { JANUARY=1, FEBRUARY, MARCH, APRIL, MAY, JUNE, JULY,
                 AUGUST, SEPTEMBER, OCTOBER, NOVEMBER, DECEMBER };
inline bool waitForSync(unsigned = 0) { return true; }
inline void setTime(time_t) {}
inline void setTime(int,int,int,int,int,int) {}
struct Timezone {
  bool setLocation(const String& = String("")) { return true; }
  bool setPosix(const String&) { return true; }
  time_t now() { return g_now; }
  String dateTime(const String& = String("")) { return String("stub"); }
  int hour() { return 0; }
  void setTime(int, int, int, int, int, int) {}
  void setTime(time_t) {}
};
