/**
 * ╔══════════════════════════════════════════════════════════╗
 * ║          ArborGuard v2.0 — Tree Protection System        ║
 * ║  Platform : ESP32                                        ║
 * ║  Author   : Sivakumar K · Sathyabama Institute           ║
 * ║  Features : Sensor Fusion · OLED · WiFi · Telegram Bot  ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * Hardware map:
 *   OLED    → SH1106 1.3" I2C  (SDA=21, SCL=22)
 *   Light   → BH1750 I2C       (SDA=21, SCL=22)
 *   DHT22   → Single-wire      GPIO 27
 *   Soil    → Analog            GPIO 34
 *   Vib     → Analog            GPIO 35
 *   Sound   → Analog            GPIO 32
 *   Buzzer  → Digital           GPIO 25
 *
 * Libraries required (install via Arduino Library Manager):
 *   U8g2           — Oliver Kraus
 *   BH1750         — Christopher Laws
 *   DHT sensor lib — Adafruit  (+Adafruit Unified Sensor dependency)
 *   ArduinoJson    — Benoit Blanchon
 *   WiFi / WiFiClientSecure / HTTPClient — bundled with ESP32 core
 *
 * Credentials:
 *   Copy secrets.h.template → secrets.h and fill in your values.
 *   secrets.h is gitignored and must NEVER be committed.
 */

#include "secrets.h"

#include <Wire.h>
#include <U8g2lib.h>
#include <BH1750.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ================================================================
//  PIN MAP
// ================================================================
#define PIN_DHT       27
#define PIN_SOIL      34
#define PIN_VIBRATION 35
#define PIN_SOUND     32
#define PIN_BUZZER    25

// ================================================================
//  DHT22
// ================================================================
#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);

// ================================================================
//  THRESHOLD CONFIGURATION  ← tune all values here, nowhere else
// ================================================================
namespace Thresh {
  constexpr int      SOIL_DRY_RAW    = 2800;
  constexpr int      VIB_HIGH        = 1400;
  constexpr int      SOUND_HIGH      = 1600;
  constexpr float    LIGHT_LOW_LUX   = 80.0f;
  constexpr uint32_t FUSION_HOLD_MS  = 500;
  constexpr uint32_t COOLDOWN_MS     = 12000;
  constexpr uint32_t SOIL_WARN_CD_MS = 8000;
  constexpr float    TEMP_HIGH_C     = 40.0f;
  constexpr float    HUM_LOW_PCT     = 25.0f;
}

// ================================================================
//  TIMING CONFIGURATION
// ================================================================
namespace Timing {
  constexpr uint32_t SENSOR_MS     = 300;
  constexpr uint32_t DISPLAY_MS    = 2500;
  constexpr uint32_t LOG_MS        = 1000;
  constexpr uint32_t DHT_MS        = 2000;
  constexpr uint32_t WIFI_RETRY_MS = 30000;
  constexpr uint32_t TG_COOLDOWN_MS= 60000;
  constexpr uint32_t TG_STATUS_MS  = 300000;
  constexpr uint32_t BOT_POLL_MS   = 3000;
  constexpr int      WDT_TIMEOUT_S = 30;
}

// ================================================================
//  MOVING AVERAGE FILTER
// ================================================================
template <uint8_t N>
struct MovAvg {
  int32_t buf[N] = {};
  uint8_t idx    = 0;
  int32_t sum    = 0;
  bool    full   = false;

  void push(int32_t v) {
    sum     -= buf[idx];
    buf[idx] = v;
    sum     += v;
    idx      = (idx + 1) % N;
    if (!full && idx == 0) full = true;
  }
  int32_t get() const {
    uint8_t n = full ? N : (idx == 0 ? 1 : idx);
    return sum / n;
  }
};

// ================================================================
//  DATA STRUCTURES
// ================================================================
struct SensorData {
  int     soilRaw  = 0;
  int     vibRaw   = 0;
  int     soundRaw = 0;
  float   lightLux = 0.0f;
  int32_t soilF    = 0;
  int32_t vibF     = 0;
  int32_t soundF   = 0;
  uint8_t soilPct  = 0;
  float   tempC    = 0.0f;
  float   humPct   = 0.0f;
  bool    dhtValid = false;
};

enum class Alert : uint8_t { NONE, SOIL_DRY, TREE_CUTTING, HEAT_STRESS };

struct SystemState {
  Alert    alert            = Alert::NONE;
  bool     fusionArmed      = false;
  uint32_t fusionStart      = 0;
  uint32_t lastCuttingAlert = 0;
  uint32_t lastSoilAlert    = 0;
  uint32_t lastHeatAlert    = 0;
  uint8_t  page             = 0;
  bool     alertOverride    = false;
  uint32_t alertOverrideAt  = 0;
  constexpr static uint32_t ALERT_DISPLAY_MS = 8000;
  bool     buzActive  = false;
  uint8_t  buzStep    = 0;
  uint32_t buzNext    = 0;
  Alert    buzPattern = Alert::NONE;
};

// ================================================================
//  NETWORK STATE
// ================================================================
namespace Net {
  bool     online        = false;
  uint32_t lastReconnect = 0;
  uint32_t lastTgSent    = 0;
  uint32_t lastStatus    = 0;
  uint32_t lastBotPoll   = 0;
  int64_t  lastUpdateId  = 0;
}

// ================================================================
//  GLOBALS
// ================================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
BH1750 bh1750;

SensorData  S;
SystemState ST;

MovAvg<8> soilAvg;
MovAvg<8> vibAvg;
MovAvg<8> soundAvg;

uint32_t tSensor  = 0;
uint32_t tDisplay = 0;
uint32_t tLog     = 0;
uint32_t tDHT     = 0;

// ================================================================
//  BUZZER — NON-BLOCKING PATTERN ENGINE
// ================================================================
struct BuzStep { uint16_t ton; uint16_t off; };

const BuzStep PAT_CUTTING[] = {
  {120,60},{120,60},{120,60},{300,180},
  {120,60},{120,60},{120,60},{300,0},{0,0}
};
const BuzStep PAT_SOIL[] = { {400,300},{400,300},{400,0},{0,0} };
const BuzStep PAT_HEAT[] = { {200,150},{200,150},{600,0},{0,0} };

void buzzerTrigger(Alert type) {
  noTone(PIN_BUZZER);
  ST.buzActive  = true;
  ST.buzStep    = 0;
  ST.buzPattern = type;
  ST.buzNext    = millis();
}

void buzzerUpdate(uint32_t now) {
  if (!ST.buzActive || now < ST.buzNext) return;
  const BuzStep* pat;
  switch (ST.buzPattern) {
    case Alert::TREE_CUTTING: pat = PAT_CUTTING; break;
    case Alert::HEAT_STRESS:  pat = PAT_HEAT;    break;
    default:                  pat = PAT_SOIL;    break;
  }
  uint8_t pi  = ST.buzStep >> 1;
  bool    ton = !(ST.buzStep & 1);
  if (pat[pi].ton == 0) { noTone(PIN_BUZZER); ST.buzActive = false; return; }
  if (ton) {
    uint16_t freq = (ST.buzPattern == Alert::TREE_CUTTING) ? 1300 :
                    (ST.buzPattern == Alert::HEAT_STRESS)  ? 1000 : 900;
    tone(PIN_BUZZER, freq);
    ST.buzNext = now + pat[pi].ton;
  } else {
    noTone(PIN_BUZZER);
    ST.buzNext = now + pat[pi].off;
  }
  ST.buzStep++;
}

// ================================================================
//  WIFI MANAGER — mostly non-blocking
// ================================================================
void wifiTask(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) { Net::online = true; return; }
  Net::online = false;
  if ((now - Net::lastReconnect) < Timing::WIFI_RETRY_MS) return;
  Net::lastReconnect = now;
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t) < 10000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Net::online = true;
    Serial.println("[WiFi] Online — IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("[WiFi] Failed — retrying in 30s");
  }
}

// ================================================================
//  TELEGRAM — SEND MESSAGE
// ================================================================
bool sendTelegram(const String& msg) {
  if (!Net::online) { Serial.println("[TG] Skipped — no WiFi"); return false; }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient https;
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/sendMessage";

  https.begin(client, url);
  https.addHeader("Content-Type", "application/json");
  https.setTimeout(6000);

  String body = "{\"chat_id\":\"";
  body += CHAT_ID;
  body += "\",\"text\":\"";
  body += msg;
  body += "\",\"parse_mode\":\"Markdown\"}";

  int code = https.POST(body);
  https.end();

  if (code == 200) { Serial.println("[TG] Sent OK"); return true; }
  Serial.printf("[TG] Error HTTP %d\n", code);
  return false;
}

void sendAlert(const String& msg, uint32_t now) {
  if ((now - Net::lastTgSent) < Timing::TG_COOLDOWN_MS) {
    Serial.println("[TG] Rate-limited"); return;
  }
  if (sendTelegram(msg)) Net::lastTgSent = now;
}

// ================================================================
//  TELEGRAM — BOT COMMAND HANDLER
// ================================================================
void handleBotCommand(const String& cmd) {
  Serial.println("[Bot] Command: " + cmd);

  if (cmd == "/status" || cmd == "/start") {
    const char* alertStr =
      (ST.alert == Alert::TREE_CUTTING) ? "🚨 TREE CUTTING" :
      (ST.alert == Alert::SOIL_DRY)     ? "⚠ SOIL DRY"     :
      (ST.alert == Alert::HEAT_STRESS)  ? "🌡 HEAT STRESS"  : "✅ ALL CLEAR";
    String msg  = "*🌳 ArborGuard v2.0 — Status*\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    msg += "*Alert:* "  + String(alertStr) + "\n";
    msg += "*WiFi:* "   + WiFi.localIP().toString() + "\n";
    msg += "*Uptime:* " + String(millis() / 1000) + "s\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    msg += "Use /sensors for live readings";
    sendTelegram(msg);

  } else if (cmd == "/sensors") {
    String msg = "*📡 Live Sensor Readings*\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    if (S.dhtValid) {
      msg += "🌡 *Temp:* "     + String(S.tempC, 1) + "°C\n";
      msg += "💧 *Humidity:* " + String(S.humPct, 0) + "%\n";
    } else {
      msg += "🌡 *Temp/Hum:* Initialising...\n";
    }
    msg += "🌱 *Soil:* " + String(S.soilPct) + "%";
    msg += (S.soilPct < 20) ? " ⚠ DRY\n" : "\n";
    msg += "☀ *Light:* " + String(S.lightLux, 0) + " lx";
    msg += (S.lightLux < Thresh::LIGHT_LOW_LUX) ? " ⚠ LOW\n" : "\n";
    msg += "📳 *Vibration:* " + String(S.vibF);
    msg += (S.vibF > Thresh::VIB_HIGH)   ? " 🔴 HIGH\n" : " 🟢\n";
    msg += "🔊 *Sound:* " + String(S.soundF);
    msg += (S.soundF > Thresh::SOUND_HIGH) ? " 🔴 HIGH\n" : " 🟢\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    msg += "_Vib>" + String(Thresh::VIB_HIGH) +
           " | Snd>" + String(Thresh::SOUND_HIGH) +
           " | Soil<20%_";
    sendTelegram(msg);

  } else if (cmd == "/help") {
    String msg = "*🌳 ArborGuard Bot Commands*\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    msg += "/status  — System status & alert state\n";
    msg += "/sensors — Live readings from all sensors\n";
    msg += "/test    — Send a test message\n";
    msg += "/silence — Silence current alert\n";
    msg += "/help    — Show this menu\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    msg += "_Alerts fire automatically on detection._";
    sendTelegram(msg);

  } else if (cmd == "/test") {
    String msg = "*🧪 Test — ArborGuard*\n";
    msg += "━━━━━━━━━━━━━━━━━━━\n";
    msg += "All systems functional.\n";
    msg += "Uptime: " + String(millis() / 1000) + "s";
    sendTelegram(msg);

  } else if (cmd == "/silence") {
    ST.alert         = Alert::NONE;
    ST.alertOverride = false;
    ST.buzActive     = false;
    noTone(PIN_BUZZER);
    sendTelegram("🔕 *Alert silenced.*\nMonitoring continues normally.");

  } else {
    sendTelegram("❓ Unknown command. Use /help to see available commands.");
  }
}

// ================================================================
//  TELEGRAM — BOT POLLING  (ArduinoJson — robust parsing)
// ================================================================
void pollBot(uint32_t now) {
  if (!Net::online) return;
  if ((now - Net::lastBotPoll) < Timing::BOT_POLL_MS) return;
  Net::lastBotPoll = now;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient https;
  String url = "https://api.telegram.org/bot";
  url += BOT_TOKEN;
  url += "/getUpdates?timeout=0&limit=5&offset=";
  url += String(Net::lastUpdateId + 1);

  https.begin(client, url);
  int code = https.GET();
  if (code != 200) { https.end(); return; }

  String payload = https.getString();
  https.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) { Serial.println("[Bot] JSON parse error"); return; }

  for (JsonObject update : doc["result"].as<JsonArray>()) {
    int64_t uid = update["update_id"];
    if (uid <= Net::lastUpdateId) continue;
    Net::lastUpdateId = uid;

    String text = update["message"]["text"] | "";
    if (text.length() == 0) continue;

    // Strip @botname suffix (group chats)
    int atSign = text.indexOf('@');
    if (atSign > 0) text = text.substring(0, atSign);
    text.toLowerCase();

    if (text.startsWith("/")) handleBotCommand(text);
  }
}

// ================================================================
//  PERIODIC 5-MINUTE STATUS REPORT
// ================================================================
void periodicStatus(uint32_t now) {
  if (!Net::online) return;
  if ((now - Net::lastStatus) < Timing::TG_STATUS_MS) return;
  Net::lastStatus = now;

  const char* alertStr =
    (ST.alert == Alert::TREE_CUTTING) ? "🚨 CUTTING"  :
    (ST.alert == Alert::SOIL_DRY)     ? "⚠ SOIL DRY" :
    (ST.alert == Alert::HEAT_STRESS)  ? "🌡 HEAT"     : "✅ Clear";

  String msg = "*📊 ArborGuard — 5-min Report*\n";
  msg += "━━━━━━━━━━━━━━━━━━━\n";
  if (S.dhtValid)
    msg += "🌡 " + String(S.tempC,1) + "°C  💧 " + String(S.humPct,0) + "%\n";
  msg += "🌱 Soil: " + String(S.soilPct) + "%\n";
  msg += "☀ Light: " + String(S.lightLux,0) + " lx\n";
  msg += "📳 Vib: " + String(S.vibF) + "  🔊 Snd: " + String(S.soundF) + "\n";
  msg += "━━━━━━━━━━━━━━━━━━━\n";
  msg += "Status: " + String(alertStr) + " | Up: " + String(now/1000) + "s";
  sendTelegram(msg);
}

// ================================================================
//  SENSOR READS
// ================================================================
void readSoil() {
  S.soilRaw = analogRead(PIN_SOIL);
  soilAvg.push(S.soilRaw);
  S.soilF   = soilAvg.get();
  S.soilPct = (uint8_t)constrain(map(S.soilF, 3500, 500, 0, 100), 0, 100);
}

void readLight() {
  float lux = bh1750.readLightLevel();
  if (lux >= 0) S.lightLux = lux;
}

void readVibration() {
  S.vibRaw = analogRead(PIN_VIBRATION);
  vibAvg.push(S.vibRaw);
  S.vibF   = vibAvg.get();
}

void readSound() {
  S.soundRaw = analogRead(PIN_SOUND);
  soundAvg.push(S.soundRaw);
  S.soundF   = soundAvg.get();
}

void dhtTask(uint32_t now) {
  if ((now - tDHT) < Timing::DHT_MS) return;
  tDHT = now;
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    S.tempC    = t;
    S.humPct   = h;
    S.dhtValid = true;
  } else {
    Serial.println("[DHT22] Read failed");
  }
}

// ================================================================
//  TREE CUTTING DETECTION — sensor fusion + persistence + cooldown
// ================================================================
void detectTreeCutting(uint32_t now) {
  bool fused = (S.vibF > Thresh::VIB_HIGH) && (S.soundF > Thresh::SOUND_HIGH);

  if (fused) {
    if (!ST.fusionArmed) {
      ST.fusionArmed = true;
      ST.fusionStart = now;
    } else if ((now - ST.fusionStart)  >= Thresh::FUSION_HOLD_MS &&
               (now - ST.lastCuttingAlert) >= Thresh::COOLDOWN_MS) {

      ST.alert            = Alert::TREE_CUTTING;
      ST.alertOverride    = true;
      ST.alertOverrideAt  = now;
      ST.lastCuttingAlert = now;
      buzzerTrigger(Alert::TREE_CUTTING);

      String msg = "*🚨 TREE CUTTING ALERT*\n";
      msg += "━━━━━━━━━━━━━━━━━━━\n";
      msg += "🔴 Sensor fusion triggered!\n";
      msg += "📳 Vib: "   + String(S.vibF)   + " _(thresh: " + Thresh::VIB_HIGH   + ")_\n";
      msg += "🔊 Sound: " + String(S.soundF) + " _(thresh: " + Thresh::SOUND_HIGH + ")_\n";
      if (S.dhtValid)
        msg += "🌡 " + String(S.tempC,1) + "°C  💧 " + String(S.humPct,0) + "%\n";
      msg += "🌱 Soil: " + String(S.soilPct) + "%\n";
      msg += "━━━━━━━━━━━━━━━━━━━\n";
      msg += "⏱ Uptime: " + String(now/1000) + "s\n";
      msg += "_Reply /silence to acknowledge_";
      sendAlert(msg, now);
      Serial.println("[!!! ALERT !!!] TREE CUTTING DETECTED");
    }
  } else {
    ST.fusionArmed = false;
  }
}

// ================================================================
//  ALERT HANDLER
// ================================================================
void handleAlerts(uint32_t now) {
  detectTreeCutting(now);

  // Soil dry
  if (ST.alert != Alert::TREE_CUTTING) {
    bool soilDry = S.soilPct < 20;
    bool cooled  = (now - ST.lastSoilAlert) >= Thresh::SOIL_WARN_CD_MS;
    if (soilDry && cooled && !ST.buzActive) {
      ST.alert         = Alert::SOIL_DRY;
      ST.lastSoilAlert = now;
      buzzerTrigger(Alert::SOIL_DRY);
      String msg = "*⚠ Soil Moisture Alert*\n";
      msg += "━━━━━━━━━━━━━━━━━━━\n";
      msg += "🌱 Moisture: " + String(S.soilPct) + "% _(raw: " + String(S.soilF) + ")_\n";
      msg += "💧 Humidity: " + String(S.humPct,0) + "%\n";
      msg += "🌡 Temp: "     + String(S.tempC,1)  + "°C\n";
      msg += "━━━━━━━━━━━━━━━━━━━\n";
      msg += "The tree needs watering.";
      sendAlert(msg, now);
    } else if (!soilDry && ST.alert == Alert::SOIL_DRY) {
      ST.alert = Alert::NONE;
    }
  }

  // Heat / drought stress
  if (ST.alert == Alert::NONE && S.dhtValid) {
    bool tooHot = S.tempC  > Thresh::TEMP_HIGH_C;
    bool tooDry = S.humPct < Thresh::HUM_LOW_PCT;
    bool cooled = (now - ST.lastHeatAlert) >= Thresh::COOLDOWN_MS;
    if ((tooHot || tooDry) && cooled) {
      ST.alert         = Alert::HEAT_STRESS;
      ST.lastHeatAlert = now;
      buzzerTrigger(Alert::HEAT_STRESS);
      String msg = "*🌡 Environmental Alert*\n";
      msg += "━━━━━━━━━━━━━━━━━━━\n";
      if (tooHot) msg += "🔴 *High Temp:* "    + String(S.tempC,1)  + "°C\n";
      if (tooDry) msg += "🔴 *Low Humidity:* " + String(S.humPct,0) + "%\n";
      msg += "🌱 Soil: " + String(S.soilPct) + "%\n";
      msg += "━━━━━━━━━━━━━━━━━━━\n";
      msg += "Heat or drought stress detected.";
      sendAlert(msg, now);
    } else if (!tooHot && !tooDry && ST.alert == Alert::HEAT_STRESS) {
      ST.alert = Alert::NONE;
    }
  }

  // Auto-expire fullscreen override after 8s
  if (ST.alertOverride && (now - ST.alertOverrideAt) >= SystemState::ALERT_DISPLAY_MS) {
    ST.alertOverride = false;
    if (ST.alert == Alert::TREE_CUTTING) ST.alert = Alert::NONE;
  }
}

// ================================================================
//  OLED — 3 ROTATING PAGES + FULLSCREEN ALERT
// ================================================================
void drawPage0_EnvSoil() {
  char buf[24];
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "\xb7 ENV & SOIL");
  u8g2.drawHLine(0, 12, 128);

  if (S.dhtValid) snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", S.tempC, S.humPct);
  else            snprintf(buf, sizeof(buf), "T:-- H:--");
  u8g2.drawStr(0, 25, buf);

  snprintf(buf, sizeof(buf), "Soil %3d%%", S.soilPct);
  u8g2.drawStr(0, 37, buf);
  u8g2.drawFrame(0, 39, 118, 7);
  int bw = (int)((S.soilPct / 100.0f) * 116);
  if (bw > 0) u8g2.drawBox(1, 40, bw, 5);

  snprintf(buf, sizeof(buf), "Lux: %.0f", S.lightLux);
  u8g2.drawStr(0, 55, buf);
  if (S.lightLux < Thresh::LIGHT_LOW_LUX) u8g2.drawStr(96, 55, "LOW");

  u8g2.drawBox(54,61,4,3); u8g2.drawFrame(60,61,4,3); u8g2.drawFrame(66,61,4,3);
}

void drawPage1_VibSound() {
  char buf[24];
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "\xb7 VIB & SOUND");
  u8g2.drawHLine(0, 12, 128);

  snprintf(buf, sizeof(buf), "Vib  %4ld", S.vibF);
  u8g2.drawStr(0, 26, buf);
  u8g2.drawStr(90, 26, (S.vibF > Thresh::VIB_HIGH) ? "[HIGH]" : "[ ok ]");

  snprintf(buf, sizeof(buf), "Snd  %4ld", S.soundF);
  u8g2.drawStr(0, 40, buf);
  u8g2.drawStr(90, 40, (S.soundF > Thresh::SOUND_HIGH) ? "[HIGH]" : "[ ok ]");

  u8g2.setFont(u8g2_font_5x7_tf);
  snprintf(buf, sizeof(buf), "Fusion: %s", ST.fusionArmed ? "ARMED" : "idle");
  u8g2.drawStr(0, 54, buf);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawFrame(54,61,4,3); u8g2.drawBox(60,61,4,3); u8g2.drawFrame(66,61,4,3);
}

void drawPage2_Status() {
  char buf[24];
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "\xb7 SYSTEM");
  u8g2.drawHLine(0, 12, 128);

  const char* a =
    (ST.alert == Alert::TREE_CUTTING) ? "CUTTING!" :
    (ST.alert == Alert::SOIL_DRY)     ? "SOIL DRY" :
    (ST.alert == Alert::HEAT_STRESS)  ? "HEAT STR" : "ALL OK";
  snprintf(buf, sizeof(buf), "Alert: %s", a);
  u8g2.drawStr(0, 26, buf);

  snprintf(buf, sizeof(buf), "WiFi: %s", Net::online ? "Online " : "Offline");
  u8g2.drawStr(0, 38, buf);

  snprintf(buf, sizeof(buf), "Up: %lus", millis() / 1000);
  u8g2.drawStr(0, 50, buf);

  u8g2.drawFrame(54,61,4,3); u8g2.drawFrame(60,61,4,3); u8g2.drawBox(66,61,4,3);
}

void drawAlertFullscreen() {
  if (ST.alert == Alert::TREE_CUTTING) {
    u8g2.drawBox(0,0,128,64);
    u8g2.setDrawColor(0);
    u8g2.setFont(u8g2_font_9x15B_tf); u8g2.drawStr(12,18,"!! ALERT !!");
    u8g2.setFont(u8g2_font_7x14B_tf); u8g2.drawStr(10,38,"TREE CUTTING");
    u8g2.setFont(u8g2_font_6x10_tf);  u8g2.drawStr(22,54,"DETECTED!");
    u8g2.setDrawColor(1);

  } else if (ST.alert == Alert::SOIL_DRY) {
    u8g2.setFont(u8g2_font_7x14B_tf); u8g2.drawStr(18,20,"SOIL DRY");
    u8g2.drawHLine(0,25,128);
    u8g2.setFont(u8g2_font_6x10_tf);  u8g2.drawStr(5,40,"Tree needs water.");
    char buf[22];
    snprintf(buf, sizeof(buf), "Moisture: %3d%%", S.soilPct);
    u8g2.drawStr(12,56,buf);

  } else if (ST.alert == Alert::HEAT_STRESS) {
    u8g2.setFont(u8g2_font_7x14B_tf); u8g2.drawStr(10,20,"HEAT STRESS");
    u8g2.drawHLine(0,25,128);
    char buf[22];
    snprintf(buf,sizeof(buf),"T:%.1fC H:%.0f%%",S.tempC,S.humPct);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0,44,buf);
    u8g2.drawStr(12,56,"Check environment");
  }
}

void updateDisplay(uint32_t now) {
  if (!ST.alertOverride && (now - tDisplay) >= Timing::DISPLAY_MS) {
    ST.page = (ST.page + 1) % 3;
    tDisplay = now;
  }
  u8g2.clearBuffer();
  if (ST.alertOverride) {
    drawAlertFullscreen();
  } else {
    switch (ST.page) {
      case 0: drawPage0_EnvSoil();  break;
      case 1: drawPage1_VibSound(); break;
      case 2: drawPage2_Status();   break;
    }
  }
  u8g2.sendBuffer();
}

// ================================================================
//  SERIAL LOG
// ================================================================
void serialLog(uint32_t now) {
  static const char* names[] = {"NONE    ","SOIL_DRY","CUTTING!","HEAT_STR"};
  Serial.printf(
    "[%8lu ms] T:%.1fC H:%.0f%% | SOIL:%3u%%(raw:%4d) | LUX:%7.1f | VIB:%4ld | SND:%4ld | %s | WiFi:%s\n",
    now, S.tempC, S.humPct, S.soilPct, S.soilRaw,
    S.lightLux, S.vibF, S.soundF,
    names[(int)ST.alert], Net::online ? "ON" : "OFF"
  );
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔══════════════════════════════╗");
  Serial.println("║  ArborGuard v2.0 — Booting   ║");
  Serial.println("╚══════════════════════════════╝");

  // Watchdog — reboot if loop hangs for 30s
  esp_task_wdt_init(Timing::WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(PIN_BUZZER, OUTPUT);
  noTone(PIN_BUZZER);
  Wire.begin(21, 22);

  // OLED
  if (!u8g2.begin()) {
    Serial.println("[FAIL] OLED init failed");
  } else {
    Serial.println("[OK]   OLED ready");
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_7x14B_tf); u8g2.drawStr(14,20,"ArborGuard");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(18,36,"Tree Monitor");
    u8g2.drawStr(26,50,"v2.0 — SK");
    u8g2.sendBuffer();
  }

  // BH1750
  if (!bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
    Serial.println("[FAIL] BH1750 not found");
  else
    Serial.println("[OK]   BH1750 ready");

  // DHT22
  dht.begin();
  Serial.println("[OK]   DHT22 ready (GPIO 27)");

  // Prime moving average buffers
  Serial.print("[..] Priming filters");
  for (uint8_t i = 0; i < 8; i++) {
    soilAvg.push(analogRead(PIN_SOIL));
    vibAvg.push(analogRead(PIN_VIBRATION));
    soundAvg.push(analogRead(PIN_SOUND));
    delay(20); Serial.print(".");
  }
  Serial.println(" done");

  delay(1500); // splash hold

  // WiFi boot connect (max 15s)
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000) {
    delay(300); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Net::online        = true;
    Net::lastReconnect = millis();
    Serial.println("[WiFi] Online — IP: " + WiFi.localIP().toString());

    String bootMsg  = "*🌳 ArborGuard v2.0 Online*\n";
    bootMsg += "━━━━━━━━━━━━━━━━━━━\n";
    bootMsg += "✅ System started successfully\n";
    bootMsg += "📡 IP: " + WiFi.localIP().toString() + "\n";
    bootMsg += "━━━━━━━━━━━━━━━━━━━\n";
    bootMsg += "Send /help for available commands.";
    sendTelegram(bootMsg);

    tone(PIN_BUZZER,880);delay(100);
    tone(PIN_BUZZER,1100);delay(100);
    tone(PIN_BUZZER,1400);delay(150);
    noTone(PIN_BUZZER);
  } else {
    Serial.println("[WiFi] Failed — will retry in background");
  }

  Net::lastStatus = millis(); // first status report after 5 min
  Serial.println("[OK]   ArborGuard running\n");
}

// ================================================================
//  LOOP — cooperative multitasking scheduler
// ================================================================
void loop() {
  uint32_t now = millis();

  esp_task_wdt_reset(); // pat the watchdog

  wifiTask(now);
  pollBot(now);
  periodicStatus(now);
  dhtTask(now);

  if ((now - tSensor) >= Timing::SENSOR_MS) {
    readSoil();
    readLight();
    readVibration();
    readSound();
    handleAlerts(now);
    tSensor = now;
  }

  updateDisplay(now);
  buzzerUpdate(now);

  if ((now - tLog) >= Timing::LOG_MS) {
    serialLog(now);
    tLog = now;
  }
}
