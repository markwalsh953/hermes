/*
 * Hermes - WiFi v1 firmware
 * Board: DPTechnics Walter (ESP32-S3 + Sequans GM02SP modem with built-in GNSS)
 *
 * What it does:
 *   1. Connects to WiFi and gets the real time from NTP.
 *   2. Feeds that time into the Walter's GNSS receiver (gnssSetUTCTime) so GPS
 *      works WITHOUT a cellular connection.
 *   3. Signs in to Firebase anonymously and starts a new run "session" - each
 *      boot gets its own session id, so runs don't overwrite each other and
 *      the web app can list past runs instead of just the live one.
 *   4. Every FIX_INTERVAL_MS, takes a GNSS fix and POSTs {lat, lon, t} to the
 *      Firebase Realtime Database over WiFi, so the Hermes web app can draw the route.
 *
 * v1 uses WiFi only - no SIM / no LTE. Because we skip cellular "assistance data",
 * the FIRST fix outdoors can take a few minutes (cold start, clear sky needed).
 * Later fixes are faster. The cellular upgrade (v2, firmware/hermes_cellular) is
 * what makes first fix quick.
 *
 * Hardware note: connect the GNSS/GPS antenna to the Walter's GNSS connector, and
 * be outdoors with a view of the sky. The cellular antenna is not needed in v1.
 *
 * Fill in your WiFi + Firebase details in secrets.h (never committed to git).
 */

#include <WalterModem.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

// ---------------- Tunables ----------------
#define FIX_INTERVAL_MS      20000     // gap between GPS points (ms)
#define MAX_GNSS_CONFIDENCE  100.0     // accept a fix at/under this confidence value
#define GNSS_FIX_TIMEOUT_MS  300000    // give up on one fix after 5 min (cold first fix is slow)

// ---------------- Globals -----------------
WalterModem modem;
volatile bool gnssFixReceived = false;
WMGNSSFixEvent latestFix = {};

// Firebase Auth (anonymous sign-in) - see "Firebase security" in the README.
String idToken;
String refreshToken;
unsigned long tokenExpiryMs = 0;

// This run's session id (also doubles as its Firebase key).
String sessionId;

// GNSS event handler - the modem calls this asynchronously when a fix arrives.
// Keep it lightweight (just copy the data and set a flag).
void onGnssEvent(WMGNSSEventType type, const WMGNSSEventData* data, void* args) {
  if (type == WALTER_MODEM_GNSS_EVENT_FIX) {
    memcpy(&latestFix, &data->gnssfix, sizeof(WMGNSSFixEvent));
    gnssFixReceived = true;
  }
}

void connectWifi() {
  Serial.printf("WiFi: connecting to \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.printf("\nWiFi: connected, IP %s\n", WiFi.localIP().toString().c_str());
}

void syncTime() {
  // NTP over WiFi -> real Unix epoch time
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("NTP: syncing");
  time_t now = time(nullptr);
  while (now < 1700000000) { delay(300); Serial.print("."); now = time(nullptr); }
  Serial.printf("\nNTP: epoch = %ld\n", (long)now);
}

// Pull the string value for "key":"value" out of a flat JSON response. Good
// enough for Google's Identity Toolkit responses - avoids a full JSON library.
String extractJsonString(const String& json, const char* key) {
  String needle = String("\"") + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();
  int end = json.indexOf('"', start);
  if (end < 0) return "";
  return json.substring(start, end);
}

// Sign in anonymously (Firebase Auth) so writes carry a real auth token.
// Requires the "Anonymous" sign-in provider enabled in the Firebase console
// and a Web API key in secrets.h. See firebase/database.rules.json.
bool firebaseSignIn() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", "application/json");
  int code = https.POST("{\"returnSecureToken\":true}");
  bool ok = false;
  if (code == 200) {
    String body = https.getString();
    idToken = extractJsonString(body, "idToken");
    refreshToken = extractJsonString(body, "refreshToken");
    String expiresIn = extractJsonString(body, "expiresIn");
    unsigned long ttlMs = (expiresIn.length() ? (unsigned long)expiresIn.toInt() : 3600UL) * 1000UL;
    tokenExpiryMs = millis() + ttlMs - 60000UL;  // refresh a minute before it actually expires
    ok = idToken.length() > 0;
  }
  Serial.printf("Firebase auth sign-in -> HTTP %d (%s)\n", code, ok ? "ok" : "failed");
  https.end();
  return ok;
}

// Trade the refresh token for a new id token, falling back to a fresh
// anonymous sign-in if we don't have a refresh token yet.
bool firebaseRefreshToken() {
  if (refreshToken.length() == 0) return firebaseSignIn();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "grant_type=refresh_token&refresh_token=" + refreshToken;
  int code = https.POST(body);
  bool ok = false;
  if (code == 200) {
    String resp = https.getString();
    idToken = extractJsonString(resp, "id_token");
    refreshToken = extractJsonString(resp, "refresh_token");
    String expiresIn = extractJsonString(resp, "expires_in");
    unsigned long ttlMs = (expiresIn.length() ? (unsigned long)expiresIn.toInt() : 3600UL) * 1000UL;
    tokenExpiryMs = millis() + ttlMs - 60000UL;
    ok = idToken.length() > 0;
  }
  Serial.printf("Firebase auth refresh -> HTTP %d (%s)\n", code, ok ? "ok" : "failed");
  https.end();
  return ok ? true : firebaseSignIn();
}

void ensureFreshToken() {
  if (idToken.length() == 0 || (long)(millis() - tokenExpiryMs) >= 0) {
    firebaseRefreshToken();
  }
}

// Build a Firebase Realtime Database URL for a given path, with the auth
// token attached so writes satisfy database.rules.json.
String firebaseUrl(const String& path) {
  String url = String("https://") + FIREBASE_HOST + "/" + path + ".json";
  if (idToken.length() > 0) url += "?auth=" + idToken;
  return url;
}

// POST one GPS point to this run's session (Firebase assigns a chronological push id).
bool pushPoint(double lat, double lon, uint64_t tMs) {
  ensureFreshToken();
  WiFiClientSecure client;
  client.setInsecure();  // v1: skip TLS cert validation (fine for a hobby demo; tighten later)
  HTTPClient https;
  if (!https.begin(client, firebaseUrl("sessions/" + sessionId + "/points"))) {
    Serial.println("HTTP: begin() failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");
  char body[128];
  snprintf(body, sizeof(body), "{\"lat\":%.6f,\"lon\":%.6f,\"t\":%llu}",
           lat, lon, (unsigned long long)tMs);
  int code = https.POST((uint8_t*)body, strlen(body));
  Serial.printf("Firebase POST -> HTTP %d  %s\n", code, body);
  https.end();
  return code > 0 && code < 300;
}

// Start a brand-new run: pick a session id, publish it as the live session,
// and stamp the session's start time - so each run gets its own history
// entry instead of overwriting the previous one.
void startSession() {
  sessionId = String((uint32_t)time(nullptr));
  ensureFreshToken();

  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    if (https.begin(client, firebaseUrl("currentSession"))) {
      https.addHeader("Content-Type", "application/json");
      int code = https.PUT("\"" + sessionId + "\"");
      Serial.printf("Firebase set currentSession -> HTTP %d\n", code);
      https.end();
    }
  }
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    if (https.begin(client, firebaseUrl("sessions/" + sessionId + "/meta"))) {
      https.addHeader("Content-Type", "application/json");
      char body[64];
      snprintf(body, sizeof(body), "{\"start\":%llu}", (unsigned long long)((uint64_t)time(nullptr) * 1000ULL));
      int code = https.PUT((uint8_t*)body, strlen(body));
      Serial.printf("Firebase set session meta -> HTTP %d\n", code);
      https.end();
    }
  }
  Serial.printf("Session started: %s\n", sessionId.c_str());
}

// Request a single GNSS fix and wait for it (or time out).
bool getFix() {
  gnssFixReceived = false;
  if (!modem.gnssPerformAction()) { Serial.println("GNSS: could not start fix"); return false; }
  Serial.print("GNSS: waiting for fix");
  uint32_t start = millis();
  while (!gnssFixReceived) {
    if (millis() - start > GNSS_FIX_TIMEOUT_MS) { Serial.println("\nGNSS: fix timed out"); return false; }
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\nGNSS: lat=%.6f lon=%.6f confidence=%.1f sats=%d\n",
                latestFix.latitude, latestFix.longitude,
                latestFix.estimatedConfidence, latestFix.satCount);

  // Accept any fix that is a real lock: non-zero coordinates and at least one
  // satellite. (Confidence is logged above for info only - early/cold fixes can
  // report high confidence values, so we don't reject on it in v1.)
  bool valid = latestFix.latitude != 0.0 && latestFix.longitude != 0.0 &&
               latestFix.satCount > 0;
  if (!valid) Serial.println("GNSS: not a usable lock yet - will retry");
  return valid;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Hermes WiFi v1 ===");

  connectWifi();
  syncTime();

  // Start talking to the Walter's modem (the library knows the Walter's pinout).
  if (!modem.begin(&Serial2)) {
    Serial.println("Modem init failed - restarting");
    delay(3000);
    ESP.restart();
  }
  Serial.println("Modem initialized");

  modem.setGNSSEventHandler(onGnssEvent, NULL);

  // Turn the cellular RF off so the GNSS receiver is usable. We never use LTE in v1.
  modem.setOpState(WALTER_MODEM_OPSTATE_MINIMUM);

  if (!modem.gnssConfig()) Serial.println("Warning: gnssConfig() failed");

  // Hand the modem the real time from NTP (this is what lets GNSS work without cellular).
  modem.gnssSetUTCTime((uint64_t)time(nullptr));

  firebaseSignIn();
  startSession();
  Serial.println("Setup complete - tracking run. First fix may take a few minutes outdoors.\n");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWifi();

  if (getFix()) {
    uint64_t tMs = (uint64_t)time(nullptr) * 1000ULL;   // web app expects milliseconds
    pushPoint(latestFix.latitude, latestFix.longitude, tMs);
  }

  delay(FIX_INTERVAL_MS);
}
