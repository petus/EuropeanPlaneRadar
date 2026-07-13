// =============================================================================
//  European Plane Radar
//  Live aircraft radar (adsb.fi) on a round touchscreen.
// =============================================================================
//
//  Author:  Petr / chiptron.cz
//  Web:     https://chiptron.cz
//  Board:   Waveshare ESP32-S3-Touch-LCD-2.1
//           - ESP32-S3R8 (8 MB PSRAM, 16 MB flash)
//           - round 480x480 display, ST7701 controller (RGB interface)
//           - CST820 capacitive touch (I2C)
//           - TCA9554 I/O expander (LCD reset / CS / power control)
//
//  Screens (switch by swiping):
//    1) Aircraft radar - adsb.fi, tap an aircraft for details
//    2) Settings       - brightness, WiFi, location
//
//  Controls:
//    - swipe                       = switch screen
//    - short tap on an aircraft    = aircraft detail
//    - long press                  = change range
//    - hold BOOT at startup (~3 s) = factory reset
//
//  Libraries used (Arduino IDE, ESP32 core 3.x):
//    - GFX Library for Arduino (moononournation) - drawing
//    - ArduinoJson (bblanchon)                   - parsing the ADS-B data
//    - WiFiManager (tzapu)                       - WiFi configuration portal
//    - QRCode (ricmoo)                           - QR code (bundled with the project)
//    - Preferences, Wire, HTTPClient, esp_lcd    - part of the ESP32 core
//
//  Data sources (attribution required, personal non-commercial use only):
//    - Aircraft: adsb.fi, https://adsb.fi
//    - Location: ip-api.com (automatic detection by IP)
//
//  Projects that inspired this one:
//    - MatixYo/ESP32-Plane-Radar    - aircraft radar based on adsb.fi
//    - Selbyl/ESP32-S30Touch-...    - ADS-B on the Waveshare ST7701 display
//
//  Licence: MIT (see the LICENSE file)
//
//  You are free to use, modify and commercially deploy this code.
//  Beyond the licence, I would be glad if you kept the "chiptron.cz" line on
//  the settings screen at the same size and colour as in the original.
//  It is a request, not a condition - the licence applies in full either way.
//
//  Note: the free adsb.fi API is intended for personal use. For a commercial
//  product, arrange appropriate access to the data.
// =============================================================================

#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <time.h>
#include "esp_heap_caps.h"

#include "TCA9554.h"
#include "Display_ST7701.h"
#include "Touch_CST820.h"
#include "Settings.h"
#include "UI.h"
#include "WiFiPortal.h"
#include "GeoIP.h"
#include "ADSB.h"
#include "ScreenPlanes.h"
#include "ScreenSettings.h"
#include "Watchdog.h"

#define I2C_SDA 15
#define I2C_SCL 7
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
#define BOOT_PIN 0

// --- Arduino_GFX layer on top of the esp_lcd panel ---
class Arduino_ST7701_RGB : public Arduino_GFX {
 public:
  Arduino_ST7701_RGB(int16_t w, int16_t h) : Arduino_GFX(w, h) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override { return true; }

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    LCD_DrawBitmap(x, y, x, y, &color);
  }
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (h <= 0) return;
    uint16_t* line = (uint16_t*)malloc(h * sizeof(uint16_t));
    if (!line) return;
    for (int16_t i = 0; i < h; i++) line[i] = color;
    LCD_DrawBitmap(x, y, x, y + h - 1, line);
    free(line);
  }
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (w <= 0) return;
    uint16_t* line = (uint16_t*)malloc(w * sizeof(uint16_t));
    if (!line) return;
    for (int16_t i = 0; i < w; i++) line[i] = color;
    LCD_DrawBitmap(x, y, x + w - 1, y, line);
    free(line);
  }
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                               uint16_t color) override {
    if (w <= 0 || h <= 0) return;
    uint32_t n = (uint32_t)w * h;
    uint16_t* buf = (uint16_t*)heap_caps_malloc(n * sizeof(uint16_t), MALLOC_CAP_DEFAULT);
    if (!buf) return;
    for (uint32_t i = 0; i < n; i++) buf[i] = color;
    LCD_DrawBitmap(x, y, x + w - 1, y + h - 1, buf);
    free(buf);
  }
};

// Output layer - writes straight into the RGB panel (used by the canvas on flush).
Arduino_ST7701_RGB* outputPanel = nullptr;

// gfx = off-screen canvas in PSRAM. All drawing goes here, then flush() pushes
// the whole frame to the panel in one go -> no flicker.
Arduino_GFX* gfx = nullptr;

static void netPoll() { yield(); }

static void checkBootReset() {
  pinMode(BOOT_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_PIN) != LOW) return;
  gfx->fillScreen(C_BLACK);
  UI_TextCentered("Hold to reset...", LCD_HEIGHT / 2, C_WHITE, 2);
  gfx->flush();
  unsigned long start = millis();
  while (digitalRead(BOOT_PIN) == LOW) {
    if (millis() - start >= 3000) {
      UI_TextCentered("Erasing settings", LCD_HEIGHT / 2 + 30, C_RED, 2);
      gfx->flush();
      Settings_ClearAll();
      WiFi_Reset();
      delay(800);
      ESP.restart();
    }
    delay(20);
  }
}

// --- Screen manager ---
// 0 = aircraft radar, 1 = settings.
#define SCREEN_COUNT 2
static int s_screen = 0;

// Screen indicator - dots near the top edge (inside the circle).
static void drawScreenDots() {
  int gap = 20;
  int cx = LCD_WIDTH / 2;
  int y = 18;
  int startX = cx - (SCREEN_COUNT - 1) * gap / 2;
  for (int i = 0; i < SCREEN_COUNT; i++) {
    int x = startX + i * gap;
    if (i == s_screen) gfx->fillCircle(x, y, 4, C_WHITE);
    else               gfx->drawCircle(x, y, 4, C_GRAY);
  }
}

static void drawActive() {
  switch (s_screen) {
    case 0: ScreenPlanes_Draw(); break;
    case 1: ScreenSettings_Draw(); break;
  }
  drawScreenDots();
  gfx->flush();    // push the whole frame to the panel at once
}

static void enterActive() {
  switch (s_screen) {
    case 0: ScreenPlanes_Enter(); break;
    case 1: ScreenSettings_Enter(); break;
  }
  drawActive();
}

static void switchScreen(int dir) {
  s_screen = (s_screen + dir + SCREEN_COUNT) % SCREEN_COUNT;
  Serial.printf("Screen: %d\n", s_screen);
  enterActive();
}

static bool activeTick() {
  switch (s_screen) {
    case 0: return ScreenPlanes_Tick();
    case 1: return ScreenSettings_Tick();
  }
  return false;
}
static bool activeTap(int x, int y) {
  switch (s_screen) {
    case 0: return ScreenPlanes_HandleTap(x, y);
    case 1: return ScreenSettings_HandleTap(x, y);
  }
  return false;
}
static bool activeLongPress(int x, int y) {
  switch (s_screen) {
    case 0: return ScreenPlanes_HandleLongPress(x, y);
    case 1: return false;   // settings does not need a long press
  }
  return false;
}
// Is a modal window (the detail panel) open on the active screen? If so, block swiping.
static bool activeModalOpen() {
  return (s_screen == 0) && ScreenPlanes_DetailOpen();
}


void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== PlaneRadar ===");

  Settings_Begin();

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  delay(50);
  TCA9554_Init();
  TCA9554_SetPin(EXIO_LCD_PWR, false);
  delay(10);

  Backlight_Init();
  Set_Backlight(Settings_Backlight());
  ST7701_Init();

  // Output panel + a canvas in PSRAM on top of it (to avoid flicker).
  outputPanel = new Arduino_ST7701_RGB(LCD_WIDTH, LCD_HEIGHT);
  outputPanel->begin();
  Arduino_Canvas* canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, outputPanel);
  canvas->begin();
  gfx = canvas;
  gfx->fillScreen(C_BLACK);
  gfx->flush();

  Touch_Init();


  checkBootReset();

  ADSB_SetPollFn(netPoll);

  WiFi_ConnectOrPortal();

  if (WiFi_IsConnected()) {
    configTzTime(TZ_INFO, "pool.ntp.org");
    GeoIP_DetectIfNeeded();   // fill in the location by IP if the user did not set one
  }

  s_screen = 0;
  ScreenPlanes_Enter();
  drawActive();

  Watchdog_Begin();   // hardware watchdog for 24/7 operation
  Serial.println("Setup done");
}

void loop() {
  // --- Touch: swipe (switch screen) vs tap (range) ---
  static bool touching = false;
  static int  startX = 0, startY = 0;
  static int  lastX = 0, lastY = 0;
  static unsigned long startMs = 0;

  TouchData t;
  Touch_Read(&t);

  if (t.points > 0) {
    if (!touching) {                 // touch begins
      touching = true;
      startX = t.x; startY = t.y; startMs = millis();
    }
    lastX = t.x; lastY = t.y;        // last valid position
  } else if (touching) {             // touch ends -> evaluate the gesture
    touching = false;
    int dx = lastX - startX;
    int dy = lastY - startY;
    unsigned long dur = millis() - startMs;
    bool smallMove = (abs(dx) < 60 && abs(dy) < 60);

    if (!activeModalOpen() && abs(dx) >= 70 && abs(dy) <= 90 && dur <= 700) {
      switchScreen(dx < 0 ? +1 : -1);            // horizontal swipe (not while a detail is open)
    } else if (smallMove && dur >= 500) {
      if (activeLongPress(lastX, lastY)) drawActive();   // long press -> range
    } else if (smallMove && dur < 500) {
      if (activeTap(lastX, lastY)) drawActive();         // short tap -> detail/selection
    }
  }

  WiFi_Loop();

  // Request for the WiFi portal coming from the settings screen.
  if (ScreenSettings_WantsPortal()) {
    ScreenSettings_ClearPortal();
    Watchdog_Suspend();                    // the portal blocks - stop watching
    WiFi_StartPortal();                    // blocking - draws its own AP screen
    Watchdog_Resume();
    if (WiFi_IsConnected()) {
      configTzTime(TZ_INFO, "pool.ntp.org");
    }
    enterActive();                         // redraw the settings once it returns
  }

  // Redrawing is decoupled from reading the touch and capped at ~12 FPS.
  static unsigned long lastDraw = 0;
  bool wantDraw = activeTick();
  // With the detail open, redraw on every tick (so the data stays up to date).
  if (wantDraw && millis() - lastDraw >= 80) {
    drawActive();
    lastDraw = millis();
  }

  Watchdog_Feed();
  delay(5);
}
