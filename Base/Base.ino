#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>

// OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================== PINS ==================
#define I2C_SDA     21
#define I2C_SCL     22

#define GREEN_LED   32
#define RED_LED     33
#define BUZZER_PIN  27
#define RESET_BTN   25   // Active-low tactical button

// ================== WIFI (SOFTAP) ==================
static const char* AP_SSID = "AGRI_BASE";
static const char* AP_PASS = "agri12345"; // 8+ chars

// ================== OLED ==================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== SERVER ==================
WebServer server(80);

// ================== STATE ==================
bool wifiOk = false;
bool oledOk = false;

bool fallActive = false;
bool timeoutActive = false;
bool inSync = false;

unsigned long lastRedBlink = 0;
unsigned long lastRapidBeep = 0;

// Non-blocking OK blink state (0.5s ON every 2s)
unsigned long lastOkPulse = 0;
bool okBlinkOn = false;
unsigned long okBlinkStartMs = 0;

// Reassembly buffer for incoming CSV lines
String rxBuffer;

// -------- CSV -> JSON (for uploader.js) --------
// Expected CSV from Logger: epoch_ms,temp,humidity,pressure,soil
// Returns a compact JSON line: {"temp":..,"humidity":..,"pressure":..,"soil":..}
// If parsing fails, falls back to raw line wrapped as {"raw":"..."}
String csvToJson(const String &csvLine) {
  // Quick split (no quoted fields expected)
  String parts[6];
  int count = 0;
  int start = 0;

  for (int i = 0; i <= (int)csvLine.length(); i++) {
    if (i == (int)csvLine.length() || csvLine[i] == ',') {
      if (count < 6) parts[count++] = csvLine.substring(start, i);
      start = i + 1;
    }
  }

  // Need at least 5 fields: epoch,temp,humidity,pressure,soil
  if (count >= 5) {
    String temp     = parts[1]; // temp
    String humidity = parts[2]; // humidity
    String pressure = parts[3]; // pressure
    String soil     = parts[4]; // soil (percent)
    // Build JSON (numbers kept as-is)
    return String("{\"temp\":") + temp +
           ",\"humidity\":" + humidity +
           ",\"pressure\":" + pressure +
           ",\"soil\":" + soil + "}";
  }

  // Fallback (escape quotes minimally)
  String raw = csvLine;
  raw.replace("\\", "\\\\");
  raw.replace("\"", "\\\"");
  return String("{\"raw\":\"") + raw + "\"}";
}


// Expected next chunk sequence (optional sanity)
uint32_t expectedSeq = 0;

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

// Priority: 1) ERROR 2) FALL 3) TIMEOUT 4) OK
void oledUpdate() {
  if (!wifiOk) oledShow("ERROR");
  else if (fallActive) oledShow("FALL DETECTED");
  else if (timeoutActive) oledShow("TIMEOUT");
  else oledShow("OK");
}

// ---------------- Indicators (Base) ----------------

// OK: Green LED blink 0.5 sec every 2 sec (NON-BLOCKING)
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

// Red blink (.5s periodic) if WiFi/server issue or timeouts
void redBlinkIfDue() {
  if (millis() - lastRedBlink >= 500) {
    lastRedBlink = millis();
    digitalWrite(RED_LED, !digitalRead(RED_LED));
  }
}

// Rapid beeps (1 sec periodic) on timeout or WiFi fail
void rapidBeepIfDue() {
  if (millis() - lastRapidBeep >= 1000) {
    lastRapidBeep = millis();
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// Per line saved: green blink + short beep
void packetSavedPulse() {
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(80);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// Sync successful: solid green 2s + solid beep 2s
void syncSuccessSignal() {
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(2000);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// ---------------- Utility ----------------
bool resetPressed() { return digitalRead(RESET_BTN) == LOW; }

// ---------------- HTTP Handlers ----------------
void handleReady() {
  // Called by Logger after WiFi connect
  inSync = true;
  timeoutActive = false;
  expectedSeq = 0;
  rxBuffer = "";
  server.send(200, "text/plain", "OK");
}

void handleUpload() {
  // Body is raw chunk text; query parameter "seq" is chunk index
  if (!server.hasArg("seq")) {
    server.send(400, "text/plain", "Missing seq");
    return;
  }

  uint32_t seq = (uint32_t) server.arg("seq").toInt();
  String payload = server.arg("plain"); // POST body

  // Minimal sanity check (optional): accept out-of-order but mark timeout
  if (seq != expectedSeq) {
    // Not fatal; but helpful to detect duplicates/out-of-order
    // We'll still process to avoid data loss, but we warn.
    Serial.printf("WARN: expected seq %lu, got %lu\n", (unsigned long)expectedSeq, (unsigned long)seq);
    // If it's a duplicate older packet, you may want to ignore; for now we process.
  }

  expectedSeq = seq + 1;
  timeoutActive = false;

  rxBuffer += payload;

  // Print complete lines to Serial (same behavior as LoRa version)
  int nl;
  while ((nl = rxBuffer.indexOf('\n')) >= 0) {
    String line = rxBuffer.substring(0, nl);
    rxBuffer.remove(0, nl + 1);
    if (line.length() > 0) {
      Serial.println(csvToJson(line));
      packetSavedPulse();
    }
  }

  server.send(200, "text/plain", "OK:" + String(seq));
}

void handleDone() {
  // End of file confirmation from Logger
  // If last chunk didn't end with newline, flush remainder
  if (rxBuffer.length() > 0) {
    Serial.println(rxBuffer);
    rxBuffer = "";
    packetSavedPulse();
  }

  syncSuccessSignal();
  inSync = false;
  timeoutActive = false;
  server.send(200, "text/plain", "DONE");
}

void handleFall() {
  fallActive = true;
  inSync = false;
  server.send(200, "text/plain", "FALL");
}

void handleReset() {
  fallActive = false;
  timeoutActive = false;
  inSync = false;
  expectedSeq = 0;
  rxBuffer = "";
  server.send(200, "text/plain", "RESET");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) Serial.println("OLED NOT FOUND");
  else {
    Serial.println("OLED OK");
    display.clearDisplay();
    display.display();
  }

  // Start SoftAP
  WiFi.mode(WIFI_AP);
  wifiOk = WiFi.softAP(AP_SSID, AP_PASS);
  if (!wifiOk) {
    Serial.println("SoftAP START FAILED");
  } else {
    IPAddress ip = WiFi.softAPIP();
    Serial.print("SoftAP OK. SSID=");
    Serial.print(AP_SSID);
    Serial.print("  IP=");
    Serial.println(ip);
  }

  // Routes
  server.on("/ready", HTTP_GET, handleReady);
  server.on("/upload", HTTP_POST, handleUpload);
  server.on("/done", HTTP_POST, handleDone);
  server.on("/fall", HTTP_POST, handleFall);
  server.on("/reset", HTTP_POST, handleReset);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started on port 80");

  oledUpdate();
}

// ---------------- Loop ----------------
void loop() {
  // Local reset button: clears fall/timeout (and allows you to recover without Logger)
  if (resetPressed()) {
    fallActive = false;
    timeoutActive = false;
    inSync = false;
    expectedSeq = 0;
    rxBuffer = "";

    okBlinkOn = false;
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(RED_LED, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    oledUpdate();
    Serial.println("Reset pressed (Base) -> cleared flags");
    delay(300);
  }

  // If WiFi init failed, show ERROR and blink red
  if (!wifiOk) {
    okBlinkOn = false;
    digitalWrite(GREEN_LED, LOW);
    oledUpdate();
    redBlinkIfDue();
    rapidBeepIfDue();
    delay(20);
    return;
  }

  // FALL overrides everything
  if (fallActive) {
    okBlinkOn = false;
    digitalWrite(GREEN_LED, LOW);
    oledUpdate();
    redBlinkIfDue();
    rapidBeepIfDue();
  } else {
    // Normal OK: non-blocking green blink
    okPulseIfDue();
    oledUpdate();
  }

  // Handle HTTP requests
  server.handleClient();

  delay(5);
}
