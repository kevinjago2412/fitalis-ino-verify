/*******************************************************************************
 * FITALIS — Smart Medicine Reminder
 * Pengingat Minum Obat berbasis ESP32 DevKit V1
 * -----------------------------------------------------------------------------
 * Versi    : 0.1.0
 * Board    : ESP32 DevKit V1  (Arduino IDE: "ESP32 Dev Module")
 * Serial   : 115200 baud
 *
 * CARA PAKAI:
 *   1. Edit bagian CONFIG di bawah — isi STA_SSID dan STA_PASSWORD dengan
 *      WiFi kamu (atau set WIFI_MODE ke WIFI_MODE_AP buat mode hotspot).
 *   2. Upload ke ESP32.
 *   3. Buka Serial Monitor (115200). IP address akan muncul.
 *   4. Buka IP itu di browser HP untuk setting jadwal minum obat.
 *
 * HARDWARE:
 *   Buzzer aktif (+) -> GPIO 25      Buzzer (-) -> GND
 *   Tombol konfirmasi -> GPIO 27     kaki lain  -> GND
 *   LED indikator    -> GPIO 2 (onboard)
 *
 * File ini adalah gabungan seluruh source code jadi satu sketch, supaya bisa
 * langsung dibuka di Arduino IDE tanpa perlu file tambahan.
 ******************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>

/* =========================================================================
 * BAGIAN 1 — KONFIGURASI
 * ========================================================================= */

/*
 * FITALIS — Smart Medicine Reminder
 * Configuration
 * ---------------------------------------------------------------
 * Board  : ESP32 DevKit V1 (ESP32-WROOM-32)
 * Target : Arduino IDE 2.x  /  PlatformIO (env:esp32dev)
 *
 * Change values here only. Nothing else in the project hardcodes pins.
 */


// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define FITALIS_NAME          "FITALIS"
#define FITALIS_VERSION       "0.1.0"
#define FITALIS_DEVICE_ID     "fitalis-01"

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
// Two modes:
//   WIFI_MODE_AP      -> ESP32 becomes its own access point (no router needed).
//                        Dashboard at http://192.168.4.1
//                        BUT: no internet => NTP time sync unavailable.
//   WIFI_MODE_STATION -> ESP32 joins your home WiFi/router.
//                        Gets NTP time. Dashboard at the IP printed on Serial.
#define WIFI_MODE_AP          0
#define WIFI_MODE_STATION     1

#define WIFI_MODE             WIFI_MODE_STATION

#define AP_SSID               "FITALIS"
#define AP_PASSWORD           "fitalis123"     // min 8 chars; "" for open AP

#define STA_SSID              "GANTI_DENGAN_WIFI_KAMU"
#define STA_PASSWORD          "GANTI_DENGAN_PASSWORD"

// Try to reconnect to WiFi for this long before giving up on NTP.
#define WIFI_CONNECT_TIMEOUT_MS   20000UL

// ---------------------------------------------------------------------------
// Pins — ESP32 DevKit V1
// ---------------------------------------------------------------------------
#define PIN_BUZZER            25    // active buzzer (+)  — (--) to GND
#define PIN_LED_STATUS        2     // onboard LED, blinks while alarm rings
#define PIN_BUTTON_ACK        27    // push button to GND = "I took my medicine"

// Reserved for a later phase (not used in v0.1.0):
// #define PIN_TFT_CS         5
// #define PIN_TFT_DC         4
// #define PIN_TFT_RST        15
// #define PIN_TFT_MOSI       23
// #define PIN_TFT_SCLK       18
// #define PIN_IR_SENSOR      34

// Active buzzer: HIGH = sounding. Set to 0 if you use a passive buzzer.
#define BUZZER_ACTIVE_HIGH    1

// Button uses INPUT_PULLUP, so pressed == LOW
#define BUTTON_PRESSED_LEVEL  LOW

// ---------------------------------------------------------------------------
// Schedule limits
// ---------------------------------------------------------------------------
#define MAX_SCHEDULES         8      // concurrent medicine schedules
#define MAX_NAME_LEN          32
#define MAX_DOSE_LEN          16     // e.g. "1 tablet", "5 ml"
#define NTP_SERVER_1          "pool.ntp.org"
#define NTP_SERVER_2          "time.google.com"
#define TZ_OFFSET_HOURS       7      // WIB = UTC+7. Change for your timezone.

// ---------------------------------------------------------------------------
// Alarm behaviour
// ---------------------------------------------------------------------------
#define ALARM_DURATION_MS     60000UL   // how long the alarm keeps ringing
#define ALARM_BEEP_ON_MS      400UL     // buzzer on/off rhythm
#define ALARM_BEEP_OFF_MS     600UL
#define ALARM_LED_BLINK_MS    200UL
#define ALARM_REPEAT_MINS     5         // re-ring this many minutes after being ignored
#define ALARM_MAX_REPEATS     2         // then give up until the next day

// Only fire an alarm when the current minute is within this many minutes of
// the scheduled time. Guards against a reboot at 23:59 firing a 08:00 alarm.
#define ALARM_WINDOW_MINS     2

// ---------------------------------------------------------------------------
// Notification (feature 4 — optional, off by default)
// ---------------------------------------------------------------------------
// Fires an HTTP POST to a URL of your choice exactly once when an alarm
// starts. Works with Telegram Bot API, ntfy.sh, or your own server.
// The reminder itself works fine with this disabled.
#define NOTIFY_ENABLED        0
#define NOTIFY_MAX_FAILURES   3        // stop trying after N consecutive failures
#define NOTIFY_TIMEOUT_MS     8000

// Example — ntfy.sh (public, no account needed):
//   NOTIFY_URL "http://ntfy.sh/fitalis-obat-kevin-9x2"
// Example — Telegram:
//   NOTIFY_URL "https://api.telegram.org/bot<TOKEN>/sendMessage?chat_id=<ID>&text={msg}"
// {msg} is replaced with the URL-encoded message. If the URL has no {msg},
// the message is sent as a JSON body: {"text": "..."}
#define NOTIFY_URL            "http://ntfy.sh/fitalis-obat-ganti-ini"

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------
#define WEB_PORT              80

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------
#define FITALIS_DEBUG         1

#if FITALIS_DEBUG
  #define LOGF(...)   Serial.printf(__VA_ARGS__)
  #define LOGLN(x)    Serial.println(x)
#else
  #define LOGF(...)   do {} while (0)
  #define LOGLN(x)    do {} while (0)
#endif

/* =========================================================================
 * BAGIAN 2 — HEADER: SCHEDULE
 * ========================================================================= */

/*
 * FITALIS — Schedule storage (NVS / Preferences)
 * ---------------------------------------------------------------
 * Schedules persist in flash, so they survive power loss and WiFi
 * outages. The reminder keeps working with no network at all.
 */


#include <Arduino.h>

#define DAY_SUN  0x01
#define DAY_MON  0x02
#define DAY_TUE  0x04
#define DAY_WED  0x08
#define DAY_THU  0x10
#define DAY_FRI  0x20
#define DAY_SAT  0x40
#define DAY_ALL  0x7F

struct Schedule {
  bool     enabled;
  uint8_t  hour;        // 0-23
  uint8_t  minute;      // 0-59
  uint8_t  days;        // bitmask, bit 0 = Sunday
  char     name[MAX_NAME_LEN];
  char     dose[MAX_DOSE_LEN];
  uint32_t takenDateKey;  // YYYYMMDD of last acknowledgment; 0 = never
};

void scheduleInit();
int  scheduleCount();
Schedule* scheduleGet(int index);
int  scheduleAdd(const char* name, const char* dose,
                 uint8_t hour, uint8_t minute, uint8_t days);
bool scheduleUpdate(int index, const char* name, const char* dose,
                    uint8_t hour, uint8_t minute, uint8_t days, bool enabled);
bool scheduleDelete(int index);
void scheduleSave();
void scheduleClearTakenFlags();

// Returns a compact JSON array (no pretty printing — the dashboard parses it).
String scheduleToJson();

/* =========================================================================
 * BAGIAN 3 — HEADER: REMINDER
 * ========================================================================= */

/*
 * FITALIS — Reminder engine
 * ---------------------------------------------------------------
 * Checks every schedule once per second and rings the buzzer when a
 * dose is due. Acknowledging (button or dashboard) stops the alarm
 * and marks that dose as taken for the day.
 */


#include <Arduino.h>

enum ReminderState {
  REMINDER_IDLE = 0,
  REMINDER_RINGING,
  REMINDER_SNOOZED
};

struct ActiveAlarm {
  bool     active;
  int      scheduleIndex;
  uint32_t startedAt;      // millis()
  uint32_t nextRepeatAt;   // millis()
  uint8_t  repeatCount;
};

void reminderInit();
void reminderLoop();

// Acknowledge the currently ringing alarm ("I took my medicine").
bool reminderAcknowledge();
// Skip this occurrence without marking it taken.
bool reminderDismiss();
// Ring again after ALARM_REPEAT_MINS.
bool reminderSnooze();

// --- Status for the web UI ---------------------------------------------
ReminderState reminderGetState();
int           reminderActiveIndex();
String        reminderStatusJson();

// --- Time helpers -------------------------------------------------------
bool     timeIsSynced();
String   timeNowString();       // "14:05:33"
String   dateNowString();       // "2026-11-06"
uint32_t todayDateKey();        // 20261106, 0 if time not synced
bool     timeSetFromClient(const String& isoLocal);  // "2026-11-06T14:05"

/* =========================================================================
 * BAGIAN 4 — HEADER: WEB
 * ========================================================================= */

/*
 * FITALIS — Network + Web Dashboard
 * ---------------------------------------------------------------
 * Serves the dashboard and the JSON API the page talks to.
 * Runs on port 80 so you just open http://192.168.4.1 (AP mode) or
 * the IP shown on Serial (station mode).
 */


#include <Arduino.h>

void  webInit();          // start WiFi + NTP (non-blocking parts)
void  webLoop();          // handle clients
String webLocalIp();      // current IP as a string
bool  webIsConnected();

// Called once at boot; returns true if WiFi connected and time synced.
bool  webConnectAndSyncTime();

/* =========================================================================
 * BAGIAN 5 — SCHEDULE (penyimpanan jadwal di flash)
 * ========================================================================= */

#include <Preferences.h>

/*
 * Schedules are stored in NVS (non-volatile storage) via the Preferences
 * library that ships with the ESP32 Arduino core.
 *
 * Layout: one blob holding the whole array. Simple, atomic, and survives
 * power cuts. MAX_SCHEDULES is small so size is not a concern.
 */

static const char* NVS_NAMESPACE = "fitalis";
static const char* NVS_KEY       = "sched";

static Schedule s_schedules[MAX_SCHEDULES];
static int      s_count = 0;

static Preferences s_prefs;

// ---------------------------------------------------------------------------

void scheduleInit() {
  s_prefs.begin(NVS_NAMESPACE, false);

  size_t len = s_prefs.getBytesLength(NVS_KEY);
  if (len == sizeof(s_schedules)) {
    s_prefs.getBytes(NVS_KEY, s_schedules, len);
    s_count = 0;
    // Count how many leading entries look initialized (name non-empty).
    for (int i = 0; i < MAX_SCHEDULES; i++) {
      if (s_schedules[i].name[0] != '\0') s_count = i + 1;
    }
    LOGF("[schedule] loaded %d schedule(s) from NVS\n", s_count);
  } else {
    s_count = 0;
    memset(s_schedules, 0, sizeof(s_schedules));
    LOGLN("[schedule] no saved schedules, starting empty");
  }
}

int scheduleCount() {
  return s_count;
}

Schedule* scheduleGet(int index) {
  if (index < 0 || index >= s_count) return nullptr;
  return &s_schedules[index];
}

static void copyStr(char* dst, size_t dstLen, const char* src) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

int scheduleAdd(const char* name, const char* dose,
                uint8_t hour, uint8_t minute, uint8_t days) {
  if (s_count >= MAX_SCHEDULES) {
    LOGLN("[schedule] add failed: table full");
    return -1;
  }
  if (hour > 23 || minute > 59 || days == 0) {
    LOGLN("[schedule] add failed: invalid time/days");
    return -1;
  }

  Schedule& s = s_schedules[s_count];
  s.enabled      = true;
  s.hour         = hour;
  s.minute       = minute;
  s.days         = days;
  s.takenDateKey = 0;
  copyStr(s.name, MAX_NAME_LEN, (name && name[0]) ? name : "Obat");
  copyStr(s.dose, MAX_DOSE_LEN, (dose && dose[0]) ? dose : "-");

  s_count++;
  scheduleSave();
  LOGF("[schedule] added #%d %s at %02d:%02d\n", s_count - 1, s.name, hour, minute);
  return s_count - 1;
}

bool scheduleUpdate(int index, const char* name, const char* dose,
                    uint8_t hour, uint8_t minute, uint8_t days, bool enabled) {
  if (index < 0 || index >= s_count) return false;
  if (hour > 23 || minute > 59 || days == 0) return false;

  Schedule& s = s_schedules[index];
  // If the time changes, the previous "taken" marker no longer applies.
  if (s.hour != hour || s.minute != minute) {
    s.takenDateKey = 0;
  }
  s.hour    = hour;
  s.minute  = minute;
  s.days    = days;
  s.enabled = enabled;
  copyStr(s.name, MAX_NAME_LEN, name);
  copyStr(s.dose, MAX_DOSE_LEN, dose);

  scheduleSave();
  return true;
}

bool scheduleDelete(int index) {
  if (index < 0 || index >= s_count) return false;

  // Shift the tail down so indices stay contiguous.
  for (int i = index; i < s_count - 1; i++) {
    s_schedules[i] = s_schedules[i + 1];
  }
  s_count--;
  memset(&s_schedules[s_count], 0, sizeof(Schedule));
  scheduleSave();
  LOGF("[schedule] deleted #%d, %d left\n", index, s_count);
  return true;
}

void scheduleSave() {
  s_prefs.putBytes(NVS_KEY, s_schedules, sizeof(s_schedules));
  LOGLN("[schedule] saved to NVS");
}

void scheduleClearTakenFlags() {
  for (int i = 0; i < s_count; i++) {
    s_schedules[i].takenDateKey = 0;
  }
  scheduleSave();
}

// ---------------------------------------------------------------------------
// JSON output — hand-built to avoid a JSON library dependency.
// ---------------------------------------------------------------------------

static void appendEscaped(String& out, const char* s) {
  for (const char* p = s; *p; ++p) {
    char c = *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n' || c == '\r') {
      out += ' ';
    } else {
      out += c;
    }
  }
}

String scheduleToJson() {
  String out;
  out.reserve(256 + s_count * 160);
  out += "{\"count\":";
  out += s_count;
  out += ",\"max\":";
  out += MAX_SCHEDULES;
  out += ",\"schedules\":[";

  for (int i = 0; i < s_count; i++) {
    Schedule& s = s_schedules[i];
    if (i) out += ',';
    out += "{\"index\":";
    out += i;
    out += ",\"name\":\"";
    appendEscaped(out, s.name);
    out += "\",\"dose\":\"";
    appendEscaped(out, s.dose);
    out += "\",\"hour\":";
    out += s.hour;
    out += ",\"minute\":";
    out += s.minute;
    out += ",\"days\":";
    out += s.days;
    out += ",\"enabled\":";
    out += s.enabled ? "true" : "false";
    out += ",\"taken\":";
    out += (s.takenDateKey != 0) ? "true" : "false";
    out += '}';
  }

  out += "]}";
  return out;
}

/* =========================================================================
 * BAGIAN 6 — REMINDER (mesin alarm + jam)
 * ========================================================================= */

#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>

static ActiveAlarm    s_alarm;
static uint32_t       s_lastCheck = 0;

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

bool timeIsSynced() {
  // Anything past 2021-01-01 means NTP (or the browser) set the clock.
  return time(nullptr) > 1609459200;
}

struct tm timeNowTm() {
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return t;
}

String timeNowString() {
  char buf[16];
  if (!timeIsSynced()) return String("--:--:--");
  struct tm t = timeNowTm();
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

String dateNowString() {
  char buf[16];
  if (!timeIsSynced()) return String("---- -- --");
  struct tm t = timeNowTm();
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

uint32_t todayDateKey() {
  if (!timeIsSynced()) return 0;
  struct tm t = timeNowTm();
  return (uint32_t)(t.tm_year + 1900) * 10000UL
       + (uint32_t)(t.tm_mon + 1) * 100UL
       + (uint32_t)t.tm_mday;
}

// Called by the dashboard when NTP is unavailable (AP mode).
// Format: "2026-11-06T14:05"
bool timeSetFromClient(const String& isoLocal) {
  if (isoLocal.length() < 16) return false;

  int year  = isoLocal.substring(0, 4).toInt();
  int month = isoLocal.substring(5, 7).toInt();
  int day   = isoLocal.substring(8, 10).toInt();
  int hour  = isoLocal.substring(11, 13).toInt();
  int mins  = isoLocal.substring(14, 16).toInt();

  if (year < 2021 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (hour < 0 || hour > 23 || mins < 0 || mins > 59) return false;

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = mins;
  t.tm_sec  = 0;
  t.tm_isdst = 0;

  time_t epoch = mktime(&t);
  if (epoch < 1609459200) return false;

  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  LOGF("[reminder] clock set manually -> %04d-%02d-%02d %02d:%02d\n",
       year, month, day, hour, mins);
  return true;
}

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

static void buzzerOn()  { digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_HIGH ? HIGH : LOW); }
static void buzzerOff() { digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_HIGH ? LOW : HIGH); }

// ---------------------------------------------------------------------------
// Notification (optional — feature 4)
// ---------------------------------------------------------------------------

#if NOTIFY_ENABLED

static uint8_t s_notifyFailures = 0;

static String urlEncode(const String& in) {
  String out;
  out.reserve(in.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < in.length(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static void sendNotification(const Schedule& s) {
  if (WiFi.status() != WL_CONNECTED) {
    LOGLN("[notify] skipped: no WiFi");
    return;
  }
  if (s_notifyFailures >= NOTIFY_MAX_FAILURES) {
    LOGLN("[notify] skipped: too many previous failures");
    return;
  }

  String message = String(FITALIS_NAME) + ": waktunya minum " + s.name
                 + " (" + s.dose + ") jam " + timeNowString().substring(0, 5);

  String url = String(NOTIFY_URL);
  bool hasPlaceholder = url.indexOf("{msg}") >= 0;
  if (hasPlaceholder) {
    url.replace("{msg}", urlEncode(message));
  }

  HTTPClient http;
  http.setConnectTimeout(NOTIFY_TIMEOUT_MS);
  http.setTimeout(NOTIFY_TIMEOUT_MS);

  if (!http.begin(url)) {
    LOGLN("[notify] begin failed");
    s_notifyFailures++;
    return;
  }

  int code;
  if (hasPlaceholder) {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    String body = "{\"text\":\"" + message + "\"}";
    code = http.POST(body);
  }
  http.end();

  if (code >= 200 && code < 300) {
    LOGF("[notify] sent (HTTP %d)\n", code);
    s_notifyFailures = 0;
  } else {
    LOGF("[notify] failed (HTTP %d)\n", code);
    s_notifyFailures++;
  }
}

#endif // NOTIFY_ENABLED

// ---------------------------------------------------------------------------
// Alarm control
// ---------------------------------------------------------------------------

static void startAlarm(int index) {
  Schedule* s = scheduleGet(index);
  if (!s) return;

  s_alarm.active        = true;
  s_alarm.scheduleIndex = index;
  s_alarm.startedAt     = millis();
  s_alarm.repeatCount   = 0;
  s_alarm.nextRepeatAt  = millis() + (uint32_t)ALARM_REPEAT_MINS * 60000UL;

  LOGF("[reminder] ALARM -> %s (%s) %02d:%02d\n",
       s->name, s->dose, s->hour, s->minute);

#if NOTIFY_ENABLED
  sendNotification(*s);
#endif
}

static void stopAlarm() {
  s_alarm.active = false;
  s_alarm.scheduleIndex = -1;
  buzzerOff();
  digitalWrite(PIN_LED_STATUS, LOW);
}

void reminderInit() {
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  pinMode(PIN_BUTTON_ACK, INPUT_PULLUP);
  buzzerOff();
  digitalWrite(PIN_LED_STATUS, LOW);

  s_alarm.active        = false;
  s_alarm.scheduleIndex = -1;
  s_alarm.repeatCount   = 0;
  s_alarm.startedAt     = 0;
  s_alarm.nextRepeatAt  = 0;
}

// ---------------------------------------------------------------------------
// Ringing pattern — non-blocking
// ---------------------------------------------------------------------------

static void driveBuzzerPattern() {
  uint32_t elapsed = millis() - s_alarm.startedAt;
  uint32_t cycle   = ALARM_BEEP_ON_MS + ALARM_BEEP_OFF_MS;
  bool on = (elapsed % cycle) < ALARM_BEEP_ON_MS;

  if (BUZZER_ACTIVE_HIGH) {
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
  } else {
    digitalWrite(PIN_BUZZER, on ? LOW : HIGH);
  }

  bool ledOn = ((millis() / ALARM_LED_BLINK_MS) % 2) == 0;
  digitalWrite(PIN_LED_STATUS, ledOn ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// Button — debounced
// ---------------------------------------------------------------------------

static bool readButtonPress() {
  static uint32_t lastPress = 0;
  static bool     lastState = false;

  bool pressed = (digitalRead(PIN_BUTTON_ACK) == BUTTON_PRESSED_LEVEL);
  bool edge = pressed && !lastState;
  lastState = pressed;

  if (edge && (millis() - lastPress > 400)) {
    lastPress = millis();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Schedule matching
// ---------------------------------------------------------------------------

static bool dayMatches(uint8_t daysMask, int wday) {
  // struct tm: 0 = Sunday. Bit 0 of the mask is also Sunday.
  return (daysMask & (1 << wday)) != 0;
}

static void checkSchedules() {
  if (!timeIsSynced()) return;

  struct tm t = timeNowTm();
  uint32_t dk = todayDateKey();

  for (int i = 0; i < scheduleCount(); i++) {
    Schedule* s = scheduleGet(i);
    if (!s || !s->enabled) continue;

    if (!dayMatches(s->days, t.tm_wday))   continue;
    if (s->takenDateKey == dk)             continue;   // already taken today

    // Is the current minute the scheduled minute?
    if (t.tm_hour != s->hour || t.tm_min != s->minute) continue;

    // Window guard — don't fire on a reboot hours late.
    // (Exact minute match above already handles this; the window is here
    //  so a schedule set for :00 still fires if the loop lands at :01.)
    if (t.tm_sec > 59) continue;

    if (s_alarm.active && s_alarm.scheduleIndex == i) continue;  // already ringing

    startAlarm(i);
    break;   // one alarm at a time
  }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void reminderLoop() {
  // 1. Handle an active alarm
  if (s_alarm.active) {
    driveBuzzerPattern();

    if (readButtonPress()) {
      LOGLN("[reminder] acknowledged by button");
      reminderAcknowledge();
      return;
    }

    uint32_t elapsed = millis() - s_alarm.startedAt;

    // Auto-stop after ALARM_DURATION_MS
    if (elapsed >= ALARM_DURATION_MS) {
      buzzerOff();
      digitalWrite(PIN_LED_STATUS, LOW);

      if (s_alarm.repeatCount < ALARM_MAX_REPEATS) {
        // Wait, then ring again
        if ((int32_t)(millis() - s_alarm.nextRepeatAt) >= 0) {
          s_alarm.repeatCount++;
          s_alarm.startedAt = millis();
          s_alarm.nextRepeatAt = millis() + (uint32_t)ALARM_REPEAT_MINS * 60000UL;
          LOGF("[reminder] repeat #%d\n", s_alarm.repeatCount);
        }
      } else {
        LOGLN("[reminder] giving up after max repeats");
        stopAlarm();
      }
    }
    return;
  }

  // 2. Poll schedules once per second
  if (millis() - s_lastCheck < 1000) return;
  s_lastCheck = millis();

  checkSchedules();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

bool reminderAcknowledge() {
  if (!s_alarm.active) return false;

  Schedule* s = scheduleGet(s_alarm.scheduleIndex);
  if (s) {
    s->takenDateKey = todayDateKey();
    scheduleSave();
    LOGF("[reminder] marked taken: %s @ %s\n", s->name, timeNowString().c_str());
  }
  stopAlarm();
  return true;
}

bool reminderDismiss() {
  if (!s_alarm.active) return false;
  LOGLN("[reminder] dismissed without marking taken");
  stopAlarm();
  return true;
}

bool reminderSnooze() {
  if (!s_alarm.active) return false;

  s_alarm.startedAt    = millis();
  s_alarm.nextRepeatAt = millis() + (uint32_t)ALARM_REPEAT_MINS * 60000UL;
  s_alarm.repeatCount  = 0;
  LOGF("[reminder] snoozed %d min\n", ALARM_REPEAT_MINS);
  return true;
}

// ---------------------------------------------------------------------------
// Status for the dashboard
// ---------------------------------------------------------------------------

ReminderState reminderGetState() {
  return s_alarm.active ? REMINDER_RINGING : REMINDER_IDLE;
}

int reminderActiveIndex() {
  return s_alarm.active ? s_alarm.scheduleIndex : -1;
}

String reminderStatusJson() {
  String out;
  out.reserve(320);

  out += "{\"state\":\"";
  out += s_alarm.active ? "ringing" : "idle";
  out += "\",\"activeIndex\":";
  out += s_alarm.active ? s_alarm.scheduleIndex : -1;

  out += ",\"time\":\"";   out += timeNowString();  out += "\"";
  out += ",\"date\":\"";   out += dateNowString();  out += "\"";
  out += ",\"synced\":";   out += timeIsSynced() ? "true" : "false";
  out += ",\"uptime\":";   out += (millis() / 1000);
  out += ",\"heap\":";     out += ESP.getFreeHeap();

  if (s_alarm.active) {
    Schedule* s = scheduleGet(s_alarm.scheduleIndex);
    if (s) {
      out += ",\"activeName\":\"";  out += s->name;  out += "\"";
      out += ",\"activeDose\":\"";  out += s->dose;  out += "\"";
    }
    out += ",\"elapsed\":"; out += (millis() - s_alarm.startedAt);
    out += ",\"repeats\":"; out += s_alarm.repeatCount;
  }

  out += "}";
  return out;
}

/* =========================================================================
 * BAGIAN 7 — WEB SERVER + DASHBOARD
 * ========================================================================= */

#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

static WebServer server(WEB_PORT);

// ===========================================================================
//  HTML — dashboard page
// ===========================================================================

static const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1220">
<title>FITALIS — Pengingat Obat</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0;overscroll-behavior:none}
body{
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  background:#0b1220;color:#e6edf7;min-height:100dvh;
  padding:16px 14px calc(24px + env(safe-area-inset-bottom));
  -webkit-user-select:none;user-select:none;
}
h1{font-size:19px;margin:0;letter-spacing:.5px}
h1 span{color:#4ade80}
.sub{font-size:11px;color:#64748b;margin-top:2px}
.card{background:#131c2e;border:1px solid #1e2b45;border-radius:14px;padding:14px;margin-bottom:12px}
.card h2{font-size:13px;margin:0 0 10px;color:#94a3b8;font-weight:600;text-transform:uppercase;letter-spacing:.6px}
.clock{font-size:38px;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1}
.clock.small{font-size:20px}
.meta{font-size:11px;color:#64748b;margin-top:4px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.stat{background:#0f1729;border-radius:10px;padding:10px}
.stat .k{font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.4px}
.stat .v{font-size:15px;font-weight:600;margin-top:3px}
.pill{display:inline-block;font-size:10px;padding:3px 8px;border-radius:99px;font-weight:600}
.pill.ok{background:#052e1a;color:#4ade80}
.pill.warn{background:#3b2a05;color:#fbbf24}
.pill.bad{background:#3b0d0d;color:#f87171}
.sched{background:#0f1729;border-radius:11px;padding:11px;margin-bottom:8px;border-left:3px solid #2563eb}
.sched.taken{border-left-color:#4ade80;opacity:.72}
.sched.off{border-left-color:#475569;opacity:.5}
.sched .row1{display:flex;justify-content:space-between;align-items:center;gap:8px}
.sched .nm{font-weight:600;font-size:14px}
.sched .tm{font-size:19px;font-weight:700;font-variant-numeric:tabular-nums}
.sched .dz{font-size:11px;color:#94a3b8;margin-top:3px}
.days{display:flex;gap:3px;margin-top:7px;flex-wrap:wrap}
.day{font-size:9px;padding:3px 6px;border-radius:5px;background:#1e2b45;color:#64748b}
.day.on{background:#1d4ed8;color:#fff}
button{font-family:inherit;font-size:13px;border:0;border-radius:10px;padding:11px 15px;min-height:44px;font-weight:600;cursor:pointer;color:#fff;background:#2563eb}
button:active{opacity:.75}
button.ghost{background:#1e2b45}
button.danger{background:#7f1d1d}
button.ok{background:#15803d}
button.tiny{padding:7px 11px;min-height:34px;font-size:11px;border-radius:8px}
input,select{font-family:inherit;font-size:15px;width:100%;padding:11px;border-radius:10px;border:1px solid #24314d;background:#0b1220;color:#e6edf7;margin-top:4px;min-height:44px}
label{font-size:11px;color:#94a3b8;display:block;margin-top:10px}
.frm{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.frm .full{grid-column:1/-1}
.daysel{display:flex;gap:5px;flex-wrap:wrap;margin-top:6px}
.daysel label{margin:0}
.daysel input{display:none}
.daysel span{display:inline-block;font-size:11px;padding:9px 11px;border-radius:8px;background:#1e2b45;color:#94a3b8;cursor:pointer;min-width:38px;text-align:center}
.daysel input:checked+span{background:#2563eb;color:#fff}
.hidden{display:none}
.alarm{background:#450a0a;border:1px solid #7f1d1d;border-radius:14px;padding:14px;margin-bottom:12px;text-align:center;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
.alarm .t{font-size:11px;color:#fca5a5;text-transform:uppercase;letter-spacing:1px}
.alarm .n{font-size:22px;font-weight:700;margin:7px 0}
.btnrow{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.btnrow button{flex:1;min-width:96px}
.bar{display:flex;gap:6px;margin-bottom:12px}
.bar button{flex:1;background:#1e2b45;font-size:11px;padding:9px;min-height:40px}
.bar button.on{background:#2563eb}
#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:24px;background:#1e2b45;padding:11px 18px;border-radius:10px;font-size:13px;opacity:0;transition:.25s;pointer-events:none;z-index:99;max-width:90vw;text-align:center}
#toast.show{opacity:1}
.empty{font-size:12px;color:#64748b;text-align:center;padding:18px}
</style>
</head>
<body>

<h1>FITALIS <span>&#9679;</span></h1>
<div class="sub" id="devid">memuat...</div>

<div id="alarmBox" class="alarm hidden">
  <div class="t">Waktunya minum obat</div>
  <div class="n" id="alarmName">-</div>
  <div class="sub" id="alarmDose">-</div>
  <div class="btnrow">
    <button class="ok" onclick="ack()">Sudah Minum</button>
    <button class="ghost" onclick="snooze()">Tunda 5 mnt</button>
  </div>
</div>

<div class="bar">
  <button id="tabLive" class="on" onclick="showTab('live')">Beranda</button>
  <button id="tabSched" onclick="showTab('sched')">Jadwal</button>
  <button id="tabInfo" onclick="showTab('info')">Info</button>
</div>

<!-- ============ TAB: LIVE ============ -->
<div id="tab-live">
  <div class="card">
    <h2>Waktu Sekarang</h2>
    <div class="clock" id="clock">--:--:--</div>
    <div class="meta" id="date">-</div>
    <div class="meta" id="syncstate">&nbsp;</div>
  </div>

  <div class="card">
    <h2>Ringkasan</h2>
    <div class="grid">
      <div class="stat"><div class="k">Jadwal Aktif</div><div class="v" id="statActive">0</div></div>
      <div class="stat"><div class="k">Sudah Diminum</div><div class="v" id="statTaken">0</div></div>
      <div class="stat"><div class="k">Jadwal Berikutnya</div><div class="v" id="statNext">-</div></div>
      <div class="stat"><div class="k">Status Alarm</div><div class="v" id="statAlarm">-</div></div>
    </div>
  </div>

  <div class="card">
    <h2>Jadwal Hari Ini</h2>
    <div id="todayList"><div class="empty">Belum ada jadwal</div></div>
  </div>
</div>

<!-- ============ TAB: SCHEDULE ============ -->
<div id="tab-sched" class="hidden">
  <div class="card">
    <h2>Daftar Jadwal</h2>
    <div id="schedList"><div class="empty">Belum ada jadwal</div></div>
  </div>

  <div class="card">
    <h2 id="formTitle">Tambah Jadwal</h2>
    <div class="frm">
      <div class="full">
        <label>Nama Obat</label>
        <input id="fName" placeholder="Contoh: Amlodipine" maxlength="31">
      </div>
      <div class="full">
        <label>Dosis</label>
        <input id="fDose" placeholder="Contoh: 1 tablet" maxlength="15">
      </div>
      <div>
        <label>Jam</label>
        <input id="fHour" type="number" min="0" max="23" value="8">
      </div>
      <div>
        <label>Menit</label>
        <input id="fMin" type="number" min="0" max="59" value="0">
      </div>
      <div class="full">
        <label>Hari</label>
        <div class="daysel" id="daySel"></div>
        <button type="button" class="ghost tiny" style="margin-top:8px" onclick="setDays('all')">Setiap Hari</button>
        <button type="button" class="ghost tiny" style="margin-top:8px" onclick="setDays('none')">Kosongkan</button>
      </div>
    </div>
    <div class="btnrow">
      <button id="btnSave" onclick="saveSched()">Simpan Jadwal</button>
      <button id="btnCancel" class="ghost hidden" onclick="resetForm()">Batal</button>
    </div>
    <input type="hidden" id="fIndex" value="-1">
  </div>
</div>

<!-- ============ TAB: INFO ============ -->
<div id="tab-info" class="hidden">
  <div class="card">
    <h2>Perangkat</h2>
    <div class="grid">
      <div class="stat"><div class="k">Device ID</div><div class="v" id="iDev">-</div></div>
      <div class="stat"><div class="k">Versi</div><div class="v" id="iVer">-</div></div>
      <div class="stat"><div class="k">IP</div><div class="v" id="iIp">-</div></div>
      <div class="stat"><div class="k">WiFi</div><div class="v" id="iRssi">-</div></div>
      <div class="stat"><div class="k">Uptime</div><div class="v" id="iUp">-</div></div>
      <div class="stat"><div class="k">RAM Bebas</div><div class="v" id="iHeap">-</div></div>
    </div>
  </div>

  <div class="card">
    <h2>Set Jam Manual</h2>
    <div class="meta" style="margin-bottom:8px">Kalau ESP32 tidak dapat internet (mode AP), set jamnya dari sini. Jam akan dipakai untuk memicu alarm.</div>
    <input id="iManualTime" type="datetime-local">
    <div class="btnrow">
      <button onclick="setTime()">Set Jam</button>
    </div>
  </div>

  <div class="card">
    <h2>Uji Coba</h2>
    <div class="btnrow">
      <button class="ghost" onclick="testAlarm()">Tes Alarm 5 Detik</button>
      <button class="ghost" onclick="resetTaken()">Reset Penanda Harian</button>
    </div>
  </div>
</div>

<div id="toast"></div>

<script>
const DAYS = ['Min','Sen','Sel','Rab','Kam','Jum','Sab'];
let editingIndex = -1;

function toast(m){const t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2200);}

async function api(path, opts){
  const r = await fetch(path, opts);
  if(!r.ok) throw new Error('HTTP '+r.status);
  return r.json();
}

function showTab(t){
  ['live','sched','info'].forEach(x=>{
    document.getElementById('tab-'+x).classList.toggle('hidden', x!==t);
    document.getElementById('tab'+x.charAt(0).toUpperCase()+x.slice(1)).classList.toggle('on', x===t);
  });
  if(t==='sched') loadSchedules();
  if(t==='info') loadInfo();
}

// ---- clock + status polling ----
let lastState = 'idle';

async function refresh(){
  try{
    const st = await api('/api/status');
    document.getElementById('clock').textContent = st.time;
    document.getElementById('date').textContent = st.date;
    document.getElementById('syncstate').innerHTML = st.synced
      ? '<span class="pill ok">Jam sinkron (NTP)</span>'
      : '<span class="pill warn">Jam belum diset — buka tab Info</span>';
    document.getElementById('devid').textContent = 'Device: ' + (st.deviceId||'-');
    document.getElementById('statAlarm').textContent = st.state==='ringing' ? 'BERBUNYI' : 'Siaga';

    const box = document.getElementById('alarmBox');
    if(st.state === 'ringing'){
      box.classList.remove('hidden');
      document.getElementById('alarmName').textContent = st.activeName || '-';
      document.getElementById('alarmDose').textContent = st.activeDose || '';
      if(lastState !== 'ringing'){
        toast('Waktunya minum obat!');
        if(navigator.vibrate) navigator.vibrate([400,200,400,200,400]);
      }
    } else {
      box.classList.add('hidden');
    }
    lastState = st.state;
  }catch(e){ /* backend busy, try again next tick */ }
}

async function loadToday(){
  try{
    const d = await api('/api/schedules');
    renderSchedules(d, 'todayList', true);
    const active = d.schedules.filter(s=>s.enabled).length;
    const taken  = d.schedules.filter(s=>s.taken).length;
    document.getElementById('statActive').textContent = active;
    document.getElementById('statTaken').textContent = taken;

    const now = new Date();
    const nowMin = now.getHours()*60 + now.getMinutes();
    let next = null;
    d.schedules.filter(s=>s.enabled && !s.taken).forEach(s=>{
      const m = s.hour*60 + s.minute;
      if(m >= nowMin && (!next || m < next.hour*60+next.minute)) next = s;
    });
    document.getElementById('statNext').textContent = next
      ? String(next.hour).padStart(2,'0')+':'+String(next.minute).padStart(2,'0')
      : '-';
  }catch(e){}
}

function renderSchedules(d, targetId, compactOnly){
  const el = document.getElementById(targetId);
  if(!d.schedules.length){
    el.innerHTML = '<div class="empty">Belum ada jadwal. Buka tab Jadwal untuk menambah.</div>';
    return;
  }
  el.innerHTML = d.schedules.map(s=>{
    let cls = 'sched';
    if(s.taken) cls += ' taken';
    else if(!s.enabled) cls += ' off';

    const dayHtml = DAYS.map((dn,i)=>
      `<span class="day ${(s.days & (1<<i)) ? 'on':''}">${dn}</span>`).join('');

    const timeStr = String(s.hour).padStart(2,'0')+':'+String(s.minute).padStart(2,'0');

    if(compactOnly){
      return `<div class="${cls}">
        <div class="row1"><div class="nm">${esc(s.name)}</div><div class="tm">${timeStr}</div></div>
        <div class="dz">${esc(s.dose)} ${s.taken?'&middot; <span style="color:#4ade80">sudah diminum</span>':''}</div>
      </div>`;
    }

    return `<div class="${cls}">
      <div class="row1"><div class="nm">${esc(s.name)}</div><div class="tm">${timeStr}</div></div>
      <div class="dz">${esc(s.dose)} &middot; ${s.enabled?'aktif':'nonaktif'}${s.taken?' &middot; sudah diminum':''}</div>
      <div class="days">${dayHtml}</div>
      <div class="btnrow">
        <button class="ghost tiny" onclick="editSched(${s.index})">Ubah</button>
        <button class="ghost tiny" onclick="toggleSched(${s.index},${s.enabled?0:1})">${s.enabled?'Matikan':'Nyalakan'}</button>
        <button class="danger tiny" onclick="delSched(${s.index})">Hapus</button>
      </div>
    </div>`;
  }).join('');
}

function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}

// ---- day selector ----
function buildDaySel(){
  document.getElementById('daySel').innerHTML = DAYS.map((dn,i)=>
    `<label><input type="checkbox" class="daycb" value="${i}"><span>${dn}</span></label>`).join('');
}
function setDays(mode){
  document.querySelectorAll('.daycb').forEach(cb=>cb.checked = (mode==='all'));
}
function getDays(){
  let m = 0;
  document.querySelectorAll('.daycb').forEach(cb=>{ if(cb.checked) m |= (1<<parseInt(cb.value)); });
  return m;
}
function applyDays(mask){
  document.querySelectorAll('.daycb').forEach(cb=>{ cb.checked = !!(mask & (1<<parseInt(cb.value))); });
}

// ---- schedule CRUD ----
async function loadSchedules(){
  try{
    const d = await api('/api/schedules');
    renderSchedules(d, 'schedList', false);
  }catch(e){ toast('Gagal memuat jadwal'); }
}

function editSched(i){
  api('/api/schedules').then(d=>{
    const s = d.schedules.find(x=>x.index===i);
    if(!s) return;
    document.getElementById('fName').value = s.name;
    document.getElementById('fDose').value = s.dose;
    document.getElementById('fHour').value = s.hour;
    document.getElementById('fMin').value  = s.minute;
    applyDays(s.days);
    document.getElementById('fIndex').value = i;
    editingIndex = i;
    document.getElementById('formTitle').textContent = 'Ubah Jadwal';
    document.getElementById('btnCancel').classList.remove('hidden');
    window.scrollTo({top:document.body.scrollHeight, behavior:'smooth'});
  });
}

function resetForm(){
  document.getElementById('fName').value = '';
  document.getElementById('fDose').value = '';
  document.getElementById('fHour').value = 8;
  document.getElementById('fMin').value = 0;
  applyDays(0x7F);
  document.getElementById('fIndex').value = -1;
  editingIndex = -1;
  document.getElementById('formTitle').textContent = 'Tambah Jadwal';
  document.getElementById('btnCancel').classList.add('hidden');
}

async function saveSched(){
  const name = document.getElementById('fName').value.trim();
  const dose = document.getElementById('fDose').value.trim();
  const hour = parseInt(document.getElementById('fHour').value);
  const min  = parseInt(document.getElementById('fMin').value);
  const days = getDays();
  const idx  = parseInt(document.getElementById('fIndex').value);

  if(!name){ toast('Nama obat wajib diisi'); return; }
  if(isNaN(hour)||hour<0||hour>23){ toast('Jam harus 0-23'); return; }
  if(isNaN(min)||min<0||min>59){ toast('Menit harus 0-59'); return; }
  if(days===0){ toast('Pilih minimal satu hari'); return; }

  const body = {name, dose: dose||'-', hour, minute: min, days};
  try{
    if(idx >= 0){
      await api('/api/schedules/'+idx, {method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
      toast('Jadwal diperbarui');
    } else {
      await api('/api/schedules', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
      toast('Jadwal ditambahkan');
    }
    resetForm();
    loadSchedules();
    loadToday();
  }catch(e){ toast('Gagal menyimpan: '+e.message); }
}

async function toggleSched(i, en){
  const d = await api('/api/schedules');
  const s = d.schedules.find(x=>x.index===i);
  if(!s) return;
  await api('/api/schedules/'+i, {method:'PUT',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name:s.name,dose:s.dose,hour:s.hour,minute:s.minute,days:s.days,enabled:!!en})});
  loadSchedules(); loadToday();
}

async function delSched(i){
  if(!confirm('Hapus jadwal ini?')) return;
  await api('/api/schedules/'+i, {method:'DELETE'});
  toast('Jadwal dihapus');
  loadSchedules(); loadToday();
}

// ---- alarm actions ----
async function ack(){ await api('/api/alarm/ack',{method:'POST'}); toast('Tercatat, terima kasih'); loadToday(); }
async function snooze(){ await api('/api/alarm/snooze',{method:'POST'}); toast('Ditunda 5 menit'); }
async function testAlarm(){
  await api('/api/test/alarm',{method:'POST'});
  toast('Alarm tes berbunyi 5 detik');
}
async function resetTaken(){ await api('/api/schedules/reset-taken',{method:'POST'}); toast('Penanda harian direset'); loadToday(); }

// ---- info ----
async function loadInfo(){
  try{
    const i = await api('/api/status');
    document.getElementById('iDev').textContent  = i.deviceId;
    document.getElementById('iVer').textContent  = i.version;
    document.getElementById('iIp').textContent   = i.ip;
    document.getElementById('iRssi').textContent = i.rssi ? i.rssi+' dBm' : 'AP mode';
    const up = i.uptime;
    document.getElementById('iUp').textContent   = Math.floor(up/3600)+'j '+Math.floor((up%3600)/60)+'m';
    document.getElementById('iHeap').textContent = Math.round(i.heap/1024)+' KB';
  }catch(e){}
}

async function setTime(){
  const v = document.getElementById('iManualTime').value;
  if(!v){ toast('Pilih tanggal dan jam dulu'); return; }
  try{
    await api('/api/time', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({datetime:v})});
    toast('Jam diatur');
    refresh();
  }catch(e){ toast('Gagal set jam'); }
}

// ---- boot ----
buildDaySel();
applyDays(0x7F);
resetForm();
refresh();
loadToday();
setInterval(refresh, 2000);
setInterval(loadToday, 15000);
setInterval(()=>{ if(!document.getElementById('tab-sched').classList.contains('hidden')) loadSchedules(); }, 20000);
</script>
</body>
</html>)HTMLPAGE";

// ===========================================================================
//  Helpers
// ===========================================================================

static void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendOk(const char* msg) {
  String b = "{\"ok\":true,\"message\":\"";
  b += msg;
  b += "\"}";
  sendJson(200, b);
}

static void sendErr(int code, const char* msg) {
  String b = "{\"ok\":false,\"error\":\"";
  b += msg;
  b += "\"}";
  sendJson(code, b);
}

// Pull a string field out of a flat JSON object body.
// Deliberately minimal: avoids a JSON library for 4 fields.
static bool jsonField(const String& body, const char* key, String& out) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return false;
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return false;

  int i = colon + 1;
  while (i < (int)body.length() && isspace((unsigned char)body[i])) i++;
  if (i >= (int)body.length()) return false;

  if (body[i] == '"') {
    i++;
    String val;
    while (i < (int)body.length() && body[i] != '"') {
      if (body[i] == '\\' && i + 1 < (int)body.length()) i++;
      val += body[i++];
    }
    out = val;
    return true;
  }

  // number / bool / null
  int start = i;
  while (i < (int)body.length() && body[i] != ',' && body[i] != '}') i++;
  out = body.substring(start, i);
  out.trim();
  return true;
}

static long jsonInt(const String& body, const char* key, long fallback) {
  String v;
  if (!jsonField(body, key, v) || v.length() == 0) return fallback;
  if (v == "true")  return 1;
  if (v == "false") return 0;
  return v.toInt();
}

// ===========================================================================
//  Route handlers
// ===========================================================================

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

static void handleApiStatus() {
  String out;
  out.reserve(400);
  out += "{\"deviceId\":\"";  out += FITALIS_DEVICE_ID; out += "\"";
  out += ",\"version\":\"";   out += FITALIS_VERSION;   out += "\"";
  out += ",\"ip\":\"";        out += webLocalIp();      out += "\"";

  int32_t rssi = (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  out += ",\"rssi\":";  out += rssi;

  // Merge reminder status (it is already a JSON object).
  String rem = reminderStatusJson();
  if (rem.startsWith("{")) {
    out += ",";
    out += rem.substring(1, rem.length() - 1);   // strip outer braces
  }
  out += "}";
  sendJson(200, out);
}

static void handleGetSchedules() {
  sendJson(200, scheduleToJson());
}

static void handleAddSchedule() {
  String body = server.arg("plain");
  String name, dose;
  jsonField(body, "name", name);
  jsonField(body, "dose", dose);

  long hour = jsonInt(body, "hour", -1);
  long min  = jsonInt(body, "minute", -1);
  long days = jsonInt(body, "days", 0);

  if (name.length() == 0)          { sendErr(400, "name required"); return; }
  if (hour < 0 || hour > 23)       { sendErr(400, "hour out of range"); return; }
  if (min < 0 || min > 59)         { sendErr(400, "minute out of range"); return; }
  if (days <= 0)                   { sendErr(400, "days required"); return; }

  int idx = scheduleAdd(name.c_str(), dose.c_str(), (uint8_t)hour, (uint8_t)min, (uint8_t)days);
  if (idx < 0) { sendErr(409, "schedule table full"); return; }

  String out = "{\"ok\":true,\"index\":" + String(idx) + "}";
  sendJson(201, out);
}

static void handleUpdateSchedule() {
  int idx = server.pathArg("index").toInt();
  String body = server.arg("plain");

  if (idx < 0 || idx >= scheduleCount()) { sendErr(404, "schedule not found"); return; }

  Schedule* cur = scheduleGet(idx);

  String name, dose;
  if (!jsonField(body, "name", name)) name = cur->name;
  if (!jsonField(body, "dose", dose)) dose = cur->dose;

  long hour = jsonInt(body, "hour",   cur->hour);
  long min  = jsonInt(body, "minute", cur->minute);
  long days = jsonInt(body, "days",   cur->days);
  long en   = jsonInt(body, "enabled", cur->enabled ? 1 : 0);

  if (hour < 0 || hour > 23 || min < 0 || min > 59 || days <= 0) {
    sendErr(400, "invalid time or days");
    return;
  }

  scheduleUpdate(idx, name.c_str(), dose.c_str(),
                 (uint8_t)hour, (uint8_t)min, (uint8_t)days, en != 0);
  sendOk("schedule updated");
}

static void handleDeleteSchedule() {
  int idx = server.pathArg("index").toInt();
  if (!scheduleDelete(idx)) { sendErr(404, "schedule not found"); return; }
  sendOk("schedule deleted");
}

static void handleAlarmAck() {
  if (reminderAcknowledge()) sendOk("acknowledged");
  else                       sendErr(409, "no active alarm");
}

static void handleAlarmDismiss() {
  if (reminderDismiss()) sendOk("dismissed");
  else                   sendErr(409, "no active alarm");
}

static void handleAlarmSnooze() {
  if (reminderSnooze()) sendOk("snoozed");
  else                  sendErr(409, "no active alarm");
}

static void handleTestAlarm() {
  // Ring for 5 seconds regardless of schedule state, so the user can
  // verify wiring before relying on it.
  digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_HIGH ? HIGH : LOW);
  delay(5000);
  digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_HIGH ? LOW : HIGH);
  LOGLN("[web] test alarm fired");
  sendOk("test alarm fired");
}

static void handleResetTaken() {
  scheduleClearTakenFlags();
  sendOk("taken flags cleared");
}

static void handleSetTime() {
  String body = server.arg("plain");
  String dt;
  if (!jsonField(body, "datetime", dt)) { sendErr(400, "datetime required"); return; }
  if (!timeSetFromClient(dt))           { sendErr(400, "invalid datetime format"); return; }
  sendOk("time set");
}

static void handleNotFound() {
  sendErr(404, "not found");
}

// ===========================================================================
//  WiFi / NTP
// ===========================================================================

bool webIsConnected() {
#if WIFI_MODE == WIFI_MODE_STATION
  return WiFi.status() == WL_CONNECTED;
#else
  return true;   // AP is always "up"
#endif
}

String webLocalIp() {
#if WIFI_MODE == WIFI_MODE_STATION
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return String("0.0.0.0");
#else
  return WiFi.softAPIP().toString();
#endif
}

static void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD[0] ? AP_PASSWORD : nullptr);
  LOGF("[wifi] AP %s -> %s\n", ok ? "started" : "FAILED",
       WiFi.softAPIP().toString().c_str());
}

static bool connectStation() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    LOGF("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  LOGLN("[wifi] station connect failed");
  return false;
}

bool webConnectAndSyncTime() {
#if WIFI_MODE == WIFI_MODE_AP
  startAccessPoint();
  // No internet in AP mode — the dashboard sets the clock instead.
  return false;
#else
  if (!connectStation()) {
    // Fall back to AP so the dashboard is still reachable.
    LOGLN("[wifi] falling back to AP mode");
    startAccessPoint();
    return false;
  }

  configTime(TZ_OFFSET_HOURS * 3600, 0, NTP_SERVER_1, NTP_SERVER_2);

  LOGLN("[ntp] waiting for time sync...");
  uint32_t start = millis();
  while (!timeIsSynced() && millis() - start < 15000) {
    delay(300);
  }

  if (timeIsSynced()) {
    LOGF("[ntp] synced: %s %s\n", dateNowString().c_str(), timeNowString().c_str());
    return true;
  }
  LOGLN("[ntp] sync timed out — set the clock from the dashboard");
  return false;
#endif
}

// ===========================================================================
//  Init / loop
// ===========================================================================

void webInit() {
  server.on("/", HTTP_GET, handleRoot);

  server.on("/api/status",    HTTP_GET, handleApiStatus);
  server.on("/api/schedules", HTTP_GET, handleGetSchedules);
  server.on("/api/schedules", HTTP_POST, handleAddSchedule);

  // Path parameters (<index>) need the newer ESP32 core URI matcher.
  server.on(UriBraces("/api/schedules/{}"), HTTP_PUT, handleUpdateSchedule);
  server.on(UriBraces("/api/schedules/{}"), HTTP_DELETE, handleDeleteSchedule);

  server.on("/api/alarm/ack",     HTTP_POST, handleAlarmAck);
  server.on("/api/alarm/dismiss", HTTP_POST, handleAlarmDismiss);
  server.on("/api/alarm/snooze",  HTTP_POST, handleAlarmSnooze);

  server.on("/api/test/alarm",             HTTP_POST, handleTestAlarm);
  server.on("/api/schedules/reset-taken",  HTTP_POST, handleResetTaken);
  server.on("/api/time",                   HTTP_POST, handleSetTime);

  server.onNotFound(handleNotFound);
  server.begin();
  LOGF("[web] dashboard on http://%s/\n", webLocalIp().c_str());
}

void webLoop() {
  server.handleClient();

#if WIFI_MODE == WIFI_MODE_STATION
  // Reconnect in the background if the link drops. The reminder keeps
  // running from flash with no network at all.
  static uint32_t lastAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastAttempt > 30000) {
    lastAttempt = millis();
    LOGLN("[wifi] reconnecting...");
    WiFi.disconnect();
    WiFi.begin(STA_SSID, STA_PASSWORD);
  }
#endif
}

/* =========================================================================
 * BAGIAN 8 — MAIN (setup + loop)
 * ========================================================================= */

/*
 * FITALIS — Smart Medicine Reminder
 * ===============================================================
 * Board : ESP32 DevKit V1
 * Build : Arduino IDE 2.x  (board: "ESP32 Dev Module")
 *         or PlatformIO (env:esp32dev)
 *
 * WHAT IT DOES
 *   - Stores medicine schedules in flash (survive power loss).
 *   - Rings an active buzzer at each scheduled time.
 *   - Serves a local web dashboard for adding/editing schedules.
 *   - Keeps time via NTP; the dashboard can set the clock when there
 *     is no internet (AP mode).
 *   - Works with no WiFi at all: schedules stay in flash and the
 *     alarm still fires.
 *
 * HARDWARE (see include/config.h to change pins)
 *   Buzzer (+)  -> GPIO 25
 *   Buzzer (-)  -> GND
 *   Button      -> GPIO 27 and GND   (acknowledge: "I took my medicine")
 *   LED         -> GPIO 2 (onboard)
 *
 * FIRST RUN
 *   1. Edit include/config.h: set STA_SSID / STA_PASSWORD (or switch to
 *      WIFI_MODE_AP for a standalone hotspot).
 *   2. Flash, open Serial Monitor at 115200.
 *   3. The IP address is printed. Open it in a phone browser.
 */

#include <Arduino.h>


// ---------------------------------------------------------------------------
// Boot banner
// ---------------------------------------------------------------------------

static void printBanner() {
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.printf( "  %s  v%s\n", FITALIS_NAME, FITALIS_VERSION);
  Serial.println(F("  Smart Medicine Reminder"));
  Serial.println(F("=============================================="));
  Serial.printf( "  Device ID : %s\n", FITALIS_DEVICE_ID);
  Serial.printf( "  Buzzer    : GPIO %d\n", PIN_BUZZER);
  Serial.printf( "  Button    : GPIO %d\n", PIN_BUTTON_ACK);
  Serial.printf( "  WiFi mode : %s\n",
                 (WIFI_MODE == WIFI_MODE_AP) ? "AP (standalone hotspot)" : "STATION (join router)");
  Serial.println(F("----------------------------------------------"));
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  printBanner();

  // 1. Load persisted schedules first — the reminder must work even if
  //    networking fails completely.
  scheduleInit();
  reminderInit();
  LOGF("[main] %d schedule(s) in flash\n", scheduleCount());

  // 2. Bring up networking + NTP. Never blocks the reminder for long:
  //    worst case it falls back to AP mode and the clock is set from
  //    the dashboard.
  bool timeOk = webConnectAndSyncTime();

  if (!timeOk && WIFI_MODE == WIFI_MODE_STATION) {
    LOGLN("[main] running without synced clock — set time from the dashboard");
  }

  // 3. Start the dashboard.
  webInit();

  Serial.println(F("----------------------------------------------"));
  Serial.printf( "  Dashboard : http://%s/\n", webLocalIp().c_str());
  Serial.printf( "  Clock     : %s %s\n", dateNowString().c_str(), timeNowString().c_str());
  Serial.printf( "  Free RAM  : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println(F("=============================================="));
  Serial.println();
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void loop() {
  // Order matters: the alarm check runs every iteration so a due dose
  // is never missed while the web server is busy handling a request.
  reminderLoop();
  webLoop();
  delay(2);
}
