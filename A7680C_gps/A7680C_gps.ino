/*
  A7680C GPS Tracker — Minimal Approach
  ======================================
  Based on jaymar921's working method:
  - Just AT+CGNSSPWR=1 to power on GNSS
  - Poll AT+CGPSINFO for coordinates
  - Poll AT+CGNSSINFO for satellite count
  - NO NMEA streaming, NO port switching, NO cold/warm start

  + DFPlayer MP3, MPU-6050 fall detection, LBS fallback, SMS
*/

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
//            MP3 FILES (on SD card)
//
//  001.mp3  →  "Internet connected"
//  002.mp3  →  "Location ready"
//  003.mp3  →  "Fall detected! Sending alert!"
//  004.mp3  →  "Tracker online"
//  005.mp3  →  "SMS sent"
//  006.mp3  →  "Warning: No internet"
//  007.mp3  →  "Warning: Location unavailable"
//  008.mp3  →  "GPS fix acquired"
//  009.mp3  →  "Searching for satellites"
// ═══════════════════════════════════════════════════════════════

#define SOUND_INTERNET_OK 1
#define SOUND_LOCATION_OK 2
#define SOUND_FALL_DETECTED 3
#define SOUND_TRACKER_ONLINE 4
#define SOUND_SMS_SENT 5
#define SOUND_NO_INTERNET 6
#define SOUND_NO_LOCATION 7
#define SOUND_GPS_FIX 8
#define SOUND_SEARCHING_SATS 9

#define FALL_THRESHOLD 2.5
#define FALL_COOLDOWN 30000
#define POLL_INTERVAL 5000 // Poll GPS every 5 seconds
#define SMS_INTERVAL 60000 // Send SMS every 1 minute

// ═══════════════════════════════════════════════════════════════
//                     GLOBALS
// ═══════════════════════════════════════════════════════════════

HardwareSerial SerialAT(2);  // UART2 — default pins ARE 16,17 (modem pins)
HardwareSerial mp3Serial(1); // UART1 — remapped to 26,27 (no conflict)
DFRobotDFPlayerMini dfPlayer;
Adafruit_MPU6050 mpu;

bool mpuReady = false;
bool dfPlayerReady = false;
bool smsSentBoot = false;
bool smsSentSats = false;
bool smsSentFix = false;
unsigned long lastFallTime = 0;
unsigned long lastPollTime = 0;
unsigned long lastSmsTime = 0;
int attempt = 0;
int fixCount = 0;
bool useCGPS =
    false; // true = AT+CGPS command set, false = AT+CGNSS command set

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

GpsData lastGps = {0, 0, 0, 0, 0, "", "", false}; // Cache last valid GPS fix

// ═══════════════════════════════════════════════════════════════
//                 AT COMMAND (with silence wait)
// ═══════════════════════════════════════════════════════════════

String sendAT(String cmd, unsigned long timeout = 5000) {
  // Flush any old data
  while (SerialAT.available())
    SerialAT.read();

  SerialAT.println(cmd);
  delay(500);

  String response = "";
  unsigned long lastByteTime = millis();
  unsigned long start = millis();

  while (millis() - start < timeout) {
    if (SerialAT.available()) {
      while (SerialAT.available()) {
        response += (char)SerialAT.read();
      }
      lastByteTime = millis();
    }
    // If we got data and 1.5 sec of silence, we're done
    if (response.length() > 0 && millis() - lastByteTime >= 1500)
      break;
    delay(10);
  }

  response.trim();
  Serial.println(">> " + cmd);
  Serial.println("<< " + response);
  return response;
}

// ═══════════════════════════════════════════════════════════════
//                     POWER ON MODULE
// ═══════════════════════════════════════════════════════════════

void powerOnModule() {
  Serial.println("Pulsing PWRKEY...");
  pinMode(PWRKEY_PIN, OUTPUT);
  digitalWrite(PWRKEY_PIN, HIGH);
  delay(100);
  digitalWrite(PWRKEY_PIN, LOW);
  delay(1500);
  digitalWrite(PWRKEY_PIN, HIGH);
  delay(5000);
}

// ═══════════════════════════════════════════════════════════════
//                     MP3 PLAYER
// ═══════════════════════════════════════════════════════════════

void initDFPlayer() {
  Serial.println("==== INIT DFPlayer ====");

  mp3Serial.end();
  delay(200);

  // Important: start UART first
  mp3Serial.begin(9600, SERIAL_8N1, MP3_RX, MP3_TX);
  delay(3000); // DFPlayer needs longer boot time

  dfPlayerReady = false;

  for (int i = 0; i < 5; i++) {
    Serial.println("Trying DFPlayer... attempt " + String(i + 1));

    if (dfPlayer.begin(mp3Serial, false,
                       false)) { // Disable ACK waiting to prevent hanging
      Serial.println("DFPlayer detected!");

      // Longer wait for SD card initialization
      Serial.println("Waiting for SD card...");
      delay(2000);

      // Initialize DFPlayer properly
      dfPlayer.reset();
      delay(1500);

      // Configure DFPlayer
      dfPlayer.volume(25);
      delay(200);
      dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
      delay(200);
      dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
      delay(1000); // Wait longer for SD to be ready

      // Check if SD card is ready
      int fileCount = dfPlayer.readFileCounts();
      Serial.println("Files on SD: " + String(fileCount));

      if (fileCount <= 0) {
        Serial.println(">> SD card not ready or empty, retrying...");
        delay(2000);
        fileCount = dfPlayer.readFileCounts();
        Serial.println("Files on SD (retry): " + String(fileCount));
      }

      dfPlayerReady = true;

      Serial.println("Playing test sound...");

      // Set volume and play
      dfPlayer.volume(25);
      delay(200);
      dfPlayer.play(4); // play 004.mp3 - must be named "004.mp3" on SD root

      delay(4000); // Wait for sound to finish

      return;
    }

    delay(1000);
  }

  Serial.println("DFPlayer NOT detected - continuing without MP3!");
}

void playSound(int fileNum) {
  if (!dfPlayerReady)
    return;

  dfPlayer.play(fileNum);
  delay(100); // short delay only
}

// ═══════════════════════════════════════════════════════════════
//                     MPU-6050
// ═══════════════════════════════════════════════════════════════

void initMPU() {
  Serial.println("==== INIT MPU-6050 ====");
  Wire.begin(MPU_SDA, MPU_SCL);
  if (mpu.begin()) {
    Serial.println(">>> MPU-6050 ready!");
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    mpuReady = true;
  } else {
    Serial.println(">>> MPU-6050 not found");
    mpuReady = false;
  }
}

bool checkFallDetected() {
  if (!mpuReady)
    return false;
  if (millis() - lastFallTime < FALL_COOLDOWN)
    return false;
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float totalG = sqrt(a.acceleration.x * a.acceleration.x +
                      a.acceleration.y * a.acceleration.y +
                      a.acceleration.z * a.acceleration.z) /
                 9.81;
  if (totalG > FALL_THRESHOLD) {
    Serial.println("!!! FALL DETECTED! G=" + String(totalG, 2));
    lastFallTime = millis();
    return true;
  }
  return false;
}

// ═══════════════════════════════════════════════════════════════
//                         SMS
// ═══════════════════════════════════════════════════════════════

bool sendSMS(String number, String message) {
  Serial.println("---- SENDING SMS ----");
  Serial.println("To: " + number);
  Serial.println("Msg: " + message);

  String r = sendAT("AT+CMGF=1");
  if (r.indexOf("OK") == -1) {
    Serial.println(">>> SMS: text mode failed");
    return false;
  }

  SerialAT.println("AT+CMGS=\"" + number + "\"");
  delay(2000);

  SerialAT.write(message.c_str(), message.length());
  SerialAT.write(26); // Ctrl+Z

  String resp = "";
  unsigned long start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) {
      resp += (char)SerialAT.read();
    }
    if (resp.indexOf("+CMGS:") != -1)
      break;
    if (resp.indexOf("ERROR") != -1)
      break;
    delay(100);
  }

  if (resp.indexOf("+CMGS:") != -1) {
    Serial.println(">>> SMS sent OK!");
    playSound(SOUND_SMS_SENT);
    return true;
  }
  Serial.println(">>> SMS failed: " + resp);
  return false;
}

// ═══════════════════════════════════════════════════════════════
//              LBS (Cell Tower) — fallback
// ═══════════════════════════════════════════════════════════════

// LBS Modes for AT+CLBS:
// 1 = Current serving cell only (fastest, least accurate)
// 2 = Multiple cell tower triangulation (better accuracy)
// 3 = AGPS assisted location
// 4 = Auto mode (module decides)

// Set timezone to Manila (UTC+8)
void setManilaTimezone() {
  // AT+CCLK sets the real-time clock timezone
  // Format: AT+CCLK="yy/MM/dd,hh:mm:ss±zz"
  // For Manila UTC+8: +08
  sendAT("AT+CTZU=1"); // Enable automatic timezone update
  delay(500);
  sendAT("AT+CTZR=1"); // Enable timezone reporting
  delay(500);
  Serial.println(">> Timezone set to Manila (UTC+8)");
}

// Get current time in Manila timezone
String getManilaTime() {
  String resp = sendAT("AT+CCLK?");
  int idx = resp.indexOf("+CCLK: \"");
  if (idx >= 0) {
    String timeStr = resp.substring(idx + 8);
    int endIdx = timeStr.indexOf('"');
    if (endIdx > 0) {
      timeStr = timeStr.substring(0, endIdx);
      return timeStr;
    }
  }
  return "";
}

String getLBSLocation(int mode) {
  String cmd = "AT+CLBS=" + String(mode) + ",1"; // cid=1 is fine
  String resp = sendAT(cmd, 15000);
  int idx = resp.indexOf("+CLBS:");
  if (idx >= 0) {
    String data = resp.substring(idx + 7);
    data.trim();
    int c1 = data.indexOf(',');
    String statusCode = data.substring(0, c1);
    statusCode.trim();
    if (c1 > 0 && statusCode == "0") {
      int c2 = data.indexOf(',', c1 + 1);
      int c3 = data.indexOf(',', c2 + 1);
      if (c2 > c1 && c3 > c2) {
        String lat = data.substring(c1 + 1, c2);
        String lon = data.substring(c2 + 1, c3);
        lat.trim();
        lon.trim();
        return lat + "," + lon;
      }
    }
  }
  return "";
}

// Legacy wrapper for backward compatibility
String getLBSLocation() {
  return getLBSLocation(4); // Use auto mode (4) as default
}

// ═══════════════════════════════════════════════════════════════
//    GET WIFI LOCATION — Using WiFi AP scanning for geolocation
//
//  Some A76XX modules support WiFi geolocation via AT+CWLOC
//  This provides better indoor accuracy than cell towers alone.
// ═══════════════════════════════════════════════════════════════

String getWiFiLocation() {
  Serial.println(">> Trying WiFi geolocation...");

  // Enable WiFi if not already enabled
  sendAT("AT+CWLOC=1", 5000);

  // Get WiFi location
  String resp = sendAT("AT+CWLOC=2", 15000);

  // Parse response: +CWLOC: <status>,<lat>,<lon>,<accuracy>
  int idx = resp.indexOf("+CWLOC:");
  if (idx >= 0) {
    String data = resp.substring(idx + 7);
    data.trim();

    String fields[4];
    int fieldCount = 0, prev = 0;
    for (int i = 0; i <= (int)data.length() && fieldCount < 4; i++) {
      if (i == (int)data.length() || data[i] == ',') {
        fields[fieldCount++] = data.substring(prev, i);
        fields[fieldCount - 1].trim();
        prev = i + 1;
      }
    }

    // status 0 = success
    if (fieldCount >= 3 && fields[0] == "0") {
      String lat = fields[1];
      String lon = fields[2];
      lat.trim();
      lon.trim();
      Serial.println(">> WiFi location: " + lat + "," + lon);
      return lat + "," + lon;
    }
  }

  Serial.println(">> WiFi location failed");
  return "";
}

// ═══════════════════════════════════════════════════════════════
//    GET VALIDATED LBS LOCATION — Multiple readings for accuracy
//
//  Takes multiple LBS readings and validates they are consistent.
//  Returns coordinates only if readings are stable.
// ═══════════════════════════════════════════════════════════════

String getValidatedLBSLocation(int numReadings, int delayMs) {
  Serial.println(">> Validating LBS location (" + String(numReadings) +
                 " readings)...");

  // Try different LBS modes for best accuracy
  // Mode 2 = Multiple cell tower triangulation (best balance)
  // Mode 3 = AGPS assisted (if available)
  // Mode 4 = Auto mode
  int modes[] = {2, 3, 4};
  String bestResult = "";
  int bestValidCount = 0;

  for (int modeIdx = 0; modeIdx < 3; modeIdx++) {
    int mode = modes[modeIdx];
    Serial.println(">> Trying LBS mode " + String(mode) + "...");

    float latSum = 0, lonSum = 0;
    int validCount = 0;
    String firstReading = "";
    float firstLat = 0, firstLon = 0;

    for (int i = 0; i < numReadings; i++) {
      String lbs = getLBSLocation(mode);
      // Validate that response contains coordinates (has decimal point)
      if (lbs.length() > 0 && lbs.indexOf(".") > 0) {
        // Parse lat/lon from "lat,lon"
        int commaIdx = lbs.indexOf(',');
        if (commaIdx > 0) {
          float lat = lbs.substring(0, commaIdx).toFloat();
          float lon = lbs.substring(commaIdx + 1).toFloat();
          // Validate coordinates are non-zero
          if (lat == 0.0 || lon == 0.0) {
            Serial.println(">> LBS mode " + String(mode) + " reading " +
                           String(i + 1) +
                           " has zero coordinates, skipping...");
            continue;
          }

          if (validCount == 0) {
            firstReading = lbs;
            firstLat = lat;
            firstLon = lon;
          } else {
            // Check if this reading is close to the first one (within ~100m)
            float latDiff = abs(lat - firstLat);
            float lonDiff = abs(lon - firstLon);

            // Rough conversion: 0.001 degrees ≈ 100m
            if (latDiff > 0.001 || lonDiff > 0.001) {
              Serial.println(">> LBS reading " + String(i + 1) +
                             " inconsistent, skipping...");
              continue;
            }
          }

          latSum += lat;
          lonSum += lon;
          validCount++;
          Serial.println(">> LBS mode " + String(mode) + " reading " +
                         String(i + 1) + ": " + lbs);
        }
      } else {
        if (lbs.length() > 0) {
          Serial.println(">> LBS mode " + String(mode) + " reading " +
                         String(i + 1) + " invalid (not coordinates): " + lbs);
        } else {
          Serial.println(">> LBS mode " + String(mode) + " reading " +
                         String(i + 1) + " failed");
        }
      }

      if (i < numReadings - 1) {
        delay(delayMs);
      }
    }

    // Keep the best result (most valid readings)
    if (validCount > bestValidCount) {
      bestValidCount = validCount;
      if (validCount >= numReadings / 2) {
        float avgLat = latSum / validCount;
        float avgLon = lonSum / validCount;
        bestResult = String(avgLat, 6) + "," + String(avgLon, 6);
        Serial.println(">> Mode " + String(mode) + " result: " + bestResult +
                       " (" + String(validCount) + " valid)");
      }
    }

    // If we got good results with mode 2, use it
    if (mode == 2 && validCount >= numReadings / 2) {
      break;
    }
  }

  if (bestResult.length() > 0 && bestValidCount >= numReadings / 2) {
    Serial.println(">> Validated LBS: " + bestResult + " (" +
                   String(bestValidCount) + " valid readings)");
    return bestResult;
  }

  Serial.println(">> LBS validation failed, best had only " +
                 String(bestValidCount) + " valid readings");
  return "";
}

// ═══════════════════════════════════════════════════════════════
//    GET LOCATION — PRIORITIZE LBS (Cell Tower) FIRST
//
//  Tries LBS first for fast cell tower location.
//  If LBS fails, falls back to GPS.
//  Returns: "lat,lon" string + sets source to "LBS" or "GPS"
//  Returns empty string if nothing available.
// ═══════════════════════════════════════════════════════════════

String getLocationLBSFirst(String &source) {
  Serial.println(">> Getting FRESH location (GPS 5s -> LBS fresh)...");

  // ── GPS first (your existing 5s poll) ──
  unsigned long gpsStartTime = millis();
  while (millis() - gpsStartTime < 5000) {
    String raw = sendAT("AT+CGPSINFO");
    GpsData gps = parseCgpsInfo(raw);
    if (gps.valid) {
      source = "GPS";
      return String(gps.latitude, 6) + "," + String(gps.longitude, 6);
    }
    delay(1000);
  }

  Serial.println(">> GPS no fix → forcing FRESH LBS...");

  // ── FORCE FRESH NETWORK (this is the key fix) ──
  Serial.println(">> Full radio reset + re-registration for fresh cell...");
  sendAT("AT+CFUN=0", 8000); // radio off
  delay(6000);
  sendAT("AT+CFUN=1", 15000); // radio on
  delay(8000);

  // Wait up to 25s for proper LTE registration (critical!)
  bool registered = false;
  unsigned long regStart = millis();
  while (millis() - regStart < 25000) {
    String resp = sendAT("AT+CEREG?");
    if (resp.indexOf(",1") != -1 ||
        resp.indexOf(",5") != -1) { // registered/home or roaming
      registered = true;
      break;
    }
    delay(2000);
  }
  if (!registered)
    Serial.println(">> Warning: registration timeout, trying LBS anyway");

  // Optional but recommended: activate PDP context (LBS often needs data
  // bearer)
  sendAT("AT+CGDCONT=1,\"IP\",\"internet\""); // Philippines common APN
                                              // (Globe/Smart/DITO). Change if
                                              // needed
  sendAT("AT+CGACT=1,1", 10000);

  // ── Now take 4 LBS readings with type=4 (gives timestamp too) ──
  String lastLbs = "";
  String prevLbs = "";
  for (int i = 0; i < 4; i++) {
    String lbs = getLBSLocation(4); // type=4 → lat,lon,acc,date,time
    Serial.println(">> LBS #" + String(i + 1) + ": " + lbs);

    if (lbs.length() > 0 && lbs.indexOf(".") > 0) {
      if (prevLbs.length() > 0 && lbs != prevLbs) {
        source = "LBS (fresh)";
        Serial.println(">> FRESH LBS detected (coordinates changed)!");
        return lbs.substring(
            0, lbs.indexOf(',', lbs.indexOf(',') + 1)); // return just lat,lon
      }
      prevLbs = lastLbs;
      lastLbs = lbs;
    }
    delay(2500);
  }

  if (lastLbs.length() > 0) {
    source = "LBS";
    return lastLbs.substring(0, lastLbs.indexOf(',', lastLbs.indexOf(',') + 1));
  }

  source = "none";
  return "";
}

// ═══════════════════════════════════════════════════════════════
//    GET BEST LOCATION — wait for fresh GPS, fallback to LBS
//
//  Polls GPS multiple times to get a fresh, accurate fix.
//  If GPS fails after maxWaitSec, falls back to LBS.
//  Returns: "lat,lon" string + sets source to "GPS" or "LBS"
//  Returns empty string if nothing available.
// ═══════════════════════════════════════════════════════════════

String getBestLocation(int maxWaitSec, String &source) {
  Serial.println(">> Getting best location (max " + String(maxWaitSec) +
                 "s)...");

  // ── Try GPS: poll every 3 seconds for up to maxWaitSec ──
  unsigned long startTime = millis();
  unsigned long maxWaitMs = (unsigned long)maxWaitSec * 1000UL;
  int polls = 0;

  while (millis() - startTime < maxWaitMs) {
    polls++;
    String raw = sendAT("AT+CGPSINFO");
    GpsData gps = parseCgpsInfo(raw);

    if (gps.valid) {
      // Got a valid fix — wait for a second reading to confirm it's fresh
      Serial.println(">> GPS reading #" + String(polls) +
                     " valid, confirming...");
      delay(2000);

      // Second poll to confirm consistency
      raw = sendAT("AT+CGPSINFO");
      GpsData gps2 = parseCgpsInfo(raw);

      if (gps2.valid) {
        lastGps = gps2; // Cache the freshest reading
        source = "GPS";
        String coords =
            String(gps2.latitude, 6) + "," + String(gps2.longitude, 6);
        Serial.println(">> Fresh GPS fix confirmed: " + coords);
        return coords;
      }
    }

    Serial.println(">> GPS poll #" + String(polls) + " — no fix yet");
    delay(3000); // Wait 3 seconds before next poll
  }

  Serial.println(">> GPS timed out after " + String(maxWaitSec) +
                 "s, trying LBS...");

  // ── Fallback: LBS (cell tower) ──
  String lbs = getLBSLocation();
  if (lbs.length() > 0) {
    source = "LBS";
    Serial.println(">> LBS location: " + lbs);
    return lbs;
  }

  // ── Last resort: cached GPS if we ever had a fix ──
  if (lastGps.valid) {
    source = "GPS (cached)";
    String coords =
        String(lastGps.latitude, 6) + "," + String(lastGps.longitude, 6);
    Serial.println(">> Using cached GPS: " + coords);
    return coords;
  }

  source = "none";
  Serial.println(">> No location available!");
  return "";
}

// ═══════════════════════════════════════════════════════════════
//         PARSE AT+CGPSINFO (the simple way!)
//
//  Response: +CGPSINFO:lat,N/S,lon,E/W,date,time,alt,speed,course
//  Example:
//  +CGPSINFO:1015.471638,N,12344.234064,E,170226,091318.00,94.5,0.77,349.7 No
//  fix:   +CGPSINFO:,,,,,,,,
// ═══════════════════════════════════════════════════════════════

GpsData parseCgpsInfo(String raw) {
  GpsData data = {0, 0, 0, 0, 0, "", "", false};

  int start = raw.indexOf("+CGPSINFO:");
  if (start == -1)
    return data;

  String payload = raw.substring(start + 10);
  int lineEnd = payload.indexOf('\n');
  if (lineEnd != -1)
    payload = payload.substring(0, lineEnd);
  payload.trim();

  // Split into 9 fields
  String fields[9];
  int fieldCount = 0, prev = 0;
  for (int i = 0; i <= (int)payload.length() && fieldCount < 9; i++) {
    if (i == (int)payload.length() || payload[i] == ',') {
      fields[fieldCount++] = payload.substring(prev, i);
      fields[fieldCount - 1].trim();
      prev = i + 1;
    }
  }

  // Need all 9 fields and lat must not be empty
  if (fieldCount < 9 || fields[0].length() == 0)
    return data;

  // Convert ddmm.mmmmmm → decimal degrees
  float rawLat = fields[0].toFloat();
  int latDeg = (int)(rawLat / 100);
  float lat = latDeg + (rawLat - latDeg * 100) / 60.0;
  if (fields[1] == "S")
    lat = -lat;

  float rawLon = fields[2].toFloat();
  int lonDeg = (int)(rawLon / 100);
  float lon = lonDeg + (rawLon - lonDeg * 100) / 60.0;
  if (fields[3] == "W")
    lon = -lon;

  data.latitude = lat;
  data.longitude = lon;
  data.date = fields[4];
  data.utcTime = fields[5];
  data.altitude = fields[6].toFloat();
  data.speedKnots = fields[7].toFloat();
  data.course = fields[8].toFloat();
  data.valid = true;

  return data;
}

// ═══════════════════════════════════════════════════════════════
//         GET SATELLITE COUNT from AT+CGNSSINFO
// ═══════════════════════════════════════════════════════════════

int getSatelliteCount() {
  String raw;

  if (useCGPS) {
    raw = sendAT("AT+CGPSSTATUS?");
  } else {
    raw = sendAT("AT+CGNSSINFO");
  }

  int idx = raw.indexOf("+CGNSSINFO:");
  if (idx != -1) {
    String payload = raw.substring(idx + 11);
    payload.trim();

    String fields[6];
    int fieldCount = 0, prev = 0;

    for (int i = 0; i <= (int)payload.length() && fieldCount < 6; i++) {
      if (i == payload.length() || payload[i] == ',') {
        fields[fieldCount++] = payload.substring(prev, i);
        fields[fieldCount - 1].trim();
        prev = i + 1;
      }
    }

    if (fieldCount >= 2 && fields[1].length() > 0) {
      return fields[1].toInt();
    }
  }

  return 0;
}

// ═══════════════════════════════════════════════════════════════
//                         SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("================================");
  Serial.println("  A7680C GPS Tracker (Minimal)");
  Serial.println("================================");

  // Start modem serial FIRST — UART2 claims its default pins 16,17
  SerialAT.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
  powerOnModule();

  // Basic modem init — nothing fancy
  sendAT("AT");
  sendAT("ATE0");
  sendAT("AT+CMEE=2");

  // Set timezone to Manila (UTC+8)
  setManilaTimezone();

  // Now init DFPlayer — UART1 remaps to pins 26,27 (no conflict)
  initDFPlayer();
  initMPU();

  // ═══════════════════════════════════════════════════════
  //  GNSS — JUST power it on. That's it!
  //  No port switching, no NMEA config, no cold start.
  // ═══════════════════════════════════════════════════════

  Serial.println("==== GNSS INIT ====");

  // Try CGNSS first
  String resp = sendAT("AT+CGNSSPWR=1", 10000);

  if (resp.indexOf("OK") != -1) {
    Serial.println("Using CGNSS command set");
    useCGPS = false;
  } else {
    Serial.println("CGNSS failed, trying CGPS...");
    resp = sendAT("AT+CGPS=1", 10000);
    if (resp.indexOf("OK") != -1) {
      Serial.println("Using CGPS command set");
      useCGPS = true;
    } else {
      Serial.println("GNSS FAILED TO START!");
    }
  }

  // Verify power state
  if (!useCGPS) {
    sendAT("AT+CGNSSSTATUS?");
  }

  playSound(SOUND_SEARCHING_SATS);

  // SMS 1: boot notification with LBS-first location (fast)
  String locSource;
  String bootCoords = getLocationLBSFirst(locSource);

  String bootMsg;
  if (bootCoords.length() > 0) {
    bootMsg = bootCoords;
    playSound(SOUND_LOCATION_OK); // 002.mp3
  } else {
    bootMsg = "No location";
    playSound(SOUND_NO_LOCATION); // 007.mp3
  }
  Serial.println(">> Sending boot SMS: " + bootMsg);
  bool bootSmsSent = sendSMS(PHONE_NUMBER, bootMsg);
  Serial.println(bootSmsSent ? ">> Boot SMS sent" : ">> Boot SMS failed");
  smsSentBoot = true;

  Serial.println("\n========================================");
  Serial.println("  Sending location every 1 minute");
  Serial.println("  LBS prioritized for fast location");
  Serial.println("========================================\n");
}

// ═══════════════════════════════════════════════════════════════
//                         LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  // ── Fall detection (fast, every 50ms) ──
  if (checkFallDetected()) {
    playSound(SOUND_FALL_DETECTED); // 003.mp3
    delay(2000);

    // Get location for fall alert — LBS first for speed
    String fallSource;
    String fallCoords = getLocationLBSFirst(fallSource);

    String fallMsg;
    if (fallCoords.length() > 0) {
      fallMsg = "FALL:" + fallCoords;
    } else {
      fallMsg = "FALL:No location";
    }
    Serial.println(">> Sending fall SMS: " + fallMsg);
    bool fallSmsSent = sendSMS(PHONE_NUMBER, fallMsg);
    Serial.println(fallSmsSent ? ">> Fall SMS sent" : ">> Fall SMS failed");
  }

  // ── Send location SMS every 1 minute (LBS prioritized) ──
  if (millis() - lastSmsTime >= SMS_INTERVAL) {
    lastSmsTime = millis();

    // Get location with LBS first (fast cell tower location)
    String locSource;
    String coords = getLocationLBSFirst(locSource);

    if (coords.length() > 0) {
      // Get Manila time
      String manilaTime = getManilaTime();

      // Send first SMS: coordinates + time
      String msg1 = coords;
      if (manilaTime.length() > 0) {
        msg1 += " " + manilaTime;
      }
      Serial.println(">> Sending SMS 1: " + msg1);
      bool sms1Success = sendSMS(PHONE_NUMBER, msg1);

      if (sms1Success) {
        Serial.println("─── SMS 1 SENT ───");
        Serial.println("Msg: " + msg1);
      } else {
        Serial.println(">> SMS 1 FAILED!");
      }

      // Wait longer before sending second SMS (modem needs time)
      Serial.println(">> Waiting 5 seconds before SMS 2...");
      delay(5000);

      // Send second SMS: Google Maps link
      String msg2 = "https://maps.google.com/?q=" + coords;
      Serial.println(">> Sending SMS 2: " + msg2);
      bool sms2Success = sendSMS(PHONE_NUMBER, msg2);

      if (sms2Success) {
        Serial.println("─── SMS 2 SENT ───");
        Serial.println("Msg: " + msg2);
        Serial.println("─────────────────");
      } else {
        Serial.println(">> SMS 2 FAILED!");
      }
    } else {
      Serial.println(">> No location available for SMS");
    }
  }

  // ── Poll GPS every 5 seconds (for display/logging only) ──
  if (millis() - lastPollTime >= POLL_INTERVAL) {
    lastPollTime = millis();
    attempt++;

    // Poll AT+CGPSINFO
    String raw = sendAT("AT+CGPSINFO");
    GpsData gps = parseCgpsInfo(raw);

    if (gps.valid) {
      lastGps = gps; // Always cache the latest valid fix
      fixCount++;

      Serial.println("────────────────────────────────");
      Serial.println(">>> GPS FIX #" + String(fixCount));
      Serial.println("  Lat:   " + String(gps.latitude, 6));
      Serial.println("  Lon:   " + String(gps.longitude, 6));
      Serial.println("  Alt:   " + String(gps.altitude, 1) + " m");
      Serial.println("  Speed: " + String(gps.speedKnots, 2) + " knots");
      Serial.println("  Maps:  https://maps.google.com/?q=" +
                     String(gps.latitude, 6) + "," + String(gps.longitude, 6));
      Serial.println("────────────────────────────────");

    } else {
      // No fix yet — show satellite progress
      int sats = getSatelliteCount();

      Serial.print("[" + String(attempt) + "] Waiting for fix... ");
      if (sats >= 0) {
        Serial.print("Satellites: " + String(sats) + "  [");
        for (int i = 0; i < 12; i++)
          Serial.print(i < sats ? "#" : "-");
        Serial.print("]");
      } else {
        Serial.print("(querying...)");
      }
      Serial.println();
    }
  }

  delay(50); // Fast loop for fall detection
}