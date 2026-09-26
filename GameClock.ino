// YourClock firmware - XIAO ESP32-C3 + ST7789 (284x76) + 4 buttons + buzzer
//
// Buttons:  SW1 = MENU / NEXT FIELD   SW2 = UP   SW3 = DOWN   SW4 = SNOOZE (starts a quest)
// Clock screen: SW1 -> set time -> set alarm -> back to clock.
// When the alarm rings, press SW4. You must win a random mini-game to snooze.
// Fail (wrong answer / timeout) and it keeps ringing.
//
// The board has no RTC, so the time is lost on power-off unless you fill in WiFi below
// (the clock then syncs from the internet at boot). The alarm time is saved in flash.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

// ---------- optional WiFi time sync (leave SSID empty to disable) ----------
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* TZ_INFO   = "CST6CDT,M3.2.0,M11.1.0";   // US Central. Change for your zone.

// ---------- pins (XIAO ESP32-C3 GPIO numbers, from the PCB) ----------
#define TFT_SCLK 9
#define TFT_MOSI 10
#define TFT_RST  8
#define TFT_DC   6
#define TFT_CS   7
#define TFT_BL   21
#define BUZZER   20
const uint8_t BTN[4] = {2, 3, 4, 5};   // SW1..SW4

// ---------- settings ----------
#define SNOOZE_MINUTES 5
#define MAX_SNOOZES    3      // after this many snoozes, winning a game turns the alarm OFF
#define GAME_TIME_MS   20000

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Preferences prefs;

const int W = 284, H = 76;

// ---------- clock state ----------
uint32_t secOfDay = 12 * 3600;
uint32_t lastMs = 0;
bool newSecond = false;

uint8_t alarmH = 7, alarmM = 0;
bool alarmOn = true;

bool snoozeActive = false;
uint32_t snoozeTarget = 0;
uint8_t snoozeCount = 0;
bool alarmRinging = false;

enum State { S_CLOCK, S_SET_TIME, S_SET_ALARM, S_RING, S_GAME, S_MSG };
State state = S_CLOCK;
State msgNext = S_CLOCK;
uint32_t msgUntil = 0;
uint8_t field = 0;
uint8_t setH = 0, setM = 0;

// ---------- buttons ----------
bool held[4], evt[4];
uint32_t lastChange[4], holdStart[4], lastRepeat[4];

void readButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    evt[i] = false;
    bool d = (digitalRead(BTN[i]) == LOW);
    if (d != held[i] && now - lastChange[i] > 30) {
      lastChange[i] = now;
      held[i] = d;
      if (d) { evt[i] = true; holdStart[i] = now; lastRepeat[i] = now; }
    } else if (d && now - holdStart[i] > 500 && now - lastRepeat[i] > 120) {
      lastRepeat[i] = now;
      if (state == S_SET_TIME || state == S_SET_ALARM) evt[i] = true;  // auto-repeat only when setting
    }
  }
}

// ---------- buzzer ----------
void buzzInit() {
#if ESP_ARDUINO_VERSION_MAJOR < 3
  ledcSetup(0, 2000, 8);
  ledcAttachPin(BUZZER, 0);
#endif
}
void buzz(bool on) {
  static bool cur = false;
  if (on == cur) return;
  cur = on;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (on) tone(BUZZER, 2400); else noTone(BUZZER);
#else
  ledcWriteTone(0, on ? 2400 : 0);
#endif
}
void buzzerUpdate() {
  if (!alarmRinging) { buzz(false); return; }
  uint32_t t = millis();
  if (state == S_GAME) {                       // calmer pulse while playing, still ringing
    buzz((t % 1500) < 200);
  } else {                                     // beep-beep-beep ... pause
    uint32_t p = t % 1400;
    buzz(p < 150 || (p >= 300 && p < 450) || (p >= 600 && p < 750));
  }
}

// ---------- drawing helpers ----------
void printAt(int x, int y, uint8_t size, uint16_t fg, uint16_t bg, const char* s) {
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
  tft.setCursor(x, y);
  tft.print(s);
}
void printCentered(int y, uint8_t size, uint16_t fg, uint16_t bg, const char* s) {
  int w = strlen(s) * 6 * size;
  printAt((W - w) / 2, y, size, fg, bg, s);
}

void drawClock(bool full) {
  if (full) tft.fillScreen(ST77XX_BLACK);
  char b[24];
  snprintf(b, sizeof b, "%02u:%02u", (unsigned)(secOfDay / 3600), (unsigned)((secOfDay / 60) % 60));
  printAt(8, 4, 5, ST77XX_WHITE, ST77XX_BLACK, b);
  snprintf(b, sizeof b, "%02u", (unsigned)(secOfDay % 60));
  printAt(170, 20, 3, ST77XX_CYAN, ST77XX_BLACK, b);
  printAt(236, 20, 3, ST77XX_YELLOW, ST77XX_BLACK, snoozeActive ? "zZ" : "  ");
  snprintf(b, sizeof b, "ALARM %02u:%02u %s", alarmH, alarmM, alarmOn ? "ON " : "OFF");
  printAt(8, 54, 2, alarmOn ? ST77XX_GREEN : ST77XX_RED, ST77XX_BLACK, b);
}

void drawSet() {
  tft.fillScreen(ST77XX_BLACK);
  bool isAlarm = (state == S_SET_ALARM);
  printAt(8, 2, 2, ST77XX_CYAN, ST77XX_BLACK, isAlarm ? "SET ALARM" : "SET TIME");
  printAt(150, 6, 1, ST77XX_WHITE, ST77XX_BLACK, "UP/DOWN change  SW1 next");
  uint8_t h = isAlarm ? alarmH : setH, m = isAlarm ? alarmM : setM;
  char b[8];
  snprintf(b, sizeof b, "%02u", h);
  printAt(8, 30, 5, field == 0 ? ST77XX_YELLOW : ST77XX_WHITE, ST77XX_BLACK, b);
  printAt(68, 30, 5, ST77XX_WHITE, ST77XX_BLACK, ":");
  snprintf(b, sizeof b, "%02u", m);
  printAt(98, 30, 5, field == 1 ? ST77XX_YELLOW : ST77XX_WHITE, ST77XX_BLACK, b);
  if (isAlarm) printAt(200, 38, 3, field == 2 ? ST77XX_YELLOW : (alarmOn ? ST77XX_GREEN : ST77XX_RED), ST77XX_BLACK, alarmOn ? " ON" : "OFF");
}

void showMessage(const char* l1, const char* l2, uint16_t col, uint32_t ms, State next) {
  tft.fillScreen(ST77XX_BLACK);
  printCentered(10, 3, col, ST77XX_BLACK, l1);
  printCentered(46, 2, ST77XX_WHITE, ST77XX_BLACK, l2);
  msgUntil = millis() + ms;
  msgNext = next;
  state = S_MSG;
}

// ---------- alarm flow ----------
void startRing(bool fresh) {
  alarmRinging = true;
  snoozeActive = false;
  if (fresh) snoozeCount = 0;
  state = S_RING;
}

uint32_t ringFlashMs = 0;
bool ringInv = false;
void drawRing(bool force) {
  if (!force && millis() - ringFlashMs < 400) return;
  ringFlashMs = millis();
  ringInv = !ringInv;
  uint16_t bg = ringInv ? ST77XX_RED : ST77XX_BLACK;
  uint16_t fg = ringInv ? ST77XX_WHITE : ST77XX_RED;
  tft.fillScreen(bg);
  printCentered(6, 4, fg, bg, "WAKE UP!");
  char b[8];
  snprintf(b, sizeof b, "%02u:%02u", (unsigned)(secOfDay / 3600), (unsigned)((secOfDay / 60) % 60));
  printAt(8, 52, 2, fg, bg, b);
  printAt(100, 52, 2, fg, bg, "SW4: SNOOZE");
}

// ---------- mini-games ----------
uint8_t gameType = 0;
uint32_t gameStart = 0, gameLimit = GAME_TIME_MS;
uint32_t lastTimeBar = 0;

void drawTimeLeft() {
  if (millis() - lastTimeBar < 250) return;
  lastTimeBar = millis();
  long left = (long)gameLimit - (long)(millis() - gameStart);
  if (left < 0) left = 0;
  int w = (int)((long)W * left / gameLimit);
  tft.fillRect(0, H - 4, w, 4, ST77XX_YELLOW);
  tft.fillRect(w, H - 4, W - w, 4, ST77XX_BLACK);
}

void winGame() {
  snoozeCount++;
  if (snoozeCount > MAX_SNOOZES) {
    alarmRinging = false;
    snoozeActive = false;
    buzz(false);
    showMessage("ALARM OFF", "Good morning!", ST77XX_GREEN, 2500, S_CLOCK);
  } else {
    alarmRinging = false;
    snoozeActive = true;
    snoozeTarget = (secOfDay + SNOOZE_MINUTES * 60) % 86400;
    buzz(false);
    char b[24];
    snprintf(b, sizeof b, "Back in %d min", SNOOZE_MINUTES);
    showMessage("SNOOZED", b, ST77XX_GREEN, 2500, S_CLOCK);
  }
}
void loseGame(const char* why) {
  showMessage("FAILED!", why, ST77XX_RED, 1500, S_RING);   // alarm keeps ringing
}

// --- game 0: memory sequence ---
uint8_t seq[4], simonPhase, simonIn;
uint32_t simonStart;
int simonShown;
void drawDigit(int d) {
  tft.fillRect(100, 14, 84, 52, ST77XX_BLACK);
  if (d >= 0) {
    char b[2] = { (char)('1' + d), 0 };
    printAt(124, 18, 6, ST77XX_WHITE, ST77XX_BLACK, b);
  }
}
void simonInit() {
  for (int i = 0; i < 4; i++) seq[i] = random(4);
  simonPhase = 0; simonIn = 0; simonShown = -2;
  simonStart = millis();
  gameStart = millis(); gameLimit = 600000;   // no timeout while memorizing
  printAt(4, 2, 2, ST77XX_CYAN, ST77XX_BLACK, "MEMORIZE");
}
void simonUpdate() {
  if (simonPhase == 0) {
    uint32_t el = millis() - simonStart;
    uint32_t idx = el / 800, within = el % 800;
    if (idx >= 4) {
      simonPhase = 1; simonIn = 0;
      gameStart = millis(); gameLimit = 15000;
      tft.fillScreen(ST77XX_BLACK);
      printAt(4, 2, 2, ST77XX_YELLOW, ST77XX_BLACK, "YOUR TURN: press the buttons in order");
      return;
    }
    int want = (within < 600) ? seq[idx] : -1;
    if (want != simonShown) { simonShown = want; drawDigit(want); }
  } else {
    drawTimeLeft();
    for (int i = 0; i < 4; i++) if (evt[i]) {
      if (i == seq[simonIn]) {
        simonIn++;
        char b[12]; snprintf(b, sizeof b, "%d/4", simonIn);
        printAt(124, 30, 3, ST77XX_WHITE, ST77XX_BLACK, b);
        if (simonIn >= 4) { winGame(); return; }
      } else { loseGame("Wrong button"); return; }
    }
  }
}

// --- game 1: quick math ---
int mathOpt[4], mathAns;
void mathInit() {
  int a, b; bool mul = random(2);
  if (mul) { a = random(3, 10); b = random(3, 10); mathAns = a * b; }
  else     { a = random(11, 60); b = random(11, 60); mathAns = a + b; }
  const int off[8] = {-3, -2, -1, 1, 2, 3, 4, 5};
  int used[8] = {0}, n = 0, slot = random(4);
  for (int i = 0; i < 4; i++) {
    if (i == slot) { mathOpt[i] = mathAns; continue; }
    int k; do { k = random(8); } while (used[k]);
    used[k] = 1; mathOpt[i] = mathAns + off[k]; n++;
  }
  char q[24]; snprintf(q, sizeof q, "%d %c %d = ?", a, mul ? 'x' : '+', b);
  printCentered(6, 3, ST77XX_WHITE, ST77XX_BLACK, q);
  for (int i = 0; i < 4; i++) {
    char o[12]; snprintf(o, sizeof o, "%d:%d", i + 1, mathOpt[i]);
    printAt(4 + i * 71, 46, 2, ST77XX_YELLOW, ST77XX_BLACK, o);
  }
  gameStart = millis(); gameLimit = GAME_TIME_MS;
}
void mathUpdate() {
  drawTimeLeft();
  for (int i = 0; i < 4; i++) if (evt[i]) {
    if (mathOpt[i] == mathAns) winGame(); else loseGame("Wrong answer");
    return;
  }
}

// --- game 2: button mash (SW2) ---
int mashCount;
#define MASH_TARGET 25
void mashDraw() {
  char b[12]; snprintf(b, sizeof b, "%2d/%d", mashCount, MASH_TARGET);
  printCentered(30, 4, ST77XX_WHITE, ST77XX_BLACK, b);
}
void mashInit() {
  mashCount = 0;
  printCentered(4, 2, ST77XX_CYAN, ST77XX_BLACK, "MASH SW2 FAST!");
  mashDraw();
  gameStart = millis(); gameLimit = 8000;
}
void mashUpdate() {
  drawTimeLeft();
  if (evt[1]) { mashCount++; mashDraw(); if (mashCount >= MASH_TARGET) { winGame(); return; } }
}

// --- game 3: timing bar (any button) ---
const int ZX = 122, ZW = 40, CW = 6, BY = 40, BH = 24;
int hits, oldPos = -1;
void timingHits() {
  char b[16]; snprintf(b, sizeof b, "HITS %d/3", hits);
  printAt(4, 2, 2, ST77XX_CYAN, ST77XX_BLACK, "STOP IN THE ZONE  ");
  printAt(4, 22, 1, ST77XX_WHITE, ST77XX_BLACK, b);
}
void timingInit() {
  hits = 0; oldPos = -1;
  timingHits();
  gameStart = millis(); gameLimit = 25000;
}
int timingPos() {
  int span = W - CW;
  int p = (millis() / 5) % (2 * span);
  return p < span ? p : 2 * span - p;
}
void timingUpdate() {
  drawTimeLeft();
  int pos = timingPos();
  if (pos != oldPos) {
    if (oldPos >= 0) tft.fillRect(oldPos, BY, CW, BH, ST77XX_BLACK);
    tft.drawRect(ZX, BY - 2, ZW, BH + 4, ST77XX_GREEN);
    tft.fillRect(pos, BY, CW, BH, ST77XX_WHITE);
    oldPos = pos;
  }
  for (int i = 0; i < 4; i++) if (evt[i]) {
    bool ok = (pos + CW / 2 >= ZX && pos + CW / 2 <= ZX + ZW);
    hits = ok ? hits + 1 : 0;
    timingHits();
    if (hits >= 3) { winGame(); return; }
  }
}

void startGame() {
  tft.fillScreen(ST77XX_BLACK);
  state = S_GAME;
  gameType = random(4);
  lastTimeBar = 0;
  switch (gameType) {
    case 0: simonInit(); break;
    case 1: mathInit();  break;
    case 2: mashInit();  break;
    default: timingInit(); break;
  }
}
void updateGame() {
  switch (gameType) {
    case 0: simonUpdate(); break;
    case 1: mathUpdate();  break;
    case 2: mashUpdate();  break;
    default: timingUpdate(); break;
  }
  if (state == S_GAME && millis() - gameStart > gameLimit) loseGame("Too slow");
}

// ---------- time sync ----------
void syncNTP() {
  if (strlen(WIFI_SSID) == 0) return;
  printCentered(20, 2, ST77XX_WHITE, ST77XX_BLACK, "Syncing time...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    struct tm ti;
    if (getLocalTime(&ti, 8000)) {
      secOfDay = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
      lastMs = millis();
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  for (int i = 0; i < 4; i++) pinMode(BTN[i], INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  buzzInit();

  tft.init(76, 284);            // panel size (portrait)
  // (this panel's (82,18) offsets are applied automatically by the library)
  tft.invertDisplay(false);
  tft.setRotation(1);           // landscape; use 3 if upside down
  tft.fillScreen(ST77XX_BLACK);

  randomSeed(esp_random());
  prefs.begin("clock", false);
  alarmH  = prefs.getUChar("ah", 7);
  alarmM  = prefs.getUChar("am", 0);
  alarmOn = prefs.getBool("on", true);

  lastMs = millis();
  syncNTP();
  lastMs = millis();
  drawClock(true);
  Serial.println("Clock ready");
}

void loop() {
  uint32_t now = millis();
  newSecond = false;
  while (now - lastMs >= 1000) { lastMs += 1000; secOfDay = (secOfDay + 1) % 86400; newSecond = true; }

  readButtons();

  // alarm triggers
  if (newSecond && !alarmRinging && state != S_SET_TIME && state != S_SET_ALARM) {
    if (alarmOn && secOfDay == (uint32_t)alarmH * 3600 + alarmM * 60) startRing(true);
    else if (snoozeActive && secOfDay == snoozeTarget) startRing(false);
    if (alarmRinging) drawRing(true);
  }

  switch (state) {
    case S_CLOCK:
      if (newSecond) drawClock(false);
      if (evt[0]) { setH = secOfDay / 3600; setM = (secOfDay / 60) % 60; field = 0; state = S_SET_TIME; drawSet(); }
      break;

    case S_SET_TIME:
      if (evt[1]) { if (field == 0) setH = (setH + 1) % 24; else setM = (setM + 1) % 60; drawSet(); }
      if (evt[2]) { if (field == 0) setH = (setH + 23) % 24; else setM = (setM + 59) % 60; drawSet(); }
      if (evt[0]) {
        if (field == 0) { field = 1; drawSet(); }
        else {
          secOfDay = (uint32_t)setH * 3600 + setM * 60; lastMs = millis();
          field = 0; state = S_SET_ALARM; drawSet();
        }
      }
      break;

    case S_SET_ALARM:
      if (evt[1]) { if (field == 0) alarmH = (alarmH + 1) % 24; else if (field == 1) alarmM = (alarmM + 1) % 60; else alarmOn = !alarmOn; drawSet(); }
      if (evt[2]) { if (field == 0) alarmH = (alarmH + 23) % 24; else if (field == 1) alarmM = (alarmM + 59) % 60; else alarmOn = !alarmOn; drawSet(); }
      if (evt[0]) {
        if (field < 2) { field++; drawSet(); }
        else {
          prefs.putUChar("ah", alarmH); prefs.putUChar("am", alarmM); prefs.putBool("on", alarmOn);
          snoozeActive = false;
          field = 0; state = S_CLOCK; drawClock(true);
        }
      }
      break;

    case S_RING:
      drawRing(false);
      if (evt[3]) startGame();          // SNOOZE -> must win a quest
      break;

    case S_GAME:
      updateGame();
      break;

    case S_MSG:
      if ((int32_t)(millis() - msgUntil) >= 0) {
        state = msgNext;
        if (state == S_RING) drawRing(true);
        else if (state == S_CLOCK) drawClock(true);
      }
      break;
  }

  buzzerUpdate();
}






