// ============================================================
//  WEATHER STATION — ESP32 + ST7789 240×135
//  Open-Meteo API (no API key needed) + NTP time sync
//  Libraries: Adafruit ST7789, ArduinoJson 6.x, HTTPClient
// ============================================================
//
//  PLATFORMIO DEPENDENCIES (platformio.ini):
//  lib_deps =
//    adafruit/Adafruit ST7789@^1.10.3
//    adafruit/Adafruit GFX Library@^1.11.9
//    bblanchon/ArduinoJson@^6.21.5
//
// ============================================================

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// ── Display Pins ─────────────────────────────────────────────
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   32
#define TFT_WIDTH    320
#define TFT_HEIGHT   170
#define TFT_DISP_W   320
#define TFT_DISP_H   170

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── WiFi ──────────────────────────────────────────────────────
// Credentials are injected via platformio.ini build_flags
#ifndef WIFI_SSID
#endif
#ifndef WIFI_PASSWORD
#endif

// ── Location (Ogden, UT) ────────────────────────────
// Change these values to move the station to another location.
static const float  LOC_LAT      = 41.1092;
static const float  LOC_LON      = -111.9465;
static const char*  LOC_NAME     = "OGDEN UTAH";

// POSIX timezone rules for Mountain Time, including the daylight-saving
// transition dates used in the United States.
static const char*  TIMEZONE     = "MST7MDT,M3.2.0,M11.1.0";

// ── Update Interval ───────────────────────────────────────────
#define UPDATE_INTERVAL_MS  (5UL * 60UL * 1000UL)       // refresh readings every 5 minutes
#define WIFI_RETRY_INTERVAL_MS (30UL * 1000UL)          // start another connection attempt after 30 seconds
#define WIFI_CONNECT_TIMEOUT_MS (18UL * 1000UL)        // let each attempt run for up to 18 seconds

// ── Color Palette ─────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b){
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// Deep navy background
#define BG          rgb(0, 0, 0)
#define BG_CARD     rgb(0, 0, 0)
#define CLR_BLACK   0x0000u
#define CLR_WHITE   0xFFFFu

// Accent colors
#define ACCENT_BLUE rgb( 32,160,255)
#define ACCENT_TEAL rgb(  0,220,190)
#define ACCENT_GOLD rgb(255,195, 40)
#define ACCENT_RED  rgb(255, 75, 80)
#define ACCENT_ORNG rgb(255,140, 40)

// Text shades
#define TXT_PRIMARY   CLR_WHITE
#define TXT_SECONDARY rgb(140,170,200)
#define TXT_DIM       rgb( 60, 90,120)
#define TXT_LABEL     rgb( 80,130,170)

// UI elements
#define SEP         rgb( 20, 45, 80)
#define GREEN       rgb(  0,230,100)
#define DOT_OFF     rgb(  5, 30, 15)

// Degree symbol in Adafruit GFX default font (code 0xF8)
#define DEG "\xF8"

// ── Weather Data Struct ───────────────────────────────────────
struct WeatherData {
    float  temp_f;          // current temperature, converted from the API's Celsius
    float  feels_like_f;    // apparent temperature, converted from the API's Celsius
    float  temp_max_f;      // daily maximum temperature, converted to Fahrenheit
    float  temp_min_f;      // daily minimum temperature, converted to Fahrenheit
    int    humidity;        // relative humidity %
    float  wind_kmh;        // wind speed km/h
    int    wind_dir;        // wind direction degrees
    float  precip_mm;       // daily precipitation total in millimeters, as returned by the API
    int    weather_code;    // WMO weather code
    int    uv_index;        // UV index (integer)
    bool   is_day;          // daytime flag
    bool   valid;           // data is valid
};

WeatherData wx = {};
bool        wifiConnected  = false;
bool        pulseOn        = true;
unsigned long lastPulse    = 0;
unsigned long lastUpdate   = 0;    // time the last weather request finished
unsigned long lastWifiAttempt = 0; // start time of the most recent Wi-Fi attempt
unsigned long wifiAttemptStarted = 0;
bool        wifiAttemptPending = false; // true while Wi-Fi is still associating
bool        weatherFetchPending = true; // fetch immediately when the station connects
bool        wifiStatusChanged = false;  // redraw the header when the IP becomes available or is lost

// ── WMO Code → Short Label ────────────────────────────────────
// Returns a short description (max ~10 chars for size-1 font)
const char* wmoLabel(int code){
    if(code == 0)              return "CLEAR";
    if(code == 1)              return "MOSTLY CLR";
    if(code == 2)              return "PARTLY CLD";
    if(code == 3)              return "OVERCAST";
    if(code >= 45 && code<=48) return "FOGGY";
    if(code >= 51 && code<=55) return "DRIZZLE";
    if(code >= 56 && code<=57) return "FRZE DRZL";
    if(code >= 61 && code<=63) return "RAIN";
    if(code == 65)             return "HEAVY RAIN";
    if(code >= 66 && code<=67) return "FRZE RAIN";
    if(code >= 71 && code<=73) return "SNOW";
    if(code == 75)             return "HEAVY SNOW";
    if(code == 77)             return "SNOW GRNS";
    if(code >= 80 && code<=82) return "SHOWERS";
    if(code >= 85 && code<=86) return "SNOW SHWRS";
    if(code >= 95 && code<=99) return "STORM";
    return "UNKNOWN";
}

// ── WMO Code → Color Accent ───────────────────────────────────
uint16_t wmoColor(int code){
    if(code == 0 || code == 1)           return ACCENT_GOLD;
    if(code == 2 || code == 3)           return TXT_SECONDARY;
    if(code >= 45 && code <= 48)         return TXT_DIM;
    if(code >= 51 && code <= 67)         return ACCENT_BLUE;
    if(code >= 71 && code <= 77)         return CLR_WHITE;
    if(code >= 80 && code <= 86)         return ACCENT_TEAL;
    if(code >= 95 && code <= 99)         return ACCENT_RED;
    return TXT_SECONDARY;
}

// ── Wind Direction Arrow ──────────────────────────────────────
// Returns a unicode-like 2-char compass abbreviation
const char* windDir(int deg){
    // 8-point compass
    if(deg < 23 || deg >= 338)  return "N";
    if(deg < 68)                return "NE";
    if(deg < 113)               return "E";
    if(deg < 158)               return "SE";
    if(deg < 203)               return "S";
    if(deg < 248)               return "SW";
    if(deg < 293)               return "W";
    return "NW";
}

// ── UV Index Label ────────────────────────────────────────────
const char* uvLabel(int uv){
    if(uv <= 2)  return "LOW";
    if(uv <= 5)  return "MOD";
    if(uv <= 7)  return "HIGH";
    if(uv <= 10) return "V.HIGH";
    return "EXTREME";
}

uint16_t uvColor(int uv){
    if(uv <= 2)  return GREEN;
    if(uv <= 5)  return ACCENT_GOLD;
    if(uv <= 7)  return ACCENT_ORNG;
    if(uv <= 10) return ACCENT_RED;
    return rgb(200, 0, 200);
}

// ── Temperature Conversion ───────────────────────────────────
static float celsiusToFahrenheit(float c) {
    return (c * 9.0f / 5.0f) + 32.0f;
}

// ── Wind Speed Conversion ─────────────────────────────────
static float kmhToMph(float kmh) {
    return kmh * 0.621371f;
}

// ── Draw Text Helpers ─────────────────────────────────────────
static void txt(const char* s, int x, int y, uint16_t c, uint8_t sz = 1){
    tft.setTextSize(sz);
    tft.setTextColor(c);
    tft.setCursor(x, y);
    tft.print(s);
}

static void txtR(const char* s, int rightX, int y, uint16_t c, uint8_t sz = 1){
    // Right-align text
    int w = strlen(s) * 6 * sz;
    tft.setTextSize(sz);
    tft.setTextColor(c);
    tft.setCursor(rightX - w, y);
    tft.print(s);
}

static void txtC(const char* s, int y, uint16_t c, uint8_t sz = 1){
    int w = strlen(s) * 6 * sz;
    int x = (TFT_DISP_W - w) / 2;
    if(x < 0) x = 0;
    tft.setTextSize(sz);
    tft.setTextColor(c);
    tft.setCursor(x, y);
    tft.print(s);
}

// ── Draw a thin horizontal separator ─────────────────────────
static void drawSep(int y, int x0=0, int x1=TFT_DISP_W-1){
    tft.drawFastHLine(x0, y, x1 - x0, SEP);
}

// ── Draw a small bar (for UV / humidity) ─────────────────────
static void drawBar(int x, int y, int w, int h, float pct, uint16_t col){
    tft.fillRect(x, y, w, h, TXT_DIM);
    int filled = (int)(pct * w);
    if(filled > w) filled = w;
    if(filled > 0)
        tft.fillRect(x, y, filled, h, col);
}

// ── Stars background ──────────────────────────────────────────
static void drawStars(){
    srand(0xBEEF);
    for(int i = 0; i < 55; i++){
        int x = rand() % TFT_DISP_W;
        int y = 15 + rand() % (TFT_DISP_H - 30);  // avoid header/footer
        uint8_t b = (rand() % 4 == 0) ? 160 : (rand() % 2 ? 80 : 40);
        tft.drawPixel(x, y, rgb(b, b, b + 20));
    }
}

// ── Main Weather Screen ───────────────────────────────────────
void drawWeatherScreen(){
    tft.fillScreen(BG);
    drawStars();

    // ── TOP BAR ──────────────────────────────────────────────
    tft.fillRect(0, 0, TFT_DISP_W, 15, BG_CARD);
    drawSep(15);

    // Location name (left)
    txt(LOC_NAME, 4, 4, ACCENT_TEAL, 1);

    // Show the current local-network address beside the location only while
    // connected. Keeping it out of the stored weather data ensures it always
    // reflects the ESP32's current Wi-Fi connection.
    if(WiFi.status() == WL_CONNECTED){
        int ipLabelX = 4 + strlen(LOC_NAME) * 6 + 8;
        txt("IP:", ipLabelX, 4, TXT_LABEL, 1);
        String ipAddress = WiFi.localIP().toString();
        txt(ipAddress.c_str(), ipLabelX + 20, 4, TXT_SECONDARY, 1);
    }

    // Single tm declaration for the whole function
    struct tm ti;

    // NTP clock (center/right)
    if(getLocalTime(&ti)){
        char timebuf[10];
        snprintf(timebuf, sizeof(timebuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
        txtR(timebuf, TFT_DISP_W - 4, 4, TXT_SECONDARY, 1);
    }

    // ── MAIN TEMPERATURE ─────────────────────────────────────
    if(wx.valid){
        // Large temperature number — use size-3 for the number
        char tbuf[4];
        int t = (int)roundf(wx.temp_f);
        snprintf(tbuf, sizeof(tbuf), "%d", t);

        // Calculate width to center: digits * 18px (sz=3) + degree symbol
        int numW  = strlen(tbuf) * 18;
        int totalW = numW + 12; // +12 for "°F" at size 1.5 area
        int startX = (TFT_DISP_W - totalW) / 2 - 4;

        tft.setTextSize(3);
        tft.setTextColor(TXT_PRIMARY);
        tft.setCursor(startX, 22);
        tft.print(tbuf);

        // Degree + unit at size 2, aligned to top-right of big number
        tft.setTextSize(2);
        tft.setTextColor(TXT_SECONDARY);
        tft.setCursor(startX + numW + 3, 24);
        tft.print(DEG "F");

        // Weather condition label below temperature
        uint16_t condColor = wmoColor(wx.weather_code);
        txtC(wmoLabel(wx.weather_code), 52, condColor, 1);

        // ── FEELS LIKE + MAX/MIN ──────────────────────────────
        char flbuf[24];
        snprintf(flbuf, sizeof(flbuf), "FEELS %d" DEG "F", (int)roundf(wx.feels_like_f));
        txt(flbuf, 4, 65, TXT_LABEL, 1);

        char mmbuf[24];
        snprintf(mmbuf, sizeof(mmbuf), "%d" DEG "/%d" DEG, (int)roundf(wx.temp_max_f), (int)roundf(wx.temp_min_f));
        txtR(mmbuf, TFT_DISP_W - 4, 65, TXT_LABEL, 1);

        // ── SEPARATOR ────────────────────────────────────────
        drawSep(77);

        // ── STAT ROW 1: Humidity | Wind ───────────────────────
        // Humidity
        txt("HUM", 4, 81, TXT_LABEL, 1);
        char hbuf[8];
        snprintf(hbuf, sizeof(hbuf), "%d%%", wx.humidity);
        txt(hbuf, 4, 91, ACCENT_BLUE, 1);
        drawBar(4, 102, 52, 3, wx.humidity / 100.0f, ACCENT_BLUE);

        // Wind
        char wbuf[16];
        snprintf(wbuf, sizeof(wbuf), "%d mph", (int)roundf(kmhToMph(wx.wind_kmh)));
        int midX = 80;
        txt("WIND", midX, 81, TXT_LABEL, 1);
        txt(wbuf, midX, 91, ACCENT_TEAL, 1);

        // Wind compass label
        char dirb[4];
        snprintf(dirb, sizeof(dirb), "%s", windDir(wx.wind_dir));
        txt(dirb, midX + 52, 91, TXT_DIM, 1);

        // UV Index
        int uvX = TFT_DISP_W - 95;
        txt("UV IDX", uvX, 81, TXT_LABEL, 1);
        char uvb[8];
        snprintf(uvb, sizeof(uvb), "%d", wx.uv_index);
        uint16_t uvC = uvColor(wx.uv_index);
        txt(uvb, uvX, 91, uvC, 1);
        txt(uvLabel(wx.uv_index), uvX + 12, 91, uvC, 1);
        drawBar(uvX, 102, 64, 3, wx.uv_index / 11.0f, uvC);

        // ── SEPARATOR ────────────────────────────────────────
        drawSep(107);

        // ── PRECIP ────────────────────────────────────────────
        char pbuf[24];
        if(wx.precip_mm < 0.1f)
            snprintf(pbuf, sizeof(pbuf), "NO PRECIP TODAY");
        else
            snprintf(pbuf, sizeof(pbuf), "PRECIP: %.2f in", wx.precip_mm / 25.4f);
        txtC(pbuf, 111, TXT_SECONDARY, 1);

    } else {
        // No data yet
        txtC("FETCHING WEATHER", 45, TXT_LABEL, 1);
        txtC("PLEASE WAIT...",   58, TXT_DIM,   1);
    }

    // ── BOTTOM BAR ───────────────────────────────────────────
    drawSep(TFT_DISP_H - 48);
    tft.fillRect(0, TFT_DISP_H - 47, TFT_DISP_W, 12, BG_CARD);

    // Show date if NTP is synced
    if(getLocalTime(&ti)){
        const char* days[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
        const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                 "JUL","AUG","SEP","OCT","NOV","DEC"};
        char datebuf[24];
        snprintf(datebuf, sizeof(datebuf), "%s %02d %s %04d",
                 days[ti.tm_wday], ti.tm_mday, months[ti.tm_mon], ti.tm_year + 1900);
        txt(datebuf, 4, TFT_DISP_H - 45, TXT_DIM, 1);
    }

    // Pulse dot (live indicator)
    tft.fillCircle(TFT_DISP_W - 7, TFT_DISP_H - 41, 2, pulseOn ? GREEN : DOT_OFF);
}

// ── Fetch Weather from Open-Meteo ────────────────────────────
bool fetchWeather(){
    if(WiFi.status() != WL_CONNECTED) return false;

    char url[400];
    snprintf(url, sizeof(url),
        "http://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        // The API returns temperatures in Celsius, wind in km/h, and
        // precipitation in millimeters; the display converts as needed.
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
        "precipitation,weather_code,wind_speed_10m,wind_direction_10m,is_day"
        "&daily=temperature_2m_max,temperature_2m_min,uv_index_max,precipitation_sum"
        "&timezone=auto"
        // Only today's daily totals are needed, so each daily array uses [0].
        "&forecast_days=1",
        LOC_LAT, LOC_LON);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(8000);
    int code = http.GET();

    if(code != 200){
        Serial.printf("[HTTP] GET failed, code: %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    // Parse JSON
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if(err){
        Serial.println("[JSON] Parse error: " + String(err.c_str()));
        return false;
    }

    // Check the response shape and every value used by the display before
    // changing wx. This keeps a partial API response from replacing good data
    // with zeroes or a mixture of old and new readings.
    JsonObjectConst cur = doc["current"].as<JsonObjectConst>();
    JsonObjectConst dai = doc["daily"].as<JsonObjectConst>();
    JsonVariantConst tempC = cur["temperature_2m"];
    JsonVariantConst feelsLikeC = cur["apparent_temperature"];
    JsonVariantConst humidity = cur["relative_humidity_2m"];
    JsonVariantConst weatherCode = cur["weather_code"];
    JsonVariantConst windKmh = cur["wind_speed_10m"];
    JsonVariantConst windDir = cur["wind_direction_10m"];
    JsonVariantConst isDay = cur["is_day"];
    JsonVariantConst precipMm = dai["precipitation_sum"][0];
    JsonVariantConst tempMaxC = dai["temperature_2m_max"][0];
    JsonVariantConst tempMinC = dai["temperature_2m_min"][0];
    JsonVariantConst uvIndex = dai["uv_index_max"][0];

    if(cur.isNull() || dai.isNull() ||
       (!tempC.is<int>() && !tempC.is<float>()) ||
       (!feelsLikeC.is<int>() && !feelsLikeC.is<float>()) ||
       !humidity.is<int>() || !weatherCode.is<int>() ||
       (!windKmh.is<int>() && !windKmh.is<float>()) ||
       !windDir.is<int>() || !isDay.is<int>() ||
       (!precipMm.is<int>() && !precipMm.is<float>()) ||
       (!tempMaxC.is<int>() && !tempMaxC.is<float>()) ||
       (!tempMinC.is<int>() && !tempMinC.is<float>()) ||
       (!uvIndex.is<int>() && !uvIndex.is<float>())){
        Serial.println("[JSON] Response is missing one or more weather values");
        return false;
    }

    // Build the next reading locally and publish it only after validation.
    WeatherData next = {};
    next.temp_f       = celsiusToFahrenheit(tempC.as<float>());
    next.feels_like_f = celsiusToFahrenheit(feelsLikeC.as<float>());
    next.humidity     = humidity.as<int>();
    next.precip_mm    = precipMm.as<float>();
    next.weather_code = weatherCode.as<int>();
    next.wind_kmh     = windKmh.as<float>();
    next.wind_dir     = windDir.as<int>();
    next.is_day       = isDay.as<int>() == 1;
    next.temp_max_f   = celsiusToFahrenheit(tempMaxC.as<float>());
    next.temp_min_f   = celsiusToFahrenheit(tempMinC.as<float>());
    next.uv_index     = (int)uvIndex.as<float>();
    next.valid        = true;
    wx = next;

    Serial.printf("[WX] T=%.1f FL=%.1f H=%d%% W=%.0f code=%d UV=%d\n",
                  wx.temp_f, wx.feels_like_f, wx.humidity,
                  kmhToMph(wx.wind_kmh), wx.weather_code, wx.uv_index);
    return true;
}

// ── Loading Screen ────────────────────────────────────────────
void showLoading(const char* msg){
    tft.fillScreen(BG);
    drawStars();
    tft.fillRect(0, 0, 240, 15, BG_CARD);
    drawSep(15);
    txtC("WEATHER STATION", 4, ACCENT_TEAL, 1);
    txtC(msg, 58, TXT_LABEL, 1);
}

// ── Boot Intro ────────────────────────────────────────────────
void playIntro(){
    tft.fillScreen(CLR_BLACK);
    // Fade the title in smoothly without redrawing the whole screen each frame.
    for(int i = 0; i < 8; i++){
        uint8_t b = 28 + i * 28;
        uint16_t col = rgb(0, b, b);
        txtC("WEATHER",  36, col, 2);
        txtC("STATION",  56, col, 2);
        delay(60);
    }
    // Show the final title and initialization status at full accent color.
    txtC("WEATHER",  36, ACCENT_TEAL, 2);
    txtC("STATION",  56, ACCENT_TEAL, 2);
    txtC("INITIALIZING", 84, TXT_DIM, 1);
    delay(700);
}

// ── WiFi Connection Management ────────────────────────────────
// Start an attempt without waiting here; the display and weather refresh can
// continue while the ESP32 associates with the access point.
void beginWifiAttempt(){
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiAttempt = millis();
    wifiAttemptStarted = lastWifiAttempt;
    wifiAttemptPending = true;
    Serial.println("[WiFi] Connection attempt started");
}

// Track connection transitions, configure local time after each successful
// connection, and retry periodically instead of permanently blocking on error.
void updateWifiConnection(){
    unsigned long now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;

    if(connected){
        if(!wifiConnected){
            wifiConnected = true;
            wifiAttemptPending = false;
            configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
            weatherFetchPending = true;
            wifiStatusChanged = true;
            Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
        }
        return;
    }

    if(wifiConnected){
        wifiConnected = false;
        wifiStatusChanged = true;
        Serial.println("[WiFi] Connection lost; waiting to retry");
    }

    // A timed-out attempt is allowed to expire; the retry interval below
    // prevents calling WiFi.begin repeatedly while the radio is still trying.
    if(wifiAttemptPending &&
       now - wifiAttemptStarted >= WIFI_CONNECT_TIMEOUT_MS){
        wifiAttemptPending = false;
        Serial.println("[WiFi] Connection attempt timed out");
    }

    if(!wifiAttemptPending &&
       (lastWifiAttempt == 0 || now - lastWifiAttempt >= WIFI_RETRY_INTERVAL_MS)){
        beginWifiAttempt();
    }
}

// ── Setup ─────────────────────────────────────────────────────
void setup(){
    Serial.begin(115200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.init(TFT_HEIGHT, TFT_WIDTH);
    tft.setRotation(1);
    tft.fillScreen(CLR_BLACK);

    playIntro();
    showLoading("CONNECTING TO WIFI");
    beginWifiAttempt();
    lastUpdate = millis();
    drawWeatherScreen();
}

// ── Loop ──────────────────────────────────────────────────────
void loop(){
    unsigned long now = millis();
    updateWifiConnection();
    now = millis();

    // Refresh the header as soon as the network address becomes available,
    // or clear it after Wi-Fi disconnects.
    if(wifiStatusChanged){
        wifiStatusChanged = false;
        drawWeatherScreen();
    }

    //   heartbeat dot every second
    if(now - lastPulse >= 1000UL){
        lastPulse = now;
        pulseOn = !pulseOn;
        tft.fillCircle(233, 129, 2, pulseOn ? GREEN : DOT_OFF);
    }

    // Fetch once after connecting, then refresh at the normal interval.
    // Failed fetches keep the last valid reading and are retried next interval.
    if(wifiConnected &&
       (weatherFetchPending || now - lastUpdate >= UPDATE_INTERVAL_MS)){
        weatherFetchPending = false;
        bool ok = fetchWeather();
        lastUpdate = millis();
        if(ok || wx.valid){
            drawWeatherScreen();
        }
    }
}