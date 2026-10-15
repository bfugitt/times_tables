#pragma once
// Shared pieces used by the times tables (main.cpp) and multi-digit (algo.cpp) parts.
#include <M5Cardputer.h>
#include <ArduinoJson.h>
#include <vector>
#include <set>

/* ---------- Colors and screen ---------- */
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static constexpr uint16_t C_BG    = rgb(0xEE, 0xF3, 0xF1);
static constexpr uint16_t C_INK   = rgb(0x1F, 0x2D, 0x3A);
static constexpr uint16_t C_SOFT  = rgb(0x5F, 0x70, 0x7C);
static constexpr uint16_t C_LINE  = rgb(0xC9, 0xD6, 0xD1);
static constexpr uint16_t C_BLUE  = rgb(0x2E, 0x6F, 0x9E);
static constexpr uint16_t C_GREEN = rgb(0x2F, 0x7D, 0x4F);
static constexpr uint16_t C_WHITE = rgb(0xFF, 0xFF, 0xFF);
static constexpr int W = 240, H = 135;

extern M5Canvas cv;

/* ---------- Student ---------- */
struct Student {
  String name;
  // times tables
  bool hasCheck = false;
  String rating[13];
  uint32_t limitMs = 5000;
  std::set<int> flagged, learned, working;
  // multi-digit
  bool algoLoaded = false;
  int algoLevel = 1, algoHelp = 1, algoStreak = 0, algoMax = 5;
};
extern Student S;
extern bool online;

/* ---------- Keyboard ---------- */
struct KeyEv { bool any = false, enter = false, del = false, up = false, down = false; char ch = 0; };
KeyEv readKey();
void flushKeys();
void waitEnter();

/* ---------- Drawing + screens ---------- */
void present();
int drawWrapped(const String& text, int y, const lgfx::IFont* font, uint16_t color, int lineH);
void messageScreen(const String& title, const String& body, const String& footer = "");
int runMenu(const String& title, const String& body, const std::vector<String>& items, int sel = 0);

/* ---------- Misc ---------- */
int rnd(int n);
String joinList(const std::vector<String>& v, const char* sep = ", ");
bool trySave(JsonDocument& doc, const String& title, String& status);

/* ---------- Multi-digit app (algo.cpp) ---------- */
void algoLoop();
