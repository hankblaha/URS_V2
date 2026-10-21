// status_display.h — status screen for the DFRobot DFR0997 2.0" I2C display
//
// Layout (320 x 240):
//   +--------------------------------------+
//   | URS-V2                               |  header: model number
//   +--------------------------------------+
//   | (wifi)  Connected                    |  WiFi status (+ loading dots
//   |         wrong password?              |   while connecting, reason on fail)
//   | ------------------------------------ |
//   | Uploading                    • • •   |  upload status (+ loading dots)
//   | [==============              ]       |  progress bar (only during uploads)
//   | 3 of 7 files                         |
//   +--------------------------------------+
//
// The DFR0997 has its own controller; we send it small commands over I2C.
// Loading dots are animated by a small background task, so they keep moving
// while the main loop is busy connecting or uploading.
//
// Every function is safe to call when no display is attached: if
// displayBegin() fails, the rest quietly do nothing.
//
// Never call these from the USB MSC callbacks (onRead/onWrite/onStartStop).
#pragma once
#include <Arduino.h>

// Text colors (24-bit RGB, same format the DFRobot library uses)
#define DISP_GREEN   0x00C000
#define DISP_RED     0xFF3030
#define DISP_YELLOW  0xFFB000
#define DISP_WHITE   0xFFFFFF
#define DISP_GREY    0x909090

// Draws the static layout. Call once in setup() AFTER Wire.begin(sda, scl).
// deviceId / fwVersion are not shown on screen (they're in the serial log);
// kept in the signature so callers don't change.
bool displayBegin(const char* modelNumber, const String& deviceId, const char* fwVersion);

// WiFi row: "Connecting" with animated dots.
void displayWiFiConnecting();

// WiFi row: "Connected" (green) or "No WiFi" (red). Stops the dots.
void displaySetWiFi(bool connected, int rssi, const String& ip);

// Small grey line under the WiFi status, e.g. the reason a connect failed.
// "" clears it. A leading "WiFi: " is stripped.
void displaySetWiFiDetail(const String& text);

// Upload status line, e.g. "Ready", "Uploading", "Upload failed".
// busy = true shows animated loading dots next to it.
void displaySetStatus(const String& text, uint32_t color = DISP_WHITE, bool busy = false);

// Progress bar + "3 of 7 files". total > 0 shows the bar; total <= 0 hides it.
void displaySetProgress(int done, int total);

// Small grey line under the progress bar: the file being uploaded ("" clears).
void displaySetFile(const String& name);

// Call every pass of loop(). Re-checks WiFi every 2 s and refreshes the
// WiFi row only when the connection state changes.
void displayTick();
