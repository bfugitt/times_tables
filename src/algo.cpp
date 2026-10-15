// Multi-digit multiplication (standard algorithm) for the Cardputer ADV.
// Same levels, help levels, scoring, and Sheet logging as the Chromebook version.

#include "app.h"
#include "net.h"
#include <map>
#include <set>
#include <algorithm>

/* ================= Settings ================= */
static const int ALGO_PROBLEMS = 5;
static const char* HELP_NAMES[4] = {"Guided", "Highlights", "Row checks", "Independent"};
// On screen (the Cardputer fonts have no × sign)
static const char* LEVEL_SCREEN[9] = {"",
  "2-digit x 1-digit, no carrying", "2-digit x 1-digit", "3-digit x 1-digit",
  "4- and 5-digit x 1-digit", "Zero in the middle",
  "2-digit x 2-digit", "3-digit x 2-digit", "4-digit x 2-digit"};
// In the Sheet (matches the Chromebook version)
static const char* LEVEL_SHEET[9] = {"",
  "2-digit × 1-digit, no carrying", "2-digit × 1-digit", "3-digit × 1-digit",
  "4- and 5-digit × 1-digit", "Zero in the middle",
  "2-digit × 2-digit", "3-digit × 2-digit", "4-digit × 2-digit"};

static constexpr uint16_t C_TINT0 = rgb(0xE1, 0xED, 0xF7);   // ones row
static constexpr uint16_t C_TINT1 = rgb(0xE2, 0xF1, 0xE6);   // tens row
static constexpr uint16_t C_HL    = rgb(0xFF, 0xF0, 0xB8);
static constexpr uint16_t C_AMBER = rgb(0xB8, 0x69, 0x1C);

/* ================= Problem model ================= */
enum Kind : uint8_t { K_DIGIT, K_LEAD, K_CARRY, K_ZERO, K_SUM, K_SUMLEAD };
enum CStat : uint8_t { CS_NONE, CS_WRONG, CS_SHOWN };

struct Cell { int val = -1; int expected = 0; Kind kind = K_DIGIT; CStat st = CS_NONE; bool scratch = false; };
struct Step {
  String id; int expected; Kind kind; int row;     // row: 0 ones row, 1 tens row, 2 adding
  int ai = 0, bd = 0, c = 0;
  std::vector<String> hl;
  String shortP, longP;
};

struct Problem {
  long A = 0, B = 0;
  std::vector<int> a, b;
  std::map<String, Cell> cells;
  std::vector<Step> steps;
  std::map<String, int> idx;
  int cols = 0;
  // progress through the problem
  int help = 1, lowest = 1, mistakes = 0, shown = 0, answerSteps = 0;
  std::set<String> errored;
  std::vector<String> errors;
  bool modelUsed = false, skipped = false, done = false, skipArmed = false;
  // Guided + Highlights
  std::vector<int> seq; int cur = 0, tries = 0;
  // Row checks + Independent
  std::vector<int> groups; int gi = 0; std::vector<String> editable; int cursor = 0, checkTries = 0;
  String carrySel;
  // bottom message
  String msg; bool msgGood = false;
  float sessionProg = 0;
};

static int ri(int a, int b) { return a + rnd(b - a + 1); }
static std::vector<int> digs(long n) {
  std::vector<int> d;
  if (n == 0) d.push_back(0);
  while (n > 0) { d.push_back(n % 10); n /= 10; }
  return d;
}
static long noZero(int len) { long v = 0; for (int i = 0; i < len; i++) v = v * 10 + ri(1, 9); return v; }
static bool carries(long A, int b) { for (int d : digs(A)) if (d * b >= 10) return true; return false; }
static String fmt(long n) {
  String s = String(n), out;
  int c = 0;
  for (int i = s.length() - 1; i >= 0; i--) {
    out = String(s[i]) + out;
    if (++c % 3 == 0 && i > 0) out = String(",") + out;
  }
  return out;
}
static String cid(const char* pre, int r, int pos) { return String(pre) + r + "-" + pos; }

static void generate(int level, long& A, long& B) {
  switch (level) {
    case 1: { B = ri(2, 4); int lim = 9 / B; A = ri(1, lim) * 10 + ri(1, lim); break; }
    case 2: do { A = noZero(2); B = ri(3, 9); } while (!carries(A, B)); break;
    case 3: do { A = noZero(3); B = ri(2, 9); } while (!carries(A, B)); break;
    case 4: A = noZero(ri(4, 5)); B = ri(3, 9); break;
    case 5:
      // A zero in the middle that receives a carry (like 405 x 6)
      while (true) {
        int len = ri(3, 5);
        std::vector<int> d = digs(noZero(len));
        int z = ri(1, len - 2);
        d[z] = 0;
        B = ri(3, 9);
        if (d[z - 1] * B >= 10) { A = 0; for (int i = len - 1; i >= 0; i--) A = A * 10 + d[i]; break; }
      }
      break;
    case 6: A = noZero(2); B = noZero(2); break;
    case 7: A = noZero(3); B = noZero(2); break;
    default: A = noZero(4); B = noZero(2);
  }
}

static void addStep(Problem& P, const String& id, int expected, Kind k, int row, int ai, int bd, int c,
                    const std::vector<String>& hl, const String& sp, const String& lp) {
  Cell cell; cell.expected = expected; cell.kind = k;
  P.cells[id] = cell;
  Step s; s.id = id; s.expected = expected; s.kind = k; s.row = row; s.ai = ai; s.bd = bd; s.c = c;
  s.hl = hl; s.shortP = sp; s.longP = lp;
  P.idx[id] = P.steps.size();
  P.steps.push_back(s);
}

// Each step is one box the student fills, in the same order as on paper.
static void makeProblem(Problem& P, long A, long B) {
  P = Problem();
  P.A = A; P.B = B;
  P.a = digs(A); P.b = digs(B);
  int n = P.a.size();

  for (int r = 0; r < (int)P.b.size(); r++) {
    int bd = P.b[r];
    for (int z = 0; z < r; z++) {
      addStep(P, cid("p", r, z), 0, K_ZERO, r, 0, bd, 0, {String("b-") + r},
              "Tens row: type 0 first.",
              "Now multiply by the " + String(bd) + " in the tens place. First type 0 in the ones place. It holds the place.");
    }
    int c = 0;
    for (int i = 0; i < n; i++) {
      int ai = P.a[i], prod = ai * bd + c, pos = i + r;
      bool last = (i == n - 1);
      std::vector<String> hl = {String("a-") + i, String("b-") + r};
      if (c > 0) hl.push_back(cid("c", r, i));
      String base = String(bd) + " x " + String(ai);
      String sp = c > 0 ? base + ", add " + String(c) + (prod >= 10 ? ". Ones digit?" : ".")
                        : (prod >= 10 ? base + ". Write the ones digit." : "What is " + base + "?");
      String lp = (c > 0 ? "What is " + base + "? Then add the " + String(c) + " you carried."
                         : "What is " + base + "?") +
                  (prod >= 10 ? " Write the ones digit in the box." : " Write it in the box.");
      addStep(P, cid("p", r, pos), prod % 10, K_DIGIT, r, ai, bd, c, hl, sp, lp);
      if (last && prod >= 10) {
        addStep(P, cid("p", r, pos + 1), prod / 10, K_LEAD, r, ai, bd, c, hl,
                "Write the tens digit in front.", "This is the last digit, so write the tens digit in front.");
      }
      int nc = prod / 10;
      if (!last && nc > 0) {
        addStep(P, cid("c", r, i + 1), nc, K_CARRY, r, 0, 0, 0, {cid("p", r, pos)},
                "Carry the tens digit up top.", "Carry the tens digit. Write it in the small box above the next number.");
      }
      c = last ? 0 : nc;
    }
    // Every column gets a carry box, so the boxes don't give away where carries go
    for (int p = 1; p < n; p++) {
      String id = cid("c", r, p);
      if (!P.cells.count(id)) { Cell cell; cell.kind = K_CARRY; cell.scratch = true; P.cells[id] = cell; }
    }
  }

  if (P.b.size() == 2) {
    std::vector<int> d0 = digs(A * P.b[0]), d1 = digs(A * P.b[1] * 10);
    int len = std::max(d0.size(), d1.size());
    int c = 0;
    for (int pos = 0; pos < len; pos++) {
      int x = (pos < (int)d0.size() ? d0[pos] : 0) + (pos < (int)d1.size() ? d1[pos] : 0) + c;
      std::vector<String> hl;
      if (pos < (int)d0.size()) hl.push_back(cid("p", 0, pos));
      if (pos < (int)d1.size()) hl.push_back(cid("p", 1, pos));
      String sp = c ? "Add this column + 1 carried." : "Add this column.";
      String lp = String("Add the numbers in this column") + (c ? ", plus the 1 you carried." : ".") +
                  (x >= 10 && pos < len - 1 ? " Write the ones digit and carry the 1." : "");
      addStep(P, String("s-") + pos, x % 10, K_SUM, 2, 0, 0, c, hl, sp, lp);
      if (pos == len - 1 && x >= 10) addStep(P, String("s-") + (pos + 1), 1, K_SUMLEAD, 2, 0, 0, 0, hl, "Write the 1 in front.", "Write the 1 in front.");
      c = x / 10;
    }
  }
  P.cols = digs(A * B).size() + 1;
}

static String classify(const Step& s, int v) {
  switch (s.kind) {
    case K_ZERO: return "Placeholder zero";
    case K_CARRY: return "Carrying";
    case K_SUM: case K_SUMLEAD: return "Adding";
    case K_LEAD:
      if (s.c > 0 && v == (s.ai * s.bd) / 10) return "Forgot to add the carry";
      if (s.c > 0 && v == ((s.ai + s.c) * s.bd) / 10) return "Added the carry first";
      return "Times fact";
    default:
      if (s.c > 0 && v == (s.ai * s.bd) % 10) return "Forgot to add the carry";
      if (s.c > 0 && v == ((s.ai + s.c) * s.bd) % 10) return "Added the carry first";
      return "Times fact";
  }
}

/* ================= State helpers ================= */
static bool isFree(Problem& P) { return P.help >= 2; }

static Step* currentStep(Problem& P) {
  if (P.done) return nullptr;
  if (!isFree(P)) return P.cur < (int)P.seq.size() ? &P.steps[P.seq[P.cur]] : nullptr;
  if (P.cursor >= (int)P.editable.size()) return nullptr;
  return &P.steps[P.idx[P.editable[P.cursor]]];
}

static void recordError(Problem& P, const Step& s, int v) {
  if (P.errored.count(s.id)) return;
  P.errored.insert(s.id);
  P.mistakes++;
  P.errors.push_back(classify(s, v));
}

static void setMsg(Problem& P, const String& m, bool good = false) { P.msg = m; P.msgGood = good; }

/* ================= Drawing ================= */
static void drawBox(Problem& P, const String& id, int cx, int top, int w, int h,
                    const lgfx::IFont* font, uint16_t tint, const std::set<String>& hl, Step* cs) {
  auto it = P.cells.find(id);
  if (it == P.cells.end()) return;
  Cell& c = it->second;
  bool fr = isFree(P);
  bool isCur = !P.done && ((fr && P.carrySel == id) || (!P.carrySel.length() && cs && cs->id == id));
  bool inGroup = std::find(P.editable.begin(), P.editable.end(), id) != P.editable.end();
  bool later = fr && !P.done && c.kind != K_CARRY && !inGroup && c.val < 0;
  if (P.done && c.kind == K_CARRY && c.val < 0) return;   // hide unused carry boxes at the end

  uint16_t fill = hl.count(id) ? C_HL : later ? C_BG : (tint ? tint : C_WHITE);
  int x = cx - w / 2;
  cv.fillRoundRect(x, top, w, h, 5, fill);
  uint16_t border = isCur ? C_BLUE : c.st == CS_WRONG ? C_AMBER : C_LINE;
  cv.drawRoundRect(x, top, w, h, 5, border);
  if (isCur || c.st == CS_WRONG) cv.drawRoundRect(x + 1, top + 1, w - 2, h - 2, 4, border);
  if (c.val >= 0) {
    cv.setFont(font);
    cv.setTextDatum(middle_center);
    cv.setTextColor(c.st == CS_WRONG ? C_AMBER : c.st == CS_SHOWN ? C_BLUE : C_INK);
    cv.drawString(String(c.val).c_str(), cx, top + h / 2 + 1);
  }
}

static int chip(int x, int y, const String& text, uint16_t fill) {
  cv.setFont(&fonts::FreeSansBold9pt7b);
  int w = cv.textWidth(text.c_str()) + 10;
  cv.fillRoundRect(x, y, w, 15, 5, fill);
  cv.setTextColor(C_INK);
  cv.setTextDatum(middle_left);
  cv.drawString(text.c_str(), x + 5, y + 8);
  return x + w + 4;
}

static String rowDigits(Problem& P, int r) {
  String s;
  for (int pos = P.cols - 2; pos >= 0; pos--) {
    auto it = P.cells.find(cid("p", r, pos));
    if (it == P.cells.end()) continue;
    s += it->second.val >= 0 ? String(it->second.val) : String("_");
  }
  return s;
}

static void drawAlgo(Problem& P) {
  cv.fillScreen(C_BG);
  bool two = P.b.size() == 2;
  Step* cs = currentStep(P);
  int phase = cs ? cs->row : (two ? 2 : 0);
  std::set<String> hl;
  if (cs && P.help <= 1) for (auto& h : cs->hl) hl.insert(h);

  int n = P.a.size();
  int cw = std::min(30, (W - 8) / P.cols);
  int x0 = (W - P.cols * cw) / 2;
  auto cx = [&](int pos) { return x0 + (P.cols - 1 - pos) * cw + cw / 2; };
  const lgfx::IFont* font = two ? &fonts::FreeSansBold12pt7b : &fonts::FreeSansBold18pt7b;
  const int boxH = two ? 24 : 28, numH = two ? 22 : 28;
  int y = 2;

  auto drawNum = [&](int digit, int col, int top, bool lit) {
    if (lit) cv.fillRoundRect(col - cw / 2 + 1, top, cw - 2, numH, 5, C_HL);
    cv.setFont(font);
    cv.setTextDatum(middle_center);
    cv.setTextColor(C_INK);
    cv.drawString(String(digit).c_str(), col, top + numH / 2 + 1);
  };
  auto sign = [&](const char* t, int top, int h) {
    cv.setFont(font);
    cv.setTextDatum(middle_center);
    cv.setTextColor(C_SOFT);
    cv.drawString(t, x0 + cw / 2, top + h / 2 + 1);
  };
  auto rule = [&]() { cv.fillRect(x0, y + 1, P.cols * cw, 2, C_INK); y += 5; };

  // Header for 2-digit multipliers: which row they're on
  if (two) {
    if (phase == 0) chip(4, y, "Ones row (x " + String(P.b[0]) + ")", C_TINT0);
    else if (phase == 1) { int nx = chip(4, y, "Ones row: " + rowDigits(P, 0), C_TINT0); chip(nx, y, "Tens row", C_TINT1); }
    else {
      cv.setFont(&fonts::FreeSansBold9pt7b);
      cv.setTextColor(C_SOFT);
      cv.setTextDatum(top_left);
      cv.drawString((fmt(P.A) + " x " + fmt(P.B)).c_str(), 6, y + 1);
    }
    y += 17;
  }

  if (phase < 2) {
    // Carry boxes for the row being worked on (earlier carries are put away)
    for (int pos = 1; pos < n; pos++) drawBox(P, cid("c", phase, pos), cx(pos), y, 16, 14, &fonts::FreeSansBold9pt7b, 0, hl, cs);
    y += 16;
    for (int i = 0; i < n; i++) drawNum(P.a[i], cx(i), y, hl.count(String("a-") + i));
    y += numH;
    sign("x", y, numH);
    for (int r = 0; r < (int)P.b.size(); r++) drawNum(P.b[r], cx(r), y, hl.count(String("b-") + r));
    y += numH;
    rule();
    uint16_t tint = two ? (phase == 0 ? C_TINT0 : C_TINT1) : 0;
    for (int pos = 0; pos < P.cols - 1; pos++) drawBox(P, cid("p", phase, pos), cx(pos), y, cw - 4, boxH, font, tint, hl, cs);
    y += boxH;
  } else {
    for (int r = 0; r < 2; r++) {
      if (r == 1) sign("+", y, boxH);
      for (int pos = 0; pos < P.cols - 1; pos++) drawBox(P, cid("p", r, pos), cx(pos), y, cw - 4, boxH, font, r ? C_TINT1 : C_TINT0, hl, cs);
      y += boxH + 2;
    }
    rule();
    for (int pos = 0; pos < P.cols - 1; pos++) drawBox(P, String("s-") + pos, cx(pos), y, cw - 4, boxH, font, 0, hl, cs);
    y += boxH;
  }

  if (P.msg.length()) drawWrapped(P.msg, y + 3, &fonts::FreeSans9pt7b, P.msgGood ? C_GREEN : C_INK, 15);
  cv.fillRect(0, H - 2, (int)(W * P.sessionProg), 2, C_BLUE);
  present();
}

/* ================= Model screen ================= */
static std::vector<long> expandParts(long n) {
  std::vector<int> d = digs(n);
  std::vector<long> parts;
  for (int i = d.size() - 1; i >= 0; i--) if (d[i]) { long p = d[i]; for (int k = 0; k < i; k++) p *= 10; parts.push_back(p); }
  return parts;
}

static void showModel(Problem& P) {
  P.modelUsed = true;
  std::vector<long> pa = expandParts(P.A), pb = expandParts(P.B);
  bool two = P.b.size() == 2;
  int scroll = 0, contentH = 0;
  while (true) {
    cv.fillScreen(C_BG);
    int y = 4 - scroll;

    // Area model, when it fits the screen
    int colW = (W - 46) / pa.size();
    cv.setFont(&fonts::FreeSansBold9pt7b);
    bool fits = true;
    for (long x : pa) for (long yb : pb) if (cv.textWidth(fmt(x * yb).c_str()) > colW - 4) fits = false;
    if (fits) {
      cv.setTextColor(C_INK); cv.setTextDatum(top_left);
      cv.drawString("Area model", 6, y); y += 18;
      cv.setTextColor(C_SOFT); cv.setTextDatum(middle_center);
      for (size_t k = 0; k < pa.size(); k++) cv.drawString(fmt(pa[k]).c_str(), 44 + k * colW + colW / 2, y + 7);
      y += 16;
      for (long yb : pb) {
        cv.setTextColor(C_SOFT); cv.setTextDatum(middle_right);
        cv.drawString(fmt(yb).c_str(), 40, y + 10);
        for (size_t k = 0; k < pa.size(); k++) {
          cv.fillRoundRect(44 + k * colW + 1, y, colW - 2, 20, 4, (two && yb >= 10) ? C_TINT1 : C_TINT0);
          cv.setTextColor(C_INK); cv.setTextDatum(middle_center);
          cv.drawString(fmt(pa[k] * yb).c_str(), 44 + k * colW + colW / 2, y + 11);
        }
        y += 22;
      }
      y += 6;
    }

    // Partial products, ones row first
    cv.setFont(&fonts::FreeSansBold9pt7b);
    cv.setTextColor(C_INK); cv.setTextDatum(top_left);
    cv.drawString("Partial products", 6, y); y += 18;
    cv.setFont(&fonts::FreeSans9pt7b);
    for (int j = pb.size() - 1; j >= 0; j--) {
      for (int k = pa.size() - 1; k >= 0; k--) {
        cv.fillRoundRect(4, y, W - 8, 16, 4, (two && pb[j] >= 10) ? C_TINT1 : C_TINT0);
        cv.setTextColor(C_INK); cv.setTextDatum(middle_left);
        cv.drawString((fmt(pb[j]) + " x " + fmt(pa[k]) + " = " + fmt(pb[j] * pa[k])).c_str(), 10, y + 8);
        y += 18;
      }
    }
    contentH = y + scroll + 4;

    // Footer
    cv.fillRect(0, H - 16, W, 16, C_BG);
    cv.setFont(&fonts::FreeSans9pt7b);
    cv.setTextColor(C_BLUE); cv.setTextDatum(bottom_center);
    cv.drawString(contentH > H - 16 ? "Up/down to scroll. Enter to go back." : "Press Enter to go back.", W / 2, H - 2);
    present();

    while (true) {
      KeyEv e = readKey();
      if (!e.any) { delay(5); continue; }
      if (e.enter || tolower(e.ch) == 'm') { flushKeys(); return; }
      int maxScroll = std::max(0, contentH - (H - 16));
      if (e.up && scroll > 0) { scroll = std::max(0, scroll - 18); break; }
      if (e.down && scroll < maxScroll) { scroll = std::min(maxScroll, scroll + 18); break; }
    }
  }
}

/* ================= Solving ================= */
static void finishProblem(Problem& P);

static void startGroups(Problem& P) {
  P.gi = 0;
  P.checkTries = 0;
  P.groups.clear();
  if (P.help == 3) P.groups.push_back(-1);
  else for (int i : P.seq) { int g = P.steps[i].row; if (std::find(P.groups.begin(), P.groups.end(), g) == P.groups.end()) P.groups.push_back(g); }
}

static void loadGroup(Problem& P) {
  P.editable.clear();
  int g = P.groups[P.gi];
  for (int i : P.seq) if (g == -1 || P.steps[i].row == g) P.editable.push_back(P.steps[i].id);
  P.cursor = 0;
  P.checkTries = 0;
}

static void nextGroup(Problem& P) {
  P.gi++;
  if (P.gi >= (int)P.groups.size()) { P.done = true; return; }
  loadGroup(P);
}

static void revealStep(Problem& P) {
  Step& s = P.steps[P.seq[P.cur]];
  Cell& c = P.cells[s.id];
  c.val = s.expected; c.st = CS_SHOWN;
  if (s.kind != K_CARRY) P.shown++;
  P.tries = 0;
  P.cur++;
  if (P.cur >= (int)P.seq.size()) { P.done = true; return; }
  setMsg(P, "That box is " + String(s.expected) + ". " + (P.help == 0 ? P.steps[P.seq[P.cur]].shortP : String("Keep going!")));
}

static void typeGuided(Problem& P, int v) {
  if (P.cur >= (int)P.seq.size()) return;
  Step& s = P.steps[P.seq[P.cur]];
  Cell& cell = P.cells[s.id];
  if (v == s.expected) {
    cell.val = v; cell.st = CS_NONE;
    P.cur++; P.tries = 0;
    if (P.cur >= (int)P.seq.size()) { P.done = true; return; }
    setMsg(P, P.help == 0 ? P.steps[P.seq[P.cur]].shortP : String(""));
    return;
  }
  recordError(P, s, v);
  P.tries++;
  int revealAt = P.help == 0 ? 2 : 3;
  if (P.tries >= revealAt) { revealStep(P); return; }
  if (P.help == 1 && P.tries == 2) P.lowest = 0;          // needed the prompt
  setMsg(P, "Not quite. " + ((P.help == 0 || P.tries >= 2) ? s.shortP : String("Try again.")));
  cell.val = v; cell.st = CS_WRONG;
  drawAlgo(P);
  delay(600);
  cell.val = -1; cell.st = CS_NONE;
  flushKeys();
}

static void typeFree(Problem& P, int v) {
  if (P.carrySel.length()) { P.cells[P.carrySel].val = v; P.carrySel = ""; return; }
  if (P.cursor >= (int)P.editable.size()) return;
  Cell& c = P.cells[P.editable[P.cursor]];
  if (c.st != CS_SHOWN) { c.val = v; if (c.st == CS_WRONG) c.st = CS_NONE; }
  if (P.cursor < (int)P.editable.size() - 1) P.cursor++;
}

static void backspaceFree(Problem& P) {
  if (P.carrySel.length()) { P.cells[P.carrySel].val = -1; P.carrySel = ""; return; }
  String id = P.editable[P.cursor];
  if (P.cells[id].val < 0 && P.cursor > 0) { P.cursor--; id = P.editable[P.cursor]; }
  if (P.cells[id].st != CS_SHOWN) P.cells[id].val = -1;
}

static void checkFree(Problem& P) {
  for (auto& id : P.editable) if (P.cells[id].val < 0) { setMsg(P, "Fill in every box, then press Enter."); return; }
  std::vector<String> wrong;
  for (auto& id : P.editable) if (P.cells[id].val != P.cells[id].expected) wrong.push_back(id);
  if (wrong.empty()) {
    bool more = P.gi < (int)P.groups.size() - 1;
    setMsg(P, more ? "That row is right! Keep going." : "", true);
    nextGroup(P);
    return;
  }
  P.checkTries++;
  for (auto& id : wrong) recordError(P, P.steps[P.idx[id]], P.cells[id].val);
  if (P.checkTries >= 2) {
    for (auto& id : wrong) { P.cells[id].val = P.cells[id].expected; P.cells[id].st = CS_SHOWN; P.shown++; }
    setMsg(P, "The blue digits show the fix.");
    nextGroup(P);
    return;
  }
  for (auto& id : wrong) P.cells[id].st = CS_WRONG;
  P.editable = wrong;
  P.cursor = 0;
  setMsg(P, wrong.size() == 1 ? "Fix the orange box, then press Enter." : "Fix the orange boxes, then press Enter.");
}

static void showMe(Problem& P) {
  if (!isFree(P)) { revealStep(P); return; }
  if (P.cursor >= (int)P.editable.size()) return;
  Cell& c = P.cells[P.editable[P.cursor]];
  if (c.st != CS_SHOWN) { c.val = c.expected; c.st = CS_SHOWN; P.shown++; }
  if (P.cursor < (int)P.editable.size() - 1) P.cursor++;
  setMsg(P, "The blue digit is filled in for you.");
}

static void hint(Problem& P) {
  Step* s = currentStep(P);
  if (!s) return;
  if (P.help >= 1) P.lowest = 0;                          // same as Guided help
  messageScreen("Hint", s->longP, "Press Enter");
  waitEnter();
  flushKeys();
}

static int scoreProblem(Problem& P) {
  if (P.skipped) return 0;
  if (P.shown * 2 > P.answerSteps) return 1;
  if (P.shown > 0) return 2;
  bool clean = P.mistakes == 0;
  const int scores[4][2] = {{3, 4}, {5, 6}, {7, 8}, {9, 10}};
  return scores[P.lowest][clean ? 1 : 0];
}

static void adapt(Problem& P, int score) {
  bool clean = !P.skipped && P.mistakes == 0 && P.shown == 0;
  if (clean) {
    S.algoStreak++;
    if (S.algoStreak >= 2) {
      S.algoStreak = 0;
      if (S.algoHelp < 3) S.algoHelp++;
      else if (S.algoLevel < S.algoMax) { S.algoLevel++; S.algoHelp = S.algoLevel == 6 ? 1 : 2; }
    }
  } else if (score <= 2 || P.mistakes >= 2) {
    S.algoHelp = std::max(0, S.algoHelp - 1);
    S.algoStreak = 0;
  } else {
    S.algoStreak = 0;
  }
}

// Runs one problem from start to finish. Returns its 0-10 score.
static int solveProblem(Problem& P, float sessionProg) {
  P.help = P.lowest = S.algoHelp;
  P.sessionProg = sessionProg;
  P.answerSteps = 0;
  for (auto& s : P.steps) if (s.kind != K_CARRY) P.answerSteps++;
  for (int i = 0; i < (int)P.steps.size(); i++) if (isFree(P) == false || P.steps[i].kind != K_CARRY) P.seq.push_back(i);
  if (isFree(P)) { startGroups(P); loadGroup(P); }

  if (P.help == 0) setMsg(P, P.steps[P.seq[0]].shortP);
  else if (P.help == 1) setMsg(P, "Fill the blue box. Use the yellow numbers.");
  else if (P.help == 2) setMsg(P, "Fill in the row, then press Enter.");
  else setMsg(P, "Solve it all, then press Enter.");

  flushKeys();
  while (!P.done) {
    drawAlgo(P);
    KeyEv e;
    while (!(e = readKey()).any) delay(5);
    char ch = tolower(e.ch);
    if (e.ch >= '0' && e.ch <= '9') {
      if (isFree(P)) typeFree(P, e.ch - '0'); else typeGuided(P, e.ch - '0');
    } else if (e.del) {
      if (isFree(P)) backspaceFree(P);
    } else if (e.enter) {
      if (isFree(P)) checkFree(P);
    } else if (ch == 'h') hint(P);
    else if (ch == 'a') showMe(P);
    else if (ch == 'm') showModel(P);
    else if (ch == 's') {
      if (P.skipArmed) { P.skipped = true; P.done = true; }
      else { P.skipArmed = true; setMsg(P, "Press S again to skip this problem."); }
    } else if (ch == 'c' && isFree(P)) {
      // Jump to the carry box above the next digit to the left
      Step* s = currentStep(P);
      if (s && s->row < 2) {
        int pos = 0;
        sscanf(s->id.c_str() + s->id.indexOf('-') + 1, "%d", &pos);
        String id = cid("c", s->row, pos - s->row + 1);
        if (P.cells.count(id)) P.carrySel = (P.carrySel == id) ? String("") : id;
      }
    } else if (ch == ',' && isFree(P)) {                    // left arrow
      if (P.cursor < (int)P.editable.size() - 1) P.cursor++;
      P.carrySel = "";
    } else if (ch == '/' && isFree(P)) {                    // right arrow
      if (P.cursor > 0) P.cursor--;
      P.carrySel = "";
    }
  }
  finishProblem(P);
  int score = scoreProblem(P);
  adapt(P, score);

  String answer = fmt(P.A) + " x " + fmt(P.B) + " = " + fmt(P.A * P.B);
  String praise = score >= 9 ? "Great job!" : score >= 5 ? "Nice work!" : score >= 1 ? "You finished it!" : "That one was tricky.";
  setMsg(P, praise + " " + answer, score >= 5);
  drawAlgo(P);
  // "Press Enter" in the corner
  cv.setFont(&fonts::FreeSans9pt7b);
  cv.setTextColor(C_BLUE);
  cv.setTextDatum(top_right);
  cv.drawString("Enter", W - 4, 2);
  present();
  waitEnter();
  flushKeys();
  return score;
}

static void finishProblem(Problem& P) {
  P.done = true;
  P.carrySel = "";
  if (P.skipped) {
    for (auto& s : P.steps) {
      Cell& c = P.cells[s.id];
      if (c.val < 0 || c.st == CS_WRONG) { c.val = s.expected; c.st = CS_SHOWN; }
    }
  }
}

/* ================= Estimate ================= */
static long roundLead(long n) {
  long p = 1;
  while (n / p >= 10) p *= 10;
  return ((n + p / 2) / p) * p;
}

static bool estimate(long A, long B) {
  long rA = roundLead(A), rB = roundLead(B), est = rA * rB;
  std::vector<long> opts;
  if (est / 10 >= 1) opts.push_back(est / 10);
  opts.push_back(est);
  opts.push_back(est * 10);
  for (int i = opts.size() - 1; i > 0; i--) std::swap(opts[i], opts[rnd(i + 1)]);
  std::vector<String> labels;
  for (long o : opts) labels.push_back("About " + fmt(o));
  int i = runMenu(fmt(A) + " x " + fmt(B), "About how big will the answer be?", labels);
  bool right = opts[i] == est;
  messageScreen(right ? "Yes!" : "Good try.",
                fmt(rA) + " x " + fmt(rB) + " = " + fmt(est) + ", so the answer is about " + fmt(est) + ".",
                "Press Enter to solve it");
  waitEnter();
  return right;
}

/* ================= Session + menu ================= */
static bool loadAlgoStudent() {
  JsonDocument doc;
  if (!apiGet("action=algoStudent&name=" + urlEncode(S.name), doc)) return false;
  S.algoLevel = doc["level"] | 1;
  S.algoHelp = doc["help"] | 1;
  S.algoStreak = doc["streak"] | 0;
  S.algoMax = doc["maxLevel"] | 5;
  S.algoLoaded = true;
  return true;
}

static void runSession() {
  messageScreen("Keys",
                "Enter = check.  H = hint.  A = show answer.  M = model.  S = skip.  "
                "Row checks: arrows move, C = carry box.",
                "Press Enter to start");
  waitEnter();

  int startLevel = S.algoLevel, startHelp = S.algoHelp;
  std::vector<int> scores;
  std::map<String, int> counts;
  int model = 0, estRight = 0, estTotal = 0;
  Problem P;

  for (int k = 0; k < ALGO_PROBLEMS; k++) {
    long A, B;
    generate(S.algoLevel, A, B);
    if (k == 0 || k == 3) { estTotal++; if (estimate(A, B)) estRight++; }
    makeProblem(P, A, B);
    scores.push_back(solveProblem(P, (float)(k + 1) / ALGO_PROBLEMS));
    for (auto& e : P.errors) counts[e]++;
    if (P.modelUsed) model++;
  }

  // Report
  float sum = 0;
  std::vector<String> scoreText;
  for (int s : scores) { sum += s; scoreText.push_back(String(s)); }
  std::vector<std::pair<String, int>> sorted(counts.begin(), counts.end());
  std::sort(sorted.begin(), sorted.end(), [](const std::pair<String, int>& x, const std::pair<String, int>& y) { return x.second > y.second; });
  std::vector<String> mistakeText;
  for (auto& m : sorted) mistakeText.push_back(m.first + " (" + String(m.second) + ")");

  JsonDocument doc;
  doc["action"] = "algo";
  doc["name"] = S.name;
  doc["level"] = startLevel;
  doc["levelName"] = LEVEL_SHEET[startLevel];
  doc["endLevel"] = S.algoLevel;
  doc["problems"] = (int)scores.size();
  doc["confidence"] = round(sum / scores.size() * 10) / 10.0;
  doc["scores"] = joinList(scoreText);
  doc["estimates"] = String(estRight) + " of " + String(estTotal);
  doc["model"] = model;
  doc["common"] = sorted.empty() ? String("") : sorted[0].first;
  doc["mistakes"] = joinList(mistakeText);
  doc["help"] = S.algoHelp;
  doc["helpName"] = HELP_NAMES[S.algoHelp];
  doc["streak"] = S.algoStreak;

  String title = "Nice work, " + S.name + "!";
  String msg = "You solved " + String((int)scores.size()) + " problems.";
  if (S.algoLevel > startLevel) msg += String(" You moved up to: ") + LEVEL_SCREEN[S.algoLevel] + "!";
  else if (S.algoHelp > startHelp) msg += " Next time: fewer hints.";

  String status;
  bool saved = trySave(doc, title, status);
  while (!saved && online) {
    int i = runMenu(title, msg + " " + status, {"Try saving again", "Continue"});
    if (i == 1) break;
    saved = trySave(doc, title, status);
  }
  if (saved || !online) {
    messageScreen(title, msg + " " + status, "Press Enter");
    waitEnter();
  }
}

void algoLoop() {
  if (online && !S.algoLoaded) {
    messageScreen("Multi-digit", "Loading...");
    if (!loadAlgoStudent()) {
      messageScreen("Hmm", "Your level did not load. You can still practice.");
      delay(2500);
    }
  }
  while (true) {
    int i = runMenu("Multi-digit", String("Your problems: ") + LEVEL_SCREEN[S.algoLevel],
                    {"Start 5 problems", "Back"});
    if (i == 1) return;
    runSession();
  }
}
