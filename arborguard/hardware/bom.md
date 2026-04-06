# Bill of Materials — ArborGuard v2.0

All components are widely available on Amazon, Robu.in, or any local electronics shop.
Prices are approximate (INR) as of 2024.

---

## Core Components

| # | Component | Specification | Qty | Est. Price (INR) | Search Keywords |
|---|-----------|---------------|-----|------------------|-----------------|
| 1 | ESP32 DevKit V1 | 38-pin, CP2102 USB-UART | 1 | ₹350–450 | "ESP32 DevKit V1 38 pin" |
| 2 | SH1106 OLED Display | 1.3 inch, I²C, 128×64 | 1 | ₹180–250 | "SH1106 1.3 inch OLED I2C" |
| 3 | BH1750 Light Sensor | GY-302 module, I²C | 1 | ₹80–120 | "BH1750 GY-302 light sensor" |
| 4 | DHT22 Temperature & Humidity | AM2302, module version preferred | 1 | ₹150–200 | "DHT22 AM2302 module" |
| 5 | Soil Moisture Sensor | Capacitive v1.2 (not resistive) | 1 | ₹120–180 | "capacitive soil moisture v1.2" |
| 6 | Vibration Sensor | SW-420 analog module | 1 | ₹50–80 | "SW-420 vibration sensor module" |
| 7 | Sound Sensor | KY-038 or LM393 analog module | 1 | ₹60–100 | "KY-038 sound sensor module" |
| 8 | Active Buzzer | 3.3V/5V active piezo buzzer | 1 | ₹20–40 | "active buzzer 5V arduino" |

**Component subtotal: ~₹1,010–1,420**

---

## Supporting Components

| # | Component | Qty | Est. Price (INR) | Notes |
|---|-----------|-----|------------------|-------|
| 9 | Jumper wires (M-M) | 20 | ₹50 | For breadboard connections |
| 10 | Jumper wires (M-F) | 10 | ₹30 | For sensor modules to ESP32 header |
| 11 | Breadboard 830-point | 1 | ₹60–80 | Only needed during prototyping |
| 12 | 10 kΩ resistor | 1 | ₹2 | Only if using bare DHT22 (not module) |
| 13 | USB Micro-B cable | 1 | ₹80–120 | For programming and power |
| 14 | USB power bank / 5V adapter | 1 | ₹300–600 | Field power supply for deployment |

**Supporting subtotal: ~₹522–882**

---

## Optional / Enclosure

| # | Component | Est. Price (INR) | Notes |
|---|-----------|------------------|-------|
| 15 | IP65 waterproof junction box | ₹150–300 | Protects electronics outdoors |
| 16 | Cable glands (M16) | ₹30–50 | Sensor wire entry into box |
| 17 | Epoxy putty / silicone sealant | ₹80–150 | Seal cable entry points |

---

## Total Estimated Cost

| Category | Range |
|----------|-------|
| Core components | ₹1,010 – ₹1,420 |
| Supporting | ₹522 – ₹882 |
| **Total (without enclosure)** | **₹1,532 – ₹2,302** |
| Total (with enclosure) | ₹1,762 – ₹2,752 |

---

## Notes

- **Capacitive vs resistive soil sensor:** Use the capacitive version (blue, v1.2). Resistive sensors corrode within weeks when buried in soil.
- **DHT22 module vs bare sensor:** The module version (on a PCB board) has the 10 kΩ pull-up resistor pre-installed and is easier to connect. Bare sensor requires adding the resistor yourself.
- **Power for field deployment:** A 10,000 mAh power bank can power the ESP32 for ~20–30 hours. For permanent installation, use a 5V 2A wall adapter or a solar + 18650 Li-ion setup.
