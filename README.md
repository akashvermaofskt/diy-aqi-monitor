# DIY AQI Monitor (ESP8266 + SDS011)

A low-cost, real-time **Air Quality Index (AQI) monitor** built using an **ESP8266** and **Nova SDS011** particulate matter sensor.  
It provides a modern web dashboard with live updates, burst sampling, and sensor life-extending duty cycling.

## Photos
- DIY Air purifier
<img width="600" height="800" alt="image" src="https://github.com/user-attachments/assets/2d72ecc4-bb28-4725-b0d4-5c350a5bd5d4" />


- Nova PM Sensor SDS011
<img width="600" height="1000" alt="image" src="https://github.com/user-attachments/assets/a2d8e516-9529-4830-b259-5025a40131b9" />


- ESP8266 with the PM Sensor
<img width="800" height="600" alt="image" src="https://github.com/user-attachments/assets/de618a92-07e6-4479-a881-62ab95888f92" />


- AQI Monitor UI
<img width="200" height="450" alt="image" src="https://github.com/user-attachments/assets/2fe00b02-b4b3-4431-bb47-4bccf868b96c" />
<img width="200" height="450" alt="image" src="https://github.com/user-attachments/assets/0813eb01-f11d-4ff9-bf7a-9f25a2b8c5d9" />
<img width="200" height="450" alt="image" src="https://github.com/user-attachments/assets/5bb7fc6b-2631-40f6-b8f3-f6c6b0a2d449" />
<img width="200" height="450" alt="image" src="https://github.com/user-attachments/assets/caf907a3-ac7a-4600-b2ec-51978ebf06bc" />
<img width="200" height="450" alt="image" src="https://github.com/user-attachments/assets/2a8db2d9-ac21-4eb1-96f5-b41a2751fb69" />




---

## Features
- 📊 Live **PM2.5, PM10 & Indian AQI** (CPCB standard)
- 🌐 Built-in **web dashboard** (no app required)
- 🔄 **Real-time updates** (no page reload)
- ⚡ **Burst Mode (60s)** for continuous measurements
- 💤 **Duty-cycled sensor** to prolong laser & fan life
- 📱 Works on phone / laptop (same Wi-Fi)
- 💸 Built under ₹2,500 total cost

---

## Hardware Used
- ESP8266 (NodeMCU / ESP-12)
- Nova SDS011 PM2.5 / PM10 sensor
- Breadboard + jumper wires
- 5V power source (USB / power bank)

---

## Wiring
| SDS011 | ESP8266 |
|------|--------|
| 5V   | VIN (5V) |
| GND  | GND |
| TX   | D5 (GPIO14) |
| RX   | D6 (GPIO12) |

> ⚠️ Power SDS011 from **VIN (5V)**, not 3.3V.

---

## Setup
1. Open the `.ino` file in **Arduino IDE**
2. Install:
   - ESP8266 board support
   - `SoftwareSerial`
3. Update Wi-Fi credentials:
   ```cpp
   const char* ssid = "YOUR_WIFI";
   const char* password = "YOUR_PASSWORD";

4. Upload the sketch

5. Open Serial Monitor @ 115200 baud

6. Note the printed IP:

AQI Dashboard -> http://192.168.1.50

## Usage
- Open the IP address in a browser
- Refresh Now → triggers a fresh measurement cycle
- Burst 60s → runs continuous sampling for 1 minute
- Sensor automatically sleeps between cycles to reduce wear

## Sensor Behavior
- Warm-up: ~30 seconds
- Sampling window: ~10 seconds
- Measurement cycle: every 5 minutes
- Burst mode bypasses sleep temporarily

## Accuracy Notes
- SDS011 is good for trend & relative AQI
- Absolute accuracy: ±10–15%
- Best used for indoor monitoring & purifier effectiveness

## Future Improvements
- AP (hotspot) mode for portable use
- OTA firmware updates
- Persistent configuration (EEPROM)
- MQTT / Home Assistant integration
- OLED display or RGB AQI indicator
