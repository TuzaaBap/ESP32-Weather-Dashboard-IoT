#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>          // for NTP time

// ------------- USER SETTINGS -------------
// >>>> CHANGE THESE <<<<
// ============================
// WiFi Credentials (Replace with your own)
// ============================

const char* SSID     = "YOUR_WIFI_NAME";
const char* PASSWORD = "YOUR_WIFI_PASSWORD";

// ============================
// Location Configuration
// ============================

// Example Coordinates (Replace with your city)
const float LATITUDE      = 00.0000;
const float LONGITUDE     = 00.0000;
const char* LOCATION_NAME = "Your_City";
// -----------------------------------------

// India (IST) offset
const long  gmtOffset_sec      = 19800;  // +5:30
const int   daylightOffset_sec = 0;

TFT_eSPI tft = TFT_eSPI();

// Layout
int screenW, screenH;

const int HEADER_H    = 26;
const int GRID_MARGIN = 4;
const int CELL_GAP    = 4;
int cellW, cellH;

// header time box (center region)
int headerTimeBoxX = 0;
int headerTimeBoxW = 0;
char lastTimeStrHdr[10] = "";  // "HH:MM:SS" + '\0'

// Colors
uint16_t COL_BG;
uint16_t COL_CARD;
uint16_t COL_TITLE;
uint16_t COL_LABEL;

// Data model
struct DashboardData {
  float  temp;        // 0
  int    humidity;    // 1
  String cond;        // 2
  float  wind;        // 3 (km/h)
  float  uv;          // 4
  float  vis;         // 5 (km)
  int    aqi;         // 6
  unsigned long age;  // 7 (seconds)
  String location;    // 8
};

DashboardData targetData;
DashboardData displayData;

// last drawn strings (per cell) to avoid flicker
char lastStr[9][20] = {0};

// update timers
const unsigned long WEATHER_UPDATE_INTERVAL_MS = 60UL * 1000UL; // 60 sec
unsigned long lastWeatherFetch   = 0;
unsigned long lastSmooth         = 0;
unsigned long lastAgeTick        = 0;
unsigned long lastHeaderUpdate   = 0;

// ------------- HELPERS: layout -------------

void indexToRowCol(int idx, int &row, int &col) {
  row = idx / 3;
  col = idx % 3;
}

void getCellXY(int row, int col, int &x, int &y) {
  x = GRID_MARGIN + col * (cellW + CELL_GAP);
  y = HEADER_H + GRID_MARGIN + row * (cellH + CELL_GAP);
}

// ------------- WIFI + TIME HELPERS -------------

String getLocalIPString() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return String("No WiFi");
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String("--:--:--");
  }
  char buf[9]; // "HH:MM:SS"
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

// ------------- DRAWING: HEADER -------------

// Draw header background + static text (title, IP) ONCE
void drawHeaderStatic() {
  tft.fillRect(0, 0, screenW, HEADER_H, COL_CARD);
  tft.setTextSize(1);

  // Left: title
  tft.setTextColor(COL_TITLE, COL_CARD);
  tft.setCursor(4, 4);
  tft.print("Weather Dashboard");

  // Right: IP
  String ipStr = "IP: " + getLocalIPString();
  uint16_t w = tft.textWidth(ipStr.c_str());
  tft.setCursor(screenW - w - 4, 4);
  tft.setTextColor(COL_LABEL, COL_CARD);
  tft.print(ipStr);

  // define a center box where time will live (not drawn yet)
  headerTimeBoxW = 80;                       // width of time area
  headerTimeBoxX = (screenW - headerTimeBoxW) / 2;
}

// Update only the time area in the header
void updateHeaderTime() {
  String timeStr = getTimeString();
  if (timeStr.length() >= sizeof(lastTimeStrHdr)) {
    timeStr = timeStr.substring(0, sizeof(lastTimeStrHdr) - 1);
  }

  // if same as last time, no redraw
  if (strcmp(lastTimeStrHdr, timeStr.c_str()) == 0) return;

  // store new time string
  strncpy(lastTimeStrHdr, timeStr.c_str(), sizeof(lastTimeStrHdr) - 1);
  lastTimeStrHdr[sizeof(lastTimeStrHdr) - 1] = '\0';

  // clear only the time region (small center box)
  tft.fillRect(headerTimeBoxX, 0, headerTimeBoxW, HEADER_H, COL_CARD);

  // draw time centered in that box
  tft.setTextSize(1);
  tft.setTextColor(COL_LABEL, COL_CARD);
  uint16_t w = tft.textWidth(lastTimeStrHdr);
  int tx = headerTimeBoxX + (headerTimeBoxW - w) / 2;
  tft.setCursor(tx, 4);
  tft.print(lastTimeStrHdr);
}

// ------------- DRAWING: CARDS -------------

void drawCardFrame(int idx, const char* label) {
  int row, col, x, y;
  indexToRowCol(idx, row, col);
  getCellXY(row, col, x, y);

  tft.fillRoundRect(x, y, cellW, cellH, 6, COL_CARD);
  tft.drawRoundRect(x, y, cellW, cellH, 6, tft.color565(0, 200, 180)); // neon border

  tft.setTextColor(COL_LABEL, COL_CARD);
  tft.setTextSize(1);
  tft.setCursor(x + 4, y + 4);
  tft.print(label);
}

void drawCardValueIfChanged(int idx, const char* value, uint16_t color) {
  if (strcmp(lastStr[idx], value) == 0) return; // no change

  strncpy(lastStr[idx], value, sizeof(lastStr[idx]) - 1);
  lastStr[idx][sizeof(lastStr[idx]) - 1] = '\0';

  int row, col, x, y;
  indexToRowCol(idx, row, col);
  getCellXY(row, col, x, y);

  int innerY = y + 16;
  int innerH = cellH - 20;

  tft.fillRect(x + 4, innerY, cellW - 8, innerH, COL_CARD);

  tft.setTextColor(color, COL_CARD);
  tft.setTextSize(2);

  uint16_t w = tft.textWidth(value);
  uint16_t h = tft.fontHeight();
  int vx = x + (cellW - w) / 2;
  int vy = innerY + (innerH - h) / 2;

  tft.setCursor(vx, vy);
  tft.print(value);
}

// ------------- JSON HELPERS -------------

// Map Open-Meteo weather code to text
String weatherCodeToText(int code) {
  if (code == 0) return "Clear";
  if (code == 1 || code == 2) return "Partly";
  if (code == 3) return "Cloudy";
  if (code >= 45 && code <= 48) return "Fog";
  if (code >= 51 && code <= 67) return "Drizzle";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain";
  if (code >= 95 && code <= 99) return "Storm";
  return "Unknown";
}

// Fetch weather + AQI into targetData using HTTPClient
bool fetchWeatherAndAQI() {
  // ---------- WEATHER ----------
  WiFiClientSecure client1;
  client1.setInsecure();       // don't check TLS cert
  HTTPClient http1;

  String url1 = String("https://api.open-meteo.com/v1/forecast?")
                + "latitude="   + String(LATITUDE, 4)
                + "&longitude=" + String(LONGITUDE, 4)
                + "&current_weather=true"
                + "&hourly=relative_humidity_2m,uv_index,visibility"
                + "&windspeed_unit=kmh&timezone=auto";

  Serial.println("Fetching weather...");
  http1.begin(client1, url1);
  int httpCode1 = http1.GET();

  if (httpCode1 != 200) {
    Serial.print("Weather HTTP error: ");
    Serial.println(httpCode1);
    http1.end();
    return false;
  }

  String payload1 = http1.getString();
  http1.end();

  DynamicJsonDocument doc(8192);
  DeserializationError err1 = deserializeJson(doc, payload1);
  if (err1) {
    Serial.print("Weather JSON error: ");
    Serial.println(err1.c_str());
    return false;
  }

  float temp      = doc["current_weather"]["temperature"].as<float>();
  float windspeed = doc["current_weather"]["windspeed"].as<float>();
  int   wcode     = doc["current_weather"]["weathercode"].as<int>();

  float humidity   = doc["hourly"]["relative_humidity_2m"][0].as<float>();
  float uv_index   = doc["hourly"]["uv_index"][0].as<float>();
  float visibility = doc["hourly"]["visibility"][0].as<float>() / 1000.0f;  // m -> km

  // ---------- AQI ----------
  WiFiClientSecure client2;
  client2.setInsecure();
  HTTPClient http2;

  String url2 = String("https://air-quality-api.open-meteo.com/v1/air-quality?")
                + "latitude="   + String(LATITUDE, 4)
                + "&longitude=" + String(LONGITUDE, 4)
                + "&hourly=us_aqi&timezone=auto";

  Serial.println("Fetching AQI...");
  http2.begin(client2, url2);
  int httpCode2 = http2.GET();

  if (httpCode2 != 200) {
    Serial.print("AQI HTTP error: ");
    Serial.println(httpCode2);
    http2.end();
    return false;
  }

  String payload2 = http2.getString();
  http2.end();

  DynamicJsonDocument docAQI(4096);
  DeserializationError err2 = deserializeJson(docAQI, payload2);
  if (err2) {
    Serial.print("AQI JSON error: ");
    Serial.println(err2.c_str());
    return false;
  }

  int aqi = docAQI["hourly"]["us_aqi"][0].as<int>();

  // ---------- Write into targetData ----------
  targetData.temp     = temp;
  targetData.humidity = (int)humidity;
  targetData.cond     = weatherCodeToText(wcode);
  targetData.wind     = windspeed;
  targetData.uv       = uv_index;
  targetData.vis      = visibility;
  targetData.aqi      = aqi;
  targetData.location = LOCATION_NAME;
  targetData.age      = 0;   // reset "Updated" seconds

  Serial.println("Weather + AQI updated OK");
  return true;
}

// ------------- SMOOTHING + REDRAW -------------

float smoothFloat(float current, float target, float alpha) {
  return current + (target - current) * alpha;
}

int smoothInt(int current, int target, float alpha) {
  return (int)(current + (target - current) * alpha);
}

void smoothStep() {
  const float alpha = 0.3f;

  displayData.temp     = smoothFloat(displayData.temp,     targetData.temp,     alpha);
  displayData.humidity = smoothInt  (displayData.humidity, targetData.humidity, alpha);
  displayData.wind     = smoothFloat(displayData.wind,     targetData.wind,     alpha);
  displayData.uv       = smoothFloat(displayData.uv,       targetData.uv,       alpha);
  displayData.vis      = smoothFloat(displayData.vis,      targetData.vis,      alpha);
  displayData.aqi      = smoothInt  (displayData.aqi,      targetData.aqi,      alpha);

  displayData.cond     = targetData.cond;
  displayData.location = targetData.location;
  displayData.age      = targetData.age;
}

void redrawValuesFromDisplayData() {
  char buf[20];

  // 0: Temp
  snprintf(buf, sizeof(buf), "%.1f", displayData.temp);
  drawCardValueIfChanged(0, buf, tft.color565(255, 120, 80));

  // 1: Humidity
  snprintf(buf, sizeof(buf), "%d", displayData.humidity);
  drawCardValueIfChanged(1, buf, tft.color565(80, 180, 255));

  // 2: Cond
  drawCardValueIfChanged(2, displayData.cond.c_str(), tft.color565(200, 200, 255));

  // 3: Wind
  snprintf(buf, sizeof(buf), "%.1f", displayData.wind);
  drawCardValueIfChanged(3, buf, tft.color565(150, 220, 255));

  // 4: UV
  snprintf(buf, sizeof(buf), "%.1f", displayData.uv);
  drawCardValueIfChanged(4, buf, tft.color565(255, 230, 80));

  // 5: Visibility
  snprintf(buf, sizeof(buf), "%.1f", displayData.vis);
  drawCardValueIfChanged(5, buf, tft.color565(160, 210, 255));

  // 6: AQI
  snprintf(buf, sizeof(buf), "%d", displayData.aqi);
  drawCardValueIfChanged(6, buf, tft.color565(120, 255, 120));

  // 7: Updated age (pretty format)
  if (displayData.age < 60) {
    snprintf(buf, sizeof(buf), "%lus", displayData.age);
  } else {
    unsigned long m = displayData.age / 60;
    unsigned long s = displayData.age % 60;
    snprintf(buf, sizeof(buf), "%lum%lus", m, s);
  }
  drawCardValueIfChanged(7, buf, tft.color565(255, 180, 120));

  // 8: Location
  drawCardValueIfChanged(8, displayData.location.c_str(), tft.color565(200, 220, 255));
}

// ------------- SETUP -------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  // TFT
  tft.init();
  tft.setRotation(3);   // orientation you used
  screenW = tft.width();
  screenH = tft.height();

  COL_BG     = TFT_BLACK;
  COL_CARD   = tft.color565(25, 30, 40);
  COL_TITLE  = tft.color565(220, 220, 255);
  COL_LABEL  = tft.color565(140, 160, 190);

  tft.fillScreen(COL_BG);

  int gridH = screenH - HEADER_H - 2 * GRID_MARGIN;
  cellW = (screenW - 2 * GRID_MARGIN - 2 * CELL_GAP) / 3;
  cellH = (gridH  - 2 * CELL_GAP) / 3;

  // WiFi
  WiFi.begin(SSID, PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // NTP time
  configTime(gmtOffset_sec, daylightOffset_sec,
             "time.google.com", "pool.ntp.org");

  // Header & grid
  drawHeaderStatic();     // draw title + IP once
  updateHeaderTime();     // draw initial time

  const char* labels[9] = {
    "Temp (C)",
    "Humidity (%)",
    "Cond",
    "Wind (km/h)",
    "UV",
    "Vis (km)",
    "AQI",
    "Updated",
    "Location"
  };
  for (int i = 0; i < 9; i++) {
    drawCardFrame(i, labels[i]);
    lastStr[i][0] = '\0';
  }

  // initial values
  targetData.temp = displayData.temp = 0;
  targetData.humidity = displayData.humidity = 0;
  targetData.cond = displayData.cond = "---";
  targetData.wind = displayData.wind = 0;
  targetData.uv   = displayData.uv   = 0;
  targetData.vis  = displayData.vis  = 0;
  targetData.aqi  = displayData.aqi  = 0;
  targetData.location = displayData.location = LOCATION_NAME;
  targetData.age = displayData.age = 0;

  // initial fetch
  fetchWeatherAndAQI();
  lastWeatherFetch = millis();
}

// ------------- LOOP -------------

void loop() {
  unsigned long now = millis();

  // periodic header time update (only small center box)
  if (now - lastHeaderUpdate >= 1000) {  // once per second
    lastHeaderUpdate = now;
    updateHeaderTime();
  }

  // periodic weather update
  if (now - lastWeatherFetch >= WEATHER_UPDATE_INTERVAL_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      if (fetchWeatherAndAQI()) {
        lastWeatherFetch = now;
      }
    }
  }

  // tick age every second
  if (now - lastAgeTick >= 1000) {
    lastAgeTick = now;
    targetData.age++;
  }

  // smoothing + redraw every 100 ms
  if (now - lastSmooth >= 100) {
    lastSmooth = now;
    smoothStep();
    redrawValuesFromDisplayData();
  }
}