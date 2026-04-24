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

#define MP3_RX 27
#define MP3_TX 26

#define MPU_SDA 21
#define MPU_SCL 22

// ── Recipients — SMS is sent to ALL numbers in this list ──
const char* PHONE_NUMBERS[] = {
  "+639563024122",  // Kyle
  "+639102026730",  // Rolf
  "+639305701573",  // Dave
  "+639817227215",  // Francis
  "+639617331211"   // Neil
};
const int PHONE_COUNT = sizeof(PHONE_NUMBERS) / sizeof(PHONE_NUMBERS[0]);

#define BUZZER_PIN 4
#define TRIG_PIN 33
#define ECHO_PIN 34

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
//  010.mp3  →  "Obstacle ahead 1 meter"
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
#define SOUND_OBSTACLE_1METER 10  // 010.mp3 — "Obstacle ahead 1 meter"

#define FALL_THRESHOLD 2.5
#define FALL_COOLDOWN 30000
#define POLL_INTERVAL 5000  // Poll GPS every 5 seconds
#define SMS_INTERVAL  180000 // Send SMS every 3 minutes
#define HTTP_HEARTBEAT_INTERVAL 30000UL // Send HTTP heartbeat every 30s
// Disable GNSS caching — accept only fresh coordinates
#define GPS_CACHE_MAX_AGE_MS 0UL

// ═══════════════════════════════════════════════════════════════
//                     GLOBALS
// ═══════════════════════════════════════════════════════════════

String sendAT(String command, unsigned long timeout);
HardwareSerial SerialAT(2);  // UART2 — default pins ARE 16,17 (modem pins)
HardwareSerial mp3Serial(1); // UART1 — remapped to 26,27 (no conflict)
DFRobotDFPlayerMini dfPlayer;
Adafruit_MPU6050 mpu;
// ═══════════════════════════════════════════════════════════════
//                     LBS TOGGLE
// ═══════════════════════════════════════════════════════════════
bool useLBSFallback = true; // ← CHANGE TO false anytime to completely disable LBS
#define APN "internet"

bool mpuReady = false;
bool dfPlayerReady = false;
bool smsSentBoot = false;
bool smsSentSats = false;
bool smsSentFix = false;
unsigned long lastFallTime = 0;
unsigned long lastPollTime = 0;
unsigned long lastSmsTime = 0;
unsigned long lastHttpTime = 0; // last time we sent HTTP heartbeat
int attempt = 0;
int fixCount = 0;
bool useCGPS = false; // true = AT+CGPS command set, false = AT+CGNSS command set

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
unsigned long lastGpsCacheTime = 0;
unsigned long lastGpsLostTime  = 0;
#define GPS_LOST_CLEAR_MS 30000UL

// Device ID for API
const char* DEVICE_ID = "SPC-004";

// Build and send HTTP GET to the provided API using current bearer
// Returns true on 200 OK, false otherwise. Requires NETOPEN active.
bool sendHttpLocation(float lat, float lon, float accuracyMeters) {
  // Ensure data and NETOPEN for HTTP service
  if (!ensureInternetConnection()) {
    Serial.println("[HTTP] No internet; skipping HTTP send");
    return false;
  }

  // Close any previous HTTP profile
  sendAT("AT+HTTPTERM", 5000);
  delay(200);
  String r;
  bool ok = true;

  // Init HTTP service
  r = sendAT("AT+HTTPINIT", 8000);
  if (r.indexOf("OK") == -1) {
    Serial.println("[HTTP] HTTPINIT failed: " + r);
    ok = false;
  }

  // Set CID 1 (PDP context 1)
  if (ok) sendAT("AT+HTTPPARA=\"CID\",1", 3000);

  // Build URL
  String url = "http://gateway.ejeepdev.site/spc/api/v1/Live_Location.php?action=insert";
  url += "&device_id="; url += DEVICE_ID;
  url += "&latitude=";  url += String(lat, 6);
  url += "&longitude="; url += String(lon, 6);
  url += "&accuracy=";  url += String(accuracyMeters, 1);

  // Set URL
  if (ok) {
    r = sendAT(String("AT+HTTPPARA=\"URL\",\"") + url + "\"", 8000);
    if (r.indexOf("OK") == -1) {
      Serial.println("[HTTP] Set URL failed: " + r);
      ok = false;
    }
  }

  // Do GET
  if (ok) {
    r = sendAT("AT+HTTPACTION=0", 8000);
    // Wait for URC "+HTTPACTION: 0,<status>,<len>"
    String urc = "";
    unsigned long t0 = millis();
    while (millis() - t0 < 15000) {
      while (SerialAT.available()) urc += (char)SerialAT.read();
      if (urc.indexOf("+HTTPACTION:") != -1) break;
      delay(100);
    }
    Serial.println("[HTTP] URC: " + urc);
    int idx = urc.indexOf("+HTTPACTION: 0,");
    if (idx != -1) {
      int c1 = urc.indexOf(',', idx + 15);
      int status = -1;
      if (c1 != -1) status = urc.substring(idx + 15, c1).toInt();
      if (status == 200) {
        Serial.println("[HTTP] 200 OK sent: " + url);
        sendAT("AT+HTTPTERM", 5000);
        return true;
      } else {
        Serial.println("[HTTP] Non-200 status: " + String(status));
      }
    } else {
      Serial.println("[HTTP] No HTTPACTION URC");
    }
  }

  // Cleanup and fail
  sendAT("AT+HTTPTERM", 5000);
  return false;
}

// Build and send HTTP GET to SOS endpoint with IMU data.
// Returns true only if HTTP 200 and response body contains "OK" or "success".
bool sendHttpSOS(float ax, float ay, float az, float gx_dps, float gy_dps, float gz_dps) {
  if (!ensureInternetConnection()) {
    Serial.println("[HTTP][SOS] No internet; skipping SOS send");
    return false;
  }

  // Close any previous HTTP session
  sendAT("AT+HTTPTERM", 5000);
  delay(200);
  String r;
  bool ok = true;

  // Init HTTP
  r = sendAT("AT+HTTPINIT", 8000);
  if (r.indexOf("OK") == -1) {
    Serial.println("[HTTP][SOS] HTTPINIT failed: " + r);
    ok = false;
  }

  if (ok) sendAT("AT+HTTPPARA=\"CID\",1", 3000);

  // Build URL
  String url = "http://gateway.ejeepdev.site/spc/api/v1/SOS_Alert.php?action=insert";
  url += "&device_id="; url += DEVICE_ID;
  url += "&alert_type=fall";
  url += "&accel_x="; url += String(ax, 2);
  url += "&accel_y="; url += String(ay, 2);
  url += "&accel_z="; url += String(az, 2);
  url += "&gyro_x=";  url += String(gx_dps, 1);
  url += "&gyro_y=";  url += String(gy_dps, 1);
  url += "&gyro_z=";  url += String(gz_dps, 1);

  if (ok) {
    r = sendAT(String("AT+HTTPPARA=\"URL\",\"") + url + "\"", 8000);
    if (r.indexOf("OK") == -1) {
      Serial.println("[HTTP][SOS] Set URL failed: " + r);
      ok = false;
    }
  }

  bool http200 = false;
  if (ok) {
    r = sendAT("AT+HTTPACTION=0", 8000);
    String urc = "";
    unsigned long t0 = millis();
    while (millis() - t0 < 15000) {
      while (SerialAT.available()) urc += (char)SerialAT.read();
      if (urc.indexOf("+HTTPACTION:") != -1) break;
      delay(100);
    }
    Serial.println("[HTTP][SOS] URC: " + urc);
    int idx = urc.indexOf("+HTTPACTION: 0,");
    if (idx != -1) {
      int c1 = urc.indexOf(',', idx + 15);
      int status = -1;
      if (c1 != -1) status = urc.substring(idx + 15, c1).toInt();
      http200 = (status == 200);
    }
  }

  bool bodyOK = false;
  if (ok && http200) {
    // Read body and check for OK/success tokens
    String body = sendAT("AT+HTTPREAD", 15000);
    String bodyLower = body; bodyLower.toLowerCase();
    if (bodyLower.indexOf("ok") != -1 || bodyLower.indexOf("success") != -1) {
      bodyOK = true;
    }
    Serial.println("[HTTP][SOS] Body: " + body);
  }

  sendAT("AT+HTTPTERM", 5000);
  bool res = ok && http200 && bodyOK;
  Serial.println(res ? "[HTTP][SOS] Sent successfully" : "[HTTP][SOS] Send failed");
  return res;
}

void printHardwareInfo() {
  Serial.println("===== A7680C HARDWARE INFO =====");

  sendAT("ATI", 3000);          // Module model
  sendAT("AT+CGMM", 3000);      // Model identification
  sendAT("AT+CGMI", 3000);      // Manufacturer
  sendAT("AT+CGMR", 3000);      // Firmware version
  sendAT("AT+CGSN", 3000);      // IMEI
  sendAT("AT+CIMI", 3000);      // IMSI (SIM)
  sendAT("AT+ICCID", 3000);     // SIM ICCID
  sendAT("AT+CPIN?", 3000);     // SIM status
  sendAT("AT+CSQ", 3000);       // Signal quality
  sendAT("AT+COPS?", 5000);     // Network operator
  sendAT("AT+CPSI?", 5000);     // LTE detailed info
  sendAT("AT+CGNSSPWR?", 3000); // GNSS power state
  sendAT("AT+CPIN?", 3000);
  sendAT("AT+CREG?", 3000);
  sendAT("AT+CGATT?", 3000);
  sendAT("AT+CGACT?", 3000);
  sendAT("AT+CENG?", 5000); // Cell engineering mode
  Serial.println("=================================");
}
// ═══════════════════════════════════════════════════════════════
//                 AT COMMAND (with silence wait)
// ═══════════════════════════════════════════════════════════════

// Internal: read from SerialAT for up to `timeout` ms, stopping
// after 1200ms of silence once ANY data arrives.
String _readAT(unsigned long timeout) {
  String response = "";
  unsigned long lastByteTime = millis();
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (SerialAT.available()) {
      while (SerialAT.available()) response += (char)SerialAT.read();
      lastByteTime = millis();
    }
    if (response.length() > 0 && millis() - lastByteTime >= 1200) break;
    delay(10);
  }
  response.trim();
  return response;
}

String sendAT(String cmd, unsigned long timeout = 5000) {
  // Retry loop: if the module reboots mid-command (*ATREADY: in response)
  // wait 5s for it to stabilise, then resend — up to 2 retries.
  for (int attempt = 0; attempt < 3; attempt++) {
    // Flush stale bytes
    unsigned long flushDeadline = millis() + 200;
    while (millis() < flushDeadline) {
      while (SerialAT.available()) SerialAT.read();
      delay(10);
    }

    SerialAT.println(cmd);
    delay(300);

    String response = _readAT(timeout);

    Serial.println(">> " + cmd);
    Serial.println("<< " + response);

    // Detect module reboot URC — *ATREADY: 1 appears when the A7680C
    // power-cycles (brownout) mid-command. Wait for it to fully boot.
    if (response.indexOf("*ATREADY:") != -1 ||
        (response.indexOf("+CPIN:") != -1 && response.indexOf("OK") == -1 &&
         response.indexOf("+C") == response.indexOf("+CPIN:"))) {
      Serial.println("!! Module reboot detected — waiting 6s to stabilise...");
      // Drain the boot URCs
      unsigned long bootWait = millis();
      while (millis() - bootWait < 6000) {
        while (SerialAT.available()) SerialAT.read();
        delay(50);
      }
      if (attempt < 2) {
        Serial.println("!! Retrying: " + cmd);
        continue; // retry
      }
    }

    return response; // good response (or gave up after retries)
  }
  return "";
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
  printHardwareInfo();
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
//                     DEAD RECKONING
// ═══════════════════════════════════════════════════════════════

// ── Dead Reckoning globals ──
float expectedLat = 0.0;
float expectedLon = 0.0;
float currentHeading = 0.0; // Assume 0° North initially
unsigned long lastDrUpdateTime = 0;
bool drActive = false;

#define STEP_ACCEL_THRESHOLD 1.2
#define MIN_STEP_MS 300
unsigned long lastStepTimeDR = 0;
float strideLengthMeters = 0.75;

void updateDeadReckoning() {
  if (!mpuReady) return;
  
  unsigned long now = millis();
  if (now - lastDrUpdateTime < 50) return; // Limit update rate to ~20Hz
  
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  
  float dt = (now - lastDrUpdateTime) / 1000.0;
  if (lastDrUpdateTime == 0) dt = 0;
  lastDrUpdateTime = now;
  
  // Update heading using Gyro Z
  // CCW rotation gives positive g.gyro.z, but heading increases CW!
  currentHeading -= (g.gyro.z * 180.0 / PI) * dt;
  if (currentHeading >= 360.0) currentHeading -= 360.0;
  else if (currentHeading < 0) currentHeading += 360.0;
  
  // Step detection
  float totalG = sqrt(a.acceleration.x * a.acceleration.x + 
                      a.acceleration.y * a.acceleration.y + 
                      a.acceleration.z * a.acceleration.z) / 9.81;
                      
  if (totalG > STEP_ACCEL_THRESHOLD && (now - lastStepTimeDR > MIN_STEP_MS)) {
    lastStepTimeDR = now;
    if (drActive) {
      float latRad = expectedLat * PI / 180.0;
      float dLat = (strideLengthMeters * cos(currentHeading * PI / 180.0)) / 111320.0;
      float dLon = (strideLengthMeters * sin(currentHeading * PI / 180.0)) / (111320.0 * cos(latRad));
      
      expectedLat += dLat;
      expectedLon += dLon;
    }
  }
}

void syncDeadReckoning(float lat, float lon, float course) {
  expectedLat = lat;
  expectedLon = lon;
  currentHeading = course;
  drActive = true;
  lastDrUpdateTime = millis();
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

// Send the same SMS to all numbers in PHONE_NUMBERS[].
// Returns true if at least one send succeeded.
bool sendSMSToAll(String message) {
  bool anyOk = false;
  for (int i = 0; i < PHONE_COUNT; i++) {
    Serial.println("[SMS] Sending to " + String(PHONE_NUMBERS[i]));
    bool ok = sendSMS(String(PHONE_NUMBERS[i]), message);
    if (ok) anyOk = true;
    delay(500); // brief gap between sends
  }
  return anyOk;
}

// ═══════════════════════════════════════════════════════════════
//              LBS (Cell Tower) — fallback
// ═══════════════════════════════════════════════════════════════
//
// AT+CLBS IS supported on A7680C.  The key requirements are:
//   1. Module registered on network (AT+CREG / AT+CEREG = 1 or 5)
//   2. PDP context activated (AT+CGACT=1,1) with valid IP
//   3. AT+NETOPEN must succeed (wait for +NETOPEN: 0 URC)
//   4. AT+CLBSCFG server configured BEFORE AT+CLBS
//
// Error codes returned in +CLBS: <status>:
//   0  = success
//   1  = unknown error
//   2  = wrong parameter
//   3  = wrong date/time
//   4  = network error (transient — retry after 3s)
//  10  = close network error (NETOPEN not done / data bearer down)
//  11  = no network service
//  12  = network rejected
// 100  = no coordinates available (tower known but no lat/lon)

// Set timezone to Manila (UTC+8)
void setManilaTimezone() {
  sendAT("AT+CTZU=1"); // Enable automatic timezone update
  delay(300);
  sendAT("AT+CTZR=1"); // Enable timezone reporting
  delay(300);
  Serial.println(">> Timezone set to Manila (UTC+8)");
}

// ── isNetworkRegistered ─────────────────────────────────────────
// Checks registration using AT+CPSI? (most reliable on A7680C LTE)
// plus CEREG/CGREG fallback.  Returns true if PS domain is up.
bool isNetworkRegistered() {
  // Primary: AT+CPSI? — gives full system info on A7680C
  // Response: +CPSI: LTE,Online,515-03,0x27E7,...
  // "Online" = registered, "NO SERVICE" = not registered
  String cpsi = sendAT("AT+CPSI?", 5000);
  if (cpsi.indexOf("Online") != -1) {
    Serial.println(">> [REG] Registered (CPSI=Online)");
    return true;
  }
  if (cpsi.indexOf("NO SERVICE") != -1) {
    Serial.println(">> [REG] NO SERVICE (CPSI)");
    // Don't give up yet — fall through to CEREG
  }

  // Enable CEREG URC mode (required for stat field to be meaningful)
  sendAT("AT+CEREG=1", 3000);
  delay(200);
  String cereg = sendAT("AT+CEREG?", 5000);
  // +CEREG: 1,1  or  +CEREG: 1,5  = registered
  if (cereg.indexOf(",1") != -1 || cereg.indexOf(",5") != -1) {
    Serial.println(">> [REG] Registered (CEREG)");
    return true;
  }

  // CGREG fallback (covers WCDMA/HSPA)
  String cgreg = sendAT("AT+CGREG?", 5000);
  if (cgreg.indexOf(",1") != -1 || cgreg.indexOf(",5") != -1) {
    Serial.println(">> [REG] Registered (CGREG)");
    return true;
  }

  // Last resort: CREG (CS domain)
  String creg = sendAT("AT+CREG?", 5000);
  if (creg.indexOf(",1") != -1 || creg.indexOf(",5") != -1) {
    Serial.println(">> [REG] Registered (CREG)");
    return true;
  }

  Serial.println(">> [REG] Not registered");
  return false;
}

// ── waitForNETOPEN ───────────────────────────────────────────────
// Opens the TCP socket layer and waits for the +NETOPEN: 0 URC.
// Returns true on success.  Called by initLBS() and ensureInternetConnection().
bool waitForNETOPEN(unsigned long timeoutMs = 15000) {
  // Already open?
  String st = sendAT("AT+NETOPEN?", 3000);
  if (st.indexOf("+NETOPEN: 1") != -1) {
    Serial.println(">> NETOPEN: already open");
    return true;
  }

  // Send NETOPEN and wait for the URC "+NETOPEN: 0" (0 = success)
  while (SerialAT.available()) SerialAT.read(); // flush
  SerialAT.println("AT+NETOPEN");
  Serial.println(">> [NETOPEN] waiting for +NETOPEN: 0 ...");

  String buf = "";
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) buf += (char)SerialAT.read();
    if (buf.indexOf("+NETOPEN: 0") != -1) {
      Serial.println(">> NETOPEN: socket layer open (URC OK)");
      return true;
    }
    if (buf.indexOf("+NETOPEN: 1") != -1) {
      // code 1 = already open (some firmware variants report this)
      Serial.println(">> NETOPEN: already open (URC=1)");
      return true;
    }
    // Some modules report ERROR if already open — re-query state
    if (buf.indexOf("ERROR") != -1) {
      String st2 = sendAT("AT+NETOPEN?", 3000);
      if (st2.indexOf("+NETOPEN: 1") != -1) {
        Serial.println(">> NETOPEN: open (verified via query after ERROR)");
        return true;
      }
      Serial.println(">> NETOPEN: ERROR and not open");
      return false;
    }
    delay(200);
  }
  Serial.println(">> NETOPEN: timeout");
  return false;
}

// ── initLBS ──────────────────────────────────────────────────────
// Full LBS initialisation: PS registration → PDP → IP → CLBSCFG → NETOPEN.
// Returns true when the module is ready to accept AT+CLBS.
bool initLBS() {
  Serial.println(">> [LBS] Initializing...");

  // ── 1. Check network registration via CPSI + CEREG/CGREG fallback ──
  if (!isNetworkRegistered()) {
    Serial.println(">> [LBS] Not registered — aborting");
    return false;
  }

  // ── 2. Attach to packet (PS) domain ───────────────────────────
  String cgatt = sendAT("AT+CGATT?", 5000);
  if (cgatt.indexOf("+CGATT: 1") == -1) {
    Serial.println(">> [LBS] Attaching to PS domain...");
    sendAT("AT+CGATT=1", 10000);
  }

  // ── 3. Define & activate PDP context 1 ────────────────────────
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 5000);

  String cgact = sendAT("AT+CGACT?", 5000);
  if (cgact.indexOf("+CGACT: 1,1") == -1) {
    Serial.println(">> [LBS] Activating PDP context...");
    sendAT("AT+CGACT=1,1", 15000);
    delay(1000);
  }

  // ── 4. Confirm IP address assigned ────────────────────────────
  String ipaddr = sendAT("AT+CGPADDR=1", 5000);
  if (ipaddr.indexOf(".") == -1) {
    // Try deactivate → re-activate once
    Serial.println(">> [LBS] No IP — retrying PDP activation...");
    sendAT("AT+CGACT=0,1", 5000);
    delay(2000);
    sendAT("AT+CGACT=1,1", 15000);
    delay(1000);
    ipaddr = sendAT("AT+CGPADDR=1", 5000);
    if (ipaddr.indexOf(".") == -1) {
      Serial.println(">> [LBS] IP still not assigned — abort");
      return false;
    }
  }
  Serial.println(">> [LBS] IP: " + ipaddr);

  // ── 5. Configure SIMCom LBS server ───────────────────────────
  // A76XX format: AT+CLBSCFG=<optype>,<index>,<server>,<port>
  // optype 1 = write, index 1 = primary
  Serial.println(">> [LBS] Configuring LBS server (lbs.simcom.com:3005)...");
  sendAT("AT+CLBSCFG=1,1,\"lbs.simcom.com\",3005", 5000);
  delay(300);

  // ── 6. Open TCP socket layer (CRITICAL for AT+CLBS) ──────────
  if (!waitForNETOPEN(15000)) {
    Serial.println(">> [LBS] NETOPEN failed — LBS cannot proceed");
    return false;
  }

  Serial.println(">> [LBS] Init complete — ready for AT+CLBS");
  return true;
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

// ═══════════════════════════════════════════════════════════════
//    getLBSLocation — Network-based location via AT+CLBS
//
//  specificMode: 1=serving cell (fast), 2=multi-tower, 4=auto
//                -1 = try 1, 4, 2 in order
//  Returns "lat,lon" on success, "" on failure
// ═══════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────
//  sendCLBS — send one AT+CLBS=<mode>,1 and collect the response
// ─────────────────────────────────────────────────────────────────
String sendCLBS(int mode) {
  while (SerialAT.available()) SerialAT.read(); // flush
  String cmd = "AT+CLBS=" + String(mode) + ",1";
  SerialAT.println(cmd);
  Serial.println(">> [LBS] " + cmd);
  delay(300);

  String resp = "";
  unsigned long t = millis();
  // Wait up to 10s; status 10 comes back fast (<2s), success takes longer
  while (millis() - t < 10000) {
    while (SerialAT.available()) resp += (char)SerialAT.read();
    if (resp.indexOf("+CLBS:") != -1 &&
        (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1))
      break;
    delay(100);
  }
  resp.trim();
  Serial.println(">> [LBS] raw: " + resp);
  return resp;
}

// ─────────────────────────────────────────────────────────────────
//  parseCLBSCoords — extract "lat,lon" from a +CLBS: response
//  Returns "" on failure.
// ─────────────────────────────────────────────────────────────────
String parseCLBSCoords(const String &resp) {
  int idx = resp.indexOf("+CLBS:");
  if (idx == -1) return "";

  String data = resp.substring(idx + 6);
  data.trim();

  int c1 = data.indexOf(',');
  String status = (c1 == -1) ? data : data.substring(0, c1);
  status.trim();
  Serial.println(">> [LBS] status=" + status);

  if (status != "0" || c1 < 0) return "";

  int c2 = data.indexOf(',', c1 + 1);
  int c3 = data.indexOf(',', c2 + 1);
  if (c2 < 0 || c3 < 0) return "";

  String lat = data.substring(c1 + 1, c2); lat.trim();
  String lon = data.substring(c2 + 1, c3); lon.trim();

  float latV = lat.toFloat(), lonV = lon.toFloat();
  if (lat.length() > 3 && lon.length() > 3 &&
      lat.indexOf('.') > 0 && lon.indexOf('.') > 0 &&
      latV != 0.0f && lonV != 0.0f &&
      latV >= -90.0f  && latV <= 90.0f &&
      lonV >= -180.0f && lonV <= 180.0f) {
    Serial.println(">>> [LBS] coords OK: " + lat + "," + lon);
    return lat + "," + lon;
  }
  Serial.println(">> [LBS] coord validation failed: " + lat + "," + lon);
  return "";
}

// ─────────────────────────────────────────────────────────────────
//  getLBSLocation — Network-based location via AT+CLBS
//
//  specificMode: 1=serving cell (fast), 4=auto, 2=multi-tower
//               -1 = try 1 → 4 → 2 in order
//  Returns "lat,lon" on success, "" on failure.
// ─────────────────────────────────────────────────────────────────
String getLBSLocation(int specificMode = -1) {
  static bool lbsInitialized = false;
  static bool clbsSupported  = true;
  static int  consecutiveFails = 0;  // full-round failures

  if (!clbsSupported) {
    Serial.println(">> [LBS] AT+CLBS marked unsupported, skipping");
    return "";
  }

  // One-time init
  if (!lbsInitialized) {
    clbsSupported  = true;
    consecutiveFails = 0;
    lbsInitialized = initLBS();
    if (!lbsInitialized) {
      Serial.println(">> [LBS] Init failed");
      return "";
    }
  }

  // A7680C mode behaviour:
  //   mode 1 = serving cell (fast, lat/lon)
  //   mode 2 = multi-tower BUT returns hex address string on A7680C, NOT lat/lon
  //   mode 4 = time-based geolocation (lat/lon + date/time) ← best on this module
  int modesToTry[] = {4, 1}; // skip mode 2 — returns text, not coords on A7680C
  int numModes = 2;
  if (specificMode != -1) { modesToTry[0] = specificMode; numModes = 1; }

  int status10Count = 0;

  for (int i = 0; i < numModes; i++) {
    int mode = modesToTry[i];
    String resp = sendCLBS(mode);

    // ── Happy path ──
    String coords = parseCLBSCoords(resp);
    if (coords.length() > 0) {
      consecutiveFails = 0;
      return coords;
    }

    // ── Analyse error ──
    int idx = resp.indexOf("+CLBS:");
    if (idx == -1) {
      Serial.println(">> [LBS] No +CLBS: in response for mode " + String(mode));
      delay(500);
      continue;
    }
    String data = resp.substring(idx + 6); data.trim();
    int c1 = data.indexOf(',');
    String status = (c1 == -1) ? data : data.substring(0, c1);
    status.trim();

    if (status == "10") {
      // Close network error — close & reopen NETOPEN then retry once
      status10Count++;
      Serial.println(">> [LBS] status 10 — resetting NETOPEN...");
      sendAT("AT+NETCLOSE", 5000);
      delay(2000);
      if (waitForNETOPEN(12000)) {
        Serial.println(">> [LBS] NETOPEN re-opened — retrying mode " + String(mode));
        resp = sendCLBS(mode);
        coords = parseCLBSCoords(resp);
        if (coords.length() > 0) { consecutiveFails = 0; return coords; }
        // Check status again after retry
        idx = resp.indexOf("+CLBS:");
        if (idx != -1) {
          data = resp.substring(idx + 6); data.trim();
          c1 = data.indexOf(',');
          status = (c1 == -1) ? data : data.substring(0, c1);
          status.trim();
          if (status == "10") status10Count++; // still failing
        }
      }
    } else if (status == "4") {
      // Transient network error — wait 3s and retry once
      Serial.println(">> [LBS] status 4 (network error) — retrying in 3s...");
      delay(3000);
      resp = sendCLBS(mode);
      coords = parseCLBSCoords(resp);
      if (coords.length() > 0) { consecutiveFails = 0; return coords; }
    } else {
      String err;
      if      (status == "1")   err = "Unknown error";
      else if (status == "2")   err = "Wrong parameter";
      else if (status == "3")   err = "Wrong date/time";
      else if (status == "11")  err = "No network service";
      else if (status == "12")  err = "Network rejected";
      else if (status == "100") err = "No coordinates (tower known, no lat/lon)";
      else                      err = "code " + status;
      Serial.println(">> [LBS] mode " + String(mode) + " failed: " + err);
    }
    delay(500);
  }

  // All modes returned status 10 on 3 consecutive full rounds → give up
  if (status10Count >= numModes) consecutiveFails++;
  if (consecutiveFails >= 3) {
    Serial.println(">> [LBS] 3 consecutive full failures — marking AT+CLBS unsupported");
    clbsSupported = false;
  }

  Serial.println(">> [LBS] All modes exhausted");
  return "";
}



// String getLocationLBSFirst(String &source) {
//   Serial.println(">> Getting FRESH location (GPS 5s -> LBS fresh)...");

//   // ── GPS first (your existing 5s poll) ──
//   unsigned long gpsStartTime = millis();
//   while (millis() - gpsStartTime < 5000) {
//     String raw = sendAT("AT+CGNSSINFO");
//     GpsData gps = parseCgpsInfo(raw);
//     if (gps.valid) {
//       source = "GPS";
//       return String(gps.latitude, 6) + "," + String(gps.longitude, 6);
//     }
//     delay(1000);
//   }

//   Serial.println(">> GPS no fix → forcing FRESH LBS...");

//   // ── FORCE FRESH NETWORK (this is the key fix) ──
//   Serial.println(">> Full radio reset + re-registration for fresh cell...");
//   sendAT("AT+CFUN=0", 8000); // radio off
//   delay(6000);
//   sendAT("AT+CFUN=1", 15000); // radio on
//   delay(8000);

//   // Wait up to 25s for proper LTE registration (critical!)
//   bool registered = false;
//   unsigned long regStart = millis();
//   while (millis() - regStart < 25000) {
//     String resp = sendAT("AT+CEREG?");
//     if (resp.indexOf(",1") != -1 ||
//         resp.indexOf(",5") != -1) { // registered/home or roaming
//       registered = true;
//       break;
//     }
//     delay(2000);
//   }
//   if (!registered)
//     Serial.println(">> Warning: registration timeout, trying LBS anyway");

//   // Optional but recommended: activate PDP context (LBS often needs data
//   // bearer)
//   sendAT("AT+CGDCONT=1,\"IP\",\"internet\""); // Philippines common APN
//                                               // (Globe/Smart/DITO). Change
//                                               if
//                                               // needed
//   sendAT("AT+CGACT=1,1", 10000);

//   // ── Now take 4 LBS readings with type=4 (gives timestamp too) ──
//   String lastLbs = "";
//   String prevLbs = "";
//   for (int i = 0; i < 4; i++) {
//     String lbs = getLBSLocation(4); // type=4 → lat,lon,acc,date,time
//     Serial.println(">> LBS #" + String(i + 1) + ": " + lbs);

//     if (lbs.length() > 0 && lbs.indexOf(".") > 0) {
//       if (prevLbs.length() > 0 && lbs != prevLbs) {
//         source = "LBS (fresh)";
//         Serial.println(">> FRESH LBS detected (coordinates changed)!");
//         return lbs.substring(
//             0, lbs.indexOf(',', lbs.indexOf(',') + 1)); // return just
//             lat,lon
//       }
//       prevLbs = lastLbs;
//       lastLbs = lbs;
//     }
//     delay(2500);
//   }

//   if (lastLbs.length() > 0) {
//     source = "LBS";
//     return lastLbs.substring(0, lastLbs.indexOf(',', lastLbs.indexOf(',') +
//     1));
//   }

//   source = "none";
//   return "";
// }





// ═══════════════════════════════════════════════════════════════
//    GET LOCATION WITH SOURCE — GNSS first, then LBS fallback
//    Returns "lat,lon" ONLY when we have real coordinates
//    source = "GNSS" or "LBS"
// ═══════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════
//    ENSURE INTERNET / DATA CONNECTION (fixes No Internet MP3)
// ═══════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════
//    STRONGER INTERNET CHECK (fixes status 10 "Close network error")
// ═══════════════════════════════════════════════════════════════
bool ensureInternetConnection() {
  Serial.println(">> Ensuring data + socket connection for LBS...");

  // ── 1. Wait for PS registration using CPSI + CEREG (up to 30s) ─
  // Uses isNetworkRegistered() which checks CPSI first (most reliable
  // on A7680C), then CEREG/CGREG/CREG as fallbacks.
  bool registered = false;
  unsigned long regStart = millis();
  while (millis() - regStart < 30000) {
    if (isNetworkRegistered()) { registered = true; break; }
    Serial.println(">> [REG] Waiting for network... (" +
                   String((millis() - regStart) / 1000) + "s)");
    delay(3000);
  }
  if (!registered) {
    Serial.println(">>> Network not registered after 30s");
    playSound(SOUND_NO_INTERNET);
    return false;
  }

  // ── 2. Ensure PDP context & IP ────────────────────────────────
  sendAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\"", 5000);
  sendAT("AT+CGATT=1", 10000);
  sendAT("AT+CGACT=1,1", 12000);
  delay(500);

  String ipCheck = sendAT("AT+CGPADDR=1", 8000);
  if (ipCheck.indexOf(".") == -1) {
    Serial.println(">> No IP yet — retrying PDP...");
    sendAT("AT+CGACT=0,1", 5000);
    delay(2000);
    sendAT("AT+CGACT=1,1", 12000);
    delay(500);
    ipCheck = sendAT("AT+CGPADDR=1", 8000);
    if (ipCheck.indexOf(".") == -1) {
      Serial.println(">> Still no IP");
      playSound(SOUND_NO_INTERNET);
      return false;
    }
  }
  Serial.println(">> IP confirmed: " + ipCheck);

  // ── 3. Ensure TCP socket layer (NETOPEN) ─────────────────────
  // This is the fix for status 10: AT+CLBS needs NETOPEN active.
  if (!waitForNETOPEN(15000)) {
    Serial.println(">> NETOPEN failed — data bearer may be restricted");
    playSound(SOUND_NO_INTERNET);
    return false;
  }

  Serial.println(">>> Internet + NETOPEN ready!");
  playSound(SOUND_INTERNET_OK); // 001.mp3
  return true;
}
// ═══════════════════════════════════════════════════════════════
//    GET LOCATION WITH SOURCE — GNSS first, then LBS fallback
//    Returns "lat,lon" ONLY when we have real coordinates
//    source = "GNSS" or "LBS"
// ═══════════════════════════════════════════════════════════════

String getLocationWithSource(String &source) {
  source = "none";

  // ── 1. GNSS FIRST (Quick check) ──
  Serial.println(">> [LOC] Checking GNSS...");
  String raw = sendAT("AT+CGPSINFO", 5000);
  GpsData gps = parseCgpsInfo(raw);

  if (gps.valid) {
    source = "GNSS";
    String coords = String(gps.latitude, 6) + "," + String(gps.longitude, 6);
    Serial.println(">>> GNSS FIX ACQUIRED: " + coords);
    syncDeadReckoning(gps.latitude, gps.longitude, gps.course);
    return coords;
  }

  // Caching and dead reckoning disabled for GNSS; require fresh fix
  drActive = false;

  // ── 2. LBS FALLBACK via AT+CLBS (SIMCom cell-tower positioning) ──
  Serial.println(">> No GNSS → ensuring internet for LBS lookup...");
  if (!ensureInternetConnection()) {
    return ""; // plays "No Internet" sound automatically
  }

  if (useLBSFallback) {
    Serial.println(">> Trying SIMCom LBS (AT+CLBS)...");
    String lbs = getLBSLocation();
    if (lbs.length() > 8 && lbs.indexOf('.') > 0) {
      source = "LBS";
      Serial.println(">>> LBS (CLBS): " + lbs);
      return lbs;
    }
    Serial.println(">> AT+CLBS failed — no location available");
  } else {
    Serial.println(">> SIMCom LBS disabled");
  }

  Serial.println(">> All location methods failed");
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

  unsigned long startTime = millis();
  unsigned long maxWaitMs = (unsigned long)maxWaitSec * 1000UL;
  int polls = 0;

  while (millis() - startTime < maxWaitMs) {
    polls++;
    String raw = sendAT("AT+CGPSINFO");
    GpsData gps = parseCgpsInfo(raw);

    if (gps.valid) {
      Serial.println(">> GPS reading #" + String(polls) +
                     " valid, confirming...");
      delay(2000);

      raw = sendAT("AT+CGPSINFO");
      GpsData gps2 = parseCgpsInfo(raw);

      if (gps2.valid) {
        lastGps = gps2;
        source = "GPS";
        String coords =
            String(gps2.latitude, 6) + "," + String(gps2.longitude, 6);
        Serial.println(">> Fresh GPS fix confirmed: " + coords);
        syncDeadReckoning(gps2.latitude, gps2.longitude, gps2.course);
        return coords;
      }
    }

    Serial.println(">> GPS poll #" + String(polls) + " — no fix yet");
    delay(3000);
  }

  Serial.println(">> GPS timed out → trying SIMCom LBS...");
  if (useLBSFallback) {
    String lbsLoc = getLBSLocation();
    if (lbsLoc.length() > 0) {
      source = "LBS";
      Serial.println(">> LBS location: " + lbsLoc);
      return lbsLoc;
    }
  }

  // Do not use cached GPS; require fresh GNSS or LBS

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
//  GET SATELLITE COUNT from AT+CGNSSINFO
//
//  Response fields (foreign/A7680C format):
//  [mode],[GPS-SVs],[BDS-SVs],[GLONASS-SVs],[GALILEO-SVs],
//  [lat],[N/S],[lon],[E/W],[date],[UTC],[alt],[spd],[course],
//  [PDOP],[HDOP],[VDOP]
//
//  mode: 2=2D fix, 3=3D fix, empty=no fix
//  Returns total visible satellites across all constellations.
//  Returns -1 if no response, 0 if no fix yet.
// ═══════════════════════════════════════════════════════════════

int getSatelliteCount() {
  String raw = sendAT("AT+CGNSSINFO", 3000);

  int idx = raw.indexOf("+CGNSSINFO:");
  if (idx == -1) return -1;

  String payload = raw.substring(idx + 11);
  int lineEnd = payload.indexOf('\n');
  if (lineEnd != -1) payload = payload.substring(0, lineEnd);
  payload.trim();

  // All empty = no fix
  if (payload.startsWith(",,,")) return 0;

  // Parse first 5 fields: mode, GPS-SVs, BDS-SVs, GLONASS-SVs, GALILEO-SVs
  String fields[5];
  int fieldCount = 0, prev = 0;
  for (int i = 0; i <= (int)payload.length() && fieldCount < 5; i++) {
    if (i == (int)payload.length() || payload[i] == ',') {
      fields[fieldCount++] = payload.substring(prev, i);
      fields[fieldCount - 1].trim();
      prev = i + 1;
    }
  }

  if (fieldCount < 2) return 0;

  // Sum all constellation satellite counts (fields 1–4)
  int gpsSvs  = (fieldCount > 1 && fields[1].length() > 0) ? fields[1].toInt() : 0;
  int bdsSvs  = (fieldCount > 2 && fields[2].length() > 0) ? fields[2].toInt() : 0;
  int gloSvs  = (fieldCount > 3 && fields[3].length() > 0) ? fields[3].toInt() : 0;
  int galSvs  = (fieldCount > 4 && fields[4].length() > 0) ? fields[4].toInt() : 0;
  int total   = gpsSvs + bdsSvs + gloSvs + galSvs;

  // Log constellation breakdown for Serial debugging
  String fixMode = fields[0].length() > 0
                 ? (fields[0] == "3" ? "3D" : "2D")
                 : "no fix";
  Serial.println("    [SVs] GPS:" + String(gpsSvs) +
                 " BDS:" + String(bdsSvs) +
                 " GLO:" + String(gloSvs) +
                 " GAL:" + String(galSvs) +
                 "  Total:" + String(total) +
                 "  Fix:" + fixMode);

  return total;
}

// ═══════════════════════════════════════════════════════════════
//                     ULTRASONIC SENSOR
// ═══════════════════════════════════════════════════════════════

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout (~5m restrict)
  if (duration == 0) {
    return -1.0; // Timeout or out of range
  }
  
  // Speed of sound is ~343 m/s = 0.0343 cm/us
  // Divide by 2 to account for trip out and back
  return (duration * 0.0343) / 2.0;
}

void ultrasonicTask(void * pvParameters) {
  unsigned long lastObstacleSoundTime = 0;
  const unsigned long OBSTACLE_SOUND_COOLDOWN = 5000; // Play voice alert max once per 5s

  for (;;) {
    float currentDistance = getDistance();

    // Max reliable distance for typical HC-SR04 is ~400 cm
    // Beep faster when close, slower when far. No beep if out of range.
    if (currentDistance > 0 && currentDistance <= 400.0) {

      // ── 1-meter voice alert ──────────────────────────────────
      if (currentDistance <= 100.0) {
        unsigned long now = millis();
        if (now - lastObstacleSoundTime >= OBSTACLE_SOUND_COOLDOWN) {
          lastObstacleSoundTime = now;
          playSound(SOUND_OBSTACLE_1METER); // 010.mp3 — "Obstacle ahead 1 meter"
        }
      }

      // Map distance (0 to 400cm) to beep interval (150ms for near, 1500ms for far)
      long beepInterval = map((long)currentDistance, 0, 400, 150, 1500);
      beepInterval = constrain(beepInterval, 150, 1500); // Ensure interval bounds
      
      digitalWrite(BUZZER_PIN, HIGH);
      delay(150); // Increased to 150ms for a louder, clearer beep
      digitalWrite(BUZZER_PIN, LOW);
      
      // Delay for the remainder of the interval before beeping again
      if (beepInterval > 150) {
        delay(beepInterval - 150);
      }
    } else {
      // No object or out of range -> turn off buzzer
      digitalWrite(BUZZER_PIN, LOW);
      delay(100); // Wait 100ms before checking distance again to save power/cpu
    }
  }
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

  // Init Buzzer and Ultrasonic Sensor
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // ── Power-on Beep Sequence ──
  // 3 quick beeps to indicate ESP32 is ON and code is running
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(400); // Increased from 100ms to 400ms for a much louder startup beep
    digitalWrite(BUZZER_PIN, LOW);
    delay(200); // 200ms gap between beeps
  }
  delay(200); // Small pause before continuing with modem boot

  // Start the Distance Sensor Task on Core 0 so it runs completely independent of GPS blocking loops
  xTaskCreatePinnedToCore(
    ultrasonicTask,   /* Task function. */
    "UltrasonicTask", /* name of task. */
    2048,             /* Stack size in bytes. */
    NULL,             /* Parameter passed as input of the task */
    1,                /* Priority of the task. */
    NULL,             /* Task handle. */
    0);               /* Core where the task should run */

  // ═══════════════════════════════════════════════════════
  //  GNSS — JUST power it on. That's it!
  //  No port switching, no NMEA config, no cold start.
  // ═══════════════════════════════════════════════════════
  Serial.println("==== GNSS INIT ====");

  // Activate data for AGPS (required in Philippines)
  sendAT("AT+CGATT=1", 10000);
  sendAT("AT+CGDCONT=1,\"IP\",\"internet\"", 5000);
  sendAT("AT+CGACT=1,1", 10000);

  // Sync network time (before GNSS ON)
  sendAT("AT+CLBS=4,1", 10000);
  sendAT("AT+CTZU=1", 3000);
  sendAT("AT+CTZR=1", 3000);

  
  // Power GNSS — ONLY 1 parameter (A7680C only supports this)
  Serial.println("Powering GNSS...");
  String resp = sendAT("AT+CGNSSPWR=1", 15000);

  useCGPS = false;

  
  if (resp.indexOf("OK") != -1) {
    Serial.println("GNSS powered ON");

    //This allows the GNSS engine to run its internal NMEA stream which improves fix reliability.
    sendAT("AT+CGNSSPORTSWITCH=0,1", 5000);

    // Wait for READY message (takes ~9 seconds)
    Serial.println("Waiting for GNSS READY...");
    unsigned long start = millis();
    while (millis() - start < 15000) {
      if (SerialAT.find("+CGNSSPWR: READY!")) {
        Serial.println(">>> GNSS READY!");
        break;
      }
      delay(200);
    }

    // Multi-constellation (GPS + BDS + GLONASS)
    sendAT("AT+CGNSSMODE=7", 5000);
    Serial.println("GNSS mode: GPS+BDS+GLONASS");

    // Download AGPS AFTER GNSS READY (per manual). Non-fatal if unsupported.
    Serial.println("Downloading AGPS...");
    String agpsResp = sendAT("AT+CAGPS", 15000);
    if (agpsResp.indexOf("OK") == -1 && agpsResp.indexOf("+AGPS: success") == -1) {
      Serial.println("AGPS not available or failed, continuing without it");
    }
    delay(2000);

  } else {
    Serial.println("GNSS power FAILED — check SIM card / antenna");
  }

  // Final check
  sendAT("AT+CGNSSPWR?", 5000);

  playSound(SOUND_SEARCHING_SATS); // 009.mp3 — "Searching for satellites"
  ensureInternetConnection();       // ready data bearer for LBS fallback


  // ── GPS WARMUP: give GPS up to 2 minutes to acquire satellites ──
  // Cold start after AGPS typically needs 30–90s outdoors.
  // We poll every 3s and show satellite progress on Serial.
  Serial.println("\n========================================");
  Serial.println("  GPS WARMUP — waiting up to 2 min for fix");
  Serial.println("========================================");

  #define GPS_WARMUP_MS 120000UL  // 2 minutes max warmup
  #define GPS_WARMUP_POLL 5000UL  // poll every 5 seconds

  String bootCoords = "";
  String locSource  = "";
  unsigned long warmupStart = millis();
  int warmupPoll = 0;

  while (millis() - warmupStart < GPS_WARMUP_MS) {
    warmupPoll++;
    unsigned long elapsed = (millis() - warmupStart) / 1000;
    unsigned long remaining = (GPS_WARMUP_MS - (millis() - warmupStart)) / 1000;

    String raw = sendAT("AT+CGPSINFO", 3000);
    GpsData gps = parseCgpsInfo(raw);

    if (gps.valid) {
      // ✅ GPS fix acquired!
      lastGps = gps;
      lastGpsCacheTime = millis();
      lastGpsLostTime  = 0;
      fixCount++;
      bootCoords = String(gps.latitude, 6) + "," + String(gps.longitude, 6);
      locSource  = "GNSS";
      syncDeadReckoning(gps.latitude, gps.longitude, gps.course);
      Serial.println("\n>>> GPS FIX ACQUIRED after " + String(elapsed) + "s!");
      Serial.println("    Lat: " + String(gps.latitude, 6));
      Serial.println("    Lon: " + String(gps.longitude, 6));
      Serial.println("    Maps: https://maps.google.com/?q=" +
                     String(gps.latitude, 6) + "," + String(gps.longitude, 6));
      playSound(SOUND_GPS_FIX); // 008.mp3 — "GPS fix acquired"
      delay(2000);
      break;
    }

    // No fix yet — show satellite progress
    int sats = getSatelliteCount();

    if (sats > 0 && !smsSentSats) {
      Serial.println(">> Satellites detected! Sending initial SMS...");
      sendSMSToAll("GPS: " + String(sats) + " satellites found. Searching for location fix...");
      smsSentSats = true;
    }

    Serial.print("[Warmup " + String(elapsed) + "s / " +
                 String(GPS_WARMUP_MS / 1000) + "s] Sats: ");
    Serial.print(sats >= 0 ? String(sats) : "?");
    Serial.print("  [");
    if (sats > 0) {
      for (int i = 0; i < 12; i++) Serial.print(i < sats ? "#" : "-");
    } else {
      Serial.print("------------");
    }
    Serial.println("]  (" + String(remaining) + "s left)");

    delay(GPS_WARMUP_POLL);
  }

  // ── If GPS warmup failed, try LBS/OpenCellID ──
  if (bootCoords.length() == 0) {
    Serial.println("\n>> GPS warmup ended — falling back to Cell-ID location...");
    bootCoords = getLocationWithSource(locSource);
  }

  // ── Boot SMS ──────────────────────────────────────────────────
  String bootMsg;
  if (bootCoords.length() > 0) {
    bootMsg = bootCoords + " (" + locSource + ")";
    playSound(SOUND_LOCATION_OK); // 002.mp3
  } else {
    bootMsg = "No location";
    playSound(SOUND_NO_LOCATION); // 007.mp3
  }
  Serial.println(">> Sending boot SMS: " + bootMsg);
  bool bootSmsSent = sendSMSToAll(bootMsg);
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
  updateDeadReckoning();

  // ── Fall detection (fast, every 50ms) ──
  if (checkFallDetected()) {
    playSound(SOUND_FALL_DETECTED); // 003.mp3
    delay(2000);

    // Get location for fall alert — LBS first for speed
    String fallSource;
    String fallCoords = getLocationWithSource(fallSource);
    String fallMsg = fallCoords.length() > 0
                         ? "FALL:" + fallCoords + " (" + fallSource + ")"
                         : "FALL:No location";

    if (fallCoords.length() > 0) {
      fallMsg = "FALL:" + fallCoords;
    } else {
      fallMsg = "FALL:No location";
    }
    Serial.println(">> Sending fall SMS: " + fallMsg);
    bool fallSmsSent = sendSMSToAll(fallMsg);
    Serial.println(fallSmsSent ? ">> Fall SMS sent" : ">> Fall SMS failed");
    Serial.println("[HTTP] Pre-send check:");
      sendAT("AT+CSQ", 3000);
      sendAT("AT+CGPADDR=1", 3000);
      sendAT("AT+NETOPEN?", 3000);
    // Sample IMU and send SOS via HTTP once per fall
    sensors_event_t aEv, gEv, tEv;
    if (mpuReady) {
      mpu.getEvent(&aEv, &gEv, &tEv);
      float gx_dps = gEv.gyro.x * 180.0 / PI;
      float gy_dps = gEv.gyro.y * 180.0 / PI;
      float gz_dps = gEv.gyro.z * 180.0 / PI;
      bool sosOk = sendHttpSOS(aEv.acceleration.x, aEv.acceleration.y, aEv.acceleration.z,
                               gx_dps, gy_dps, gz_dps);
      Serial.println(sosOk ? ">> SOS API acknowledged" : ">> SOS API not acknowledged");
    } else {
      Serial.println(">> MPU not ready; skipping SOS API send");
    }

  }

  // ── Ultrasonic Obstacle Detection Logging and Beeping ──
  // (Removed from loop() because it is now handled automatically by the ultrasonicTask on Core 0)

  // ── Send ONE clean SMS every 1 minute ──
  if (millis() - lastSmsTime >= SMS_INTERVAL) {
    lastSmsTime = millis();

    String source;
    String coords = getLocationWithSource(source);

    if (coords.length() > 0 && source == "GNSS") {
      String manilaTime = getManilaTime();

      // Only send SMS when fresh GNSS fix from the GPS antenna
      String msg = coords + " (" + source + ")";
      if (manilaTime.length() > 0) {
        msg += " " + manilaTime;
      }

      Serial.println(">> Sending SMS: " + msg);
      bool success = sendSMSToAll(msg);

      if (success) {
        playSound(SOUND_SMS_SENT);
        Serial.println("─── SMS SENT ───");
      }
    } else if (coords.length() > 0) {
      Serial.println(">> Skipping SMS — source is '" + source + "' (GNSS only mode)");
    } else {
      Serial.println(">> No location this cycle (waiting for GNSS fix)");
    }
  }

  // ── HTTP heartbeat every 30 seconds using last known location ──
  if (millis() - lastHttpTime >= HTTP_HEARTBEAT_INTERVAL) {
    lastHttpTime = millis();

    float hLat = NAN, hLon = NAN;
    float acc = 5.0; // default/sample accuracy if unknown

    // Prefer current fresh GNSS only; otherwise try LBS explicitly
    String src;
    String coords = getLocationWithSource(src);
    if (coords.length() > 0) {
      int comma = coords.indexOf(',');
      if (comma > 0) {
        hLat = coords.substring(0, comma).toFloat();
        hLon = coords.substring(comma + 1).toFloat();
      }
      if (src.startsWith("GNSS")) acc = 5.0; // assume good accuracy from GNSS
      else if (src == "LBS") acc = 500.0;    // LBS coarse
      else if (src == "Dead Reckoning") acc = 50.0;
    }

    if (!isnan(hLat) && !isnan(hLon) && hLat != 0 && hLon != 0) {
      sendHttpLocation(hLat, hLon, acc);
    } else {
      Serial.println("[HTTP] Heartbeat skipped — no coordinates available");
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

      Serial.println("[HTTP] Pre-send check:");
        sendAT("AT+CSQ", 3000);
        sendAT("AT+CGPADDR=1", 3000);
        sendAT("AT+NETOPEN?", 3000);
      // Send immediate HTTP when a fresh GNSS fix is obtained
      sendHttpLocation(gps.latitude, gps.longitude, 5.0);

      // Keep minimal state but avoid relying on it for sending later
      lastGps = gps;
      lastGpsCacheTime = millis();
      lastGpsLostTime  = 0;
      fixCount++;
      syncDeadReckoning(gps.latitude, gps.longitude, gps.course);

      Serial.println("────────────────────────────────");
      Serial.println(">>> GPS FIX #" + String(fixCount));
      Serial.println("  Lat:   " + String(gps.latitude, 6));
      Serial.println("  Lon:   " + String(gps.longitude, 6));
      Serial.println("  Alt:   " + String(gps.altitude, 1) + " m");
      Serial.println("  Speed: " + String(gps.speedKnots, 2) + " knots");
      Serial.println("  Maps:  https://maps.google.com/?q=" +
                     String(gps.latitude, 6) + "," + String(gps.longitude, 6));
      Serial.println("────────────────────────────────");

      // Send to API immediately on fresh fix
      bool locOk = sendHttpLocation(gps.latitude, gps.longitude, 5.0);
      if (locOk) {
        Serial.println(">> Live location sent to API");
      } else {
        Serial.println(">> Live location send failed");
      }

    } else {
      // Track how long GPS has been lost
      if (lastGpsLostTime == 0) lastGpsLostTime = millis();
      unsigned long lostFor = millis() - lastGpsLostTime;

      // Caching disabled; no action on cache expiry
      if (lostFor >= GPS_LOST_CLEAR_MS) {
        smsSentSats = false; // allow early satellite notification again
      }
      // No fix yet — show satellite progress
      int sats = getSatelliteCount();

      if (sats > 0 && !smsSentSats) {
        Serial.println(">> Satellites detected! Sending SMS...");
        sendSMSToAll("GPS: " + String(sats) + " satellites found. Searching for location fix...");
        smsSentSats = true;
      }

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