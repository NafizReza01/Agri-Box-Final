#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include <Adafruit_BMP280.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include "FS.h"
#include "LittleFS.h"
#include <math.h>

// OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


// ---------------- MPU6050 low-level helpers (interrupt config) ----------------
// Adafruit_MPU6050 doesn't expose all interrupt registers, so we write them directly.
static const uint8_t MPU_ADDR = 0x68;

void mpuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t mpuReadReg(uint8_t reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0;
}

// Configure Motion Interrupt to wake ESP32 from deep sleep.
// This is "motion detected" (not perfect fall physics), but gives you always-on trigger.
// We'll still run checkFall() after wake to latch properly.
void mpuEnableMotionInterrupt() {
  // INT_PIN_CFG (0x37):
  //  - LATCH_INT_EN (bit5) = 1 (latch until cleared)
  //  - INT_RD_CLEAR (bit4) = 1 (any read clears)
  // Active-high, push-pull default.
  mpuWriteReg(0x37, 0b00110000);

  // MOT_THR (0x1F): motion threshold (LSB ~ 2mg). 20 -> ~40mg
  mpuWriteReg(0x1F, 20);

  // MOT_DUR (0x20): duration (ms). 10 -> ~10ms
  mpuWriteReg(0x20, 10);

  // INT_ENABLE (0x38): enable Motion interrupt (bit6)
  mpuWriteReg(0x38, 0b01000000);

  // Clear any pending interrupt by reading INT_STATUS (0x3A)
  (void)mpuReadReg(0x3A);
}

// Must be called before sleeping to avoid immediate re-wake if INT line is still high/latched.
void mpuClearInterrupt() {
  (void)mpuReadReg(0x3A);
}

// ================== PINS ==================
#define I2C_SDA     21
#define I2C_SCL     22

#define SOIL_PIN    34
#define GREEN_LED   32
#define RED_LED     33
#define BUZZER_PIN  27
#define RESET_BTN   26   // Active-low tactical button 
#define MPU_INT_PIN 25   // MPU6050 INT -> ESP32 GPIO25 (wake pin)

#define DHTPIN      4
#define DHTTYPE     DHT11

// ================== SOIL CALIBRATION ==================
#define SOIL_DRY  3200
#define SOIL_WET  1400

// ================== WIFI (STA to Base SoftAP) ==================
static const char* BASE_SSID = "AGRI_BASE";
static const char* BASE_PASS = "agri12345";

// Base AP default IP for ESP32 SoftAP is usually 192.168.4.1
static const char* BASE_HOST = "http://192.168.4.1";

// Chunk upload settings
#define CHUNK_SIZE_BYTES 512
#define HTTP_TIMEOUT_MS  5000
#define MAX_RETRY        6

// ================== LOGGING / SLEEP ==================
#define LOG_INTERVAL_SEC 60     // log once per 1 min
#define WIFI_WINDOW_MS   15000  // try to find/connect/upload within 15 sec after logging
#define LOG_FILE "/log.csv"

// "full" threshold (LittleFS doesn't expose exact free space reliably)
#define MAX_LOG_BYTES (220 * 1024) // ~220KB

// ================== FALL DETECTION ==================
#define FALL_THRESHOLD 11.0
#define GYRO_THRESHOLD 3.0

// ================== OLED ==================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== OBJECTS ==================
Adafruit_BMP280 bmp;
Adafruit_MPU6050 mpu;
DHT dht(DHTPIN, DHTTYPE);

// ================== STATE ==================
bool wifiOk = true;   // becomes false only if we detect persistent WiFi failures
bool oledOk = false;
bool bmpOk  = false;
bool mpuOk  = false;
bool dhtOk  = true;  // validate by reads
bool fsOk   = false;

bool storageFull = false;
bool fallLatched = false;

unsigned long lastOkPulse = 0;
unsigned long lastRedBlink = 0;
unsigned long lastRapidBeep = 0;

// Non-blocking OK blink state (0.5s ON every 2s)
bool okBlinkOn = false;
unsigned long okBlinkStartMs = 0;

// ---------------- OLED helper ----------------
void oledShow(const String &msg) {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (msg == "OK") {
    display.setTextSize(3);
    display.setCursor(40, 20);
    display.print("OK");
  } else {
    display.setTextSize(1);
    display.setCursor(10, 20);
    display.print(msg);
  }
  display.display();
}

// Priority: 1) STORAGE_FULL/ERROR 2) FALL 3) OK
void oledUpdate() {
  if (storageFull || !fsOk) {
    oledShow("STORAGE FULL");
  } else if (fallLatched) {
    oledShow("FALL DETECTED");
  } else {
    oledShow("OK");
  }
}

// ---------------- Indicators (Logger) ----------------

// OK: Green LED blink 0.5 sec every 2 sec (NON-BLOCKING). No beep.
void okPulseIfDue() {
  const unsigned long PERIOD_MS = 2000; // total cycle
  const unsigned long ON_MS     = 500;  // ON duration

  unsigned long now = millis();

  // If currently ON, turn OFF after ON_MS
  if (okBlinkOn) {
    if (now - okBlinkStartMs >= ON_MS) {
      okBlinkOn = false;
      digitalWrite(GREEN_LED, LOW);
    }
    return;
  }

  // If OFF, start a new pulse every PERIOD_MS
  if (now - lastOkPulse >= PERIOD_MS) {
    lastOkPulse = now;
    okBlinkOn = true;
    okBlinkStartMs = now;
    digitalWrite(GREEN_LED, HIGH);
  }
}

// Error: red blink (.5 sec periodic)
void redBlinkIfDue() {
  if (millis() - lastRedBlink >= 500) {
    lastRedBlink = millis();
    digitalWrite(RED_LED, !digitalRead(RED_LED));
  }
}

// Fall latched: red blink (.5s periodic) + rapid beep (1s periodic)
void fallIndicators() {
  redBlinkIfDue();
  if (millis() - lastRapidBeep >= 1000) {
    lastRapidBeep = millis();
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150); // short beep is OK to block briefly
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// Sync success EOF: solid green 2s + long beep 2s
void successEOFSignal() {
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(2000);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// Storage full / error: red ON indefinitely
void hardErrorSignal() {
  digitalWrite(RED_LED, HIGH);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// ---------------- Utility ----------------
bool resetPressed() { return digitalRead(RESET_BTN) == LOW; }

// ---------------- WiFi helpers ----------------
void wifiOff() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(50);
}

bool connectToBaseWithin(unsigned long windowMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true); // save some power
  WiFi.disconnect(true, true);
  delay(100);

  unsigned long start = millis();
  bool seen = false;

  while (millis() - start < windowMs) {
    // Scan occasionally
    int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == String(BASE_SSID)) {
        seen = true;
        break;
      }
    }
    WiFi.scanDelete();

    if (seen) break;
    if (resetPressed()) return false;
    delay(300);
  }

  if (!seen) return false;

  WiFi.begin(BASE_SSID, BASE_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    if (resetPressed()) return false;
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool httpGet(const String &url, String &outBody) {
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  int code = http.GET();
  outBody = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

bool httpPostText(const String &url, const String &body, String &outBody) {
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(url);
  http.addHeader("Content-Type", "text/plain");
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  outBody = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

// ---------------- Core: log one line ----------------
void appendLogLine() {
  if (!fsOk) return;

  float temp_c = bmpOk ? bmp.readTemperature() : NAN;
  float pressure_hpa = bmpOk ? (bmp.readPressure() / 100.0f) : NAN;
  float humidity = dht.readHumidity();
  int soilRaw = analogRead(SOIL_PIN);

  // Soil raw -> %
  float soilPercent = 100.0f * (SOIL_DRY - soilRaw) / (SOIL_DRY - SOIL_WET);
  soilPercent = constrain(soilPercent, 0.0f, 100.0f);

  if (isnan(humidity)) {
    Serial.println("DHT11 NOT FOUND / READ FAIL");
    dhtOk = false;
    return;
  } else dhtOk = true;

  if (isnan(temp_c) || isnan(pressure_hpa)) {
    Serial.println("BMP280 READ FAIL");
    return;
  }

  // CSV: epoch_ms,temp,humidity,pressure,soilPercent
  uint64_t ms = (uint64_t)millis();
  String line = String(ms) + "," + String(temp_c, 2) + "," + String(humidity, 1) + "," +
                String(pressure_hpa, 2) + "," + String(soilPercent, 1) + "\n";

  File f = LittleFS.open(LOG_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("LittleFS WRITE FAIL");
    storageFull = true;
    return;
  }
  f.print(line);
  f.close();

  File r = LittleFS.open(LOG_FILE, FILE_READ);
  if (r) {
    if (r.size() > MAX_LOG_BYTES) storageFull = true;
    r.close();
  }

  // debug
  Serial.print("SOIL raw=");
  Serial.print(soilRaw);
  Serial.print("  soil%=");
  Serial.println(soilPercent, 1);

  Serial.print("LOGGED: ");
  Serial.print(line);
}

// ---------------- Fall detection check ----------------
void checkFall() {
  if (!mpuOk) return;
  sensors_event_t accel, gyro, temp_mpu;
  mpu.getEvent(&accel, &gyro, &temp_mpu);

  float ax = accel.acceleration.x;
  float ay = accel.acceleration.y;
  float az = accel.acceleration.z;
  float acc_total = sqrt(ax * ax + ay * ay + az * az);

  float gyro_mag = sqrt(
    gyro.gyro.x * gyro.gyro.x +
    gyro.gyro.y * gyro.gyro.y +
    gyro.gyro.z * gyro.gyro.z
  );

  if (!fallLatched) {
    if (acc_total > FALL_THRESHOLD || gyro_mag > GYRO_THRESHOLD) {
      fallLatched = true;
      Serial.println("FALL DETECTED (latched)");
    }
  }
}

// ---------------- Sync (logger -> base over WiFi) ----------------
bool syncToBaseOverWiFi(unsigned long windowMs) {
  if (!fsOk) return false;
  if (fallLatched) return false;
  if (storageFull) return false;

  File f = LittleFS.open(LOG_FILE, FILE_READ);
  if (!f) return false;
  if (f.size() == 0) { f.close(); return false; }

  // 1) Connect to Base SoftAP
  bool connected = connectToBaseWithin(windowMs);
  if (!connected) {
    f.close();
    wifiOff();
    return false;
  }

  Serial.print("Connected to Base. IP=");
  Serial.println(WiFi.localIP());

  // 2) Handshake: GET /ready
  String body;
  if (!httpGet(String(BASE_HOST) + "/ready", body) || !body.startsWith("OK")) {
    Serial.println("Base /ready failed");
    f.close();
    wifiOff();
    return false;
  }

  // 3) Transfer in chunks with explicit ack via HTTP response
  uint32_t seq = 0;
  bool fail = false;

  while (f.available()) {
    checkFall();
    if (fallLatched) break;
    if (resetPressed()) { fail = true; break; }

    uint8_t buf[CHUNK_SIZE_BYTES];
    int n = f.read(buf, CHUNK_SIZE_BYTES);
    if (n <= 0) break;

    String chunk;
    chunk.reserve(n);
    for (int i = 0; i < n; i++) chunk += (char)buf[i];

    bool ok = false;
    for (int attempt = 0; attempt < MAX_RETRY; attempt++) {
      String resp;
      String url = String(BASE_HOST) + "/upload?seq=" + String(seq);
      if (httpPostText(url, chunk, resp) && resp == ("OK:" + String(seq))) {
        ok = true;
        break;
      }
      delay(50);
      if (resetPressed()) { ok = false; break; }
    }

    if (!ok) { fail = true; break; }
    seq++;
  }

  f.close();

  if (fallLatched) {
    Serial.println("Fall latched during sync; notifying Base and stopping.");
    String resp;
    httpPostText(String(BASE_HOST) + "/fall", "FALL", resp);
    wifiOff();
    return false;
  }

  if (fail) {
    Serial.println("WiFi upload failed (timeout/retry exceeded)");
    wifiOff();
    return false;
  }

  // 4) Done: POST /done then delete file
  String doneResp;
  if (!httpPostText(String(BASE_HOST) + "/done", "DONE", doneResp) || !doneResp.startsWith("DONE")) {
    Serial.println("Base /done failed");
    wifiOff();
    return false;
  }

  LittleFS.remove(LOG_FILE);
  Serial.println("Deleted log file after DONE");

  successEOFSignal();
  wifiOff();
  return true;
}

// ---------------- Sleep helper ----------------
void goToSleep1Min() {
  Serial.println("Sleeping for 60 sec (wake on timer OR MPU motion interrupt)...");

  // Timer wake for periodic logging
  esp_sleep_enable_timer_wakeup((uint64_t)LOG_INTERVAL_SEC * 1000000ULL);

  // External wake on MPU INT (active HIGH)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)MPU_INT_PIN, 1);

  // Clear latched interrupt before sleeping
  mpuClearInterrupt();
  delay(10);

  esp_deep_sleep_start();
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);

  pinMode(MPU_INT_PIN, INPUT_PULLDOWN);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);

  fsOk = LittleFS.begin(true);
  if (!fsOk) Serial.println("LittleFS NOT FOUND / INIT FAILED");
  else Serial.println("LittleFS OK");

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) Serial.println("OLED NOT FOUND");
  else {
    Serial.println("OLED OK");
    display.clearDisplay();
    display.display();
  }

  bmpOk = bmp.begin(0x76);
  if (!bmpOk) Serial.println("BMP280 NOT FOUND");
  else Serial.println("BMP280 OK");

  mpuOk = mpu.begin();
  if (!mpuOk) Serial.println("MPU6050 NOT FOUND");
  else {
    Serial.println("MPU6050 OK");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }


  // Enable MPU motion interrupt for always-on wake
  mpuEnableMotionInterrupt();
  dht.begin(); // validate by reads later

  // ---- Wake reason handling (timer vs MPU motion) ----
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up by MPU motion interrupt (EXT0).");
    // Confirm/latch fall by sampling quickly
    for (int i = 0; i < 25; i++) { // ~500ms total
      checkFall();
      if (fallLatched) break;
      delay(20);
    }
    // If motion was strong enough to trip checkFall, fallLatched will be true
    if (fallLatched) {
      Serial.println("Fall latched after motion wake.");
    } else {
      Serial.println("Motion wake but fall not latched (treating as motion-only).");
    }
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up by timer for periodic logging.");
  } else {
    Serial.println("Cold boot / other wake.");
  }

  // Start with WiFi OFF for power saving (we turn it on only during sync window)
  wifiOff();

  storageFull = false;
  if (fsOk) {
    File f = LittleFS.open(LOG_FILE, FILE_READ);
    if (f) {
      if (f.size() > MAX_LOG_BYTES) storageFull = true;
      f.close();
    }
  }

  oledUpdate();

  if (fsOk && oledOk && bmpOk && mpuOk) {
    Serial.println("All Ok");
  }
}

// ---------------- Loop ----------------
void loop() {
  // Reset button: clears fall + tells Base (if reachable)
  if (resetPressed()) {
    fallLatched = false;
    storageFull = false; // optional
    okBlinkOn = false;
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    oledUpdate();

    // Try to notify Base (best effort)
    if (connectToBaseWithin(5000)) {
      String resp;
      httpPostText(String(BASE_HOST) + "/reset", "RESET", resp);
      wifiOff();
    }
    Serial.println("Reset pressed -> cleared flags (and attempted Base reset)");
    delay(300);
  }

  // Storage full / FS error: red ON indefinitely, stop everything
  if (!fsOk || storageFull) {
    okBlinkOn = false;
    digitalWrite(GREEN_LED, LOW);
    hardErrorSignal();
    oledUpdate();
    delay(200);
    return;
  }

  // Fall latched: override everything
  checkFall();
  if (fallLatched) {
    okBlinkOn = false;
    digitalWrite(GREEN_LED, LOW);
    oledUpdate();
    fallIndicators();
    delay(50);
    return;
  }

  // Normal OK state: non-blocking blink
  okPulseIfDue();
  oledUpdate();

  // 1) Log once
  appendLogLine();

  // 2) Try WiFi sync window (no constant listen/ping needed)
  Serial.println("Trying WiFi sync window...");
  bool ok = syncToBaseOverWiFi(WIFI_WINDOW_MS);
  if (!ok) {
    // brief red blink on sync fail (non-fatal)
    unsigned long t0 = millis();
    while (millis() - t0 < 1200) redBlinkIfDue();
    digitalWrite(RED_LED, LOW);
  }

  // 3) Sleep 1 min again
  goToSleep1Min();
}
