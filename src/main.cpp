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
// Change these to your coordinates and timezone offset
static const float  LOC_LAT      = 41.1092;
static const float  LOC_LON      = -111.9465;
static const int    UTC_OFFSET   = -7;          // hours from UTC
static const char*  LOC_NAME     = "OGDEN UTAH";

// ── Update Interval ───────────────────────────────────────────
#define UPDATE_INTERVAL_MS  (5UL * 60UL * 1000UL)   // 5 minutes

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
    float  temp_c;          // current temperature °F
    float  feels_like_c;    // apparent temperature °F
    float  temp_max_c;      // daily max °F
    float  temp_min_c;      // daily min °F
    int    humidity;        // relative humidity %
    float  wind_kmh;        // wind speed km/h
    int    wind_dir;        // wind direction degrees
    float  precip_mm;       // precipitation mm
    int    weather_code;    // WMO weather code
    int    uv_index;        // UV index (integer)
    bool   is_day;          // daytime flag
    bool   valid;           // data is valid
};

WeatherData wx = {};
bool        wifiConnected  = false;
bool        pulseOn        = true;
unsigned long lastPulse    = 0;
unsigned long lastUpdate   = 0;
bool        firstDraw      = true;

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
        char tbuf[8];
        int t = (int)roundf(wx.temp_c);
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
        tft.setTextSize(1);
        tft.setTextColor(TXT_SECONDARY);
        tft.setCursor(startX + numW + 3, 24);
        tft.print(DEG "F");

        // Weather condition label below temperature
        uint16_t condColor = wmoColor(wx.weather_code);
        txtC(wmoLabel(wx.weather_code), 52, condColor, 1);

        // ── FEELS LIKE + MAX/MIN ──────────────────────────────
        char flbuf[24];
        snprintf(flbuf, sizeof(flbuf), "FEELS %d" DEG "F", (int)roundf(wx.feels_like_c));
        txt(flbuf, 4, 65, TXT_LABEL, 1);

        char mmbuf[24];
        snprintf(mmbuf, sizeof(mmbuf), "%d" DEG "/%d" DEG, (int)roundf(wx.temp_max_c), (int)roundf(wx.temp_min_c));
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
            snprintf(pbuf, sizeof(pbuf), "PRECIP: %.1f mm", wx.precip_mm);
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
        char datebuf[20];
        snprintf(datebuf, sizeof(datebuf), "%s %02d %s",
                 days[ti.tm_wday], ti.tm_mday, months[ti.tm_mon]);
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
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
        "precipitation,weather_code,wind_speed_10m,wind_direction_10m,is_day"
        "&daily=temperature_2m_max,temperature_2m_min,uv_index_max,precipitation_sum"
        "&timezone=auto"
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

    JsonObject cur = doc["current"];
    JsonObject dai = doc["daily"];

    wx.temp_c       = celsiusToFahrenheit(cur["temperature_2m"] | 0.0f);
    wx.feels_like_c = celsiusToFahrenheit(cur["apparent_temperature"] | 0.0f);
    wx.humidity     = cur["relative_humidity_2m"]    | 0;
    wx.precip_mm    = dai["precipitation_sum"][0]    | 0.0f;
    wx.weather_code = cur["weather_code"]            | 0;
    wx.wind_kmh     = cur["wind_speed_10m"]          | 0.0f;
    wx.wind_dir     = cur["wind_direction_10m"]      | 0;
    wx.is_day       = (cur["is_day"] | 1) == 1;
    wx.temp_max_c   = celsiusToFahrenheit(dai["temperature_2m_max"][0] | 0.0f);
    wx.temp_min_c   = celsiusToFahrenheit(dai["temperature_2m_min"][0] | 0.0f);
    wx.uv_index     = (int)(dai["uv_index_max"][0]   | 0.0f);
    wx.valid        = true;

    Serial.printf("[WX] T=%.1f FL=%.1f H=%d%% W=%.0f code=%d UV=%d\n",
                  wx.temp_c, wx.feels_like_c, wx.humidity,
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
    // Animate fade-in title
    for(int i = 0; i < 4; i++){
        tft.fillScreen(CLR_BLACK);
        uint8_t b = 40 + i * 55;
        uint16_t col = rgb(0, b, b);
        txtC("WEATHER",  36, col, 2);
        txtC("STATION",  56, col, 2);
        delay(120);
    }
    txtC("WEATHER",  36, ACCENT_TEAL, 2);
    txtC("STATION",  56, ACCENT_TEAL, 2);
    txtC("INITIALIZING...", 84, TXT_DIM, 1);
    delay(700);
}

// ── WiFi Connect ──────────────────────────────────────────────
void connectWifi(){
    showLoading("CONNECTING TO WIFI...");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const char* frames[] = {"[     ]","[=    ]","[==   ]","[===  ]","[==== ]","[=====]"};
    int f = 0, tries = 0;

    while(WiFi.status() != WL_CONNECTED){
        tft.fillRect(70, 78, 100, 12, BG);
        tft.setTextSize(1);
        tft.setTextColor(ACCENT_BLUE);
        tft.setCursor(70, 80);
        tft.print(frames[f % 6]);
        f++;
        delay(300);
        tries++;
        if(tries > 60){
            showLoading("WIFI TIMEOUT");
            txtC("CHECK CREDENTIALS", 80, ACCENT_RED, 1);
            txtC("IN PLATFORMIO.INI", 92, ACCENT_RED, 1);
            while(true) delay(1000);
        }
    }

    // Sync NTP
    configTime(UTC_OFFSET * 3600, 0,
               "pool.ntp.org", "time.nist.gov");

    showLoading("SYNCING TIME...");
    struct tm ti;
    int ntpTries = 0;
    while(!getLocalTime(&ti) && ntpTries++ < 20) delay(500);

    wifiConnected = true;
    String ipStr = WiFi.localIP().toString();
    Serial.println("[WiFi] Connected: " + ipStr);
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
    connectWifi();

    // First fetch
    showLoading("LOADING WEATHER...");
    fetchWeather();
    lastUpdate = millis();

    drawWeatherScreen();
}

// ── Loop ──────────────────────────────────────────────────────
void loop(){
    unsigned long now = millis();

    //   heartbeat dot every second
    if(now - lastPulse >= 1000UL){
        lastPulse = now;
        pulseOn = !pulseOn;
        tft.fillCircle(233, 129, 2, pulseOn ? GREEN : DOT_OFF);
    }

    // Refresh weather data every UPDATE_INTERVAL_MS
    if(now - lastUpdate >= UPDATE_INTERVAL_MS || firstDraw){
        firstDraw = false;
        bool ok = fetchWeather();
        lastUpdate = millis();
        if(ok || wx.valid){
            drawWeatherScreen();
        }
    }
}