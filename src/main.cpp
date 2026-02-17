/**************************************************************
 * HydroNode (Merged) for ESP32-C3 SuperMini  ✅ MQTT-FIX VERSION
 *  - EC (0–5V transmitter, powered by 12V, output terminals: + and -)
 *  - Water Level (0–5V or 0–3.3V analog)
 *  - ✅ DS18B20 temperature sensor (GPIO5)
 *
 * FIXES in this build:
 *  ✅ MQTT will NOT block UI anymore:
 *     - MQTT disabled automatically in AP mode / when WiFi not connected
 *     - MQTT reconnect retries slowed down (default 15s)
 *     - PubSubClient socket timeout reduced (1s)
 *     - Buttons are polled BEFORE MQTT work
 **************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <LiquidCrystal_I2C.h>

#include <OneWire.h>
#include <DallasTemperature.h>

/**************************************************************
 * VERSION
 **************************************************************/
static const char* FW_VERSION = "ver-2.1.2-mqttfix";
static const uint8_t API_VERSION = 1;

/**************************************************************
 * BASIC AUTH (UI only)
 **************************************************************/
static const char* UI_USER = "admin";
static const char* UI_PASS = "hydronode";   // change to your own

/**************************************************************
 * PINS (ESP32-C3 SuperMini)
 **************************************************************/
// I2C for LCD
static const int PIN_I2C_SDA = 8;
static const int PIN_I2C_SCL = 9;

// ADC pins
static const int PIN_EC_ADC    = 0;  // EC analog in
static const int PIN_LEVEL_ADC = 1;  // Level analog in

// Buttons (to GND, INPUT_PULLUP)
static const int PIN_BTN_LIGHT = 2;  // LIGHT/MODE
static const int PIN_BTN_UP    = 3;  // CAL/UP
static const int PIN_BTN_DN    = 4;  // DOWN/ENTER

// DS18B20 (1-Wire)
static const int PIN_DS18B20   = 5;  // DATA pin

// LCD
static const uint8_t LCD_ADDR = 0x27;
static const uint8_t LCD_COLS = 20;
static const uint8_t LCD_ROWS = 4;

/**************************************************************
 * DIVIDER RATIOS
 **************************************************************/
static const float EC_DIVIDER_RATIO = 2.0f;
static const float LEVEL_DIVIDER_RATIO = 2.0f;

/**************************************************************
 * TIMING
 **************************************************************/
static const uint32_t TICK_UI_MS      = 100;
static const uint32_t TICK_SENSOR_MS  = 250;
static const uint32_t TICK_MQTT_MS    = 200;

static const uint32_t SHORT_MS = 60;
static const uint32_t LONG_MS  = 700;
static const uint32_t VLONG_MS = 3500;

static const uint8_t ADC_SAMPLES_PER_TICK = 16;

/**************************************************************
 * OBJECTS
 **************************************************************/
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18(&oneWire);

/**************************************************************
 * STATUS / CONFIG
 **************************************************************/
struct WifiStatus {
  enum Mode : uint8_t { WIFI_OFF=0, WIFI_AP=1, WIFI_STA=2 } mode = WIFI_OFF;
  bool connected = false;
  String ssid = "";
  String ip = "";
};

struct MqttConfig {
  bool enabled = false;
  String host = "";
  uint16_t port = 1883;
  String user = "";
  String pass = "";
  String base_topic = "hydronode";
  bool retain = true;
  uint16_t pub_period_ms = 1000;
};

struct MqttStatus {
  bool configured = false;
  bool connected = false;
  uint32_t lastAttemptMs = 0;
  uint32_t lastPublishMs = 0;
  String err = "";
};

enum CalQuality : uint8_t { CAL_NONE=0, CAL_WEAK=1, CAL_OK=2 };

struct EcCalPoint {
  float ec_us = 1413.0f;
  float v = 0.0f;
  bool set = false;
};

struct EcCal {
  EcCalPoint A;
  EcCalPoint B;
  bool valid = false;
  float slope = 1.0f;   // EC = slope * V + offset
  float offset = 0.0f;
  CalQuality quality = CAL_NONE;
};

enum LevelUnit : uint8_t { UNIT_PERCENT=0, UNIT_CUSTOM=1 };

struct LevelCalPoint {
  float level = 0.0f;
  float v = 0.0f;
  bool set = false;
};

struct LevelCal {
  LevelCalPoint empty;
  LevelCalPoint full;
  bool valid = false;
  float slope = 0.0f;
  float offset = 0.0f;
  CalQuality quality = CAL_NONE;
  LevelUnit unit = UNIT_PERCENT;
  float custom_max = 100.0f;
};

struct Sensors {
  // EC
  uint16_t ec_adc_raw = 0;
  float ec_v = 0.0f;
  float ec_us = 0.0f;

  // LEVEL
  uint16_t lvl_adc_raw = 0;
  float lvl_v = 0.0f;
  float lvl_value = 0.0f;
  float lvl_percent = 0.0f;

  // TEMP
  float temp_c = NAN;
};

static WifiStatus wifiSt;
static MqttConfig mqttCfg;
static MqttStatus mqttSt;
static EcCal ecCal;
static LevelCal lvlCal;
static Sensors sens;

/**************************************************************
 * UI STATE
 **************************************************************/
static bool lcdBacklight = true;

static const int MENU_N = 3;
static const int CAL_N  = 3;

enum UIState : uint8_t {
  UI_HOME=0,
  UI_MENU,
  UI_SETUP,
  UI_CAL_MENU,
  UI_CAL_EC,
  UI_CAL_LEVEL,
  UI_LEVEL_UNIT,
  UI_INFO
};

static UIState ui = UI_HOME;

static int menuIndex = 0;
static int calIndex  = 0;

enum BtnId : uint8_t { BTN_LIGHT=0, BTN_UP=1, BTN_DN=2 };
enum EvType : uint8_t { EV_NONE=0, EV_SHORT=1, EV_LONG=2, EV_VLONG=3 };

struct Btn {
  int pin;
  bool down;
  uint32_t downMs;
  Btn(int p) : pin(p), down(false), downMs(0) {}
};

static Btn btns[3] = { Btn(PIN_BTN_LIGHT), Btn(PIN_BTN_UP), Btn(PIN_BTN_DN) };

/**************************************************************
 * CAL WIZARDS
 **************************************************************/
enum EcStep : uint8_t { EC_A_SET=0, EC_A_CAP, EC_B_SET, EC_B_CAP, EC_DONE };
static EcStep ecStep = EC_A_SET;
static float ecWizardA = 1413.0f;
static float ecWizardB = 27600.0f;

enum LvlStep : uint8_t { LVL_UNIT=0, LVL_EMPTY_SET, LVL_EMPTY_CAP, LVL_FULL_SET, LVL_FULL_CAP, LVL_DONE };
static LvlStep lvlStep = LVL_UNIT;
static float lvlWizardEmpty = 0.0f;
static float lvlWizardFull  = 100.0f;

/**************************************************************
 * WIFI / CAPTIVE PORTAL
 **************************************************************/
static bool apMode = false;

/**************************************************************
 * HELPERS
 **************************************************************/
static String ipToString(const IPAddress& ip){
  return String(ip[0])+"."+String(ip[1])+"."+String(ip[2])+"."+String(ip[3]);
}

static void lcdSetLine(uint8_t row, const String& s){
  char buf[LCD_COLS + 1];
  memset(buf, ' ', LCD_COLS);
  buf[LCD_COLS] = '\0';

  size_t n = s.length();
  if (n > LCD_COLS) n = LCD_COLS;
  memcpy(buf, s.c_str(), n);

  lcd.setCursor(0, row);
  lcd.print(buf);
}

static void uiSet(UIState st){
  ui = st;
  lcd.clear();
}

static void wipeWiFiAndRestart(){
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();

  WiFi.disconnect(true, true);
  delay(200);
  ESP.restart();
}

/**************************************************************
 * WIFI CREDS (NVS)
 **************************************************************/
static bool loadWiFiCreds(String &outSsid, String &outPass){
  prefs.begin("wifi", true);
  outSsid = prefs.getString("ssid", "");
  outPass = prefs.getString("pass", "");
  prefs.end();
  return outSsid.length() > 0;
}

static void saveWiFiCreds(const String &ssid, const String &pass){
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

static void startAP(){
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("HydroNode-Setup");
  IPAddress ip = WiFi.softAPIP();

  wifiSt.mode = WifiStatus::WIFI_AP;
  wifiSt.connected = true;
  wifiSt.ssid = "HydroNode-Setup";
  wifiSt.ip = ipToString(ip);

  dnsServer.start(53, "*", ip);
}

static void startSTA(){
  apMode = false;
  dnsServer.stop();

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);

  String ssid, pass;
  if (loadWiFiCreds(ssid, pass)) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

static void wifiTick(){
  if (!apMode){
    if (WiFi.status() == WL_CONNECTED){
      wifiSt.mode = WifiStatus::WIFI_STA;
      wifiSt.connected = true;
      wifiSt.ip = ipToString(WiFi.localIP());
      wifiSt.ssid = WiFi.SSID();
    } else {
      wifiSt.mode = WifiStatus::WIFI_STA;
      wifiSt.connected = false;
      wifiSt.ip = "";
      wifiSt.ssid = "";
    }
  } else {
    dnsServer.processNextRequest();
  }
}

/**************************************************************
 * PREFERENCES: MQTT
 **************************************************************/
static void loadMqtt(){
  prefs.begin("mqtt", true);
  mqttCfg.enabled       = prefs.getBool("en", false);
  mqttCfg.host          = prefs.getString("host", "");
  mqttCfg.port          = (uint16_t)prefs.getUShort("port", 1883);
  mqttCfg.user          = prefs.getString("user", "");
  mqttCfg.pass          = prefs.getString("pass", "");
  mqttCfg.base_topic    = prefs.getString("topic", "hydronode");
  mqttCfg.retain        = prefs.getBool("ret", true);
  mqttCfg.pub_period_ms = (uint16_t)prefs.getUShort("per", 1000);
  prefs.end();
}

static void saveMqtt(){
  prefs.begin("mqtt", false);
  prefs.putBool("en", mqttCfg.enabled);
  prefs.putString("host", mqttCfg.host);
  prefs.putUShort("port", mqttCfg.port);
  prefs.putString("user", mqttCfg.user);
  prefs.putString("pass", mqttCfg.pass);
  prefs.putString("topic", mqttCfg.base_topic);
  prefs.putBool("ret", mqttCfg.retain);
  prefs.putUShort("per", mqttCfg.pub_period_ms);
  prefs.end();
}

/**************************************************************
 * PREFERENCES: CAL
 **************************************************************/
static void loadEcCal(){
  prefs.begin("eccal", true);
  ecCal.A.ec_us = prefs.getFloat("A_ec", 1413.0f);
  ecCal.A.v     = prefs.getFloat("A_v", 0.0f);
  ecCal.A.set   = prefs.getBool("A_set", false);

  ecCal.B.ec_us = prefs.getFloat("B_ec", 27600.0f);
  ecCal.B.v     = prefs.getFloat("B_v", 0.0f);
  ecCal.B.set   = prefs.getBool("B_set", false);
  prefs.end();
}

static void saveEcCal(){
  prefs.begin("eccal", false);
  prefs.putFloat("A_ec", ecCal.A.ec_us);
  prefs.putFloat("A_v",  ecCal.A.v);
  prefs.putBool("A_set", ecCal.A.set);

  prefs.putFloat("B_ec", ecCal.B.ec_us);
  prefs.putFloat("B_v",  ecCal.B.v);
  prefs.putBool("B_set", ecCal.B.set);
  prefs.end();
}

static void loadLevelCal(){
  prefs.begin("lvlcal", true);
  lvlCal.empty.level = prefs.getFloat("E_lvl", 0.0f);
  lvlCal.empty.v     = prefs.getFloat("E_v",   0.0f);
  lvlCal.empty.set   = prefs.getBool("E_set",  false);

  lvlCal.full.level  = prefs.getFloat("F_lvl", 100.0f);
  lvlCal.full.v      = prefs.getFloat("F_v",   0.0f);
  lvlCal.full.set    = prefs.getBool("F_set",  false);

  lvlCal.unit        = (LevelUnit)prefs.getUChar("unit", (uint8_t)UNIT_PERCENT);
  lvlCal.custom_max  = prefs.getFloat("cmax", 100.0f);
  prefs.end();
}

static void saveLevelCal(){
  prefs.begin("lvlcal", false);
  prefs.putFloat("E_lvl", lvlCal.empty.level);
  prefs.putFloat("E_v",   lvlCal.empty.v);
  prefs.putBool("E_set",  lvlCal.empty.set);

  prefs.putFloat("F_lvl", lvlCal.full.level);
  prefs.putFloat("F_v",   lvlCal.full.v);
  prefs.putBool("F_set",  lvlCal.full.set);

  prefs.putUChar("unit", (uint8_t)lvlCal.unit);
  prefs.putFloat("cmax", lvlCal.custom_max);
  prefs.end();
}

/**************************************************************
 * CALCULATIONS
 **************************************************************/
static void computeEcCal(){
  ecCal.valid = false;
  ecCal.quality = CAL_NONE;

  if (!ecCal.A.set || !ecCal.B.set) return;

  float dv = ecCal.B.v - ecCal.A.v;
  if (fabsf(dv) < 0.02f) {
    ecCal.quality = CAL_WEAK;
    return;
  }

  ecCal.slope  = (ecCal.B.ec_us - ecCal.A.ec_us) / dv;
  ecCal.offset = ecCal.A.ec_us - ecCal.slope * ecCal.A.v;

  ecCal.valid = true;
  ecCal.quality = CAL_OK;
}

static void computeLevelCal(){
  lvlCal.valid = false;
  lvlCal.quality = CAL_NONE;

  if (!lvlCal.empty.set || !lvlCal.full.set) return;

  float dv = lvlCal.full.v - lvlCal.empty.v;
  if (fabsf(dv) < 0.05f) {
    lvlCal.quality = CAL_WEAK;
    return;
  }

  lvlCal.slope  = (lvlCal.full.level - lvlCal.empty.level) / dv;
  lvlCal.offset = lvlCal.empty.level - (lvlCal.slope * lvlCal.empty.v);

  lvlCal.valid = true;
  lvlCal.quality = CAL_OK;
}

/**************************************************************
 * ADC
 **************************************************************/
static uint16_t readAdcAvg(int pin){
  uint32_t acc = 0;
  for (uint8_t i=0;i<ADC_SAMPLES_PER_TICK;i++){
    acc += analogRead(pin);
    delayMicroseconds(200);
  }
  return (uint16_t)(acc / ADC_SAMPLES_PER_TICK);
}

static float adcToPinVoltage(uint16_t adc){
  return (adc / 4095.0f) * 3.3f;
}

static float ecAdcToProbeVoltage(uint16_t adc){
  return adcToPinVoltage(adc) * EC_DIVIDER_RATIO;
}

static float levelAdcToProbeVoltage(uint16_t adc){
  return adcToPinVoltage(adc) * LEVEL_DIVIDER_RATIO;
}

static float ecVoltageToUs(float v){
  if (!ecCal.valid) return v * 10000.0f; // fallback
  return ecCal.slope * v + ecCal.offset;
}

static float levelVoltageToLevel(float v){
  if (!lvlCal.valid) return v; // fallback
  return (lvlCal.slope * v + lvlCal.offset);
}

static void updateLevelDerived(){
  if (lvlCal.unit == UNIT_PERCENT) {
    sens.lvl_percent = sens.lvl_value;
  } else {
    if (lvlCal.custom_max <= 0.0001f) sens.lvl_percent = 0.0f;
    else sens.lvl_percent = (sens.lvl_value / lvlCal.custom_max) * 100.0f;
  }
  if (sens.lvl_percent < 0) sens.lvl_percent = 0;
  if (sens.lvl_percent > 100) sens.lvl_percent = 100;
}

static void sensorTick(){
  sens.ec_adc_raw = readAdcAvg(PIN_EC_ADC);
  sens.ec_v       = ecAdcToProbeVoltage(sens.ec_adc_raw);
  sens.ec_us      = ecVoltageToUs(sens.ec_v);

  sens.lvl_adc_raw = readAdcAvg(PIN_LEVEL_ADC);
  sens.lvl_v       = levelAdcToProbeVoltage(sens.lvl_adc_raw);

  float rawLevel   = levelVoltageToLevel(sens.lvl_v);

  if (lvlCal.unit == UNIT_PERCENT) {
    if (rawLevel < 0) rawLevel = 0;
    if (rawLevel > 100) rawLevel = 100;
    sens.lvl_value = rawLevel;
  } else {
    if (rawLevel < 0) rawLevel = 0;
    if (rawLevel > lvlCal.custom_max) rawLevel = lvlCal.custom_max;
    sens.lvl_value = rawLevel;
  }

  updateLevelDerived();

  // DS18B20 non-blocking
  static uint32_t lastTreq = 0;
  float t = ds18.getTempCByIndex(0);
  if (t > -55 && t < 125) sens.temp_c = t;

  uint32_t now = millis();
  if (now - lastTreq >= 1000) {
    lastTreq = now;
    ds18.requestTemperatures();
  }
}

/**************************************************************
 * BUTTON EVENTS
 **************************************************************/
static EvType pollButton(BtnId id){
  Btn &b = btns[(int)id];
  bool pressed = (digitalRead(b.pin) == LOW);

  if (pressed && !b.down){
    b.down = true;
    b.downMs = millis();
    return EV_NONE;
  }

  if (!pressed && b.down){
    uint32_t dur = millis() - b.downMs;
    b.down = false;

    if (dur >= VLONG_MS) return EV_VLONG;
    if (dur >= LONG_MS)  return EV_LONG;
    if (dur >= SHORT_MS) return EV_SHORT;
  }
  return EV_NONE;
}

/**************************************************************
 * LCD RENDER
 **************************************************************/
static void renderHome(){
  String w = (wifiSt.mode==WifiStatus::WIFI_STA && wifiSt.connected) ? "STA" : "AP ";
  String m = mqttSt.connected ? "M" : " ";
  lcdSetLine(0, "HydroNode " + w + " " + m);

  float ec_ms = sens.ec_us / 1000.0f;

  char l1[32];
  if (isnan(sens.temp_c)) snprintf(l1, sizeof(l1), "EC:%4.2fmS  T:--.-C", ec_ms);
  else                   snprintf(l1, sizeof(l1), "EC:%4.2fmS  T:%4.1fC", ec_ms, sens.temp_c);
  lcdSetLine(1, String(l1));

  char l2[32]; snprintf(l2, sizeof(l2), "Water: %6.1f %%", sens.lvl_percent);
  lcdSetLine(2, String(l2));

  if (wifiSt.mode==WifiStatus::WIFI_STA && wifiSt.connected) lcdSetLine(3, "IP: " + wifiSt.ip);
  else lcdSetLine(3, "AP: 192.168.4.1");
}

static void renderMenu(){
  lcdSetLine(0, "Menu");
  lcdSetLine(1, (menuIndex==0?"> ":"  ") + String("Setup"));
  lcdSetLine(2, (menuIndex==1?"> ":"  ") + String("Calibration"));
  lcdSetLine(3, (menuIndex==2?"> ":"  ") + String("Info / Exit"));
}

static void renderSetup(){
  lcdSetLine(0, "Setup");
  lcdSetLine(1, "MQTT via Web API");
  lcdSetLine(2, "Hold LIGHT = WiFiRST");
  lcdSetLine(3, "Back");
}

static void renderInfo(){
  lcdSetLine(0, String("FW: ")+FW_VERSION);
  lcdSetLine(1, String("MQTT: ")+(mqttSt.connected?"OK":"OFF"));
  lcdSetLine(2, String("Topic: ")+mqttCfg.base_topic);
  lcdSetLine(3, "Back");
}

static void renderCalMenu(){
  lcdSetLine(0, "Calibration");
  lcdSetLine(1, (calIndex==0?"> ":"  ") + String("EC Wizard"));
  lcdSetLine(2, (calIndex==1?"> ":"  ") + String("Level Wizard"));
  lcdSetLine(3, (calIndex==2?"> ":"  ") + String("Back"));
}

static void renderEcWizard(){
  lcdSetLine(0, "EC Wizard (V->EC)");
  if (ecStep == EC_A_SET){
    lcdSetLine(1, "Set A solution:");
    lcdSetLine(2, "A=" + String(ecWizardA,0) + " uS");
    lcdSetLine(3, "UP/DN adj,ENT next");
  } else if (ecStep == EC_A_CAP){
    lcdSetLine(1, "In A solution now");
    lcdSetLine(2, "ENT capture voltage");
    lcdSetLine(3, "Back");
  } else if (ecStep == EC_B_SET){
    lcdSetLine(1, "Set B solution:");
    lcdSetLine(2, "B=" + String(ecWizardB,0) + " uS");
    lcdSetLine(3, "UP/DN adj,ENT next");
  } else if (ecStep == EC_B_CAP){
    lcdSetLine(1, "In B solution now");
    lcdSetLine(2, "ENT capture voltage");
    lcdSetLine(3, "Back");
  } else {
    lcdSetLine(1, "Compute + Save");
    lcdSetLine(2, "ENT confirm");
    lcdSetLine(3, "Back");
  }
}

static void renderLevelUnit(){
  lcdSetLine(0, "Level Unit");
  String u = (lvlCal.unit==UNIT_PERCENT) ? "%" : "CUSTOM";
  lcdSetLine(1, "Unit: " + u);
  if (lvlCal.unit==UNIT_CUSTOM) lcdSetLine(2, "Max: " + String(lvlCal.custom_max,1));
  else lcdSetLine(2, " ");
  lcdSetLine(3, "UP toggle,ENT ok");
}

static void renderLevelWizard(){
  lcdSetLine(0, "Level Wizard");
  if (lvlStep == LVL_UNIT){
    lcdSetLine(1, "Select unit first");
    lcdSetLine(2, "ENT -> Unit setup");
    lcdSetLine(3, "Back");
  } else if (lvlStep == LVL_EMPTY_SET){
    lcdSetLine(1, "Empty value:");
    lcdSetLine(2, String(lvlWizardEmpty,1));
    lcdSetLine(3, "UP/DN adj,ENT next");
  } else if (lvlStep == LVL_EMPTY_CAP){
    lcdSetLine(1, "Set EMPTY state");
    lcdSetLine(2, "ENT capture voltage");
    lcdSetLine(3, "Back");
  } else if (lvlStep == LVL_FULL_SET){
    lcdSetLine(1, "Full value:");
    lcdSetLine(2, String(lvlWizardFull,1));
    lcdSetLine(3, "UP/DN adj,ENT next");
  } else if (lvlStep == LVL_FULL_CAP){
    lcdSetLine(1, "Set FULL state");
    lcdSetLine(2, "ENT capture voltage");
    lcdSetLine(3, "Back");
  } else {
    lcdSetLine(1, "Compute + Save");
    lcdSetLine(2, "ENT confirm");
    lcdSetLine(3, "Back");
  }
}

static void lcdTick(){
  if (lcdBacklight) lcd.backlight();
  else lcd.noBacklight();

  switch(ui){
    case UI_HOME:       renderHome(); break;
    case UI_MENU:       renderMenu(); break;
    case UI_SETUP:      renderSetup(); break;
    case UI_INFO:       renderInfo(); break;
    case UI_CAL_MENU:   renderCalMenu(); break;
    case UI_CAL_EC:     renderEcWizard(); break;
    case UI_LEVEL_UNIT: renderLevelUnit(); break;
    case UI_CAL_LEVEL:  renderLevelWizard(); break;
    default:            renderHome(); break;
  }
}

/**************************************************************
 * UI EVENT HANDLER
 **************************************************************/
static void handleEvent(BtnId b, EvType ev){
  if (ev == EV_NONE) return;

  if (b == BTN_LIGHT){
    if (ev == EV_SHORT){
      if (ui == UI_HOME) uiSet(UI_MENU);
      else if (ui == UI_MENU) uiSet(UI_HOME);
      else uiSet(UI_MENU);
    } else if (ev == EV_LONG){
      lcdBacklight = !lcdBacklight;
    } else if (ev == EV_VLONG){
      wipeWiFiAndRestart();
    }
    return;
  }

  if (b == BTN_UP){
    if (ev != EV_SHORT) return;

    if (ui == UI_MENU){
      menuIndex = (menuIndex + MENU_N - 1) % MENU_N;
    } else if (ui == UI_CAL_MENU){
      calIndex = (calIndex + CAL_N - 1) % CAL_N;
    } else if (ui == UI_CAL_EC){
      if (ecStep == EC_A_SET) ecWizardA += 10.0f;
      else if (ecStep == EC_B_SET) ecWizardB += 100.0f;
    } else if (ui == UI_LEVEL_UNIT){
      lvlCal.unit = (lvlCal.unit == UNIT_PERCENT) ? UNIT_CUSTOM : UNIT_PERCENT;
    } else if (ui == UI_CAL_LEVEL){
      if (lvlStep == LVL_EMPTY_SET) lvlWizardEmpty += 1.0f;
      else if (lvlStep == LVL_FULL_SET) lvlWizardFull += 1.0f;
    }
    return;
  }

  if (b == BTN_DN){
    // MENU: short=DOWN, long=ENTER
    if (ui == UI_MENU){
      if (ev == EV_SHORT){
        menuIndex = (menuIndex + 1) % MENU_N;
      } else if (ev == EV_LONG){
        if (menuIndex == 0) uiSet(UI_SETUP);
        else if (menuIndex == 1) uiSet(UI_CAL_MENU);
        else uiSet(UI_INFO);
      }
      return;
    }

    if (ui == UI_CAL_MENU){
      if (ev == EV_SHORT){
        calIndex = (calIndex + 1) % CAL_N;
      } else if (ev == EV_LONG){
        if (calIndex == 0){
          ecStep = EC_A_SET;
          ecWizardA = ecCal.A.ec_us;
          ecWizardB = ecCal.B.ec_us;
          uiSet(UI_CAL_EC);
        } else if (calIndex == 1){
          lvlStep = LVL_UNIT;
          lvlWizardEmpty = 0.0f;
          lvlWizardFull  = (lvlCal.unit==UNIT_PERCENT) ? 100.0f : lvlCal.custom_max;
          uiSet(UI_CAL_LEVEL);
        } else {
          uiSet(UI_MENU);
        }
      }
      return;
    }

    // Others: short=decrement/back, long=enter/next
    if (ui == UI_SETUP || ui == UI_INFO){
      if (ev == EV_SHORT || ev == EV_LONG) uiSet(UI_MENU);
      return;
    }

    if (ui == UI_CAL_EC){
      if (ev == EV_SHORT){
        if (ecStep == EC_A_SET) { ecWizardA -= 10.0f; if (ecWizardA < 0) ecWizardA = 0; }
        else if (ecStep == EC_B_SET) { ecWizardB -= 100.0f; if (ecWizardB < 0) ecWizardB = 0; }
        return;
      }
      if (ev == EV_LONG){
        if (ecStep == EC_A_SET) ecStep = EC_A_CAP;
        else if (ecStep == EC_A_CAP){
          ecCal.A.ec_us = ecWizardA;
          ecCal.A.v     = sens.ec_v;
          ecCal.A.set   = true;
          saveEcCal();
          ecStep = EC_B_SET;
        } else if (ecStep == EC_B_SET) ecStep = EC_B_CAP;
        else if (ecStep == EC_B_CAP){
          ecCal.B.ec_us = ecWizardB;
          ecCal.B.v     = sens.ec_v;
          ecCal.B.set   = true;
          saveEcCal();
          ecStep = EC_DONE;
        } else {
          computeEcCal();
          saveEcCal();
          uiSet(UI_MENU);
        }
        return;
      }
      return;
    }

    if (ui == UI_LEVEL_UNIT){
      if (ev == EV_SHORT){
        if (lvlCal.unit == UNIT_CUSTOM) {
          lvlCal.custom_max -= 1.0f;
          if (lvlCal.custom_max < 1.0f) lvlCal.custom_max = 1.0f;
        } else {
          lvlCal.unit = UNIT_CUSTOM;
        }
        return;
      }
      if (ev == EV_LONG){
        saveLevelCal();
        uiSet(UI_CAL_LEVEL);
        lvlStep = LVL_EMPTY_SET;
        lvlWizardEmpty = 0.0f;
        lvlWizardFull  = (lvlCal.unit==UNIT_PERCENT) ? 100.0f : lvlCal.custom_max;
        return;
      }
      return;
    }

    if (ui == UI_CAL_LEVEL){
      if (ev == EV_SHORT){
        if (lvlStep == LVL_EMPTY_SET) { lvlWizardEmpty -= 1.0f; if (lvlWizardEmpty < 0) lvlWizardEmpty = 0; }
        else if (lvlStep == LVL_FULL_SET) { lvlWizardFull -= 1.0f; if (lvlWizardFull < 0) lvlWizardFull = 0; }
        return;
      }
      if (ev == EV_LONG){
        if (lvlStep == LVL_UNIT){
          uiSet(UI_LEVEL_UNIT);
        } else if (lvlStep == LVL_EMPTY_SET) lvlStep = LVL_EMPTY_CAP;
        else if (lvlStep == LVL_EMPTY_CAP){
          lvlCal.empty.level = lvlWizardEmpty;
          lvlCal.empty.v     = sens.lvl_v;
          lvlCal.empty.set   = true;
          saveLevelCal();
          lvlStep = LVL_FULL_SET;
        } else if (lvlStep == LVL_FULL_SET) lvlStep = LVL_FULL_CAP;
        else if (lvlStep == LVL_FULL_CAP){
          lvlCal.full.level = lvlWizardFull;
          lvlCal.full.v     = sens.lvl_v;
          lvlCal.full.set   = true;
          saveLevelCal();
          lvlStep = LVL_DONE;
        } else {
          if (lvlCal.unit == UNIT_CUSTOM) lvlCal.custom_max = lvlWizardFull;
          computeLevelCal();
          saveLevelCal();
          uiSet(UI_MENU);
        }
        return;
      }
      return;
    }

    uiSet(UI_MENU);
    return;
  }
}

/**************************************************************
 * MQTT (FIXED: no UI stall)
 **************************************************************/
static void mqttEnsure(){
  // ✅ NEVER try MQTT in AP mode or without WiFi
  if (apMode || WiFi.status() != WL_CONNECTED) {
    mqttSt.connected = false;
    return;
  }

  mqttSt.configured = mqttCfg.enabled && mqttCfg.host.length() > 0;
  if (!mqttSt.configured) return;

  mqtt.setServer(mqttCfg.host.c_str(), mqttCfg.port);

  if (mqtt.connected()){
    mqttSt.connected = true;
    return;
  }

  // ✅ Slow reconnect attempts (reduces stall frequency if broker down)
  uint32_t now = millis();
  if (now - mqttSt.lastAttemptMs < 15000) return;
  mqttSt.lastAttemptMs = now;

  String cid = String("hydronode-") + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok;
  if (mqttCfg.user.length() > 0) ok = mqtt.connect(cid.c_str(), mqttCfg.user.c_str(), mqttCfg.pass.c_str());
  else ok = mqtt.connect(cid.c_str());

  mqttSt.connected = ok;
  mqttSt.err = ok ? "" : String(mqtt.state());
}

static void mqttPublish(){
  if (!mqttSt.connected) return;
  if (apMode || WiFi.status() != WL_CONNECTED) return; // safety

  uint32_t now = millis();
  if (now - mqttSt.lastPublishMs < mqttCfg.pub_period_ms) return;
  mqttSt.lastPublishMs = now;

  const String base = mqttCfg.base_topic;

  StaticJsonDocument<640> doc;
  doc["fw"] = FW_VERSION;
  doc["ip"] = wifiSt.ip;
  doc["wifi_mode"] = (uint8_t)wifiSt.mode;
  doc["mqtt"] = mqttSt.connected;
  doc["ec_us"] = sens.ec_us;
  doc["ec_v"] = sens.ec_v;
  doc["level_percent"] = sens.lvl_percent;
  doc["level_value"] = sens.lvl_value;
  doc["level_v"] = sens.lvl_v;
  doc["temp_c"] = sens.temp_c;

  String payload;
  serializeJson(doc, payload);

  mqtt.publish((base + "/status").c_str(), payload.c_str(), mqttCfg.retain);
  mqtt.publish((base + "/ec").c_str(), String(sens.ec_us, 0).c_str(), mqttCfg.retain);
  mqtt.publish((base + "/level/percent").c_str(), String(sens.lvl_percent, 1).c_str(), mqttCfg.retain);
  mqtt.publish((base + "/level/value").c_str(), String(sens.lvl_value, 2).c_str(), mqttCfg.retain);

  // avoid publishing "nan"
  if (!isnan(sens.temp_c)) {
    mqtt.publish((base + "/temp_c").c_str(), String(sens.temp_c, 1).c_str(), mqttCfg.retain);
  }
}

/**************************************************************
 * WEB: JSON helpers
 **************************************************************/
static void sendJson(AsyncWebServerRequest *req, const JsonDocument& doc){
  String s;
  serializeJson(doc, s);
  req->send(200, "application/json", s);
}

/**************************************************************
 * WEB: ROUTES
 **************************************************************/
static void setupRoutes(){
  if (apMode) {
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("ap.html");
  } else {
    server.serveStatic("/", LittleFS, "/www/")
          .setDefaultFile("index.html")
          .setAuthentication(UI_USER, UI_PASS);
  }

  server.onNotFound([](AsyncWebServerRequest *req){
    if (apMode){
      if (LittleFS.exists("/www/ap.html")) req->send(LittleFS, "/www/ap.html", "text/html");
      else req->send(200, "text/plain", "AP mode: upload /www/ap.html");
      return;
    }
    req->send(404, "text/plain", "Not found");
  });

  server.on("/api/wifi", HTTP_POST,
    [](AsyncWebServerRequest *req){},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      StaticJsonDocument<384> in;
      StaticJsonDocument<256> out;

      auto err = deserializeJson(in, data, len);
      if (err){
        out["ok"] = false;
        out["err"] = "bad_json";
        sendJson(req, out);
        return;
      }

      String ssid = in["ssid"] | "";
      String pass = in["pass"] | "";
      ssid.trim();

      if (ssid.length() == 0){
        out["ok"] = false;
        out["err"] = "ssid_required";
        sendJson(req, out);
        return;
      }

      saveWiFiCreds(ssid, pass);

      out["ok"] = true;
      out["saved"] = true;
      out["rebooting"] = true;
      sendJson(req, out);

      delay(400);
      ESP.restart();
    }
  );

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<896> doc;
    doc["ok"] = true;
    doc["fw"] = FW_VERSION;
    doc["api"] = API_VERSION;

    doc["wifi"]["mode"] = (uint8_t)wifiSt.mode;
    doc["wifi"]["connected"] = wifiSt.connected;
    doc["wifi"]["ip"] = wifiSt.ip;
    doc["wifi"]["ssid"] = wifiSt.ssid;

    doc["mqtt"]["enabled"] = mqttCfg.enabled;
    doc["mqtt"]["connected"] = mqttSt.connected;
    doc["mqtt"]["base_topic"] = mqttCfg.base_topic;
    doc["mqtt"]["err"] = mqttSt.err;

    doc["temp_c"] = sens.temp_c;
    sendJson(req, doc);
  });

  server.on("/api/ec", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<512> doc;
    doc["ok"] = true;
    doc["us_cm"] = sens.ec_us;
    doc["v"] = sens.ec_v;
    doc["adc_raw"] = sens.ec_adc_raw;
    sendJson(req, doc);
  });

  server.on("/api/level", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<512> doc;
    doc["ok"] = true;
    doc["percent"] = sens.lvl_percent;
    doc["value"] = sens.lvl_value;
    doc["v"] = sens.lvl_v;
    doc["adc_raw"] = sens.lvl_adc_raw;
    doc["unit"] = (uint8_t)lvlCal.unit;
    doc["custom_max"] = lvlCal.custom_max;
    sendJson(req, doc);
  });

  server.on("/api/temp", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<256> doc;
    doc["ok"] = true;
    doc["temp_c"] = sens.temp_c;
    sendJson(req, doc);
  });

  server.on("/api/cal", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<768> doc;
    doc["ok"] = true;

    doc["ec"]["A_set"] = ecCal.A.set;
    doc["ec"]["B_set"] = ecCal.B.set;
    doc["ec"]["A_ec"]  = ecCal.A.ec_us;
    doc["ec"]["B_ec"]  = ecCal.B.ec_us;
    doc["ec"]["A_v"]   = ecCal.A.v;
    doc["ec"]["B_v"]   = ecCal.B.v;
    doc["ec"]["valid"] = ecCal.valid;
    doc["ec"]["quality"] = (uint8_t)ecCal.quality;

    doc["level"]["E_set"] = lvlCal.empty.set;
    doc["level"]["F_set"] = lvlCal.full.set;
    doc["level"]["E_lvl"] = lvlCal.empty.level;
    doc["level"]["F_lvl"] = lvlCal.full.level;
    doc["level"]["E_v"]   = lvlCal.empty.v;
    doc["level"]["F_v"]   = lvlCal.full.v;
    doc["level"]["valid"] = lvlCal.valid;
    doc["level"]["quality"] = (uint8_t)lvlCal.quality;
    doc["level"]["unit"] = (uint8_t)lvlCal.unit;
    doc["level"]["custom_max"] = lvlCal.custom_max;

    sendJson(req, doc);
  });

  server.on("/api/settings/mqtt", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<512> doc;
    doc["ok"] = true;
    doc["enabled"] = mqttCfg.enabled;
    doc["host"] = mqttCfg.host;
    doc["port"] = mqttCfg.port;
    doc["user"] = mqttCfg.user;
    doc["pass"] = mqttCfg.pass;
    doc["base_topic"] = mqttCfg.base_topic;
    doc["retain"] = mqttCfg.retain;
    doc["pub_period_ms"] = mqttCfg.pub_period_ms;
    sendJson(req, doc);
  });

  server.on("/api/settings/mqtt", HTTP_POST,
    [](AsyncWebServerRequest *req){},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t){
      StaticJsonDocument<768> in;
      auto err = deserializeJson(in, data, len);

      StaticJsonDocument<256> out;
      if (err){
        out["ok"] = false;
        out["err"] = "bad_json";
        sendJson(req, out);
        return;
      }

      if (in.containsKey("enabled")) mqttCfg.enabled = in["enabled"].as<bool>();
      if (in.containsKey("host")) mqttCfg.host = String((const char*)in["host"]);
      if (in.containsKey("port")) mqttCfg.port = (uint16_t)in["port"].as<int>();
      if (in.containsKey("user")) mqttCfg.user = String((const char*)in["user"]);
      if (in.containsKey("pass")) mqttCfg.pass = String((const char*)in["pass"]);
      if (in.containsKey("base_topic")) mqttCfg.base_topic = String((const char*)in["base_topic"]);
      if (in.containsKey("retain")) mqttCfg.retain = in["retain"].as<bool>();
      if (in.containsKey("pub_period_ms")) mqttCfg.pub_period_ms = (uint16_t)in["pub_period_ms"].as<int>();

      saveMqtt();
      out["ok"] = true;
      sendJson(req, out);
    }
  );
}

/**************************************************************
 * SETUP / LOOP
 **************************************************************/
static void lcdInit(){
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdSetLine(0, "HydroNode");
  lcdSetLine(1, "EC + Water Level");
  lcdSetLine(2, "Booting...");
  lcdSetLine(3, "");
}

void setup(){
  Serial.begin(115200);

  pinMode(PIN_BTN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DN, INPUT_PULLUP);

  analogReadResolution(12);

  lcdInit();

  ds18.begin();
  ds18.setWaitForConversion(false);
  ds18.requestTemperatures();

  loadMqtt();
  mqtt.setSocketTimeout(1);          // ✅ key fix: reduce blocking time
  loadEcCal();
  loadLevelCal();
  computeEcCal();
  computeLevelCal();

  if (!LittleFS.begin(true)){
    Serial.println("LittleFS mount failed");
  }

  startSTA();
  uint32_t t0 = millis();
  while (millis() - t0 < 8000){
    if (WiFi.status() == WL_CONNECTED) break;
    delay(50);
  }
  if (WiFi.status() != WL_CONNECTED){
    startAP();
  }

  setupRoutes();
  server.begin();

  lcd.clear();
  uiSet(UI_HOME);
}

void loop(){
  static uint32_t lastUi=0, lastSensor=0, lastMqtt=0;
  uint32_t now = millis();

  // ✅ Buttons FIRST (so UI stays responsive)
  EvType e0 = pollButton(BTN_LIGHT);
  EvType e1 = pollButton(BTN_UP);
  EvType e2 = pollButton(BTN_DN);

  handleEvent(BTN_LIGHT, e0);
  handleEvent(BTN_UP, e1);
  handleEvent(BTN_DN, e2);

  // WiFi + captive portal
  wifiTick();

  // Sensors
  if (now - lastSensor >= TICK_SENSOR_MS){
    lastSensor = now;
    sensorTick();
  }

  // LCD
  if (now - lastUi >= TICK_UI_MS){
    lastUi = now;
    lcdTick();
  }

  // MQTT LAST (never let it block buttons)
  if (now - lastMqtt >= TICK_MQTT_MS){
    lastMqtt = now;
    mqttEnsure();
    mqtt.loop();
    mqttPublish();
  }
}
