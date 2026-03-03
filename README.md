# A7680C GPS/GNSS for ESP32 — GNSS + Auto-Restart + Fall/SMS

This repository contains ESP32 Arduino sketches for the SIMCOM A7680x module focused on GNSS (GPS + BDS + GLONASS) tracking with optional fall detection, MP3 feedback, and SMS reporting.

Branch overview:
- LBS: Uploaded base files and references
- GNSS: Adds a pure GNSS tracker example (GPS + BDS + GLONASS), auto-restart on no-fix, and this README

Manual
- A76XX_Series_AT_Command_Manual_V1.12.pdf (included in the repo root)

GNSS Example
- Path: A7680C_GPS_BDS_GLONASS/A7680C_GPS_BDS_GLONASS.ino
- Core features:
  - Pure GNSS mode with multi-constellation: GPS + BDS + GLONASS
  - Configurable start mode (Normal/Warm/Cold/Hot)
  - Auto-restart GNSS if no valid fix for N minutes (max 5 retries)
  - Optional fall detection with Adafruit MPU6050
  - Audible feedback using DFPlayer Mini (e.g., “searching satellites”, “GPS fix”, “fall detected”)
  - Periodic SMS with coordinates and Google Maps link

Hardware (ESP32)
- A7680x module serial: MODEM_TX = 16, MODEM_RX = 17, PWRKEY_PIN = 5
- DFPlayer Mini serial: MP3_RX = 26, MP3_TX = 27
- I2C for MPU6050: SDA = 21, SCL = 22

Libraries
- Adafruit_MPU6050
- Adafruit_Sensor
- DFRobotDFPlayerMini
- Wire (built-in)
- HardwareSerial (built-in)

Configuration (top of the sketch)
- GNSS_START_MODE: 0=Normal, 1=Warm (recommended), 2=Cold, 3=Hot
- AUTO_RESTART_TIMEOUT: minutes without valid fix before auto-restart (0=disable)
- AUTO_RESTART_TYPE: 1=Warm, 2=Cold, 3=Hot
- PHONE_NUMBER: destination for SMS updates

AT Commands used (examples)
- Power and status: AT+CGNSSPWR=1, AT+CGNSSPWR?
- Start mode: AT+CGPSWARM (warm), AT+CGPSCOLD (cold), AT+CGPSHOT (hot)
- Mode selection: AT+CGNSSMODE=7 (GPS + BDS + GLONASS)
- GNSS info: AT+CGNSSINFO

Operation summary
1) Module is powered, echo disabled (ATE0), verbose errors enabled (AT+CMEE=2)
2) GNSS is initialized, mode set to GPS+BDS+GLONASS, and start mode applied
3) On boot, attempts to obtain a GNSS fix and sends an initial SMS (or “waiting for fix”)
4) Loop performs:
   - Fall detection check
   - Periodic location SMS
   - GNSS info polling for live status
   - Auto-restart if no fix for configured timeout

Notes
- Ensure MP3 tracks on DFPlayer are correctly numbered to match SOUND_* constants in the sketch
- SMS interval, fall thresholds, and polling intervals are configurable via constants in the sketch

Build/Flash
- Platform: ESP32 (Arduino)
- Open A7680C_GPS_BDS_GLONASS/A7680C_GPS_BDS_GLONASS.ino in Arduino IDE or PlatformIO
- Install the listed libraries
- Select your ESP32 board and serial port, then compile and upload

Wiring tips
- Keep GNSS antenna away from high-noise sources
- Provide stable power to A7680x module; large current spikes may occur during GNSS activity

License
- Provided as-is without warranty. See individual library licenses for details.
