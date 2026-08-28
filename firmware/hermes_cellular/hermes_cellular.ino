/*
 * Hermes - Cellular v2 firmware
 * Board: DPTechnics Walter (ESP32-S3 + Sequans GM02SP modem, LTE-M/NB-IoT + GNSS)
 *
 * What it does:
 *   1. Attaches to the cellular network over LTE-M (no WiFi, no SIM PIN assumed) and
 *      gets the time from the network clock (AT+CCLK) instead of WiFi/NTP.
 *   2. Feeds that time into the GNSS receiver, same as v1. With an active cellular
 *      data (PDP) context, the modem can also fetch GNSS assistance data over the
 *      air, which is what makes the first fix much faster than v1's WiFi-only,
 *      GPS-only cold start.
 *   3. Signs in to Firebase anonymously and starts a new run "session", same data
 *      model as v1 (see firmware/hermes_wifi/hermes_wifi.ino).
 *   4. Every FIX_INTERVAL_MS, takes a GNSS fix and pushes {lat, lon, t} to the same
 *      Firebase Realtime Database, over the modem's own HTTP(S) profile (AT
 *      commands) - there's no WiFi/lwIP stack in this build, so the usual Arduino
 *      WiFiClientSecure/HTTPClient classes don't apply; the Sequans modem does
 *      HTTP(S) itself.
 *
 * IMPORTANT - unverified against real hardware: this file has not been compiled
 * or flashed in this environment (no hardware access here). The cellular/HTTP
 * AT-command API below (definePDPContext / setOpState / httpConfigProfile /
 * httpSend / httpDidRing / getClock etc.) is written from WalterModem's published
 * examples, but exact method names/signatures vary by library version. Before
 * relying on this build, compile it against your installed WalterModem version,
 * fix any signature mismatches the compiler flags, and test outdoors with a SIM
 * that has an active data plan.
 *
 * Fill in your APN + Firebase details in secrets.h (never committed to git).
 */

#include <WalterModem.h>
#include <time.h>
#include "secrets.h"

// ---------------- Tunables ----------------
#define FIX_INTERVAL_MS            20000
#define GNSS_FIX_TIMEOUT_MS        180000   // cellular assistance should make this much shorter than v1's 5 min
#define NETWORK_ATTACH_TIMEOUT_MS  120000
#define HTTP_PROFILE_ID            1
#define TLS_PROFILE_ID             1
#define HTTP_RING_TIMEOUT_MS       15000

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

void onGnssEvent(WMGNSSEventType type, const WMGNSSEventData* data, void* args) {
  if (type == WALTER_MODEM_GNSS_EVENT_FIX) {
    memcpy(&latestFix, &data->gnssfix, sizeof(WMGNSSFixEvent));
    gnssFixReceived = true;
  }
}

// Pull the string value for "key":"value" out of a flat JSON response. Good
// enough for Google's Identity Toolkit / RTDB responses - avoids a full JSON library.
String extractJsonString(const String& json, const char* key) {
  String needle = String("\"") + key + "\":\"";
  int start = json.indexOf(needle);
  if (start < 0) return "";
  start += needle.length();
  int end = json.indexOf('"', start);
  if (end < 0) return "";
  return json.substring(start, end);
}

void connectCellular() {
  Serial.println("Modem: initializing");
  if (!modem.begin(&Serial2)) {
    Serial.println("Modem init failed - restarting");
    delay(3000);
    ESP.restart();
  }

  modem.setGNSSEventHandler(onGnssEvent, NULL);

  // Bring the radio up on LTE-M and open a data (PDP) context.
  modem.setOpState(WALTER_MODEM_OPSTATE_FULL);
  modem.setRAT(WALTER_MODEM_RAT_LTEM);
  modem.setNetworkSelectionMode(WALTER_MODEM_NETWORK_SEL_MODE_AUTOMATIC);
  modem.definePDPContext(1, CELLULAR_APN);

  Serial.print("Cellular: waiting for network attach");
  uint32_t start = millis();
  while (true) {
    WalterModemNetworkRegState state = modem.getNetworkRegState();
    if (state == WALTER_MODEM_NETWORK_REG_REGISTERED_HOME ||
        state == WALTER_MODEM_NETWORK_REG_REGISTERED_ROAMING) {
      break;
    }
    if (millis() - start > NETWORK_ATTACH_TIMEOUT_MS) {
      Serial.println("\nCellular: network attach timed out - restarting");
      delay(3000);
      ESP.restart();
    }
    Serial.print(".");
    delay(1000);
  }
  Serial.println("\nCellular: network attached");

  modem.pdpContextActivate();

  // Server cert validation off for v1 parity (see README known limitations -
  // tightening this is a follow-up, same as the WiFi build).
  modem.tlsConfigProfile(TLS_PROFILE_ID, WALTER_MODEM_TLS_VALIDATION_NONE,
                          WALTER_MODEM_TLS_VERSION_12);
}

void syncTimeFromModem() {
  Serial.print("Clock: waiting for network time");
  WalterModemRsp rsp = {};
  uint32_t start = millis();
  while (millis() - start < 30000) {
    if (modem.getClock(&rsp) && rsp.data.clock > 1700000000) break;
    Serial.print(".");
    delay(1000);
  }
  Serial.printf("\nClock: epoch = %ld\n", (long)rsp.data.clock);
  modem.gnssSetUTCTime((uint64_t)rsp.data.clock);
}

// One request/response cycle over the modem's own HTTP(S) profile (AT
// commands - no socket, no WiFi/lwIP stack involved). `host` lets us reuse
// this for both the RTDB and the Identity Toolkit auth endpoint.
bool modemHttpRequest(const char* host, const String& method, const String& uri,
                       const String& body, String& outResponse) {
  modem.httpConfigProfile(HTTP_PROFILE_ID, host, 443, TLS_PROFILE_ID);

  bool sent;
  if (method == "GET") {
    sent = modem.httpQuery(HTTP_PROFILE_ID, uri.c_str(), WALTER_MODEM_HTTP_QUERY_CMD_GET);
  } else if (method == "PUT") {
    sent = modem.httpSend(HTTP_PROFILE_ID, uri.c_str(), (uint8_t*)body.c_str(), body.length(),
                           WALTER_MODEM_HTTP_SEND_CMD_PUT, WALTER_MODEM_HTTP_POST_PARAM_JSON);
  } else { // POST
    sent = modem.httpSend(HTTP_PROFILE_ID, uri.c_str(), (uint8_t*)body.c_str(), body.length(),
                           WALTER_MODEM_HTTP_SEND_CMD_POST, WALTER_MODEM_HTTP_POST_PARAM_JSON);
  }
  if (!sent) { Serial.printf("HTTP %s %s: send failed\n", method.c_str(), uri.c_str()); return false; }

  uint8_t buf[512] = {};
  uint32_t start = millis();
  while (!modem.httpDidRing(HTTP_PROFILE_ID, buf, sizeof(buf))) {
    if (millis() - start > HTTP_RING_TIMEOUT_MS) {
      Serial.printf("HTTP %s %s: timed out waiting for response\n", method.c_str(), uri.c_str());
      return false;
    }
    delay(100);
  }
  outResponse = String((char*)buf);
  Serial.printf("HTTP %s %s -> %s\n", method.c_str(), uri.c_str(), outResponse.c_str());
  return true;
}

// Firebase RTDB request: same auth-token-in-query-string scheme as v1.
bool firebaseRequest(const String& method, const String& path, const String& body, String& outResponse) {
  String uri = "/" + path + ".json";
  if (idToken.length() > 0) uri += "?auth=" + idToken;
  return modemHttpRequest(FIREBASE_HOST, method, uri, body, outResponse);
}

// Sign in anonymously (Firebase Auth) so writes carry a real auth token.
// Requires the "Anonymous" sign-in provider enabled in the Firebase console
// and a Web API key in secrets.h. See firebase/database.rules.json.
bool firebaseSignIn() {
  String uri = String("/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
  String resp;
  if (!modemHttpRequest("identitytoolkit.googleapis.com", "POST", uri,
                         "{\"returnSecureToken\":true}", resp)) {
    return false;
  }
  idToken = extractJsonString(resp, "idToken");
  refreshToken = extractJsonString(resp, "refreshToken");
  String expiresIn = extractJsonString(resp, "expiresIn");
  unsigned long ttlMs = (expiresIn.length() ? (unsigned long)expiresIn.toInt() : 3600UL) * 1000UL;
  tokenExpiryMs = millis() + ttlMs - 60000UL;  // refresh a minute before it actually expires
  Serial.printf("Firebase auth sign-in: %s\n", idToken.length() ? "ok" : "failed");
  return idToken.length() > 0;
}

void ensureFreshToken() {
  if (idToken.length() == 0 || (long)(millis() - tokenExpiryMs) >= 0) {
    firebaseSignIn();  // simplest reliable path on a device that (re)boots fresh per run anyway
  }
}

// POST one GPS point to this run's session (Firebase assigns a chronological push id).
bool pushPoint(double lat, double lon, uint64_t tMs) {
  ensureFreshToken();
  char body[128];
  snprintf(body, sizeof(body), "{\"lat\":%.6f,\"lon\":%.6f,\"t\":%llu}", lat, lon, (unsigned long long)tMs);
  String resp;
  return firebaseRequest("POST", "sessions/" + sessionId + "/points", body, resp);
}

// Start a brand-new run: pick a session id, publish it as the live session,
// and stamp the session's start time - so each run gets its own history
// entry instead of overwriting the previous one.
void startSession() {
  sessionId = String((uint32_t)time(nullptr));
  ensureFreshToken();

  String resp;
  firebaseRequest("PUT", "currentSession", "\"" + sessionId + "\"", resp);

  char body[64];
  snprintf(body, sizeof(body), "{\"start\":%llu}", (unsigned long long)((uint64_t)time(nullptr) * 1000ULL));
  firebaseRequest("PUT", "sessions/" + sessionId + "/meta", body, resp);

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

  bool valid = latestFix.latitude != 0.0 && latestFix.longitude != 0.0 &&
               latestFix.satCount > 0;
  if (!valid) Serial.println("GNSS: not a usable lock yet - will retry");
  return valid;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Hermes Cellular v2 ===");

  connectCellular();
  syncTimeFromModem();

  if (!modem.gnssConfig()) Serial.println("Warning: gnssConfig() failed");

  firebaseSignIn();
  startSession();
  Serial.println("Setup complete - tracking run.\n");
}

void loop() {
  if (getFix()) {
    uint64_t tMs = (uint64_t)time(nullptr) * 1000ULL;
    pushPoint(latestFix.latitude, latestFix.longitude, tMs);
  }

  delay(FIX_INTERVAL_MS);
}
