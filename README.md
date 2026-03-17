# A7680C GPS Tracker — Multi-Constellation & Fall Detection

A sophisticated ESP32-based GPS tracking system utilizing the **A7680C 4G LTE + GNSS** module. This project features multi-constellation support (GPS, BDS, GLONASS), fall detection, intelligent location fallback (LBS), and interactive voice/audio alerts.

## 🚀 Key Features

- **Advanced Tracking**: High-accuracy positioning using GPS, BDS, and GLONASS.
- **Intelligent Fallback**: Automatically switches to **LBS (Location Based Services)** via cell towers when GNSS signal is lost indoors or in tunnels.
- **Fall Detection**: 6-axis monitoring via **MPU-6050** with an emergency SOS protocol.
- **Voice/Audio Alerts**: Real-time status updates via **DFPlayer Mini** (Internet status, GPS fix, Fall alerts).
- **Proximity Radar**: Independent task (Core 0) using an **HC-SR04 Ultrasonic Sensor** for obstacle detection with variable-rate beeping.
- **Dead Reckoning**: Basic step-detection positioning using IMU data when all satellite/cell signals are lost.
- **Cloud Connectivity**: Real-time data logging to a remote API and SMS alert system.

---

## 🛠️ Hardware Components

| Component | Description |
| :--- | :--- |
| **ESP32** | Main Microcontroller (Logic & Processing) |
| **A7680C** | 4G LTE Modem with Integrated GNSS (GPS/GLONASS/BDS) |
| **MPU-6050** | 6-Axis Accelerometer & Gyroscope (Fall Detection) |
| **DFPlayer Mini** | MP3 Player Module (Voice Feedback) |
| **HC-SR04** | Ultrasonic Distance Sensor (Obstacle Detection) |
| **Buzzer** | Active Buzzer for status and proximity tones |

---

## 📌 Pin Configuration

### ESP32 Pin Mapping

| Peripheral | ESP32 Pin | Peripheral Pin | Notes |
| :--- | :---: | :---: | :--- |
| **A7680C Modem** | **16** | TXD | Connect ESP32 RX to Modem TX |
| | **17** | RXD | Connect ESP32 TX to Modem RX |
| | **5** | PWRKEY | Pulsed to power on/off the modem |
| **DFPlayer Mini** | **27** | TX | Connect ESP32 RX to Player TX |
| | **26** | RX | Connect ESP32 TX to Player RX |
| **MPU-6050** | **21** | SDA | I2C Data Line |
| | **22** | SCL | I2C Clock Line |
| **HC-SR04** | **33** | TRIG | Trigger Pin |
| | **34** | ECHO | Echo Pin |
| **Buzzer** | **4** | Signal | Active High |

---

## 📂 SD Card Setup (MP3 Files)

The **DFPlayer Mini** requires an SD card with a folder named `mp3` (or files in the root depending on library config). The following files should be present:

| File Name | Voice Announcement / Sound |
| :--- | :--- |
| `001.mp3` | "Internet connected" |
| `002.mp3` | "Location ready" |
| `003.mp3` | "Fall detected! Sending alert!" |
| `004.mp3` | "Tracker online" |
| `005.mp3` | "SMS sent" |
| `006.mp3` | "Warning: No internet" |
| `007.mp3` | "Warning: Location unavailable" |
| `008.mp3` | "GPS fix acquired" |
| `009.mp3` | "Searching for satellites" |

---

## ⚙️ Configuration

- **APN**: Default is set to `"internet"`. Modify this in the `.ino` file to match your SIM provider.
- **Phone Number**: Update the `#define PHONE_NUMBER` with the emergency contact number.
- **Fall Threshold**: Currently set to `2.5G`. Adjust `FALL_THRESHOLD` based on sensitivity requirements.

## 📡 API Integration

The device sends JSON-formatted data to:
- **Location**: `http://gateway.ejeepdev.site/spc/api/v1/Live_Location.php`
- **SOS/Fall**: `http://gateway.ejeepdev.site/spc/api/v1/SOS_Alert.php`
