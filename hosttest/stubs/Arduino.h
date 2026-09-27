// Host-side Arduino stub. Enough of the API for main.cpp to compile and for
// the decision logic to be driven deterministically from a test.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <cstdlib>
#include <type_traits>
#include <algorithm>
using std::min;

typedef uint8_t byte;
typedef bool boolean;

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define HEX 16
#define D0 0
#define D1 1
#define D2 2
#define D3 3
#define D4 4
#define D5 5
#define D6 6
#define D7 7
#define D8 8
#define D9 9
#define D10 10
#define LED_BUILTIN 8

// ---- controllable fake clock -------------------------------------------
extern unsigned long g_millis;   // test sets this
extern time_t        g_now;      // test sets this
inline unsigned long millis() { return g_millis; }
inline void delay(unsigned long ms) { g_millis += ms; }
inline void yield() {}

// main.cpp calls time(nullptr); redirect it to the fake clock. Function-like
// macro, so time_t is untouched.
inline time_t fake_time(time_t* p) { if (p) *p = g_now; return g_now; }
#define time(x) fake_time(x)

// ---- fake GPIO ----------------------------------------------------------
extern int g_pin[64];
inline void pinMode(int, int) {}
inline void digitalWrite(int p, int v) { if (p >= 0 && p < 64) g_pin[p] = v; }
inline int  digitalRead(int p) { return (p >= 0 && p < 64) ? g_pin[p] : 0; }
extern int  g_adc_mv;
inline int  analogReadMilliVolts(int) { return g_adc_mv; }
inline int  analogRead(int) { return g_adc_mv; }
inline void analogReadResolution(int) {}
enum adc_attenuation_t { ADC_0db, ADC_2_5db, ADC_6db, ADC_11db };
inline void analogSetPinAttenuation(int, adc_attenuation_t) {}
#define F(x) (x)


// ---- String -------------------------------------------------------------
struct String {
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& v) : s(v) {}
  String(char c) : s(1, c) {}
  String(int v)  { char b[32]; snprintf(b,sizeof b,"%d",v);  s=b; }
  String(long v) { char b[32]; snprintf(b,sizeof b,"%ld",v); s=b; }
  String(unsigned long v) { char b[32]; snprintf(b,sizeof b,"%lu",v); s=b; }
  template <typename T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
  String(T v, int base) {
    char b[40];
    if (base == 16) snprintf(b,sizeof b,"%lx",(unsigned long)v);
    else            snprintf(b,sizeof b,"%lu",(unsigned long)v);
    s=b;
  }
  String(float v, int dec) { char b[40]; snprintf(b,sizeof b,"%.*f",dec,v); s=b; }
  String(double v, int dec) { char b[40]; snprintf(b,sizeof b,"%.*f",dec,v); s=b; }
  const char* c_str() const { return s.c_str(); }
  unsigned length() const { return (unsigned)s.size(); }
  int toInt() const { return atoi(s.c_str()); }
  float toFloat() const { return (float)atof(s.c_str()); }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o)   { s += (o?o:""); return *this; }
  String& operator+=(char c)          { s += c; return *this; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
  bool operator==(const char* o) const { return s == std::string(o?o:""); }
  bool operator!=(const char* o) const { return !(*this == o); }
};
inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b)   { String r(a); r += b; return r; }
inline String operator+(const char* a, const String& b)   { String r(a); r += b; return r; }

// ---- Serial (silent unless HOSTTEST_VERBOSE) -----------------------------
extern bool g_serial_verbose;
struct SerialT {
  void begin(int) {}
  void flush() {}
  template <typename T> void print(T)   { }
  template <typename T> void println(T) { }
  void println() {}
  void print(const char* s)   { if (g_serial_verbose) fputs(s, stdout); }
  void println(const char* s) { if (g_serial_verbose) printf("%s\n", s); }
  void print(float v, int d)  { if (g_serial_verbose) printf("%.*f", d, v); }
  void printf(const char* f, ...) { (void)f; }
};
extern SerialT Serial;

struct ESPClass {
  uint32_t getFreeHeap() { return 200000; }
  uint64_t getEfuseMac() { return 0x0011223344ULL; }
};
extern ESPClass ESP;
