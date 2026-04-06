# 🌳 ArborGuard v2.0 — Tree Protection System

> Real-time tree cutting detection and environmental monitoring using ESP32, sensor fusion, OLED display, and Telegram alerts.

**Author:** Sivakumar K · Sathyabama Institute of Science and Technology · ECE 2028  

---

## What It Does

ArborGuard monitors a tree 24/7 and sends instant alerts to your phone via Telegram when:

- 🚨 **Tree cutting is detected** — vibration and sound sensors fire together for >500 ms (sensor fusion)
- ⚠ **Soil is too dry** — moisture drops below 20%
- 🌡 **Heat or drought stress** — temperature exceeds 40°C or humidity drops below 25%

A 1.3" OLED display shows live readings on 3 rotating pages. The Telegram bot responds to commands so you can query the node remotely at any time.

---

## Hardware

| Component | Model | Connection |
|-----------|-------|------------|
| Microcontroller | ESP32 DevKit V1 | — |
| Display | SH1106 1.3" OLED | I²C — SDA=GPIO21, SCL=GPIO22 |
| Light sensor | BH1750 | I²C — SDA=GPIO21, SCL=GPIO22 (shared bus) |
| Temp/Humidity | DHT22 (AM2302) | Single-wire — GPIO27 |
| Soil moisture | Capacitive analog module | AO → GPIO34 |
| Vibration | SW-420 analog module | AO → GPIO35 |
| Sound | KY-038 analog module | AO → GPIO32 |
| Buzzer | Active piezo buzzer | GPIO25 |

> **Power:** All sensors run on the ESP32's 3.3 V rail. Do NOT use 5 V — GPIO pins are not 5 V tolerant.

### Wiring Quick Reference

```
ESP32 GPIO21 (SDA) ──── OLED SDA
                   └─── BH1750 SDA

ESP32 GPIO22 (SCL) ──── OLED SCL
                   └─── BH1750 SCL

ESP32 GPIO27       ──── DHT22 DATA  (+ 10 kΩ pull-up to 3.3 V if bare sensor)
ESP32 GPIO34       ──── Soil AO
ESP32 GPIO35       ──── Vibration AO
ESP32 GPIO32       ──── Sound AO
ESP32 GPIO25       ──── Buzzer +
ESP32 3.3V         ──── All VCC
ESP32 GND          ──── All GND
```

---

## Software Setup

### 1 — Install Arduino IDE & ESP32 board package

1. Download [Arduino IDE 2.x](https://www.arduino.cc/en/software)
2. Go to **File → Preferences** and add this URL to *Additional boards manager URLs*:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Go to **Tools → Board → Boards Manager**, search `esp32`, install **esp32 by Espressif Systems**

### 2 — Install libraries

Open **Sketch → Include Library → Manage Libraries** and install:

| Library | Author | Version tested |
|---------|--------|---------------|
| U8g2 | Oliver Kraus | 2.35+ |
| BH1750 | Christopher Laws | 1.3+ |
| DHT sensor library | Adafruit | 1.4+ |
| Adafruit Unified Sensor | Adafruit | 1.1+ (dependency) |
| ArduinoJson | Benoit Blanchon | 7.x |

> WiFi, WiFiClientSecure, HTTPClient and esp_task_wdt are bundled with the ESP32 core — no separate install needed.

### 3 — Configure credentials

```bash
# In the ArborGuard folder:
cp secrets.h.template secrets.h
```

Open `secrets.h` and fill in your values:

```cpp
#define WIFI_SSID  "YourNetworkName"
#define WIFI_PASS  "YourPassword"
#define BOT_TOKEN  "1234567890:ABCdefGHI..."   // from @BotFather
#define CHAT_ID    "987654321"                  // from @userinfobot
```

> ⚠ `secrets.h` is gitignored. It will never be uploaded if you push to GitHub.

### 4 — Get Telegram credentials

1. Open Telegram, search **@BotFather**, send `/newbot`
2. Follow the prompts — copy the token you receive
3. Search **@userinfobot**, press Start — it replies with your Chat ID
4. Search your new bot's username and press **Start** (required before it can message you)

### 5 — Flash

1. Select **Tools → Board → ESP32 Dev Module**
2. Select the correct COM port
3. Click **Upload**
4. Open Serial Monitor at **115200 baud** to watch boot output

---

## Telegram Bot Commands

| Command | Description |
|---------|-------------|
| `/start` | Same as /status — confirms bot is alive |
| `/status` | Alert state, IP address, uptime |
| `/sensors` | Live readings from all 6 sensors |
| `/test` | Send a test message to verify bot works |
| `/silence` | Acknowledge and silence the current alert |
| `/help` | Show the command list |

Automatic messages sent without any command:
- **Boot message** — sent once on power-up
- **Cutting / soil / heat alerts** — sent when thresholds are exceeded
- **5-minute status report** — passive heartbeat so you know the node is alive

---

## Threshold Tuning

All tuneable values are in the `Thresh::` namespace near the top of `ArborGuard.ino`:

```cpp
namespace Thresh {
  constexpr int   VIB_HIGH       = 1400;  // vibration ADC trigger level
  constexpr int   SOUND_HIGH     = 1600;  // sound ADC trigger level
  constexpr float LIGHT_LOW_LUX  = 80.0f; // below this = low light warning
  constexpr float TEMP_HIGH_C    = 40.0f; // above this = heat stress alert
  constexpr float HUM_LOW_PCT    = 25.0f; // below this = drought alert
  // ...
}
```

To test alerts without disturbing the tree, temporarily lower `VIB_HIGH` and `SOUND_HIGH` to ~200 and tap both sensors gently. Restore original values after testing.

---

## OLED Display Pages

The display cycles through 3 pages every 2.5 seconds:

| Page | Shows |
|------|-------|
| **Page 1 — ENV & SOIL** | Temperature, humidity, soil moisture bar, light level |
| **Page 2 — VIB & SOUND** | Vibration and sound levels with HIGH/ok status, fusion state |
| **Page 3 — SYSTEM** | Alert state, WiFi status, uptime |

When an alert fires, the display overrides to a fullscreen alert for 8 seconds, then returns to normal paging.

---

## Architecture Notes

The firmware uses a **cooperative multitasking scheduler** — there are no RTOS tasks or blocking `delay()` calls in the main loop. Each subsystem checks elapsed time with `millis()` and only runs when its interval has passed:

```
loop() runs ~every 1–2 ms
  ├── wifiTask()       checks every 30s if WiFi dropped
  ├── pollBot()        polls Telegram getUpdates every 3s
  ├── periodicStatus() sends 5-min report
  ├── dhtTask()        reads DHT22 every 2s (sensor limit)
  ├── sensor reads     every 300ms (with 8-sample moving average)
  ├── handleAlerts()   sensor fusion + all alert logic
  ├── updateDisplay()  OLED update every loop
  ├── buzzerUpdate()   non-blocking buzzer pattern engine
  └── serialLog()      debug output every 1s
```

A 30-second hardware watchdog timer auto-reboots the ESP32 if the loop ever hangs (e.g. stuck TLS handshake).

---

## Repository Structure

```
ArborGuard/
├── ArborGuard.ino          Main sketch
├── secrets.h.template      Credential template (safe to commit)
├── secrets.h               Your real credentials (gitignored)
├── .gitignore
├── README.md
├── docs/
│   ├── telegram_guide.html   WiFi & Telegram setup walkthrough
│   └── dht22_guide.html      DHT22 integration guide
└── hardware/
    └── bom.md                Bill of Materials with part links
```

---

## License

MIT License — free to use, modify, and distribute with attribution.
