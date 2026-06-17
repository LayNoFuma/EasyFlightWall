#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <time.h>

// =====================================================
// WIFI
// =====================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// =====================================================
// FLIGHTAWARE
// =====================================================
const char* FLIGHTAWARE_API_KEY = "YOUR_FLIGHTAWARE_API_KEY";

// =====================================================
// OPENSKY OAUTH2
// =====================================================
const char* OPENSKY_CLIENT_ID     = "YOUR_OPENSKY_CLIENT_ID";
const char* OPENSKY_CLIENT_SECRET = "YOUR_OPENSKY_CLIENT_SECRET";

const char* OPENSKY_TOKEN_URL =
  "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";

const char* OPENSKY_URL =
  "https://opensky-network.org/api/states/all?lamin=45.72&lomin=13.23&lamax=45.79&lomax=13.47";

const float CENTER_LAT = (45.72f + 45.79f) / 2.0f;
const float CENTER_LON = (13.23f + 13.47f) / 2.0f;

// =====================================================
// ITALY TIMEZONE
// =====================================================
const char* TZ_INFO    = "CET-1CEST,M3.5.0/2,M10.5.0/3";
const char* NTP_SERVER = "pool.ntp.org";

// =====================================================
// SLEEP MODE
// =====================================================
const int SLEEP_START_HOUR = 23; // 23:00
const int SLEEP_END_HOUR   = 7;  // 07:00

const uint8_t BRIGHTNESS_NORMAL = 16;
const uint8_t BRIGHTNESS_SLEEP  = 4;

// =====================================================
// HUB75 PIN MAPPING
// =====================================================
#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27

#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13

#define A_PIN 33
#define B_PIN 32
#define C_PIN 22
#define D_PIN 17   // TX2
#define E_PIN -1   // not used

#define CLK_PIN 16 // RX2
#define LAT_PIN 4
#define OE_PIN 15

// =====================================================
// PANEL CONFIG
// =====================================================
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_CHAIN 1

MatrixPanel_I2S_DMA *dma_display = nullptr;

// =====================================================
// APP STATE
// =====================================================
String currentCallsign = "";
String currentIcao24   = "";

String cachedCallsign = "";
String routeOrigin    = "";
String routeDest      = "";
String aircraftType   = "";

unsigned long lastOpenSkyCall = 0;
// 20 seconds: safer and more robust with quota/rate limits
const unsigned long OPENSKY_INTERVAL_MS = 20000;

// Backoff if OpenSky returns 429
unsigned long openSkyBackoffUntil = 0;
unsigned long openSkyRetryDelayMs = 15000;

// Arrow animation
uint8_t arrowFrame = 0;
unsigned long lastArrowAnim = 0;
const unsigned long ARROW_ANIM_INTERVAL_MS = 180;

// Keep last valid flight on screen for a while
unsigned long lastFlightSeenMs = 0;
const unsigned long HOLD_FLIGHT_MS = 15000;

// Lightweight redraw timers
unsigned long lastDisplayUpdate = 0;
unsigned long lastClockUpdate   = 0;

// OpenSky token
String openskyAccessToken = "";
unsigned long openskyTokenExpiresAt = 0;

// =====================================================
// Utility: clean callsign
// =====================================================
String normalizeCallsign(String cs) {
  cs.trim();
  cs.toUpperCase();

  String out = "";
  for (size_t i = 0; i < cs.length(); i++) {
    char c = cs[i];
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      out += c;
    }
  }
  return out;
}

// =====================================================
// Utility: centered text
// =====================================================
int textXCentered(const String &txt, int textSize = 1) {
  int16_t x1, y1;
  uint16_t w, h;
  dma_display->setTextSize(textSize);
  dma_display->getTextBounds((char*)txt.c_str(), 0, 0, &x1, &y1, &w, &h);
  return (PANEL_RES_X - w) / 2;
}

// =====================================================
// Utility: right-aligned text
// =====================================================
int textXRight(const String &txt, int rightX, int textSize = 1) {
  int16_t x1, y1;
  uint16_t w, h;
  dma_display->setTextSize(textSize);
  dma_display->getTextBounds((char*)txt.c_str(), 0, 0, &x1, &y1, &w, &h);
  return rightX - w;
}

// =====================================================
// Utility: light frame
// =====================================================
void drawFrame(uint16_t color) {
  dma_display->drawRect(0, 0, PANEL_RES_X, PANEL_RES_Y, color);
}

// =====================================================
// Utility: animated arrow
// =====================================================
void drawAnimatedArrow(int x, int y, int length, uint16_t color, uint8_t frame) {
  dma_display->drawLine(x, y, x + length, y, color);
  dma_display->drawLine(x + length, y, x + length - 3, y - 3, color);
  dma_display->drawLine(x + length, y, x + length - 3, y + 3, color);

  int maxStep = max(1, length - 3);
  int step = frame % maxStep;
  int dotX = x + 1 + step;
  dma_display->drawPixel(dotX, y, dma_display->color565(255, 180, 0));
}

// =====================================================
// Utility: moon icon
// =====================================================
void drawMoon(int x, int y, uint16_t moonColor, uint16_t bgColor) {
  dma_display->fillCircle(x, y, 5, moonColor);
  dma_display->fillCircle(x + 2, y - 1, 5, bgColor);
}

// =====================================================
// Sleep mode?
// =====================================================
bool isSleepMode() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;

  int h = timeinfo.tm_hour;

  if (SLEEP_START_HOUR < SLEEP_END_HOUR) {
    return (h >= SLEEP_START_HOUR && h < SLEEP_END_HOUR);
  } else {
    return (h >= SLEEP_START_HOUR || h < SLEEP_END_HOUR);
  }
}

// =====================================================
// WiFi
// =====================================================
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 60) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("ERROR: WiFi NOT connected");
  }
}

// =====================================================
// TIME / NTP
// =====================================================
void setupTimeSync() {
  configTzTime(TZ_INFO, NTP_SERVER);
  Serial.println("NTP configured");
}

bool waitForTimeSync(unsigned long timeoutMs = 10000) {
  struct tm timeinfo;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (getLocalTime(&timeinfo)) {
      Serial.println("NTP synchronized");
      return true;
    }
    delay(500);
    Serial.println("Waiting for NTP...");
  }

  Serial.println("NTP timeout");
  return false;
}

// =====================================================
// OPEN SKY OAUTH2
// =====================================================
bool requestOpenSkyToken() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, OPENSKY_TOKEN_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body =
    "grant_type=client_credentials"
    "&client_id=" + String(OPENSKY_CLIENT_ID) +
    "&client_secret=" + String(OPENSKY_CLIENT_SECRET);

  int httpCode = http.POST(body);

  Serial.print("OpenSky token HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != 200) {
    String errPayload = http.getString();
    Serial.println("OpenSky token error:");
    Serial.println(errPayload);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("OpenSky token parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  if (!doc.containsKey("access_token")) {
    Serial.println("OpenSky: missing access_token");
    return false;
  }

  openskyAccessToken = doc["access_token"].as<String>();

  int expiresIn = 1800;
  if (doc.containsKey("expires_in") && !doc["expires_in"].isNull()) {
    expiresIn = doc["expires_in"].as<int>();
  }

  // Renew 60 seconds before expiration
  int safeExpires = expiresIn > 60 ? (expiresIn - 60) : expiresIn;
  openskyTokenExpiresAt = millis() + (unsigned long)safeExpires * 1000UL;

  Serial.println("OpenSky token obtained successfully");
  return true;
}

bool ensureOpenSkyToken() {
  if (openskyAccessToken.length() == 0 || millis() > openskyTokenExpiresAt) {
    Serial.println("Requesting / refreshing OpenSky token...");
    return requestOpenSkyToken();
  }
  return true;
}

// =====================================================
// Flight screen
// =====================================================
void drawFlightScreen(const String& callsign,
                      const String& origin,
                      const String& dest,
                      const String& aircraft) {
  dma_display->clearScreen();
  drawFrame(dma_display->color565(0, 40, 60));

  dma_display->setTextWrap(false);
  dma_display->setTextSize(1);

  // CALLSIGN
  dma_display->setTextColor(dma_display->color565(255, 255, 0));
  dma_display->setCursor(textXCentered(callsign, 1), 2);
  dma_display->print(callsign);

  // ROUTE
  String org = origin.length() ? origin : "---";
  String dst = dest.length()   ? dest   : "---";

  dma_display->setTextColor(dma_display->color565(0, 255, 255));
  dma_display->setCursor(2, 14);
  dma_display->print(org);

  drawAnimatedArrow(22, 18, 18, dma_display->color565(255, 255, 255), arrowFrame);

  dma_display->setCursor(44, 14);
  dma_display->print(dst);

  // AIRCRAFT TYPE
  String ac = aircraft.length() ? aircraft : "UNK";
  dma_display->setTextColor(dma_display->color565(255, 0, 255));
  dma_display->setCursor(textXCentered(ac, 1), 25);
  dma_display->print(ac);
}

// =====================================================
// Clock + date screen
// =====================================================
void drawClockScreen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    dma_display->clearScreen();
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(255, 0, 0));
    dma_display->setCursor(8, 10);
    dma_display->print("NO TIME");
    return;
  }

  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  char dateBuf[11];
  snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d",
           timeinfo.tm_mday,
           timeinfo.tm_mon + 1,
           timeinfo.tm_year + 1900);

  dma_display->clearScreen();

  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(0, 255, 0));
  dma_display->setCursor(textXCentered(String(timeBuf), 1), 5);
  dma_display->print(timeBuf);

  dma_display->setTextColor(dma_display->color565(255, 255, 0));
  dma_display->setCursor(textXCentered(String(dateBuf), 1), 19);
  dma_display->print(dateBuf);
}

// =====================================================
// Sleep screen
// =====================================================
void drawSleepScreen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    dma_display->clearScreen();
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(120, 0, 120));
    dma_display->setCursor(8, 10);
    dma_display->print("SLEEP");
    return;
  }

  char timeBuf[6];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  uint16_t bgColor    = dma_display->color565(0, 0, 0);
  uint16_t moonColor  = dma_display->color565(180, 180, 180);
  uint16_t textPurple = dma_display->color565(180, 0, 180);

  dma_display->clearScreen();
  drawMoon(56, 7, moonColor, bgColor);

  dma_display->setTextSize(1);
  dma_display->setTextColor(textPurple);
  dma_display->setCursor(textXCentered(String(timeBuf), 1), 15);
  dma_display->print(timeBuf);
}

// =====================================================
// OpenSky: nearest flight via authenticated Bearer token
// =====================================================
bool fetchNearestOpenSky(String &outCallsign, String &outIcao24) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!ensureOpenSkyToken()) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, OPENSKY_URL);
  http.addHeader("Authorization", "Bearer " + openskyAccessToken);

  int httpCode = http.GET();
  Serial.print("OpenSky HTTP code: ");
  Serial.println(httpCode);

  // Backoff on 429
  if (httpCode == 429) {
    Serial.println("OpenSky RATE LIMIT (429)");
    openSkyBackoffUntil = millis() + openSkyRetryDelayMs;
    openSkyRetryDelayMs = (openSkyRetryDelayMs < 300000UL / 2) ? openSkyRetryDelayMs * 2 : 300000UL;
    http.end();
    return false;
  }

  // Expired / invalid token
  if (httpCode == 401) {
    Serial.println("OpenSky 401, token expired? Refreshing...");
    http.end();
    openskyAccessToken = "";
    if (!ensureOpenSkyToken()) return false;

    WiFiClientSecure clientRetry;
    clientRetry.setInsecure();
    HTTPClient httpRetry;
    httpRetry.begin(clientRetry, OPENSKY_URL);
    httpRetry.addHeader("Authorization", "Bearer " + openskyAccessToken);

    httpCode = httpRetry.GET();
    Serial.print("OpenSky retry HTTP code: ");
    Serial.println(httpCode);

    if (httpCode != 200) {
      httpRetry.end();
      return false;
    }

    String payload = httpRetry.getString();
    httpRetry.end();

    DynamicJsonDocument doc(16000);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("OpenSky JSON error: ");
      Serial.println(err.c_str());
      return false;
    }

    JsonArray states = doc["states"].as<JsonArray>();
    if (states.isNull() || states.size() == 0) return false;

    float bestDist2 = 999999.0f;
    String bestCs = "";
    String bestIcao = "";

    for (JsonVariant v : states) {
      if (!v.is<JsonArray>()) continue;
      JsonArray st = v.as<JsonArray>();

      if (st.size() < 7) continue;
      if (st[0].isNull() || st[1].isNull() || st[5].isNull() || st[6].isNull()) continue;

      float lon = st[5].as<float>();
      float lat = st[6].as<float>();

      String cs = normalizeCallsign(st[1].as<String>());
      String icao = st[0].as<String>();

      if (cs.length() == 0) continue;

      float dLat = lat - CENTER_LAT;
      float dLon = lon - CENTER_LON;
      float dist2 = dLat * dLat + dLon * dLon;

      if (dist2 < bestDist2) {
        bestDist2 = dist2;
        bestCs = cs;
        bestIcao = icao;
      }
    }

    if (bestCs.length() == 0) return false;

    // Reset backoff if OK
    openSkyRetryDelayMs = 15000;
    outCallsign = bestCs;
    outIcao24 = bestIcao;
    return true;
  }

  if (httpCode != 200) {
    String errPayload = http.getString();
    Serial.println("OpenSky error:");
    Serial.println(errPayload);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(16000);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("OpenSky JSON error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray states = doc["states"].as<JsonArray>();
  if (states.isNull() || states.size() == 0) {
    return false;
  }

  float bestDist2 = 999999.0f;
  String bestCs = "";
  String bestIcao = "";

  for (JsonVariant v : states) {
    if (!v.is<JsonArray>()) continue;
    JsonArray st = v.as<JsonArray>();

    if (st.size() < 7) continue;
    if (st[0].isNull() || st[1].isNull() || st[5].isNull() || st[6].isNull()) continue;

    float lon = st[5].as<float>();
    float lat = st[6].as<float>();

    String cs = normalizeCallsign(st[1].as<String>());
    String icao = st[0].as<String>();

    if (cs.length() == 0) continue;

    float dLat = lat - CENTER_LAT;
    float dLon = lon - CENTER_LON;
    float dist2 = dLat * dLat + dLon * dLon;

    if (dist2 < bestDist2) {
      bestDist2 = dist2;
      bestCs = cs;
      bestIcao = icao;
    }
  }

  if (bestCs.length() == 0) return false;

  // Reset backoff if OK
  openSkyRetryDelayMs = 15000;
  outCallsign = bestCs;
  outIcao24   = bestIcao;
  return true;
}

// =====================================================
// FlightAware lookup
// =====================================================
bool fetchFlightAwareDetails(const String& ident, String &outOrigin, String &outDest, String &outAircraft) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ident.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://aeroapi.flightaware.com/aeroapi/flights/" + ident + "?max_pages=1";

  Serial.print("FlightAware URL: ");
  Serial.println(url);

  http.begin(client, url);
  http.addHeader("x-apikey", FLIGHTAWARE_API_KEY);

  int httpCode = http.GET();
  Serial.print("FlightAware HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != 200) {
    String errPayload = http.getString();
    Serial.println("FlightAware error payload:");
    Serial.println(errPayload);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(20000);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("FlightAware JSON error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray flights = doc["flights"].as<JsonArray>();
  if (flights.isNull() || flights.size() == 0) {
    Serial.println("FlightAware: flights[] empty");
    return false;
  }

  JsonObject f = flights[0];

  outOrigin = "";
  outDest   = "";
  outAircraft = "";

  if (f.containsKey("origin") && f["origin"].is<JsonObject>()) {
    JsonObject origin = f["origin"];
    if (origin.containsKey("code_iata") && !origin["code_iata"].isNull()) {
      outOrigin = origin["code_iata"].as<String>();
    } else if (origin.containsKey("code") && !origin["code"].isNull()) {
      outOrigin = origin["code"].as<String>();
    } else if (origin.containsKey("icao") && !origin["icao"].isNull()) {
      outOrigin = origin["icao"].as<String>();
    }
  }

  if (f.containsKey("destination") && f["destination"].is<JsonObject>()) {
    JsonObject dest = f["destination"];
    if (dest.containsKey("code_iata") && !dest["code_iata"].isNull()) {
      outDest = dest["code_iata"].as<String>();
    } else if (dest.containsKey("code") && !dest["code"].isNull()) {
      outDest = dest["code"].as<String>();
    } else if (dest.containsKey("icao") && !dest["icao"].isNull()) {
      outDest = dest["icao"].as<String>();
    }
  }

  if (f.containsKey("aircraft_type") && !f["aircraft_type"].isNull()) {
    outAircraft = f["aircraft_type"].as<String>();
  } else if (f.containsKey("type") && !f["type"].isNull()) {
    outAircraft = f["type"].as<String>();
  }

  Serial.println("FlightAware parsed:");
  Serial.print("Origin: ");
  Serial.println(outOrigin);
  Serial.print("Destination: ");
  Serial.println(outDest);
  Serial.print("Aircraft: ");
  Serial.println(outAircraft);

  return true;
}

// =====================================================
// INIT DISPLAY
// =====================================================
void setupDisplay() {
  HUB75_I2S_CFG::i2s_pins _pins = {
    R1_PIN, G1_PIN, B1_PIN,
    R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
    LAT_PIN, OE_PIN, CLK_PIN
  };

  HUB75_I2S_CFG mxconfig(
    PANEL_RES_X,
    PANEL_RES_Y,
    PANEL_CHAIN,
    _pins
  );

  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;
  mxconfig.clkphase = false;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);

  if (!dma_display->begin()) {
    Serial.println("ERROR: display init failed");
    while (true) delay(1000);
  }

  dma_display->setBrightness8(BRIGHTNESS_NORMAL);
  dma_display->setLatBlanking(2);
  dma_display->clearScreen();
}

// =====================================================
// BOOT SCREEN
// =====================================================
void drawBootScreen() {
  dma_display->clearScreen();
  drawFrame(dma_display->color565(0, 50, 0));
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(0, 255, 0));
  dma_display->setCursor(textXCentered("FLIGHTWALL", 1), 6);
  dma_display->print("FLIGHTWALL");
  dma_display->setTextColor(dma_display->color565(255, 255, 0));
  dma_display->setCursor(textXCentered("BOOT...", 1), 18);
  dma_display->print("BOOT...");
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  setupDisplay();
  drawBootScreen();
  delay(1000);

  connectWiFi();
  setupTimeSync();
  waitForTimeSync(10000);

  // Get OpenSky token immediately
  if (!requestOpenSkyToken()) {
    Serial.println("WARNING: OpenSky token not obtained at startup");
  }

  dma_display->clearScreen();
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(0, 255, 255));
  dma_display->setCursor(textXCentered("READY", 1), 10);
  dma_display->print("READY");
  delay(800);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Arrow animation
  if (millis() - lastArrowAnim >= ARROW_ANIM_INTERVAL_MS) {
    lastArrowAnim = millis();
    arrowFrame++;
  }

  // Conservative WiFi reconnect
  static unsigned long lastWiFiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetry > 10000) {
    lastWiFiRetry = millis();
    Serial.println("Trying WiFi reconnect...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // ==========================
  // SLEEP MODE
  // ==========================
  if (isSleepMode()) {
    dma_display->setBrightness8(BRIGHTNESS_SLEEP);

    // No API calls during sleep
    if (millis() - lastClockUpdate > 1000) {
      lastClockUpdate = millis();
      drawSleepScreen();
    }

    delay(50);
    return;
  } else {
    dma_display->setBrightness8(BRIGHTNESS_NORMAL);
  }

  // ==========================
  // OPEN SKY POLLING
  // ==========================
  if (millis() >= openSkyBackoffUntil) {
    if (millis() - lastOpenSkyCall >= OPENSKY_INTERVAL_MS || lastOpenSkyCall == 0) {
      lastOpenSkyCall = millis();

      String newCallsign;
      String newIcao24;

      bool foundFlight = fetchNearestOpenSky(newCallsign, newIcao24);

      if (!foundFlight) {
        currentCallsign = "";
        currentIcao24   = "";
      } else {
        currentCallsign = newCallsign;
        currentIcao24   = newIcao24;
        lastFlightSeenMs = millis();

        // FlightAware only when callsign changes
        if (currentCallsign != cachedCallsign) {
          Serial.println("New callsign, requesting FlightAware...");
          cachedCallsign = currentCallsign;

          routeOrigin = "";
          routeDest   = "";
          aircraftType = "";

          bool ok = fetchFlightAwareDetails(currentCallsign, routeOrigin, routeDest, aircraftType);
          if (!ok) {
            Serial.println("FlightAware lookup failed or returned no data");
          }
        }
      }
    }
  }

  // ==========================
  // DISPLAY LOGIC
  // ==========================
  bool holdFlight =
    (cachedCallsign.length() > 0) &&
    (millis() - lastFlightSeenMs < HOLD_FLIGHT_MS);

  if (holdFlight) {
    if (millis() - lastDisplayUpdate > 180) {
      lastDisplayUpdate = millis();
      drawFlightScreen(cachedCallsign, routeOrigin, routeDest, aircraftType);
    }
  } else {
    if (millis() - lastClockUpdate > 1000) {
      lastClockUpdate = millis();
      drawClockScreen();
    }
  }

  delay(40);
}
