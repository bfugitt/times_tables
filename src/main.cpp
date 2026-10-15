// Times Tables for M5Stack Cardputer ADV
// Same check + practice as the Chromebook version; logs to the same Google Sheet.

#include <M5Cardputer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include "config.h"
#include "net.h"
#include "app.h"

/* ================= Settings ================= */
static const uint32_t SESSION_MS = 150000;   // practice session length (2.5 min)
static const uint32_t TIMEOUT_MS = 15000;    // no answer after this counts as a miss
static const int MAX_QUESTIONS = 70;         // safety cap for the full check
static const int WORKING_SIZE = 3;           // new facts being learned at one time
static const int WARMUP[4] = {24, 7, 56, 108};

/* ================= Facts ================= */
static const int MULTS[10] = {2, 3, 4, 5, 6, 7, 8, 9, 11, 12};
enum { RF = 0, RS = 1, RW = 2 };              // fast, slow, wrong/timeout

static bool isTrivial(int f) { return f == 0 || f == 1 || f == 10; }
static int fkey(int a, int b) { return std::min(a, b) * 13 + std::max(a, b); }
static String keyStr(int k) { return String(k / 13) + "x" + String(k % 13); }
static String niceKey(int k) { return String(k / 13) + " x " + String(k % 13); }
static int parseKey(const String& s) {
  int x = s.indexOf('x');
  if (x < 0) return -1;
  return fkey(s.substring(0, x).toInt(), s.substring(x + 1).toInt());
}
int rnd(int n) { return n > 0 ? (int)(esp_random() % (uint32_t)n) : 0; }
template <class T> static void shuffleVec(std::vector<T>& v) {
  for (int i = (int)v.size() - 1; i > 0; i--) std::swap(v[i], v[rnd(i + 1)]);
}
String joinList(const std::vector<String>& v, const char* sep) {
  String out;
  for (size_t i = 0; i < v.size(); i++) { if (i) out += sep; out += v[i]; }
  return out;
}
static String listWords(const std::vector<String>& v) {
  if (v.size() < 2) return joinList(v);
  std::vector<String> head(v.begin(), v.end() - 1);
  return joinList(head) + " and " + v.back();
}

static const char* rate(const std::vector<uint8_t>& r) {
  if (r.empty()) return "";
  float score = 0; bool anyW = false;
  for (auto x : r) { score += (x == RF) ? 1.f : (x == RS) ? 0.5f : 0.f; if (x == RW) anyW = true; }
  score /= r.size();
  if (!anyW && score >= 0.85f) return "Known";
  if (score < 0.5f) return "Not yet";
  return "Developing";
}

/* ================= Student ================= */
Student S;
bool online = false;
std::vector<String> roster;
std::map<String, Student> offlineCache;   // remembers students for this power-on when offline
Preferences prefs;

/* ================= Drawing ================= */
M5Canvas cv(&M5Cardputer.Display);

void present() { cv.pushSprite(0, 0); }

static std::vector<String> wrap(const String& text, int maxW) {
  std::vector<String> lines;
  String line;
  int i = 0, n = text.length();
  while (i < n) {
    int sp = text.indexOf(' ', i);
    if (sp < 0) sp = n;
    String word = text.substring(i, sp);
    String trial = line.length() ? line + " " + word : word;
    if (cv.textWidth(trial.c_str()) > maxW && line.length()) { lines.push_back(line); line = word; }
    else line = trial;
    i = sp + 1;
  }
  if (line.length()) lines.push_back(line);
  return lines;
}

int drawWrapped(const String& text, int y, const lgfx::IFont* font, uint16_t color, int lineH) {
  cv.setFont(font);
  cv.setTextColor(color);
  cv.setTextDatum(top_center);
  for (auto& l : wrap(text, W - 12)) { cv.drawString(l.c_str(), W / 2, y); y += lineH; }
  return y;
}

static void fitFont(const String& s) {
  const lgfx::IFont* fonts[] = {&fonts::FreeSansBold24pt7b, &fonts::FreeSansBold18pt7b, &fonts::FreeSansBold12pt7b};
  for (auto f : fonts) { cv.setFont(f); if (cv.textWidth(s.c_str()) <= W - 12) return; }
}

/* ================= Keyboard ================= */
KeyEv readKey() {
  KeyEv e;
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto st = M5Cardputer.Keyboard.keysState();
    e.any = true;
    e.enter = st.enter;
    e.del = st.del;
    for (char c : st.word) {
      if (c == ';') e.up = true;        // arrow-up key
      if (c == '.') e.down = true;      // arrow-down key
      e.ch = c;
    }
  }
  return e;
}

void flushKeys() { M5Cardputer.update(); }

void waitEnter() {
  while (true) { KeyEv e = readKey(); if (e.enter) return; delay(5); }
}

/* ================= Screens ================= */
void messageScreen(const String& title, const String& body, const String& footer) {
  cv.fillScreen(C_BG);
  int y = drawWrapped(title, 10, &fonts::FreeSansBold12pt7b, C_INK, 26);
  drawWrapped(body, y + 6, &fonts::FreeSans9pt7b, C_SOFT, 19);
  if (footer.length()) {
    cv.setFont(&fonts::FreeSansBold9pt7b);
    cv.setTextColor(C_BLUE);
    cv.setTextDatum(bottom_center);
    cv.drawString(footer.c_str(), W / 2, H - 4);
  }
  present();
}

// Vertical list. Up/down (; and .) to move, Enter to choose, a letter jumps to it.
int runMenu(const String& title, const String& body, const std::vector<String>& items, int sel) {
  int top = 0;
  const int rowH = 22;
  while (true) {
    cv.fillScreen(C_BG);
    int y = drawWrapped(title, 4, &fonts::FreeSansBold12pt7b, C_INK, 24);
    if (body.length()) y = drawWrapped(body, y + 1, &fonts::FreeSans9pt7b, C_SOFT, 17);
    y += 3;
    int rows = std::max(2, (H - 2 - y) / rowH);
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;

    cv.setFont(&fonts::FreeSansBold9pt7b);
    cv.setTextDatum(middle_center);
    for (int i = 0; i < rows && top + i < (int)items.size(); i++) {
      int idx = top + i, ry = y + i * rowH;
      if (idx == sel) { cv.fillRoundRect(14, ry, W - 28, rowH - 3, 9, C_BLUE); cv.setTextColor(C_WHITE); }
      else { cv.drawRoundRect(14, ry, W - 28, rowH - 3, 9, C_LINE); cv.setTextColor(C_INK); }
      cv.drawString(items[idx].c_str(), W / 2, ry + (rowH - 3) / 2);
    }
    if (top > 0) cv.fillTriangle(W - 7, y, W - 11, y + 6, W - 3, y + 6, C_SOFT);
    if (top + rows < (int)items.size()) {
      int by = y + rows * rowH - 9;
      cv.fillTriangle(W - 7, by + 6, W - 11, by, W - 3, by, C_SOFT);
    }
    present();

    while (true) {
      KeyEv e = readKey();
      if (!e.any) { delay(5); continue; }
      if (e.enter) return sel;
      if (e.up && sel > 0) { sel--; break; }
      if (e.down && sel < (int)items.size() - 1) { sel++; break; }
      if (isalpha(e.ch)) {
        char c = tolower(e.ch);
        int n = items.size();
        bool moved = false;
        for (int k = 1; k <= n; k++) {
          int j = (sel + k) % n;
          if (items[j].length() && tolower(items[j][0]) == c) { sel = j; moved = true; break; }
        }
        if (moved) break;
      }
    }
  }
}

static String inputName() {
  String s;
  while (true) {
    cv.fillScreen(C_BG);
    drawWrapped("Type your name", 10, &fonts::FreeSansBold12pt7b, C_INK, 26);
    int bx = 16, by = 50, bw = W - 32, bh = 36;
    cv.fillRoundRect(bx, by, bw, bh, 10, C_WHITE);
    cv.drawRoundRect(bx, by, bw, bh, 10, C_BLUE);
    cv.setFont(&fonts::FreeSansBold12pt7b);
    cv.setTextColor(C_INK);
    cv.setTextDatum(middle_center);
    cv.drawString((s + "_").c_str(), W / 2, by + bh / 2);
    cv.setFont(&fonts::FreeSans9pt7b);
    cv.setTextColor(C_SOFT);
    cv.setTextDatum(bottom_center);
    cv.drawString("Press Enter when done", W / 2, H - 6);
    present();

    while (true) {
      KeyEv e = readKey();
      if (!e.any) { delay(5); continue; }
      if (e.enter) { s.trim(); if (s.length()) return s; break; }
      if (e.del && s.length()) { s.remove(s.length() - 1); break; }
      char c = e.ch;
      if (c && (isalpha(c) || c == ' ' || c == '-' || c == '\'') && s.length() < 20) { s += c; break; }
    }
  }
}

static void drawProblem(const String& prompt, const String& prob, const String& ans,
                        uint16_t probColor, float prog, bool showBox = true) {
  cv.fillScreen(C_BG);
  if (prompt.length()) {
    cv.setFont(&fonts::FreeSans9pt7b);
    cv.setTextColor(C_SOFT);
    cv.setTextDatum(top_center);
    cv.drawString(prompt.c_str(), W / 2, 4);
  }
  if (prob.length()) {
    fitFont(prob);
    cv.setTextColor(probColor);
    cv.setTextDatum(middle_center);
    cv.drawString(prob.c_str(), W / 2, showBox ? 46 : 62);
  }
  if (showBox) {
    int bw = 110, bh = 38, bx = (W - bw) / 2, by = 80;
    cv.fillRoundRect(bx, by, bw, bh, 10, C_WHITE);
    cv.drawRoundRect(bx, by, bw, bh, 10, C_BLUE);
    cv.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, C_BLUE);
    cv.setFont(&fonts::FreeSansBold18pt7b);
    cv.setTextColor(C_INK);
    cv.setTextDatum(middle_center);
    cv.drawString(ans.c_str(), W / 2, by + bh / 2 + 1);
  }
  int pw = (int)(W * std::min(1.f, std::max(0.f, prog)));
  cv.fillRect(0, H - 5, pw, 5, C_BLUE);
  present();
}

// Shows a problem and waits for an answer. Returns the number typed, or -1 on timeout.
static int askNumber(const String& prompt, const String& prob, float prog, uint32_t& elapsed) {
  String ans;
  flushKeys();
  drawProblem(prompt, prob, ans, C_INK, prog);
  uint32_t t0 = millis();
  while (true) {
    if (millis() - t0 >= TIMEOUT_MS) { elapsed = TIMEOUT_MS; return -1; }
    KeyEv e = readKey();
    if (!e.any) { delay(2); continue; }
    if (e.enter && ans.length()) { elapsed = millis() - t0; return ans.toInt(); }
    if (e.del && ans.length()) ans.remove(ans.length() - 1);
    else if (e.ch >= '0' && e.ch <= '9' && ans.length() < 3) ans += e.ch;
    else continue;
    drawProblem(prompt, prob, ans, C_INK, prog);
  }
}

static void blankPause(float prog) {
  drawProblem("", "", "", C_INK, prog);
  delay(250);
  flushKeys();
}

static void showFact(int a, int b, bool right, float prog) {
  String s = String(a) + " x " + String(b) + " = " + String(a * b);
  drawProblem("", s, "", right ? C_GREEN : C_BLUE, prog, false);
  delay(right ? 700 : 2200);
  flushKeys();
}

/* ================= Saving ================= */
// Shows "Saving..." then returns a short status line.
bool trySave(JsonDocument& doc, const String& title, String& status) {
  if (!online) { status = "Not saved (no Wi-Fi)."; return false; }
  messageScreen(title, "Saving your work...");
  bool ok = apiPost(doc);
  status = ok ? "Saved." : "Did not save. Tell your teacher.";
  return ok;
}

/* ================= Checks (full + quick) ================= */
struct Check {
  bool full = true;
  int f = -1;
  std::vector<uint8_t> res[13];
  std::set<int> used;
  std::vector<String> missed;
  int asked = 0;
  uint32_t limit = 5000, seconds = 0;
  std::vector<std::pair<int, int>> list;
  int total = 0;
};

static void needFor(int f, int& mn, int& mx) { if (isTrivial(f)) { mn = 2; mx = 4; } else { mn = 3; mx = 6; } }

static bool decided(Check& C, int f) {
  auto& r = C.res[f];
  int mn, mx; needFor(f, mn, mx);
  if ((int)r.size() >= mx) return true;
  bool allF = true; int w = 0;
  for (auto x : r) { if (x != RF) allF = false; if (x == RW) w++; }
  if ((int)r.size() >= mn && allF) return true;
  return w >= 3;
}

static std::vector<int> candidates(Check& C, int f) {
  std::vector<int> v;
  for (int n : MULTS) if (!C.used.count(fkey(f, n))) v.push_back(n);
  return v;
}

static bool nextAdaptive(Check& C, int& a, int& b) {
  if (C.asked >= MAX_QUESTIONS) return false;
  std::vector<int> open;
  for (int f = 0; f <= 12; f++) if (!decided(C, f) && !candidates(C, f).empty()) open.push_back(f);
  if (open.empty()) return false;
  size_t fewest = SIZE_MAX;
  for (int f : open) fewest = std::min(fewest, C.res[f].size());
  std::vector<int> pool;
  for (int f : open) if (C.res[f].size() == fewest) pool.push_back(f);
  int f = pool[rnd(pool.size())];
  auto c = candidates(C, f);
  if (!isTrivial(f)) {
    // Prefer facts that also check another open table (6x7 checks the 6s and 7s)
    std::vector<int> both;
    for (int n : c) if (n != f && std::find(open.begin(), open.end(), n) != open.end()) both.push_back(n);
    if (!both.empty()) c = both;
  }
  a = f; b = c[rnd(c.size())];
  return true;
}

static void runCheckQuestions(Check& C) {
  uint32_t start = millis();
  while (true) {
    float prog; int a, b; bool have;
    if (C.full) {
      float d = 0;
      for (int f = 0; f <= 12; f++) { int mn, mx; needFor(f, mn, mx); d += decided(C, f) ? 1.f : (float)C.res[f].size() / mx; }
      prog = d / 13.f;
      have = nextAdaptive(C, a, b);
    } else {
      prog = (float)(C.total - C.list.size()) / C.total;
      have = !C.list.empty();
      if (have) { a = C.list.front().first; b = C.list.front().second; C.list.erase(C.list.begin()); }
    }
    if (!have) break;
    C.used.insert(fkey(a, b));
    C.asked++;
    if (rnd(2)) std::swap(a, b);

    uint32_t el;
    int v = askNumber("", String(a) + " x " + String(b), prog, el);
    uint8_t r = (v == a * b) ? (el <= C.limit ? RF : RS) : RW;
    if (C.full) {
      if (isTrivial(a) || isTrivial(b)) {
        if (isTrivial(a)) C.res[a].push_back(r);
        if (isTrivial(b) && b != a) C.res[b].push_back(r);
      } else {
        C.res[a].push_back(r);
        if (b != a) C.res[b].push_back(r);
      }
    } else {
      C.res[C.f].push_back(r);
    }
    if (r != RF) C.missed.push_back(String(std::min(a, b)) + "×" + String(std::max(a, b)) + (r == RS ? " (slow)" : ""));
    blankPause(prog);
  }
  C.seconds = (millis() - start) / 1000;
}

static void finishCheck(Check& C) {
  JsonDocument doc;
  doc["action"] = "result";
  doc["name"] = S.name;
  doc["type"] = C.full ? String("Full") : String(C.f) + "s check";
  doc["seconds"] = C.seconds;
  doc["questions"] = C.asked;
  doc["practice"] = joinList(C.missed);
  doc["limitMs"] = C.limit;
  JsonObject fam = doc["families"].to<JsonObject>();

  S.hasCheck = true;
  for (int f = 0; f <= 12; f++) {
    if (!C.full && f != C.f) continue;
    const char* r = rate(C.res[f]);
    fam[String(f)] = r;
    if (r[0]) S.rating[f] = r;
  }

  String title, msg;
  if (C.full) {
    S.limitMs = C.limit;
    S.flagged.clear();
    for (auto& m : C.missed) {
      int x = m.indexOf("×");
      if (x > 0) S.flagged.insert(fkey(m.substring(0, x).toInt(), m.substring(x + 2).toInt()));
    }
    title = "All done, " + S.name + "!";
    msg = "Thank you for working hard.";
  } else {
    String r = S.rating[C.f];
    String t = String(C.f) + "s";
    title = r == "Known" ? "You know your " + t + "!" : r == "Developing" ? "Your " + t + " are getting faster." : "Keep going with the " + t + ".";
    msg = r == "Known" ? "Great work!" : "More practice will help. You can do this.";
  }

  String status;
  bool saved = trySave(doc, title, status);
  while (true) {
    std::vector<String> items;
    if (!saved && online) items.push_back("Try saving again");
    items.push_back("Back to menu");
    int i = runMenu(title, msg + " " + status, items);
    if (items[i] == "Try saving again") { saved = trySave(doc, title, status); continue; }
    return;
  }
}

static void runFullCheck() {
  Check C;
  C.full = true;
  messageScreen("Warm-up", "Type each number you see and press Enter.", "Press Enter to begin");
  waitEnter();
  std::vector<uint32_t> times;
  for (int i = 0; i < 4; i++) {
    uint32_t el;
    askNumber("Type this number", String(WARMUP[i]), i / 4.f, el);
    times.push_back(el);
    blankPause((i + 1) / 4.f);
  }
  std::sort(times.begin(), times.end());
  // Fast = about 2 seconds of thinking plus this student's own typing time
  C.limit = std::min<uint32_t>(8000, std::max<uint32_t>(3000, 1700 + (times[1] + times[2]) / 2));

  messageScreen("Times tables", "Type each answer, then press Enter.", "Press Enter to start");
  waitEnter();
  runCheckQuestions(C);
  finishCheck(C);
}

static void runQuickCheck(int f) {
  Check C;
  C.full = false;
  C.f = f;
  C.limit = S.limitMs;
  for (int n : MULTS) C.list.push_back({f, n});
  shuffleVec(C.list);
  C.list.resize(isTrivial(f) ? 4 : 6);
  C.total = C.list.size();
  messageScreen("Quick check: " + String(f) + "s", "Type each answer, then press Enter.", "Press Enter to start");
  waitEnter();
  runCheckQuestions(C);
  finishCheck(C);
}

/* ================= Practice ================= */
enum ActType { A_MENU, A_PRACTICE, A_QUICK, A_FULL, A_SWITCH };
struct Action { ActType type; int f; };

struct Item { int a, b, k, rank; uint8_t st; int streak, due; };  // st: 0 new, 1 working, 2 learned

static Action runPractice(int f) {
  std::vector<Item> items;
  for (int n : MULTS) {
    int k = fkey(f, n);
    int rank = S.working.count(k) ? 0 : S.flagged.count(k) ? 1 : S.rating[n] != "Known" ? 2 : 3;
    items.push_back({f, n, k, rank, (uint8_t)(S.learned.count(k) ? 2 : 0), 0, 0});
  }
  shuffleVec(items);
  std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.rank < y.rank; });

  // Easy facts mixed in for confidence: tables this student already knows
  std::vector<std::pair<int, int>> easy;
  for (int t = 2; t <= 12; t++)
    if (t != f && !isTrivial(t) && S.rating[t] == "Known") for (int n = 2; n <= 5; n++) easy.push_back({t, n});
  if (easy.empty()) for (int n = 2; n <= 5; n++) { easy.push_back({1, n}); easy.push_back({10, n}); easy.push_back({2, n}); }

  messageScreen("Practice: " + String(f) + "s",
                "Missed ones come back later for another try.",
                "Press Enter to start");
  waitEnter();

  uint32_t start = millis();
  int step = 0, asked = 0, correct = 0, lastKey = -1;
  std::vector<int> learnedNow;
  bool finished = false;

  auto learnedCount = [&]() { int c = 0; for (auto& it : items) if (it.st == 2) c++; return c; };

  while (millis() - start < SESSION_MS) {
    float prog = (float)learnedCount() / items.size();
    std::vector<Item*> working;
    for (auto& it : items) if (it.st == 1) working.push_back(&it);
    std::sort(working.begin(), working.end(), [](Item* x, Item* y) { return x->due < y->due; });
    Item* due = nullptr;
    for (auto w : working) if (w->due <= step) { due = w; break; }
    Item* fresh = nullptr;
    for (auto& it : items) if (it.st == 0) { fresh = &it; break; }

    Item* q = nullptr;
    int a = 0, b = 0;
    if (due) q = due;
    else if ((int)working.size() < WORKING_SIZE && fresh) q = fresh;
    else if (!working.empty()) {
      // Filler: a learned fact from this table or an easy fact
      std::vector<Item*> learnedItems;
      for (auto& it : items) if (it.st == 2 && it.k != lastKey) learnedItems.push_back(&it);
      std::vector<std::pair<int, int>> easyOk;
      for (auto& e : easy) if (fkey(e.first, e.second) != lastKey) easyOk.push_back(e);
      int total = learnedItems.size() + easyOk.size();
      if (total == 0) q = working[0];
      else {
        int pickI = rnd(total);
        if (pickI < (int)learnedItems.size()) q = learnedItems[pickI];
        else { auto e = easyOk[pickI - learnedItems.size()]; a = e.first; b = e.second; }
      }
    } else { finished = true; break; }

    if (q) { a = q->a; b = q->b; }
    lastKey = fkey(a, b);
    if (rnd(2)) std::swap(a, b);

    uint32_t el;
    int v = askNumber("", String(a) + " x " + String(b), prog, el);
    bool right = (v == a * b);
    bool fast = right && el <= S.limitMs;
    asked++;
    if (right) correct++;

    if (q) {
      if (q->st == 0) {
        if (fast) q->st = 2;                                   // already knows it: retire
        else { q->st = 1; q->streak = 0; q->due = step + 3; }
      } else if (q->st == 1) {
        if (fast) {
          q->streak++;
          if (q->streak >= 2) { q->st = 2; learnedNow.push_back(q->k); }
          else q->due = step + 4;
        } else { q->streak = 0; q->due = step + 3; }
      } else if (!fast) {                                      // a learned fact slipped
        q->st = 1; q->streak = 0; q->due = step + 3;
        learnedNow.erase(std::remove(learnedNow.begin(), learnedNow.end(), q->k), learnedNow.end());
      }
    }
    step++;
    showFact(a, b, right, (float)learnedCount() / items.size());
  }

  // Update this student's progress
  std::vector<String> learnedNice, workingNice;
  for (auto& it : items) {
    S.learned.erase(it.k);
    S.working.erase(it.k);
    if (it.st == 2) S.learned.insert(it.k);
    if (it.st == 1) { S.working.insert(it.k); workingNice.push_back(niceKey(it.k)); }
  }
  for (int k : learnedNow) learnedNice.push_back(niceKey(k));

  JsonDocument doc;
  doc["action"] = "practice";
  doc["name"] = S.name;
  doc["table"] = f;
  doc["seconds"] = (millis() - start) / 1000;
  doc["problems"] = asked;
  doc["percent"] = asked ? (int)round(correct * 100.0 / asked) : 0;
  doc["learned"] = joinList(learnedNice);
  doc["working"] = joinList(workingNice);
  doc["tableProgress"] = String(learnedCount()) + " of " + String(items.size());
  JsonObject prog = doc["progress"].to<JsonObject>();
  JsonArray la = prog["learned"].to<JsonArray>();
  JsonArray wa = prog["working"].to<JsonArray>();
  for (int k : S.learned) la.add(keyStr(k));
  for (int k : S.working) wa.add(keyStr(k));

  String t = String(f) + "s";
  String title = finished ? "You finished the " + t + "!" : String("Nice practice!");
  String msg = learnedNice.size() ? "You learned " + listWords(learnedNice) + "."
                                  : "You answered " + String(asked) + " problems.";
  String status;
  bool saved = trySave(doc, title, status);

  while (true) {
    std::vector<String> labels;
    std::vector<Action> acts;
    if (!saved && online) { labels.push_back("Try saving again"); acts.push_back({A_MENU, -1}); }
    if (!finished) { labels.push_back("Keep practicing the " + t); acts.push_back({A_PRACTICE, f}); }
    labels.push_back("Quick check of the " + t); acts.push_back({A_QUICK, f});
    labels.push_back("Back to menu"); acts.push_back({A_MENU, -1});
    int i = runMenu(title, msg + " " + status, labels);
    if (labels[i] == "Try saving again") { saved = trySave(doc, title, status); continue; }
    return acts[i];
  }
}

/* ================= Name + menu ================= */
static std::vector<int> weakTables() {
  std::vector<int> list;
  for (int f = 0; f <= 12; f++) if (S.rating[f] == "Not yet") list.push_back(f);
  for (int f = 0; f <= 12; f++) if (S.rating[f] == "Developing") list.push_back(f);
  return list;
}

static bool tableDone(int f) {
  for (int n : MULTS) if (!S.learned.count(fkey(f, n))) return false;
  return true;
}

static Action openMenu() {
  std::vector<String> labels;
  std::vector<Action> acts;
  String msg;
  auto add = [&](const String& l, Action a) { labels.push_back(l); acts.push_back(a); };

  if (!S.hasCheck) {
    msg = "Let's start with the times tables check.";
    add("Take the check", {A_FULL, -1});
  } else {
    auto weak = weakTables();
    int rec = -1;
    for (int f : weak) if (!tableDone(f)) { rec = f; break; }
    if (weak.empty()) {
      msg = "You know all your tables! Pick one for extra speed.";
      for (int t : MULTS) add("Practice the " + String(t) + "s", {A_PRACTICE, t});
    } else if (rec >= 0) {
      msg = "Today's goal: the " + String(rec) + "s.";
      add("Practice the " + String(rec) + "s", {A_PRACTICE, rec});
      for (int f : weak) if (f != rec) add("Practice the " + String(f) + "s", {A_PRACTICE, f});
    } else {
      msg = "You practiced your goal tables. Show what you know!";
      for (int f : weak) add("Quick check: " + String(f) + "s", {A_QUICK, f});
    }
    add("Full times tables check", {A_FULL, -1});
  }
  add("Back", {A_SWITCH, -1});
  int i = runMenu("Hi, " + S.name + "!", msg, labels);
  return acts[i];
}

static bool loadStudent(const String& name) {
  JsonDocument doc;
  if (!apiGet("action=student&name=" + urlEncode(name), doc)) return false;
  S.hasCheck = doc["hasCheck"] | false;
  for (JsonPair kv : doc["ratings"].as<JsonObject>()) {
    int f = atoi(kv.key().c_str());
    if (f >= 0 && f <= 12) S.rating[f] = String((const char*)(kv.value() | ""));
  }
  float lim = doc["limitMs"].as<float>();
  if (lim > 0) S.limitMs = (uint32_t)lim;
  for (JsonVariant v : doc["flagged"].as<JsonArray>()) { int k = parseKey(v.as<String>()); if (k >= 0) S.flagged.insert(k); }
  for (JsonVariant v : doc["progress"]["learned"].as<JsonArray>()) { int k = parseKey(v.as<String>()); if (k >= 0) S.learned.insert(k); }
  for (JsonVariant v : doc["progress"]["working"].as<JsonArray>()) { int k = parseKey(v.as<String>()); if (k >= 0) S.working.insert(k); }
  return true;
}

static void chooseName() {
  if (!online && S.name.length()) offlineCache[S.name] = S;   // keep this session's work

  std::vector<String> items = roster;
  items.push_back("Type my name");
  int i = runMenu("Who are you?", "Up/down to move, Enter to pick.", items);
  String name = (i == (int)items.size() - 1) ? inputName() : items[i];

  S = Student();
  S.name = name;
  if (online) {
    messageScreen("Hi, " + name + "!", "Loading...");
    if (!loadStudent(name)) {
      S = Student();
      S.name = name;
      messageScreen("Hmm", "Your past work did not load. You can still practice.");
      delay(2500);
    }
  } else if (offlineCache.count(name)) {
    S = offlineCache[name];
  }
}

static void loadCachedRoster() {
  String saved = prefs.getString("roster", "");
  roster.clear();
  int i = 0;
  while (i < (int)saved.length()) {
    int nl = saved.indexOf('\n', i);
    if (nl < 0) nl = saved.length();
    String n = saved.substring(i, nl);
    if (n.length()) roster.push_back(n);
    i = nl + 1;
  }
}

// Returns false if the script didn't answer (wrong URL, key, or access setting).
static bool loadRoster() {
  JsonDocument doc;
  if (!apiGet("action=roster", doc)) return false;
  roster.clear();
  for (JsonVariant v : doc["names"].as<JsonArray>()) roster.push_back(v.as<String>());
  prefs.putString("roster", joinList(roster, "\n"));      // saved for days without Wi-Fi
  return true;
}

/* ================= Main ================= */
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(120);
  cv.setColorDepth(16);
  cv.createSprite(W, H);
  prefs.begin("ttables", false);

  messageScreen("Times Tables", "Connecting to Wi-Fi...");
  online = wifiConnect(12000);
  if (online) {
    messageScreen("Times Tables", "Loading your class...");
    online = loadRoster();
  }
  if (!online) {
    loadCachedRoster();   // the class list from the last time it connected
    messageScreen("No connection", "You can still take the check and practice, but today's work will not be saved.");
    delay(3500);
  }
}

// Times tables menu loop; returns when the student picks Back.
static void tablesLoop() {
  Action next = {A_MENU, -1};
  while (true) {
    if (next.type == A_MENU) next = openMenu();
    switch (next.type) {
      case A_PRACTICE: next = runPractice(next.f); break;
      case A_QUICK:    runQuickCheck(next.f); next = {A_MENU, -1}; break;
      case A_FULL:     runFullCheck();        next = {A_MENU, -1}; break;
      case A_SWITCH:   return;
      default:         next = {A_MENU, -1};
    }
  }
}

void loop() {
  chooseName();
  int sel = 0;
  while (true) {
    sel = runMenu("Hi, " + S.name + "!", "What do you want to practice?",
                  {"Times tables", "Multi-digit multiplying", "Change name"}, sel);
    if (sel == 0) tablesLoop();
    else if (sel == 1) algoLoop();
    else return;                  // loop() runs again and asks for a name
  }
}
