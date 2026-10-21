#include "USB.h"
#include "USBMSC.h"
#include "SD.h"
#include "SPI.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "HTTPClient.h"
#include "Wire.h"
#include "status_display.h"
#include <time.h>
#include "esp_app_desc.h"
#include "esp_task_wdt.h"

#define LED_PIN 21   // onboard green LED (red LED = power, always on)
#define SD_CS   14

// ---- Tuning ----
#define WRITE_IDLE_MS    10000UL     // host must be quiet this long before we process
#define MEDIA_ABSENT_MS  5000UL      // hide media long enough that the host's
                                     // TUR polling MUST notice the removal and
                                     // do a real remount (1s can slip between polls)
#define MAX_FILE_BYTES   10485760UL  // 10 MB cap (streamed from SD, not RAM-bound)
#define MAX_DEFERRALS    3           // failed-validation retries before uploading as-is
#define SECTOR_SIZE      512
#define BACKUP_DIR       "/backup"

// DFRobot 2.0" Color Serial Command Display I2C Address
#define DISPLAY_I2C_ADDR            0x2C
#define DISPLAY_SDA                 1
#define DISPLAY_SCL                 2

// Shown in the display header bar
#define MODEL_NUMBER                "URS-V2"
// Firmware version comes from PROJECT_VER in the top-level CMakeLists.txt
#define FW_VERSION                  (esp_app_get_description()->version)

// WiFi credentials and portal URL live in secrets.h (not committed to git).
// Copy secrets.h.example to secrets.h and fill it in.
#include "secrets.h"

// US Pacific time with automatic DST. SNTP re-syncs ~hourly forever.
const char* TZ_INFO = "PST8PDT,M3.2.0,M11.1.0";

// Unique instrument ID sent with every upload.
// Leave "" to use the chip's factory MAC (unique per board, same firmware
// for the whole fleet), or set a friendly name like "analyzer-lab2".
const char* DEVICE_ID_OVERRIDE = "";

String deviceId() {
  if (strlen(DEVICE_ID_OVERRIDE) > 0) return String(DEVICE_ID_OVERRIDE);
  uint64_t mac = ESP.getEfuseMac();
  char buf[13];
  snprintf(buf, sizeof(buf), "%012llX", mac);
  return String(buf);
}

USBMSC MSC;
SemaphoreHandle_t sdMutex;

static uint32_t cardSectorCount = 0;
static volatile bool writtenTo = false;
static volatile uint32_t lastWriteTime = 0;

// Write telemetry: what did the host actually write since the last cycle?
static volatile uint32_t writeSectors = 0;
static volatile uint32_t writeMinLBA = 0xFFFFFFFF;
static volatile uint32_t writeMaxLBA = 0;

static volatile bool usbEnumerated = false;
static bool usbFlashSignaled = false;
static volatile bool apAssociated = false;

static bool systemReady = false;
static int filesDeferred = 0;   // size-0 or invalid-PDF files seen this pass
static int deferralCycles = 0;
static int uploadTotal = 0;     // files in the current upload pass (for the progress bar)
static int uploadDone = 0;
static bool uploadFailed = false;  // any POST failed this pass

// ---- LED helpers ----

void restoreLED() {
  digitalWrite(LED_PIN, systemReady ? HIGH : LOW);
}

void flashGreen(int times, int onMs = 150, int offMs = 150) {
  digitalWrite(LED_PIN, LOW);
  delay(200);
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(onMs);
    digitalWrite(LED_PIN, LOW);  delay(offMs);
  }
  restoreLED();
}

void blinkFast(int times) { flashGreen(times, 100, 100); }
void blinkSlow(int times) { flashGreen(times, 500, 500); }

// ---- USB MSC callbacks ----
// bufsize may be a multiple of 512 — loop over every sector.
// No delays/LED work in callbacks; set flags only.

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  usbEnumerated = true;
  // 5s timeout: a peek-survey holds the mutex ~1s; host I/O must WAIT for it,
  // not fail — a returned error here poisons the host's view of the volume.
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) return -1;
  bool ok = true;
  uint8_t* buf = (uint8_t*)buffer;
  for (uint32_t i = 0; ok && i < bufsize / SECTOR_SIZE; i++) {
    ok = SD.readRAW(buf + i * SECTOR_SIZE, lba + i);
  }
  xSemaphoreGive(sdMutex);
  return ok ? (int32_t)bufsize : -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) return -1;
  bool ok = true;
  uint32_t nSec = bufsize / SECTOR_SIZE;
  for (uint32_t i = 0; ok && i < nSec; i++) {
    ok = SD.writeRAW(buffer + i * SECTOR_SIZE, lba + i);
  }
  xSemaphoreGive(sdMutex);

  writeSectors += nSec;
  if (lba < writeMinLBA) writeMinLBA = lba;
  if (lba + nSec - 1 > writeMaxLBA) writeMaxLBA = lba + nSec - 1;
  writtenTo = true;
  lastWriteTime = millis();
  return ok ? (int32_t)bufsize : -1;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  Serial1.printf("StartStop start=%u eject=%u\n", start, load_eject);
  return true;
}

// ---- WiFi ----

static volatile uint8_t wifiDiscReason = 0;
static uint32_t lastWiFiAttempt = 0;   // millis() of the last failed connect
static bool uploadPending = false;     // files are waiting for WiFi
#define WIFI_RETRY_MS 30000UL          // retry WiFi every 30 s while disconnected
#define LOOP_WDT_S    300              // reboot if loop() is stuck this long (hung upload)

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) apAssociated = true;
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    // 8 = ASSOC_LEAVE, 36 = STA_LEAVING: our own code disconnecting, not the real cause
    if (r != 8 && r != 36) wifiDiscReason = r;
  }
}

// Plain-English version of the ESP-IDF disconnect reason codes
String wifiReasonText(uint8_t r) {
  switch (r) {
    case 0:   return "timed out";
    case 201: return "network not found";                 // NO_AP_FOUND
    case 2:                                               // AUTH_EXPIRE
    case 14:                                              // MIC_FAILURE
    case 15:                                              // 4WAY_HANDSHAKE_TIMEOUT
    case 202:                                             // AUTH_FAIL
    case 204: return "wrong password?";                   // HANDSHAKE_TIMEOUT
    case 210:                                             // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211: return "security type not supported";       // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 203:                                             // ASSOC_FAIL
    case 205: return "router refused connection";         // CONNECTION_FAIL
    default:  return "error code " + String(r);
  }
}

// After a failed connect: scan and report whether our SSID is visible and how it's secured
void diagnoseWiFi() {
  String reason = wifiReasonText(wifiDiscReason);
  Serial1.printf("WiFi fail reason %u: %s\n", wifiDiscReason, reason.c_str());

  WiFi.disconnect();          // stop connect attempts so the scan can run
  delay(100);
  int n = WiFi.scanNetworks();
  int found = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) { found = i; break; }
  }
  String detail;
  if (n < 0) {
    detail = "WiFi: " + reason;
  } else if (found < 0) {
    detail = "WiFi: '" + String(WIFI_SSID) + "' not seen (" + String(n) + " nets)";
  } else {
    wifi_auth_mode_t auth = WiFi.encryptionType(found);
    bool enterprise = (auth == WIFI_AUTH_WPA2_ENTERPRISE);
    Serial1.printf("SSID seen: ch %d, %d dBm, auth %d\n", (int)WiFi.channel(found), (int)WiFi.RSSI(found), (int)auth);
    if (enterprise) detail = "WiFi: enterprise login, not supported";
    else            detail = "WiFi: " + reason + " (" + String(WiFi.RSSI(found)) + " dBm)";
  }
  WiFi.scanDelete();
  Serial1.println(detail);
  displaySetWiFiDetail(detail);   // small line under the WiFi status
}

void connectWiFi() {
  Serial1.println("Connecting WiFi...");
  displayWiFiConnecting();
  apAssociated = false;
  wifiDiscReason = 0;
  static bool eventRegistered = false;
  if (!eventRegistered) { WiFi.onEvent(onWiFiEvent); eventRegistered = true; }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // more reliable with phone hotspots / busy APs
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  bool associatedSignaled = false;
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 80) {   // up to 20 s
    if (apAssociated && !associatedSignaled) {
      Serial1.println("Network found (associated)");
      flashGreen(2);
      associatedSignaled = true;
    }
    delay(250);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial1.printf("WiFi CONNECTED, IP %s\n", WiFi.localIP().toString().c_str());
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    displaySetWiFi(true, WiFi.RSSI(), WiFi.localIP().toString());
    displaySetStatus("Ready", DISP_GREEN);
    systemReady = true;
    flashGreen(3);
  } else {
    Serial1.println("WiFi FAILED");
    lastWiFiAttempt = millis();
    displaySetWiFi(false, 0, "");
    displaySetStatus("Offline", DISP_GREY);
    diagnoseWiFi();
    systemReady = false;
    restoreLED();
  }
}

// ---- Time / naming helpers ----

String timestampStr() {
  struct tm t;
  if (getLocalTime(&t, 50)) {
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &t);
    return String(buf);
  }
  return "unsynced-" + String(millis() / 1000);
}

String sanitizeHeader(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    out += (c >= 0x20 && c <= 0x7E) ? c : '_';
  }
  return out;
}

bool hasExtension(const String& name, const char* ext) {
  String n = name; n.toLowerCase();
  return n.endsWith(ext);
}

String backupPath(const String& baseName) {
  String base = String(BACKUP_DIR) + "/" + timestampStr() + "_" + baseName;
  if (!SD.exists(base)) return base;
  for (int n = 1; n < 100; n++) {
    String cand = String(BACKUP_DIR) + "/" + timestampStr() + "_" + String(n) + "_" + baseName;
    if (!SD.exists(cand)) return cand;
  }
  return base;
}

bool isJunkName(const String& name) {
  return name.startsWith(".") || name == "System Volume Information";
}

// PDF sanity check: "%PDF-" header and "%%EOF" in the last 1 KB.
bool validatePDF(File& f) {
  uint8_t head[5];
  if (!f.seek(0) || f.read(head, 5) != 5) return false;
  if (memcmp(head, "%PDF-", 5) != 0) return false;

  size_t sz = f.size();
  size_t tailLen = (sz < 1024) ? sz : 1024;
  if (!f.seek(sz - tailLen)) return false;

  uint8_t tail[1024];
  int n = f.read(tail, tailLen);
  if (n <= 0) return false;

  for (int i = 0; i <= n - 5; i++) {
    if (memcmp(tail + i, "%%EOF", 5) == 0) {
      f.seek(0);
      return true;
    }
  }
  return false;
}

// ---- Survey: read-only walk that logs EVERYTHING and counts candidates ----

int surveyDir(File dir) {
  int candidates = 0;
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());

    if (f.isDirectory()) {
      if (isJunkName(name)) {
        Serial1.printf("  [skip dir]  %s\n", f.path());
      } else if (name.equalsIgnoreCase("backup")) {
        Serial1.printf("  [backup]    %s (not scanned)\n", f.path());
      } else {
        Serial1.printf("  [dir]       %s\n", f.path());
        candidates += surveyDir(f);
      }
    } else if (isJunkName(name)) {
      Serial1.printf("  [skip junk] %s (%u B)\n", f.path(), (unsigned)f.size());
    } else if (f.size() == 0) {
      Serial1.printf("  [EMPTY]     %s dir entry exists but 0 bytes (host still writing?)\n", f.path());
      filesDeferred++;  // check again next cycle
    } else {
      Serial1.printf("  [candidate] %s (%u B)\n", f.path(), (unsigned)f.size());
      candidates++;
    }
    f.close();
    f = dir.openNextFile();
  }
  return candidates;
}

// ---- HTTP POST (streams file from SD) ----

bool postFile(File& f, const String& path) {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();  // TODO: pin the root CA for production

  HTTPClient http;
  http.begin(client, API_URL);
  http.setTimeout(30000);
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Filename", sanitizeHeader(path));
  http.addHeader("X-Device-Id", deviceId());

  f.seek(0);
  int code = http.sendRequest("POST", &f, f.size());
  http.end();

  Serial1.printf("POST %s (%u bytes) -> %d\n", path.c_str(), (unsigned)f.size(), code);
  return (code == 200);
}

// ---- Upload pass ----

void processDir(File dir) {
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());

    if (f.isDirectory()) {
      if (!isJunkName(name) && !name.equalsIgnoreCase("backup")) {
        processDir(f);
      }
      f.close();
    } else if (!isJunkName(name) && f.size() > 0) {
      String fullPath = String(f.path());
      size_t fileSize = f.size();
      esp_task_wdt_reset();   // each file gets a fresh LOOP_WDT_S budget

      // Skip corrupt directory entries (control characters in the name)
      bool badName = false;
      for (size_t i = 0; i < name.length(); i++) if ((uint8_t)name[i] < 0x20) badName = true;
      if (badName) {
        Serial1.printf("SKIPPED corrupt entry in %s\n", dir.path());
        uploadDone++;
        displaySetProgress(uploadDone, uploadTotal);
        f.close();
        f = dir.openNextFile();
        continue;
      }

      Serial1.printf("Uploading %s (%u KB)\n", name.c_str(), (unsigned)((fileSize + 1023) / 1024));
      displaySetFile(String(f.path()) + "  (" + String((unsigned)((fileSize + 1023) / 1024)) + " KB)");

      if (fileSize > MAX_FILE_BYTES) {
        Serial1.printf("SKIPPED %s too large\n", fullPath.c_str());
        uploadDone++;
        displaySetProgress(uploadDone, uploadTotal);
        f.close();
        f = dir.openNextFile();
        continue;
      }

      if (hasExtension(name, ".pdf") && !validatePDF(f)) {
        if (deferralCycles < MAX_DEFERRALS) {
          Serial1.printf("DEFERRED %s PDF incomplete\n", fullPath.c_str());
          filesDeferred++;
          uploadDone++;
          displaySetProgress(uploadDone, uploadTotal);
          f.close();
          f = dir.openNextFile();
          continue;
        }
        Serial1.printf("WARNING %s failed validation %d times, uploading anyway\n",
                       fullPath.c_str(), MAX_DEFERRALS);
      }

      // (postFile may reconnect WiFi, which changes the status line)
      if (!uploadFailed) displaySetStatus("Uploading", DISP_YELLOW, true);
      bool success = postFile(f, fullPath);
      f.close();
      uploadDone++;
      displaySetProgress(uploadDone, uploadTotal);

      if (success) {
        flashGreen(1);
        if (!SD.exists(BACKUP_DIR)) SD.mkdir(BACKUP_DIR);
        String dest = backupPath(name);
        if (SD.rename(fullPath.c_str(), dest.c_str())) {
          Serial1.printf("Backed up -> %s\n", dest.c_str());
        } else {
          SD.remove(fullPath.c_str());
          Serial1.printf("Backup rename FAILED, deleted %s\n", fullPath.c_str());
        }
      } else {
        uploadFailed = true;
        displaySetStatus("Upload failed", DISP_RED);
        blinkFast(20);
      }
    } else {
      f.close();
    }

    f = dir.openNextFile();
  }
}

bool remountSD() {
  SD.end();
  delay(100);
  if (!SD.begin(SD_CS)) {
    Serial1.println("SD re-init FAILED");
    return false;
  }
  return true;
}

// ---- Setup ----

void setup() {
  // Watchdog on the main loop: if an upload hangs (dropped connection mid-send),
  // the board reboots after LOOP_WDT_S instead of sitting on "Uploading" forever.
  // Files are only moved to /backup after a successful upload, so nothing is lost.
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = LOOP_WDT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
  esp_task_wdt_add(NULL);

  Serial1.begin(115200, SERIAL_8N1, -1, 18);
  delay(1000);
  Serial1.println("BOOT OK");
  Serial1.printf("DEVICE ID: %s\n", deviceId().c_str());
  Serial1.printf("MODEL: %s  FW: %s\n", MODEL_NUMBER, FW_VERSION);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Wire.begin(DISPLAY_SDA, DISPLAY_SCL);
  if (!displayBegin(MODEL_NUMBER, deviceId(), FW_VERSION)) {
    Serial1.println("Display not found, continuing without it");
  }
  displaySetStatus("Starting", DISP_WHITE, true);

  sdMutex = xSemaphoreCreateMutex();

  Serial1.println("Init SD...");
  SPI.begin(17, 16, 15, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial1.println("SD FAILED");
    displaySetStatus("SD card error", DISP_RED);
    blinkFast(20);
    return;
  }

  cardSectorCount = SD.cardSize() / SECTOR_SIZE;
  Serial1.printf("SD OK - %lu sectors\n", cardSectorCount);

  // Tells macOS not to build a Spotlight index on this volume — kills most
  // of the background junk writes during Mac testing. Harmless elsewhere.
  if (!SD.exists("/.metadata_never_index")) {
    File nf = SD.open("/.metadata_never_index", FILE_WRITE);
    if (nf) { nf.close(); Serial1.println("Created .metadata_never_index"); }
  }

  MSC.vendorID("ESP32");
  MSC.productID("USB_MSC");
  MSC.productRevision("1.0");
  MSC.onRead(onRead);
  MSC.onWrite(onWrite);
  MSC.onStartStop(onStartStop);
  MSC.mediaPresent(true);
  MSC.begin(cardSectorCount, SECTOR_SIZE);
  Serial1.println("MSC started");

  USB.begin();
  Serial1.println("USB started");

  delay(2000);
  connectWiFi();
  Serial1.println("Setup complete");
}

// ---- Loop ----

void loop() {
  esp_task_wdt_reset();
  displayTick();

  // Retry WiFi in the background while disconnected
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiAttempt > WIFI_RETRY_MS) {
    connectWiFi();
  }
  // WiFi is back and files were waiting: run the upload pass now
  if (uploadPending && WiFi.status() == WL_CONNECTED) {
    uploadPending = false;
    writtenTo = true;   // normal idle check still applies: waits until the host
                        // has been quiet for WRITE_IDLE_MS before touching the card
  }

  if (usbEnumerated && !usbFlashSignaled) {
    usbFlashSignaled = true;
    Serial1.println("USB enumerated by host");
    flashGreen(1);
  }

  if (writtenTo && (millis() - lastWriteTime > WRITE_IDLE_MS)) {
    writtenTo = false;

    // Report what the host actually wrote, then reset counters
    uint32_t ws = writeSectors, lo = writeMinLBA, hi = writeMaxLBA;
    writeSectors = 0; writeMinLBA = 0xFFFFFFFF; writeMaxLBA = 0;
    Serial1.printf("Write idle: %lu sectors written, LBA %lu..%lu (card has %lu)\n",
                   (unsigned long)ws, (unsigned long)lo, (unsigned long)hi,
                   (unsigned long)cardSectorCount);

    // ---- Peek: read-only scan WITHOUT toggling media ----
    // If only junk/metadata was written, this finds nothing, we skip the
    // media toggle, and the host isn't provoked into another write cycle.
    filesDeferred = 0;
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    int candidates = -1;
    if (remountSD()) {
      Serial1.println("Survey:");
      File root = SD.open("/");
      if (root) {
        candidates = surveyDir(root);
        root.close();
      }
      remountSD();  // back to a clean state for raw access
    }
    xSemaphoreGive(sdMutex);
    if (candidates < 0) displaySetStatus("SD read error", DISP_RED);

    if (candidates <= 0) {
      if (filesDeferred > 0) {
        // Empty dir entries seen: host may still be flushing. Re-check later.
        deferralCycles++;
        writtenTo = true;
        lastWriteTime = millis();
        Serial1.printf("No complete files yet (%d empty), will re-check\n", filesDeferred);
      } else {
        deferralCycles = 0;
        Serial1.println("Nothing to upload (metadata/junk write). Staying quiet.");
        if (candidates == 0) { displaySetStatus("Ready", DISP_GREEN); displaySetProgress(0, 0); }
      }
      return;
    }

    // ---- No WiFi: leave the drive alone and wait (avoids a fail/remount loop) ----
    if (WiFi.status() != WL_CONNECTED) {
      Serial1.printf("%d file(s) waiting for WiFi\n", candidates);
      uploadPending = true;
      displaySetStatus("Waiting for WiFi", DISP_YELLOW, true);
      return;
    }

    // ---- Real files present: pause media and upload ----
    Serial1.printf("%d candidate(s), processing...\n", candidates);
    uploadTotal = candidates;
    uploadDone = 0;
    uploadFailed = false;
    displaySetProgress(0, uploadTotal);
    displaySetStatus("Uploading", DISP_YELLOW, true);
    digitalWrite(LED_PIN, LOW);
    blinkFast(6);

    MSC.mediaPresent(false);
    delay(MEDIA_ABSENT_MS);  // long enough for the host to see the removal

    xSemaphoreTake(sdMutex, portMAX_DELAY);
    filesDeferred = 0;
    if (remountSD()) {
      File root = SD.open("/");
      if (root) {
        processDir(root);
        root.close();
      }
      Serial1.println("Processing done");
    } else {
      displaySetStatus("SD read error", DISP_RED);
      blinkFast(20);
    }
    remountSD();
    xSemaphoreGive(sdMutex);
    MSC.mediaPresent(true);

    if (filesDeferred > 0) {
      displaySetStatus("Waiting on files", DISP_YELLOW, true);
      deferralCycles++;
      writtenTo = true;
      lastWriteTime = millis();
      Serial1.printf("%d file(s) deferred, retry cycle %d\n", filesDeferred, deferralCycles);
    } else {
      deferralCycles = 0;
      if (uploadFailed) displaySetStatus("Some uploads failed", DISP_RED);
      else              displaySetStatus("Upload complete", DISP_GREEN);
      blinkSlow(3);
    }

    restoreLED();
    Serial1.println("Ready");
  }

  delay(100);
}