// Host-side tests for FlushWater's flush decision.
//
// decideFlush() is pure logic - level, rise rate, clock and flags in, a pump
// decision out - so it can be exercised on a laptop with no ESP32 present.
// We include main.cpp directly to reach its file-scope state.
//
// Two tests are marked KNOWN BUG. They encode behaviour observed in the Home
// Assistant history on 2026-09-17 and are EXPECTED TO FAIL until the decision
// logic is fixed. They are the regression net for that fix.

#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>

#include "../src/main.cpp"

// ---- storage for the fakes declared extern in the stubs -----------------
unsigned long g_millis = 0;
time_t        g_now    = 0;
int           g_pin[64] = {0};
int           g_adc_mv = 1500;
bool          g_serial_verbose = false;
SerialT       Serial;
ESPClass      ESP;
WiFiClass     WiFi;
int           g_wifi_status = WL_CONNECTED;
bool          g_mqtt_connected = true;
timeStatus_t  g_time_status = timeSet;
bool          g_restarted = false;

// ---- harness ------------------------------------------------------------
static int failures = 0, known_bugs = 0, fixed = 0;

// 2026-09-17 18:31:35 UTC and 23:00 UTC, for day/night cases
static const time_t T_DAY   = 1789677095;
static const time_t T_NIGHT = 1789693200;

static void reset_state(time_t when, int lvl, bool safe, bool refractory) {
  g_now    = when;
  g_millis = 10UL * 60UL * 1000UL;          // 10 min uptime: past STARTUP_GRACE_MS
  g_time_status = timeSet;
  level = lvl;
  allowActive = false;
  effectivenessAlerted = false;
  allowUntil = allowMinUntil = allowStartMs = 0;
  noRearmUntil = refractory ? g_millis + 60000UL : 0;
  pumpOperationSafe = safe;
  lastSafeMsgMs = g_millis;
  senderTableValid = true;
  for (int i = 0; i < bufferSize; i++) { readings[i].lvl = 0; readings[i].timestamp = 0; }
  currentReadingIndex = 0;
  setInhibit(true);
}

static void push_reading(int lvl, time_t ts) {
  readings[currentReadingIndex].lvl = lvl;
  readings[currentReadingIndex].timestamp = ts;
  currentReadingIndex = (currentReadingIndex + 1) % bufferSize;
}

static void check(const char* name, bool expect_allow, bool known_bug) {
  decideFlush();
  const bool got = allowActive;
  if (got == expect_allow) {
    if (known_bug) { printf("  FIXED       %s\n", name); fixed++; }
    else           { printf("  pass        %s\n", name); }
  } else {
    if (known_bug) { printf("  KNOWN BUG   %s  (expected no-flush, got flush)\n", name); known_bugs++; }
    else           { printf("  REGRESSION  %s  (expected %s, got %s)\n", name,
                            expect_allow ? "flush" : "no flush", got ? "flush" : "no flush");
                     failures++; }
  }
  if (allowActive) endAllowWindow();
}

int main() {
  // Pin the timezone so day/night boundaries are deterministic everywhere.
#ifdef _WIN32
  _putenv_s("TZ", "UTC"); _tzset();
#else
  setenv("TZ", "UTC", 1); tzset();
#endif
  printf("\nFlushWater decision tests\n-------------------------\n");

  // --- KNOWN BUG 1: post-flush refill must not re-trigger ----------------
  // Observed 2026-09-17: flush emptied the sump to 0cm at 18:31, level was
  // back to 7cm by 18:36. That is 1.4 cm/min, over FAST_RISE_CMPM (1.0), so
  // the refill reads as "heavy rain", sets critical, and critical bypasses
  // the refractory - producing the second flush at 18:37:06.
  reset_state(T_DAY, 7, true, /*refractory=*/true);
  push_reading(0, T_DAY - 300);     // sump was empty 5 min ago
  push_reading(7, T_DAY);           // now refilled
  check("post-flush refill does not re-trigger during refractory", false, true);

  // --- the rise path has an IMPLICIT level floor -------------------------
  // It looks like daytime eligibility has no level floor (it is `critical`
  // alone). In practice the geometry supplies one: riseCmPerMin only reports
  // a rate once the buffer spans RISE_WINDOW_SEC (300 s), so clearing
  // FAST_RISE_CMPM (1.0 cm/min) requires >= 5 cm of rise, which from an empty
  // sump lands you at >= 5 cm anyway - the night threshold. So a "fast rise
  // at a trivial level" cannot actually happen. Pinned here so a future
  // change to RISE_WINDOW_SEC or FAST_RISE_CMPM does not silently open it up.
  reset_state(T_DAY, 4, true, /*refractory=*/false);
  push_reading(0, T_DAY - 360);     // 4 cm over 6 min = 0.67 cm/min, under 1.0
  push_reading(4, T_DAY);
  check("rise path cannot fire at a trivial level", false, false);

  // A short buffer must not produce a rate at all: findReadingOlderThan()
  // falls back to the NEWEST sample (not the oldest, despite the comment on
  // riseCmPerMin), so nowSec <= oldTs and the rate is 0.
  reset_state(T_DAY, 7, true, /*refractory=*/false);
  push_reading(0, T_DAY - 30);      // only 30 s of history
  push_reading(7, T_DAY);
  check("sub-window history yields no rise", false, false);

  // --- behaviour that must keep working ----------------------------------
  reset_state(T_NIGHT, 7, true, false);
  push_reading(7, T_NIGHT - 300); push_reading(7, T_NIGHT);
  check("night, level above threshold, flushes", true, false);

  reset_state(T_DAY, 40, true, /*refractory=*/true);
  push_reading(40, T_DAY - 300); push_reading(40, T_DAY);
  check("full sump overrides the refractory", true, false);

  reset_state(T_DAY, 7, true, false);
  push_reading(7, T_DAY - 300); push_reading(7, T_DAY);
  check("day, steady level, does not flush", false, false);

  reset_state(T_NIGHT, 20, /*safe=*/false, false);
  push_reading(20, T_NIGHT - 300); push_reading(20, T_NIGHT);
  check("unsafe flag blocks flushing", false, false);

  reset_state(T_DAY, 7, true, false);
  g_millis = 5000;                              // 5 s uptime
  push_reading(0, T_DAY - 300); push_reading(7, T_DAY);
  check("startup grace suppresses the rise path", false, false);

  printf("\n%d regression(s), %d known bug(s) still present, %d newly fixed\n\n",
         failures, known_bugs, fixed);
  if (failures) return 1;                 // real breakage
  if (known_bugs) return 2;               // expected until the fix lands
  return 0;
}
