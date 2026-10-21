// status_display.cpp — see status_display.h for the layout
#include "status_display.h"
#include <Wire.h>
#include <WiFi.h>
#include "DFRobot_LcdDisplay.h"

#define DISPLAY_I2C_ADDR 0x2C
#define FONT_BIG   0   // 24 px
#define FONT_SMALL 1   // 12 px

#define BG_COLOR      0x000000
#define HEADER_COLOR  0x0B2A5B   // dark blue
#define DIVIDER_COLOR 0x303030
#define BAR_COLOR     0x00A0FF
#define DOT_DIM       0x303030

// ---- Layout coordinates ----
#define HEADER_H     44
#define WIFI_ICON_X  12
#define WIFI_ICON_Y  52
#define WIFI_TEXT_X  60
#define WIFI_TEXT_Y  62
#define WIFI_DETAIL_Y 92
#define DIVIDER_Y    112
#define STATUS_X     16
#define STATUS_Y     128
#define BAR_X        16
#define BAR_Y        170
#define BAR_W        288
#define BAR_H        16
#define COUNT_Y      196
#define FILE_Y       216
#define DOTS_X       262        // first dot; dots are 14 px apart
#define DOT_R        4

static DFRobot_Lcd_IIC lcd(&Wire, DISPLAY_I2C_ADDR);
static bool present = false;
static SemaphoreHandle_t lcdMutex = nullptr;

// Object handles returned by the display
static uint8_t idWifiText, idWifiDetail, idStatus, idCount, idFile;
static uint8_t idBar = 0;
static bool barShown = false;

// Loading-dot animators: one for the WiFi row, one for the upload status row
struct Dots {
  int y;
  uint8_t id[3];
  uint32_t color;
  volatile bool active;
  bool drawnOff;
};
static Dots wifiDots   = { WIFI_TEXT_Y + 12, {0, 0, 0}, DISP_YELLOW, false, true };
static Dots statusDots = { STATUS_Y + 12,    {0, 0, 0}, DISP_YELLOW, false, true };

// Last values sent, so we only talk to the display when something changes
static String lastWifiText, lastWifiDetail, lastStatus, lastCount, lastFile;
static uint32_t lastWifiColor = 0, lastStatusColor = 0;
static int lastBarPct = -1;
static int lastConnected = -1;     // -1 unknown, 0 down, 1 up
static uint32_t lastTickMs = 0;

// Small RAII lock so the main loop and the animation task never talk at once
struct LcdLock {
  bool ok;
  LcdLock()  { ok = present && xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(500)) == pdTRUE; }
  ~LcdLock() { if (ok) xSemaphoreGive(lcdMutex); }
};

// ---- helpers (caller holds the lock) ----

static void setText(uint8_t id, int x, int y, const String& text, uint8_t font,
                    uint32_t color, String& lastText, uint32_t* lastColor) {
  String t = text.length() ? text : String(" ");
  if (t == lastText && (!lastColor || *lastColor == color)) return;
  lcd.updateString(id, x, y, t, font, color);
  lastText = t;
  if (lastColor) *lastColor = color;
}

static void dotsCreate(Dots& d) {
  for (int i = 0; i < 3; i++)
    d.id[i] = lcd.drawCircle(DOTS_X + i * 14, d.y, DOT_R, 0, BG_COLOR, 1, BG_COLOR);
}

static void dotsFrame(Dots& d, int frame) {
  for (int i = 0; i < 3; i++) {
    uint32_t c = (i == frame % 3) ? d.color : DOT_DIM;
    lcd.updateCircle(d.id[i], DOTS_X + i * 14, d.y, DOT_R, 0, c, 1, c);
  }
  d.drawnOff = false;
}

static void dotsHide(Dots& d) {
  if (d.drawnOff) return;
  for (int i = 0; i < 3; i++)
    lcd.updateCircle(d.id[i], DOTS_X + i * 14, d.y, DOT_R, 0, BG_COLOR, 1, BG_COLOR);
  d.drawnOff = true;
}

// ---- animation task: steps any active loading dots every 250 ms ----

static void animTask(void*) {
  int frame = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    LcdLock lock; if (!lock.ok) continue;
    if (wifiDots.active)   dotsFrame(wifiDots, frame);   else dotsHide(wifiDots);
    if (statusDots.active) dotsFrame(statusDots, frame); else dotsHide(statusDots);
    frame++;
  }
}

// ---- public API ----

bool displayBegin(const char* modelNumber, const String& deviceId, const char* fwVersion) {
  (void)deviceId; (void)fwVersion;
  lcdMutex = xSemaphoreCreateMutex();
  if (!lcd.begin()) {          // uses the Wire bus you already started
    present = false;
    return false;
  }
  present = true;

  lcd.cleanScreen();           // ~1.5 s: wipes whatever was drawn before
  lcd.setBackgroundColor(BG_COLOR);

  // Header: model number
  lcd.drawRect(0, 0, 320, HEADER_H, 0, HEADER_COLOR, 1, HEADER_COLOR, 0);
  lcd.drawString(16, 10, String(modelNumber), FONT_BIG, DISP_WHITE);

  // WiFi row
  lcd.drawIcon(WIFI_ICON_X, WIFI_ICON_Y, DFRobot_LcdDisplay::eIconWifi, 112);
  idWifiText   = lcd.drawString(WIFI_TEXT_X, WIFI_TEXT_Y, "WiFi", FONT_BIG, DISP_GREY);
  idWifiDetail = lcd.drawString(WIFI_TEXT_X, WIFI_DETAIL_Y, " ", FONT_SMALL, DISP_GREY);
  lastWifiText = "WiFi"; lastWifiColor = DISP_GREY; lastWifiDetail = " ";

  // Divider between the WiFi and upload sections
  lcd.drawLine(16, DIVIDER_Y, 304, DIVIDER_Y, 1, DIVIDER_COLOR);

  // Upload status row
  idStatus = lcd.drawString(STATUS_X, STATUS_Y, "Starting", FONT_BIG, DISP_WHITE);
  lastStatus = "Starting"; lastStatusColor = DISP_WHITE;
  idCount  = lcd.drawString(BAR_X, COUNT_Y, " ", FONT_SMALL, DISP_GREY);
  lastCount = " ";
  idFile   = lcd.drawString(BAR_X, FILE_Y, " ", FONT_SMALL, DISP_GREY);
  lastFile = " ";

  dotsCreate(wifiDots);
  dotsCreate(statusDots);

  xTaskCreate(animTask, "dispAnim", 3072, nullptr, 1, nullptr);
  return true;
}

void displayWiFiConnecting() {
  LcdLock lock; if (!lock.ok) return;
  setText(idWifiText, WIFI_TEXT_X, WIFI_TEXT_Y, "Connecting", FONT_BIG, DISP_YELLOW,
          lastWifiText, &lastWifiColor);
  setText(idWifiDetail, WIFI_TEXT_X, WIFI_DETAIL_Y, "", FONT_SMALL, DISP_GREY, lastWifiDetail, nullptr);
  wifiDots.active = true;
}

void displaySetWiFi(bool connected, int rssi, const String& ip) {
  (void)ip;
  LcdLock lock; if (!lock.ok) return;
  wifiDots.active = false;
  if (connected) {
    setText(idWifiText, WIFI_TEXT_X, WIFI_TEXT_Y, "Connected", FONT_BIG, DISP_GREEN,
            lastWifiText, &lastWifiColor);
    // Weak signal is worth a hint; otherwise keep the row clean
    setText(idWifiDetail, WIFI_TEXT_X, WIFI_DETAIL_Y, rssi < -75 ? "weak signal" : "",
            FONT_SMALL, DISP_GREY, lastWifiDetail, nullptr);
  } else {
    setText(idWifiText, WIFI_TEXT_X, WIFI_TEXT_Y, "No WiFi", FONT_BIG, DISP_RED,
            lastWifiText, &lastWifiColor);
  }
  lastConnected = connected ? 1 : 0;
}

void displaySetWiFiDetail(const String& text) {
  LcdLock lock; if (!lock.ok) return;
  String t = text.startsWith("WiFi: ") ? text.substring(6) : text;
  if (t.length() > 44) t = t.substring(0, 41) + "...";
  setText(idWifiDetail, WIFI_TEXT_X, WIFI_DETAIL_Y, t, FONT_SMALL, DISP_GREY, lastWifiDetail, nullptr);
}

void displaySetStatus(const String& text, uint32_t color, bool busy) {
  LcdLock lock; if (!lock.ok) return;
  setText(idStatus, STATUS_X, STATUS_Y, text, FONT_BIG, color, lastStatus, &lastStatusColor);
  statusDots.color = color;
  statusDots.active = busy;
}

void displaySetProgress(int done, int total) {
  LcdLock lock; if (!lock.ok) return;

  if (total <= 0) {                       // hide bar + count
    if (barShown) { lcd.deleteBar(idBar); barShown = false; lastBarPct = -1; }
    setText(idCount, BAR_X, COUNT_Y, "", FONT_SMALL, DISP_GREY, lastCount, nullptr);
    setText(idFile, BAR_X, FILE_Y, "", FONT_SMALL, DISP_GREY, lastFile, nullptr);
    return;
  }
  if (!barShown) {
    idBar = lcd.creatBar(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_COLOR);
    barShown = true;
    lastBarPct = -1;
  }
  if (done > total) done = total;
  int pct = (done * 100) / total;
  if (pct != lastBarPct) { lcd.setBarValue(idBar, pct); lastBarPct = pct; }
  setText(idCount, BAR_X, COUNT_Y, String(done) + " of " + String(total) + " files",
          FONT_SMALL, DISP_GREY, lastCount, nullptr);
}

void displaySetFile(const String& name) {
  LcdLock lock; if (!lock.ok) return;
  String t = name;
  if (t.length() > 44) t = "..." + t.substring(t.length() - 41);
  setText(idFile, BAR_X, FILE_Y, t, FONT_SMALL, DISP_GREY, lastFile, nullptr);
}

void displayTick() {
  if (!present) return;
  if (millis() - lastTickMs < 2000) return;
  lastTickMs = millis();
  if (wifiDots.active) return;            // a connect attempt owns the WiFi row

  int connected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
  if (connected == lastConnected) return; // only redraw on a state change
  displaySetWiFi(connected, connected ? WiFi.RSSI() : 0, "");
}
