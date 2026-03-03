/*
  A7680x GPS Tracker — PURE GNSS (GPS + BDS + GLONASS) + Auto Restart
  ===================================================================
  GNSS_START_MODE (boot):
    0 = Normal | 1 = Warm | 2 = Cold | 3 = Hot

  AUTO-RESTART (new):
    If no GNSS fix for AUTO_RESTART_TIMEOUT minutes → automatically restarts
    AUTO_RESTART_TYPE: 1=Warm (recommended) | 2=Cold | 3=Hot
    Max 5 restarts to prevent spam
*/

#define GNSS_START_MODE          1     // Boot mode (1 = Warm recommended)
#define AUTO_RESTART_TIMEOUT     12    // Minutes without fix → auto restart (0 = disable)
#define AUTO_RESTART_TYPE        1     // 1=Warm, 2=Cold, 3=Hot

#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>
#include <Wire.h>

// ═══════════════════════════════════════════════════════════════
//                     PIN DEFINITIONS
// ═══════════════════════════════════════════════════════════════

#define MODEM_TX 16
#define MODEM_RX 17
#define PWRKEY_PIN 5

#define MP3_RX 26
#define MP3_TX 27

#define MPU_SDA 21
#define MPU_SCL 22

#define PHONE_NUMBER "+639816325910"

// ═══════════════════════════════════════════════════════════════
//                     MP3 SOUND FILES
// ═══════════════════════════════════════════════════════════════

#define SOUND_FALL_DETECTED 3
#define SOUND_SMS_SENT 5
#define SOUND_GPS_FIX 8
#define SOUND_SEARCHING_SATS 9

#define FALL_THRESHOLD 2.5
#define FALL_COOLDOWN 30000
#define GPS_WAIT_MAX 60000
#define POLL_INTERVAL 5000
#define SMS_INTERVAL 60000

// ═══════════════════════════════════════════════════════════════
//                     GLOBALS
// ═══════════════════════════════════════════════════════════════

HardwareSerial SerialAT(2);
HardwareSerial mp3Serial(1);
DFRobotDFPlayerMini dfPlayer;
Adafruit_MPU6050 mpu;

bool mpuReady = false;
bool dfPlayerReady = false;
unsigned long lastFallTime = 0;
unsigned long lastPollTime = 0;
unsigned long lastSmsTime = 0;
unsigned long lastValidFixTime = 0;     // ← Auto-restart tracking
int restartCount = 0;

struct GpsData {
  float latitude;
  float longitude;
  float altitude;
  float speedKnots;
  float course;
  String date;
  String utcTime;
  bool valid;
};

GpsData lastGps = {0, 0, 0, 0, 0, "", "", false};

// ═══════════════════════════════════════════════════════════════
//                 AT COMMAND HELPER
// ═══════════════════════════════════════════════════════════════

String sendAT(String cmd, unsigned long timeout = 5000) {
  while (SerialAT.available()) SerialAT.read();
  SerialAT.println(cmd);
  delay(300);

  String response = "";
  unsigned long start = millis(), lastByte = millis();
  while (millis() - start < timeout) {
    if (SerialAT.available()) {
      response += (char)SerialAT.read();
      lastByte = millis();
    }
    if (response.length() > 0 && millis() - lastByte > 800) break;
    delay(10);
  }
  response.trim();
  Serial.println(">> " + cmd);
  Serial.println("<< " + response);
  return response;
}

// ═══════════════════════════════════════════════════════════════
//                 GNSS INIT + BOOT START MODE
// ═══════════════════════════════════════════════════════════════

void initGNSS() {
  Serial.println("==== GNSS INIT (A7680 ASR5311) ====");
  sendAT("AT+CGNSSPWR=1", 10000);

  // Wait for READY!
  unsigned long start = millis();
  while (millis() - start < 15000) {
    if (sendAT("AT+CGNSSPWR?").indexOf("READY!") != -1) break;
    delay(1000);
  }

  // Boot start mode
  switch (GNSS_START_MODE) {
    case 1: sendAT("AT+CGPSWARM"); Serial.println("Boot: Warm Start"); break;
    case 2: sendAT("AT+CGPSCOLD"); Serial.println("Boot: Cold Start"); break;
    case 3: sendAT("AT+CGPSHOT");  Serial.println("Boot: Hot Start"); break;
    default: Serial.println("Boot: Normal Start"); break;
  }

  sendAT("AT+CGNSSMODE=7");
  Serial.println(">>> GNSS Mode: GPS + BDS + GLONASS");
  playSound(SOUND_SEARCHING_SATS);
}

// ═══════════════════════════════════════════════════════════════
//                 MANUAL & AUTO RESTART FUNCTIONS
// ═══════════════════════════════════════════════════════════════

void warmStartGNSS() { Serial.println(">>> Warm Start");  sendAT("AT+CGPSWARM", 5000); }
void coldStartGNSS() { Serial.println(">>> Cold Start"); sendAT("AT+CGPSCOLD", 9000); }
void hotStartGNSS()  { Serial.println(">>> Hot Start");  sendAT("AT+CGPSHOT",  3000); }

void autoRestartGNSS() {
  restartCount++;
  Serial.printf(">>> AUTO-RESTART #%d (no fix for %d min)\n", restartCount, AUTO_RESTART_TIMEOUT);

  switch (AUTO_RESTART_TYPE) {
    case 1: warmStartGNSS(); break;
    case 2: coldStartGNSS(); break;
    case 3: hotStartGNSS();  break;
  }

  lastValidFixTime = millis();   // reset timer
}

// ═══════════════════════════════════════════════════════════════
//                 PARSE + SAT COUNT (unchanged)
// ═══════════════════════════════════════════════════════════════

GpsData parseGNSS(String raw) { /* same as previous version */ }
int getSatelliteCount() { /* same as previous version */ }

// ═══════════════════════════════════════════════════════════════
//                 GET GNSS LOCATION
// ═══════════════════════════════════════════════════════════════

String getGNSSLocation(String &source) {
  unsigned long start = millis();
  while (millis() - start < GPS_WAIT_MAX) {
    String raw = sendAT("AT+CGNSSINFO");
    GpsData gps = parseGNSS(raw);

    if (gps.valid) {
      lastGps = gps;
      lastValidFixTime = millis();   // <<< critical for auto-restart
      restartCount = 0;
      source = "GNSS";
      Serial.println(">>> GNSS FIX ACQUIRED!");
      playSound(SOUND_GPS_FIX);
      return String(gps.latitude, 6) + "," + String(gps.longitude, 6);
    }
    delay(2000);
  }
  source = "none";
  return "";
}

// ═══════════════════════════════════════════════════════════════
//                 SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);

  SerialAT.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
  powerOnModule();

  sendAT("AT");
  sendAT("ATE0");
  sendAT("AT+CMEE=2");

  initDFPlayer();
  initMPU();
  initGNSS();

  Serial.println("\n>>> Auto-Restart enabled (" + String(AUTO_RESTART_TIMEOUT) + " min timeout, type " + String(AUTO_RESTART_TYPE) + ") <<<\n");

  // Boot location SMS
  String src;
  String loc = getGNSSLocation(src);
  sendSMS(PHONE_NUMBER, loc.length() ? loc : "Waiting for GNSS fix");
}

// ═══════════════════════════════════════════════════════════════
//                 LOOP + AUTO RESTART CHECK
// ═══════════════════════════════════════════════════════════════

void loop() {
  // Fall detection
  if (checkFallDetected()) {
    playSound(SOUND_FALL_DETECTED);
    String src;
    String loc = getGNSSLocation(src);
    sendSMS(PHONE_NUMBER, "FALL:" + (loc.length() ? loc : "No GNSS fix"));
  }

  // Periodic location SMS
  if (millis() - lastSmsTime >= SMS_INTERVAL) {
    lastSmsTime = millis();
    String src;
    String loc = getGNSSLocation(src);
    if (loc.length() > 0) {
      sendSMS(PHONE_NUMBER, loc);
      delay(5000);
      sendSMS(PHONE_NUMBER, "https://maps.google.com/?q=" + loc);
    } else {
      sendSMS(PHONE_NUMBER, "No GNSS fix yet");
    }
  }

  // Live polling
  if (millis() - lastPollTime >= POLL_INTERVAL) {
    lastPollTime = millis();
    String raw = sendAT("AT+CGNSSINFO");
    GpsData gps = parseGNSS(raw);

    if (gps.valid) {
      Serial.println(">>> GNSS FIX → " + String(gps.latitude, 6) + "," + String(gps.longitude, 6));
    } else {
      Serial.println("Waiting... Satellites: " + String(getSatelliteCount()));
    }
  }

  // ==================== AUTO RESTART FEATURE ====================
  if (AUTO_RESTART_TIMEOUT > 0 && lastValidFixTime > 0 && restartCount < 5) {
    if (millis() - lastValidFixTime > (unsigned long)AUTO_RESTART_TIMEOUT * 60000UL) {
      autoRestartGNSS();
    }
  }

  delay(50);
}