# AeroSaffron Smart Aeroponics System

AeroSaffron is an ESP32 + Firebase + web dashboard project for monitoring and controlling an aeroponics chamber. The system reads temperature, humidity, and Lux, pushes live data to Firebase Realtime Database, and controls the mist pump, fan, grow light, and Peltier cooler through relay outputs.

The current version supports:

- Temperature, humidity, and Lux monitoring
- Device online/offline heartbeat
- Auto and Manual control modes
- Manual relay control from the dashboard
- Automatic Fan, Light, and Peltier control using min/max thresholds
- Mist pump cycle automation
- OLED display pages for sensors, Lux, devices, and system status
- Firebase Realtime Database schema exports
- Static website dashboard with Chart.js

## Project Files

```text
New folder/
  dashboard.html
  app.js
  style.css
  index.html
  README.md
  aerosaffron.json
  aeroponics-53851-default-rtdb-export.json
  AeroSaffron_ESP32/
    AeroSaffron_ESP32.ino
```

## System Architecture

```text
Sensors + Relays + OLED
        |
      ESP32
        |
      WiFi
        |
Firebase Realtime Database
        |
Web Dashboard
```

The dashboard never controls GPIO pins directly. It writes commands to Firebase. The ESP32 listens to Firebase and triggers the relay pins.

## Hardware Connections

| Module | ESP32 Pin | Notes |
| --- | ---: | --- |
| DHT22 data | GPIO 4 | Temperature and humidity |
| Water level analog sensor | GPIO 34 | Legacy reading, no longer shown on dashboard |
| LDR / light sensor | GPIO 35 | Converted to Lux approximation |
| Mist pump relay | GPIO 26 | Relay output |
| Fan relay | GPIO 27 | Relay output |
| LED grow light relay | GPIO 14 | Relay output |
| Peltier relay | GPIO 12 | Relay output |
| OLED SDA | GPIO 21 | I2C |
| OLED SCL | GPIO 22 | I2C |

The firmware assumes common active-low relay modules:

```cpp
#define RELAY_ACTIVE_LOW true
```

If your relay module is active-high, change it to:

```cpp
#define RELAY_ACTIVE_LOW false
```

## Required Arduino Libraries

Install these in Arduino IDE Library Manager:

- Firebase ESP Client by Mobizt
- DHT sensor library by Adafruit
- Adafruit Unified Sensor
- Adafruit SSD1306
- Adafruit GFX Library
- ArduinoJson

## ESP32 Setup

Open:

```text
AeroSaffron_ESP32/AeroSaffron_ESP32.ino
```

Update these values before uploading:

```cpp
#define WIFI_SSID        "YOUR_WIFI_NAME"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define FIREBASE_HOST    "YOUR_DATABASE_HOST"
#define FIREBASE_AUTH    "YOUR_DATABASE_SECRET_OR_TOKEN"
```

Then upload the sketch to your ESP32 from Arduino IDE.

Important: relay control will not work with only the website update. The ESP32 must be flashed with the updated firmware because Auto/Manual mode and threshold logic are handled inside the ESP32 sketch.

## Firebase Database Schema

Use either of these files as the initial database import:

```text
aerosaffron.json
aeroponics-53851-default-rtdb-export.json
```

Current schema:

```json
{
  "aerosaffron": {
    "sensors": {
      "temperature": 25,
      "humidity": 60,
      "lux": 200,
      "light": 200
    },
    "controls": {
      "pump": 0,
      "light": 0,
      "fan": 0,
      "peltier": 0,
      "_refresh": 0
    },
    "settings": {
      "controlMode": "auto",
      "tempSet": 23,
      "humSet": 65,
      "mistDuration": 30,
      "mistInterval": 15,
      "ledStart": "06:00",
      "ledEnd": "22:00",
      "fanMin": 24,
      "fanMax": 30,
      "lightMin": 120,
      "lightMax": 350,
      "peltierMin": 18,
      "peltierMax": 26
    },
    "device": {
      "online": false,
      "lastSeen": 0,
      "ip": ""
    },
    "alerts": {
      "msg": "System Normal"
    },
    "automation": {
      "rule1": {
        "condition": "temperature > 30",
        "action": "fan_on",
        "status": 1
      },
      "rule2": {
        "condition": "lux < 120",
        "action": "light_on",
        "status": 1
      },
      "rule3": {
        "condition": "temperature > 26",
        "action": "peltier_on",
        "status": 1
      }
    }
  }
}
```

## Control Modes

### Auto Mode

In Auto mode, manual dashboard toggles are disabled. The ESP32 automatically controls devices from threshold settings:

| Device | ON condition | OFF condition |
| --- | --- | --- |
| Fan | `temperature >= fanMax` | `temperature <= fanMin` |
| Light | `lux <= lightMin` | `lux >= lightMax` |
| Peltier | `temperature >= peltierMax` | `temperature <= peltierMin` |
| Mist pump | Humidity/mist cycle logic | Mist duration completed |

### Manual Mode

In Manual mode, dashboard toggles are enabled. Pressing a toggle writes to:

```text
aerosaffron/controls/{device}
```

The ESP32 stream receives the command and triggers the relay pin.

When Auto/Manual is switched, the website updates:

```text
aerosaffron/settings/controlMode
aerosaffron/controls/_refresh
```

The `_refresh` value wakes the ESP32 controls stream so the firmware immediately reloads the latest control mode.

## Website Dashboard

Main dashboard file:

```text
dashboard.html
```

Main JavaScript file:

```text
app.js
```

Main stylesheet:

```text
style.css
```

The dashboard includes:

- Sensor cards for temperature, humidity, and Lux
- Device online/offline status
- Pump, Fan, Light, and Peltier status
- Auto/Manual mode buttons
- Manual device toggles
- Threshold settings
- Automation rules
- Alerts table
- Analytics charts
- CSV export

## Running the Website Locally

Because `app.js` is loaded as a JavaScript module, run a local server instead of opening `dashboard.html` directly.

From the project folder:

```powershell
python -m http.server 8080 -b 127.0.0.1
```

Open:

```text
http://127.0.0.1:8080/dashboard.html
```

## OLED Display

The firmware initializes SSD1306 OLED displays at:

- `0x3C`
- fallback `0x3D`

If OLED data is not shown:

1. Check SDA is on GPIO 21.
2. Check SCL is on GPIO 22.
3. Check OLED VCC and GND.
4. Confirm the display is SSD1306 128x64.
5. Confirm the I2C address is `0x3C` or `0x3D`.

OLED pages rotate every 2 seconds:

- Temperature and humidity
- Lux monitor
- Device status
- WiFi/Firebase/system status

## Device Online/Offline Status

The ESP32 pushes heartbeat data to:

```text
aerosaffron/device
```

Fields:

- `online`: ESP32 online flag
- `lastSeen`: timestamp in milliseconds
- `ip`: ESP32 local IP address

The dashboard marks the device offline if the heartbeat becomes stale.

## Troubleshooting Relay Control

If dashboard buttons do not trigger relays:

1. Upload the updated ESP32 sketch.
2. Switch the dashboard to Manual mode.
3. Confirm Firebase changes under `aerosaffron/controls`.
4. Open Serial Monitor at `115200`.
5. Confirm the ESP32 prints stream events.
6. Check `RELAY_ACTIVE_LOW` matches your relay module.
7. Check relay VCC, GND, and GPIO wiring.
8. Confirm your Firebase host and auth token are correct.

Expected manual control flow:

```text
Dashboard button
  -> Firebase controls value changes
  -> ESP32 controls stream receives update
  -> ESP32 calls applyDeviceStates()
  -> Relay pin changes state
```

## Troubleshooting Auto Mode

If Auto mode does not turn devices on/off:

1. Confirm `settings/controlMode` is `auto`.
2. Confirm threshold values are valid.
3. Confirm `fanMin < fanMax`.
4. Confirm `lightMin < lightMax`.
5. Confirm `peltierMin < peltierMax`.
6. Confirm sensor values are updating in `aerosaffron/sensors`.
7. Watch Serial Monitor for `[Auto]` messages.

## Notes

- The website displays Lux instead of water level.
- `sensors/light` is kept for backward compatibility.
- `sensors/lux` is the preferred current field.
- The water level sensor may still be read by firmware, but it is not shown on the dashboard.
- Do not expose production Firebase credentials in a public repository.

## Validation Performed

During the latest update:

- `app.js` syntax check passed.
- Firebase JSON schema files parsed successfully.
- Local dashboard server responded successfully.

Arduino compilation was not run in this workspace because `arduino-cli` is not installed.
