# 🛡️ MicroFlux: Hybrid IoT Smart Anti-Theft Tracker

[![Flutter](https://img.shields.io/badge/Flutter-%2302569B.svg?style=flat-square&logo=Flutter&logoColor=white)](https://flutter.dev)
[![Arduino / C++](https://img.shields.io/badge/Arduino%20C%2B%2B-00599C?style=flat-square&logo=c%2B%2B&logoColor=white)](https://www.arduino.cc/)
[![ESP32](https://img.shields.io/badge/ESP32-Espressif-E7352C?style=flat-square&logo=espressif&logoColor=white)](https://www.espressif.com)
[![Firebase](https://img.shields.io/badge/Firebase-RealtimeDB-039BE5?style=flat-square&logo=Firebase&logoColor=white)](https://firebase.google.com)
[![ThingSpeak](https://img.shields.io/badge/ThingSpeak-MQTT--Broker-blue?style=flat-square)](https://thingspeak.com)

MicroFlux is a hybrid hardware-software IoT security system designed to prevent theft and track valuables (e.g., bags, vehicles) in real-time. It leverages an ESP32 microcontroller interfaced with an Ai-Thinker A9G cellular/GPS module, linked to a Firebase Realtime Database and controlled via a premium Flutter mobile application.

---

## ⚡ Key Features

*   **Dual-Mode Localization:** High-accuracy GPS tracking combined with cellular tower LBS (Location Based Services) fallback for indoor tracking.
*   **Smart Wi-Fi Safe-Zones:** Automatically senses home/safe Wi-Fi networks to dynamically switch off high-power GPRS cellular tracking, extending battery life.
*   **Independent Warning Systems:** Dynamic, non-blocking warning alarms (Beep-Beep warning tone) on GPIO 13 and LED alerts on GPIO 14.
*   **Local Battery ADC Sensing:** Hardware-level voltage division monitored locally via ESP32 ADC (GPIO 34) for accurate live battery metrics.
*   **Live GSM Signal Tracking:** Continuous signal strength (CSQ) polling, allowing the mobile app to monitor cellular connection quality (Excellent, Good, Poor, Weak, No Signal) in real-time.
*   **Remote SMS Configuration & Sign Out:** Reset or update tracker Wi-Fi credentials via SMS commands (`WIFI:SSID,Password` or `WIFI:RESET`).
*   **Instant Command Delivery:** Hybrid connection management (Wi-Fi HTTP / GPRS MQTT) for instant warning controls, threshold configurations, and tamper resets.

---

## 📐 System Architecture

The following diagram illustrates the hardware communication and cloud synchronization of the MicroFlux project:

```mermaid
graph TD
    %% Hardware Layer
    subgraph Hardware Layer (Tracker Module)
        ESP32[ESP32 Main MCU]
        A9G[A9G GPS/GPRS Module]
        Buzzer[Piezo Buzzer - GPIO 13]
        LED[Warning LED - GPIO 14]
        Reed[Reed Tamper Switch - GPIO 26]
        Divider[Resistor Voltage Divider - GPIO 34]

        ESP32 <-->|UART2 Serial Link| A9G
        ESP32 -->|PWM/GPIO| Buzzer
        ESP32 -->|GPIO| LED
        ESP32 <-- |GPIO Pullup| Reed
        ESP32 <-- |Analog Read| Divider
    end

    %% Network & Cloud Layer
    subgraph Cloud Layer
        Firebase[Firebase Realtime Database]
        TS[ThingSpeak MQTT Broker]
    end

    %% User Layer
    subgraph Mobile Layer
        App[Flutter Mobile Application]
    end

    %% Connections
    ESP32 <-->|Wi-Fi HTTP / Firebase REST| Firebase
    A9G <-->|GPRS Cellular MQTT| TS
    TS -.->|ThingSpeak Proxy Relay| Firebase
    App <-->|Firebase SDK / Live Sync| Firebase
```

---

## 🔌 Hardware Pinout & Wiring

| ESP32 Pin | Connected Component | Description |
| :--- | :--- | :--- |
| **GPIO 13 (IO13)** | Buzzer (Active Piezo) | Positive terminal of warning alarm buzzer |
| **GPIO 14 (IO14)** | Warning LED | Active-high LED warning indicator |
| **GPIO 15 (IO15)** | A9G PWR_KEY | Connected to A9G Power pin via transistor to boot/reset the module |
| **GPIO 26 (IO26)** | Reed Switch | Magnetic tamper detection sensor (Input Pull-up) |
| **GPIO 34 (IO34)** | Battery Divider | Analog Input connected to middle of a 100k/100k voltage divider |
| **GPIO 16 (RX2)** | A9G TXD | Serial receive pin from cellular module |
| **GPIO 17 (TX2)** | A9G RXD | Serial transmit pin to cellular module |
| **GND** | Shared Ground | Connected to ESP32 GND, A9G GND, and LM2596 Output Negative |
| **VBAT (A9G)** | LM2596 Out (+) | Regulated 4.2V power supply for cell registration surges |

> [!IMPORTANT]
> Because cellular network registration and LBS sweeps can cause current surges up to 2A, it is mandatory to solder two thick copper bypass wires from the LM2596 buck converter output directly to the A9G's VBAT and GND pins to prevent brownout reboots.

---

## 📱 Mobile App UI Preview & Configurations

The Flutter mobile application provides a premium visual interface with the following dashboard components:
*   **Dynamic Title Header:** Color-shifts between Green (System Secure) and Red (Proximity Alert / Tamper Triggered).
*   **Sign Out (Logout):** Top-right header button clears saved preferences, terminates local background sync services, and opens the SMS app pre-filled with `WIFI:RESET` to clean the device configuration.
*   **Battery Status Gauge:** Custom-painted visual indicator syncing with ESP32 local ADC.
*   **GSM Signal Strength Card:** Dynamic cellular strength parser displaying network quality descriptions (Excellent, Good, Poor, Weak, No Signal) with matching status bar colors.

---

## ✉️ Remote SMS Commands Reference

You can configure the tracker by sending these SMS commands directly to the SIM card inside the module:

*   **`WIFI:SSID,Password`**
    Updates the Wi-Fi credentials on the tracker. The device will reply with a confirmation SMS and reboot to connect to the new network.
*   **`WIFI:RESET`** / **`WIFI:CLEAR`**
    Clears all saved Wi-Fi credentials from the tracker's flash memory. The device will reply and reboot into pure GPRS cellular tracking mode.
*   **`STATUS`**
    Returns a text report containing the current GPS latitude/longitude, Google Maps coordinate link, local battery level, tamper alarm status, GPRS connection state, and ringcut status.
*   **`APN:ApnName`**
    Updates the cellular network APN (defaults to `mobitel`).

---

## 🚀 Getting Started

### 1. ESP32 Firmware Installation
1. Open the [esp32_tracker.ino](esp32_tracker/esp32_tracker.ino) file in Arduino IDE.
2. Install dependencies: `TinyGPS++`, `Preferences` (built-in).
3. Configure your ThingSpeak keys and Firebase credentials inside the code.
4. Select board `ESP32 Dev Module` and flash the code.

### 2. Flutter Mobile App Setup
1. Ensure Flutter SDK is installed on your PC.
2. Run `flutter pub get` in the project root directory.
3. Configure your Firebase project options inside `lib/firebase_options.dart`.
4. Connect your mobile phone and run `flutter run` to launch the app!
