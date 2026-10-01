/*
 * ESP32-S3 Wi-Fi Repeater (routed NAT range extender)
 * ----------------------------------------------------------------------------
 * Target  : ESP32-S3, Arduino IDE, ESP32 Arduino core 3.x (3.0.0 or newer)
 * Libraries: ONLY libraries bundled with the ESP32 Arduino core
 *            (WiFi, WebServer, DNSServer, Preferences). Nothing to install.
 *
 * HOW IT WORKS
 *   - The ESP32-S3 runs in WIFI_AP_STA mode: one radio, two virtual interfaces.
 *   - STA joins your existing router (upstream).
 *   - AP creates the repeater network with its own DHCP server and its own
 *     subnet (default 192.168.50.0/24).
 *   - lwIP NAPT (Network Address and Port Translation) is enabled on the AP
 *     interface with WiFi.AP.enableNAPT(true). Packets from AP clients are
 *     routed out through STA with the source address rewritten to the ESP32's
 *     STA address. This is a ROUTED repeater (double NAT), not a transparent
 *     Layer-2 bridge. See the limitations in the README/answer.
 *   - The AP's DHCP server hands out the upstream router's DNS server, so
 *     clients resolve names directly through the upstream network.
 *
 * MODES
 *   SETUP    : no valid saved configuration -> setup AP + captive portal.
 *   REPEATER : valid configuration -> repeater AP + NAT + dashboard (login).
 *
 * Serial log tags: [BOOT] [WIFI] [AP] [NAT] [WEB] [CLIENT] [ERROR]
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "ESP32 Arduino core 3.0.0 or newer is required (WiFi.AP.enableNAPT)."
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#warning "This sketch is written for ESP32-S3. Select an ESP32S3 board in Tools > Board."
#endif

// ============================================================================
// Settings (single place for every constant / default)
// ============================================================================
#define FW_VERSION          "1.1.0"
#define HOSTNAME            "esp32s3-repeater"

#define SETUP_AP_SSID       "ESP32-S3-Repeater-Setup"
#define SETUP_AP_PASS       "repeater-setup"   // WPA2, >= 8 chars. Only used during first-time setup.
#define ADMIN_USER          "admin"            // default login user (changeable in Admin > Account)
#define DEFAULT_BRAND       "ESP32-S3 Repeater"  // panel name (changeable in Admin > Appearance)
#define DEFAULT_ACCENT      "#2f9bff"          // panel accent color
#define MAX_SAVED           5                  // remembered upstream networks

#define RESET_BUTTON_PIN    0                  // BOOT button on most ESP32-S3 boards
#define RESET_HOLD_MS       10000UL            // hold this long (while running) = factory reset

#define MAX_AP_CLIENTS      8                  // ESP32 hard limit is 10
#define CONNECT_TIMEOUT_MS  25000UL
#define BACKOFF_MIN_MS      5000UL             // reconnect backoff: 5s,10s,20s,40s,60s,60s...
#define BACKOFF_MAX_MS      60000UL
#define AP_RETRY_MS         5000UL
#define STATUS_INTERVAL_MS  1000UL
#define LOG_INTERVAL_MS     30000UL
#define LOW_HEAP_WARN       20000UL            // bytes: show warning
#define LOW_HEAP_CRIT       8000UL             // bytes: controlled restart if sustained
#define MAX_SCAN_RESULTS    20

static const IPAddress SETUP_AP_IP(192, 168, 4, 1);          // setup AP address
static const IPAddress FALLBACK_DNS(1, 1, 1, 1);             // only until upstream DNS is known
// Repeater AP subnet candidates. The first one that does not collide with the
// upstream router's subnet is used (a NAT router needs two different subnets).
static const IPAddress AP_CANDIDATES[] = {
  IPAddress(192, 168, 50, 1), IPAddress(192, 168, 77, 1),
  IPAddress(10, 77, 77, 1),   IPAddress(172, 20, 77, 1)
};
static const size_t AP_CANDIDATE_COUNT = sizeof(AP_CANDIDATES) / sizeof(AP_CANDIDATES[0]);

// ============================================================================
// Logging
// ============================================================================
#define LOG(tag, fmt, ...)  Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)      Serial.printf("[ERROR] " fmt "\n", ##__VA_ARGS__)

// ============================================================================
// Types and global state
// ============================================================================
struct Config {
  char    staSsid[33];     // upstream SSID
  char    staPass[64];     // upstream password ("" = open network)
  char    apSsid[33];      // repeater SSID
  char    apPass[64];      // repeater password (WPA2, 8..63)
  char    adminUser[17];   // dashboard username (3..16: letters, digits . _ -)
  char    adminPass[33];   // dashboard password (8..32)
  char    brand[25];       // panel name shown in the UI (1..24)
  char    accent[8];       // panel accent color "#rrggbb"
  uint8_t apChannel;       // 0 = automatic (follow upstream), 1..11
  uint8_t theme;           // 0 = auto, 1 = dark, 2 = light
};

struct SavedNet { char ssid[33]; char pass[64]; };

enum StaState : uint8_t { STA_IDLE, STA_CONNECTING, STA_CONNECTED, STA_BACKOFF };

struct ScanEntry { char ssid[33]; int8_t rssi; uint8_t ch; uint8_t sec; };

struct Status {
  bool     staUp;
  int      rssi;
  uint8_t  channel;
  uint8_t  clients;
  uint32_t uptime;
  uint32_t heap;
  uint32_t minHeap;
  bool     memLow;
  char     staIp[16];
  char     apIp[16];
};

static Config      cfg;
static SavedNet    saved[MAX_SAVED];
static uint8_t     savedCount = 0;
static bool        cfgValid  = false;
static bool        setupMode = true;
static Preferences prefs;
static const char* NVS_NS = "repeater";

static WebServer   server(80);
static DNSServer   dnsServer;

static Status      status;
static IPAddress   apIp;
static IPAddress   apDns;
static bool        apOk        = false;
static uint32_t    apRetryAt   = 0;
static bool        natActive   = false;
static bool        natPending  = false;
static uint32_t    natRetryAt  = 0;

// Written from the Wi-Fi event task, read from loop(): keep them volatile.
static volatile StaState staState      = STA_IDLE;
static volatile bool     gotIpFlag     = false;
static volatile uint32_t nextAttemptAt = 0;
static const char* volatile lastError  = "";
static uint32_t staAttemptStart = 0;
static uint32_t backoffMs       = BACKOFF_MIN_MS;
static uint32_t reconnectCount  = 0;
static bool     attemptedOnce   = false;

static ScanEntry scanCache[MAX_SCAN_RESULTS];
static uint8_t   scanCount     = 0;
static bool      scanRunning   = false;
static uint32_t  scanStartedAt = 0;

static bool      restartPending = false;
static uint32_t  restartAt      = 0;
static uint32_t  lastStatusMs   = 0;
static uint32_t  lastLogMs      = 0;
static uint32_t  btnDownAt      = 0;
static uint32_t  critHeapSince  = 0;

static uint8_t   authFails = 0;
static uint32_t  authWindowStart = 0;
static uint32_t  authBlockUntil  = 0;

extern const char INDEX_HTML[];

// ============================================================================
// Small helpers
// ============================================================================
static void scheduleRestart(uint32_t inMs) {
  restartAt = millis() + inMs;
  restartPending = true;
}

static const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic/crash";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (check power supply)";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default:                return "other";
  }
}

static const char* currentApSsid() { return setupMode ? SETUP_AP_SSID : cfg.apSsid; }

static void fmtMac(const uint8_t* m, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Escape a string for safe embedding in a JSON string literal.
static void jsonEscape(const char* in, char* out, size_t outSize) {
  size_t o = 0;
  for (; *in && o + 7 < outSize; ++in) {
    uint8_t c = (uint8_t)*in;
    if (c == '"' || c == '\\')      { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20 || c == 0x7F) { o += snprintf(out + o, outSize - o, "\\u%04x", c); }
    else                            { out[o++] = (char)c; }
  }
  out[o] = 0;
}

static bool sameSubnet(const IPAddress& a, const IPAddress& b, const IPAddress& m) {
  for (int i = 0; i < 4; i++) if ((a[i] & m[i]) != (b[i] & m[i])) return false;
  return true;
}

// Map a Wi-Fi disconnect reason code to a human message.
// Numeric values come from esp_wifi_types.h (wifi_err_reason_t).
static const char* describeDisconnect(uint8_t r) {
  switch (r) {
    case 201:                 return "Router unavailable (SSID not found)";      // NO_AP_FOUND
    case 202: case 15: case 204:
                              return "Authentication failed (wrong password?)";  // AUTH_FAIL, 4WAY/HANDSHAKE timeout
    case 200:                 return "Upstream signal lost (beacon timeout)";    // BEACON_TIMEOUT
    case 203: case 205:       return "Association with router failed";           // ASSOC_FAIL, CONNECTION_FAIL
    case 2:                   return "Authentication expired";                   // AUTH_EXPIRE
    case 3: case 4: case 6: case 7:
                              return "Router disconnected the repeater";         // AUTH_LEAVE, ASSOC_EXPIRE...
    default:                  return "Upstream connection failed or lost";
  }
}

// ============================================================================
// Input validation
// ============================================================================
static bool validSsid(const String& s) {
  size_t n = s.length();
  if (n < 1 || n > 32) return false;
  for (size_t i = 0; i < n; i++) { uint8_t c = (uint8_t)s[i]; if (c < 0x20 || c == 0x7F) return false; }
  return true;
}
static bool validPrintable(const String& s, size_t minLen, size_t maxLen) {
  size_t n = s.length();
  if (n < minLen || n > maxLen) return false;
  for (size_t i = 0; i < n; i++) { uint8_t c = (uint8_t)s[i]; if (c < 0x20 || c > 0x7E) return false; }
  return true;
}
static bool validWpaPass(const String& s) { return validPrintable(s, 8, 63); }
static bool validAdminPass(const String& s) { return validPrintable(s, 8, 32); }
static bool validUserChar(uint8_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '-';
}
static bool validUser(const char* s) {
  size_t n = strlen(s);
  if (n < 3 || n > 16) return false;
  for (size_t i = 0; i < n; i++) if (!validUserChar((uint8_t)s[i])) return false;
  return true;
}
static bool validUser(const String& s) { return validUser(s.c_str()); }
static bool validAccent(const char* s) {
  if (strlen(s) != 7 || s[0] != '#') return false;
  for (int i = 1; i < 7; i++) { char c = s[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false; }
  return true;
}
static void applyUiDefaults(Config& c) {
  if (!c.adminUser[0])        strlcpy(c.adminUser, ADMIN_USER, sizeof(c.adminUser));
  if (!c.brand[0])            strlcpy(c.brand, DEFAULT_BRAND, sizeof(c.brand));
  if (!validAccent(c.accent)) strlcpy(c.accent, DEFAULT_ACCENT, sizeof(c.accent));
  if (c.theme > 2)            c.theme = 0;
}
// Constant-time compare so password checks do not leak length/prefix timing.
static bool ctEq(const char* a, const char* b) {
  size_t la = strlen(a), lb = strlen(b), n = la < lb ? la : lb;
  uint8_t d = (la != lb) ? 1 : 0;
  for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

// Returns nullptr when the configuration is complete and valid.
static const char* configError(const Config& c) {
  size_t a = strlen(c.staSsid), b = strlen(c.apSsid), sp = strlen(c.staPass), ap = strlen(c.apPass), ad = strlen(c.adminPass);
  if (a < 1 || a > 32)                     return "Invalid upstream SSID";
  if (sp != 0 && (sp < 8 || sp > 63))      return "Invalid upstream password";
  if (b < 1 || b > 32)                     return "Invalid repeater SSID";
  if (ap < 8 || ap > 63)                   return "Invalid repeater password";
  if (ad < 8 || ad > 32)                   return "Invalid admin password";
  if (!validUser(c.adminUser))             return "Invalid admin username";
  size_t bl = strlen(c.brand);
  if (bl < 1 || bl > 24)                   return "Invalid panel name";
  if (!validAccent(c.accent))              return "Invalid accent color";
  if (c.theme > 2)                         return "Invalid theme";
  if (c.apChannel > 11)                    return "Invalid channel";
  if (strcmp(c.staSsid, c.apSsid) == 0)    return "Repeater SSID must differ from the upstream SSID";
  return nullptr;
}

// ============================================================================
// Configuration storage (Preferences / NVS)
// ============================================================================
// ---- Saved upstream networks (newest first). Passwords never leave the device. ----
static const SavedNet* findSaved(const char* ssid) {
  for (uint8_t i = 0; i < savedCount; i++) if (strcmp(saved[i].ssid, ssid) == 0) return &saved[i];
  return nullptr;
}

static void saveSaved() {
  if (!prefs.begin(NVS_NS, false)) { LOGE("NVS open failed while saving networks"); return; }
  prefs.putUChar("sv_n", savedCount);
  char k1[8], k2[8];
  for (uint8_t i = 0; i < MAX_SAVED; i++) {
    snprintf(k1, sizeof(k1), "sv%us", (unsigned)i);
    snprintf(k2, sizeof(k2), "sv%up", (unsigned)i);
    if (i < savedCount) { prefs.putString(k1, saved[i].ssid); prefs.putString(k2, saved[i].pass); }
    else                { prefs.remove(k1); prefs.remove(k2); }
  }
  prefs.end();
}

static void rememberNetwork(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  int at = -1;
  for (uint8_t i = 0; i < savedCount; i++) if (strcmp(saved[i].ssid, ssid) == 0) { at = i; break; }
  if (at < 0) { if (savedCount < MAX_SAVED) at = savedCount++; else at = MAX_SAVED - 1; }
  SavedNet tmp;
  memset(&tmp, 0, sizeof(tmp));
  strlcpy(tmp.ssid, ssid, sizeof(tmp.ssid));
  strlcpy(tmp.pass, pass ? pass : "", sizeof(tmp.pass));
  for (int i = at; i > 0; i--) saved[i] = saved[i - 1];
  saved[0] = tmp;
  saveSaved();
}

void loadConfig() {
  memset(&cfg, 0, sizeof(cfg));
  if (!prefs.begin(NVS_NS, false)) {
    LOGE("NVS open failed, starting in setup mode");
    lastError = "Storage error (NVS)";
    cfgValid = false; setupMode = true;
    applyUiDefaults(cfg);
    return;
  }
  prefs.getString("sta_ssid", cfg.staSsid,  sizeof(cfg.staSsid));
  prefs.getString("sta_pass", cfg.staPass,  sizeof(cfg.staPass));
  prefs.getString("ap_ssid",  cfg.apSsid,   sizeof(cfg.apSsid));
  prefs.getString("ap_pass",  cfg.apPass,   sizeof(cfg.apPass));
  prefs.getString("adm_pass", cfg.adminPass, sizeof(cfg.adminPass));
  cfg.apChannel = prefs.getUChar("ap_ch", 0);
  prefs.getString("adm_user", cfg.adminUser, sizeof(cfg.adminUser));
  prefs.getString("brand",    cfg.brand,     sizeof(cfg.brand));
  prefs.getString("accent",   cfg.accent,    sizeof(cfg.accent));
  cfg.theme = prefs.getUChar("theme", 0);
  savedCount = 0;
  uint8_t nSaved = prefs.getUChar("sv_n", 0);
  for (uint8_t i = 0; i < nSaved && i < MAX_SAVED; i++) {
    char k1[8], k2[8];
    snprintf(k1, sizeof(k1), "sv%us", (unsigned)i);
    snprintf(k2, sizeof(k2), "sv%up", (unsigned)i);
    SavedNet& s = saved[savedCount];
    memset(&s, 0, sizeof(s));
    prefs.getString(k1, s.ssid, sizeof(s.ssid));
    prefs.getString(k2, s.pass, sizeof(s.pass));
    if (s.ssid[0]) savedCount++;
  }
  prefs.end();
  applyUiDefaults(cfg);                   // older firmware stored none of the new fields

  bool blank = !cfg.staSsid[0] && !cfg.apSsid[0] && !cfg.adminPass[0];
  const char* err = configError(cfg);
  cfgValid  = (err == nullptr);
  setupMode = !cfgValid;
  if (!cfgValid && !blank) {
    lastError = "Invalid stored configuration - please run setup again";
    LOGE("Stored configuration invalid: %s", err);
  }
  LOG("BOOT", "Configuration %s", cfgValid ? "loaded" : (blank ? "not found (first boot)" : "invalid"));
  if (cfgValid && !findSaved(cfg.staSsid)) rememberNetwork(cfg.staSsid, cfg.staPass);   // seed list after an upgrade
}

bool saveConfig(const Config& c) {
  if (!prefs.begin(NVS_NS, false)) { LOGE("NVS open failed while saving"); return false; }
  bool ok = true;
  ok &= prefs.putString("sta_ssid", c.staSsid)   > 0;
  prefs.putString("sta_pass", c.staPass);                 // may legitimately be empty (open network)
  ok &= prefs.putString("ap_ssid",  c.apSsid)    > 0;
  ok &= prefs.putString("ap_pass",  c.apPass)    > 0;
  ok &= prefs.putString("adm_pass", c.adminPass) > 0;
  ok &= prefs.putUChar("ap_ch",     c.apChannel) > 0;
  ok &= prefs.putString("adm_user", c.adminUser) > 0;
  ok &= prefs.putString("brand",    c.brand)     > 0;
  ok &= prefs.putString("accent",   c.accent)    > 0;
  ok &= prefs.putUChar("theme",     c.theme)     > 0;
  prefs.end();
  if (!ok) LOGE("Failed to write configuration to NVS");
  return ok;
}

static void factoryReset() {
  if (prefs.begin(NVS_NS, false)) { prefs.clear(); prefs.end(); }
  LOG("BOOT", "Configuration erased (factory reset)");
}

// ============================================================================
// Wi-Fi events (run in the system event task: keep them short, set flags only)
// ============================================================================
static void onStaDisconnected(uint8_t reason) {
  if (reason == 8 && staState != STA_CONNECTED) return;     // ASSOC_LEAVE caused by our own disconnect()
  StaState s = staState;
  if (s == STA_CONNECTED) {
    lastError = describeDisconnect(reason);
    LOG("WIFI", "Upstream connection LOST (reason %u: %s)", reason, lastError);
    backoffMs = BACKOFF_MIN_MS;
    nextAttemptAt = millis() + backoffMs;
    staState = STA_BACKOFF;
  } else if (s == STA_CONNECTING) {
    lastError = describeDisconnect(reason);
    LOGE("Upstream connect failed (reason %u: %s)", reason, lastError);
    nextAttemptAt = millis() + backoffMs;
    LOG("WIFI", "Next attempt in %lu s", (unsigned long)(backoffMs / 1000));
    backoffMs = min<uint32_t>(backoffMs * 2, BACKOFF_MAX_MS);
    staState = STA_BACKOFF;
  }
  // STA_IDLE / STA_BACKOFF: ignore (setup mode, or duplicate event)
}

void handleWiFiEvents(arduino_event_id_t event, arduino_event_info_t info) {
  char mac[18];
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      LOG("WIFI", "Station interface started");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      LOG("WIFI", "Associated with upstream router (channel %u)", info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staState = STA_CONNECTED;
      backoffMs = BACKOFF_MIN_MS;
      lastError = "";
      gotIpFlag = true;                       // NAT is configured from loop()
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      LOG("WIFI", "Lost upstream IP address");
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      onStaDisconnected(info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      LOG("AP", "Access point interface started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      LOG("AP", "Access point interface stopped");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      fmtMac(info.wifi_ap_staconnected.mac, mac);
      LOG("CLIENT", "Connected    %s (clients now: %u)", mac, (unsigned)WiFi.softAPgetStationNum());
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      fmtMac(info.wifi_ap_stadisconnected.mac, mac);
      LOG("CLIENT", "Disconnected %s (clients now: %u)", mac, (unsigned)WiFi.softAPgetStationNum());
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      LOG("CLIENT", "IP assigned  %s", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
      break;
    default:
      break;
  }
}

// ============================================================================
// Access point
// ============================================================================
bool startAccessPoint() {
  const char* ssid = currentApSsid();
  const char* pass = setupMode ? SETUP_AP_PASS : cfg.apPass;
  // While STA is connected the radio is locked to the router's channel anyway,
  // so this value only matters while the upstream is not connected.
  int channel = setupMode ? 1 : (cfg.apChannel ? cfg.apChannel : 1);

  apIp  = setupMode ? SETUP_AP_IP : AP_CANDIDATES[0];
  apDns = setupMode ? apIp : FALLBACK_DNS;     // setup: DNS = ourselves (captive portal)
  IPAddress mask(255, 255, 255, 0);
  IPAddress leaseStart = apIp;
  leaseStart[3] = apIp[3] + 1;

  if (!WiFi.softAPConfig(apIp, apIp, mask, leaseStart, apDns)) {
    LOGE("AP network configuration failed");
    lastError = "Access point failed to start (network config)";
    apOk = false; apRetryAt = millis() + AP_RETRY_MS;
    return false;
  }
  if (!WiFi.softAP(ssid, pass, channel, 0, MAX_AP_CLIENTS)) {
    LOGE("softAP() failed for SSID \"%s\"", ssid);
    lastError = "Access point failed to start";
    apOk = false; apRetryAt = millis() + AP_RETRY_MS;
    return false;
  }
  apOk = true;
  LOG("AP", "%s AP \"%s\" up, IP %s, channel %d", setupMode ? "Setup" : "Repeater", ssid,
      WiFi.softAPIP().toString().c_str(), channel);

  if (setupMode) {
    // Captive portal DNS: answer every name with our own address.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());
    LOG("AP", "Setup AP password: %s  |  open http://%s/", SETUP_AP_PASS, WiFi.softAPIP().toString().c_str());
  }
  return true;
}

// ============================================================================
// Upstream Wi-Fi (non-blocking state machine)
// ============================================================================
void connectToUpstreamWiFi() {
  if (setupMode || !cfg.staSsid[0]) return;
  if (attemptedOnce) reconnectCount++;
  attemptedOnce = true;
  LOG("WIFI", "Connecting to \"%s\" (attempt %lu)...", cfg.staSsid, (unsigned long)(reconnectCount + 1));
  staAttemptStart = millis();
  staState = STA_CONNECTING;
  WiFi.begin(cfg.staSsid, cfg.staPass[0] ? cfg.staPass : nullptr);
}

// Called every loop(): drives connect / timeout / backoff without delay().
void checkWiFiConnection() {
  if (setupMode) return;
  uint32_t now = millis();
  switch (staState) {
    case STA_IDLE:
      connectToUpstreamWiFi();
      break;
    case STA_CONNECTING:
      if (now - staAttemptStart >= CONNECT_TIMEOUT_MS) {
        lastError = "Connection timeout";
        LOGE("Upstream connection timeout");
        staState = STA_BACKOFF;                 // set first so the resulting disconnect event is ignored
        nextAttemptAt = now + backoffMs;
        backoffMs = min<uint32_t>(backoffMs * 2, BACKOFF_MAX_MS);
        WiFi.disconnect(false, false);          // abort the attempt, keep the radio on
      }
      break;
    case STA_BACKOFF:
      if ((int32_t)(now - nextAttemptAt) >= 0) connectToUpstreamWiFi();
      break;
    case STA_CONNECTED:
      break;
  }
}

// ============================================================================
// NAT / routing
// ============================================================================
// Re-applies the AP address + DHCP DNS (this restarts the AP's DHCP server).
static bool applyApNetwork() {
  WiFi.AP.enableNAPT(false);                    // detach NAT before changing addressing
  natActive = false;
  IPAddress mask(255, 255, 255, 0);
  IPAddress leaseStart = apIp;
  leaseStart[3] = apIp[3] + 1;
  return WiFi.softAPConfig(apIp, apIp, mask, leaseStart, apDns);
}

// Runs after the STA got an IP: pick a non-colliding AP subnet, hand the
// upstream DNS to AP clients, and switch lwIP NAPT on for the AP interface.
static bool configureNat() {
  if (!WiFi.AP.started()) return false;         // AP not ready yet: retry shortly

  IPAddress staIp   = WiFi.localIP();
  IPAddress staMask = WiFi.subnetMask();
  IPAddress upDns   = WiFi.dnsIP(0);
  if (upDns == IPAddress((uint32_t)0)) upDns = FALLBACK_DNS;
  const IPAddress m24(255, 255, 255, 0);

  bool changed = false;
  // 1) The two subnets of a NAT router must differ.
  if (sameSubnet(staIp, apIp, m24) || sameSubnet(staIp, apIp, staMask)) {
    for (size_t i = 0; i < AP_CANDIDATE_COUNT; i++) {
      const IPAddress& cand = AP_CANDIDATES[i];
      if (!sameSubnet(staIp, cand, m24) && !sameSubnet(staIp, cand, staMask)) { apIp = cand; changed = true; break; }
    }
    LOG("NAT", "Upstream subnet collides with AP subnet, AP moved to %s", apIp.toString().c_str());
  }
  // 2) Clients should use the upstream DNS server.
  if (apDns != upDns) { apDns = upDns; changed = true; }

  if (changed && !applyApNetwork()) {
    LOGE("Could not re-apply AP network settings");
    lastError = "NAT setup failed (AP network)";
    return false;
  }
  // 3) Enable NAPT on the AP interface (requires an upstream default route = STA).
  if (!WiFi.AP.enableNAPT(true)) {
    LOGE("WiFi.AP.enableNAPT(true) failed - NAPT not available in this core build?");
    lastError = "NAT could not be enabled";
    return false;
  }
  natActive = true;
  LOG("NAT", "NAPT enabled: AP %s/24  ->  STA %s via gateway %s, client DNS %s",
      apIp.toString().c_str(), staIp.toString().c_str(),
      WiFi.gatewayIP().toString().c_str(), apDns.toString().c_str());
  return true;
}

static void serviceNat() {
  if (setupMode) return;
  if (gotIpFlag) {
    gotIpFlag = false;
    LOG("WIFI", "Connected to \"%s\"  IP %s  RSSI %d dBm  channel %u", cfg.staSsid,
        WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned)WiFi.channel());
    natPending = true;
    natRetryAt = millis();
  }
  if (natPending && (int32_t)(millis() - natRetryAt) >= 0) {
    if (configureNat()) natPending = false;
    else natRetryAt = millis() + 2000;
  }
}

// ============================================================================
// Scan (asynchronous, results cached in a fixed array: no heap churn)
// ============================================================================
static void startScan() {
  if (scanRunning) return;
  if (scanStartedAt && millis() - scanStartedAt < 4000) return;     // rate limit
  int r = WiFi.scanNetworks(true);
  scanStartedAt = millis();
  if (r == WIFI_SCAN_FAILED) { LOGE("Wi-Fi scan could not be started"); return; }
  scanRunning = true;
  LOG("WIFI", "Scan started");
}

static void pollScan() {
  if (!scanRunning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartedAt > 15000) { scanRunning = false; WiFi.scanDelete(); LOGE("Wi-Fi scan timed out"); }
    return;
  }
  scanRunning = false;
  scanCount = 0;
  if (n < 0) { LOGE("Wi-Fi scan failed"); return; }
  for (int i = 0; i < n && scanCount < MAX_SCAN_RESULTS; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0 || s.length() > 32) continue;               // skip hidden / odd entries
    bool dup = false;                                               // list is RSSI-sorted: keep strongest
    for (uint8_t k = 0; k < scanCount; k++) if (strcmp(scanCache[k].ssid, s.c_str()) == 0) { dup = true; break; }
    if (dup) continue;
    ScanEntry& e = scanCache[scanCount++];
    strlcpy(e.ssid, s.c_str(), sizeof(e.ssid));
    e.rssi = (int8_t)WiFi.RSSI(i);
    e.ch   = (uint8_t)WiFi.channel(i);
    e.sec  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
  }
  WiFi.scanDelete();
  LOG("WIFI", "Scan finished: %u networks", (unsigned)scanCount);
}

// ============================================================================
// Status snapshot
// ============================================================================
void updateStatus() {
  status.uptime  = (uint32_t)(esp_timer_get_time() / 1000000LL);
  status.heap    = ESP.getFreeHeap();
  status.minHeap = ESP.getMinFreeHeap();
  status.clients = WiFi.softAPgetStationNum();
  status.channel = (uint8_t)WiFi.channel();
  status.staUp   = (staState == STA_CONNECTED);
  status.rssi    = status.staUp ? WiFi.RSSI() : 0;
  strlcpy(status.staIp, status.staUp ? WiFi.localIP().toString().c_str() : "-", sizeof(status.staIp));
  strlcpy(status.apIp, WiFi.softAPIP().toString().c_str(), sizeof(status.apIp));

  status.memLow = status.heap < LOW_HEAP_WARN;
  uint32_t now = millis();
  if (status.heap < LOW_HEAP_CRIT) {
    if (!critHeapSince) critHeapSince = now ? now : 1;
    else if (now - critHeapSince > 10000UL && status.uptime > 60 && !restartPending) {
      LOGE("Free heap critically low (%lu bytes) - restarting", (unsigned long)status.heap);
      scheduleRestart(500);
    }
  } else critHeapSince = 0;
}

static const char* describeState(const char** level) {
  if (setupMode)                                  { *level = "warn"; return "Setup mode - waiting for configuration"; }
  if (!apOk)                                      { *level = "err";  return "Access point failed to start"; }
  switch (staState) {
    case STA_CONNECTED:  if (natActive) { *level = "ok"; return "Repeater active (NAT on)"; }
                         *level = "warn"; return "Connected - enabling NAT";
    case STA_CONNECTING: *level = "warn"; return "Connecting to upstream...";
    case STA_BACKOFF:    *level = "err";  return "Upstream unavailable - reconnecting";
    default:             *level = "warn"; return "Starting...";
  }
}

// ============================================================================
// Web server helpers
// ============================================================================
static void sendSecurityHeaders() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Content-Security-Policy",
    "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:");
}
static void sendJson(int code, const char* body) {
  sendSecurityHeaders();
  server.send(code, "application/json", body);
}
static void sendJsonError(int code, const char* msg) {   // msg must not contain quotes
  char b[200];
  snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", msg);
  sendJson(code, b);
}

// HTTP Basic auth (user "admin") in repeater mode, with simple brute-force throttling.
static bool requireAuth() {
  if (setupMode) return true;                                   // setup AP is WPA2-protected
  uint32_t now = millis();
  if (authBlockUntil && (int32_t)(now - authBlockUntil) < 0) {
    sendJsonError(429, "Too many attempts. Try again in a minute.");
    return false;
  }
  if (server.authenticate(cfg.adminUser, cfg.adminPass)) { authFails = 0; return true; }
  if (now - authWindowStart > 30000UL) { authWindowStart = now; authFails = 0; }
  if (++authFails > 12) {
    authBlockUntil = now + 60000UL; if (!authBlockUntil) authBlockUntil = 1;
    LOGE("Too many failed logins - web login blocked for 60 s");
  }
  server.requestAuthentication(BASIC_AUTH, "ESP32-S3 Repeater");
  return false;
}

// State-changing requests must carry a custom header. A cross-site HTML form
// cannot set it, which blocks CSRF against a browser that is already logged in.
static bool requireXhr() {
  if (server.header("X-Requested-With") != "repeater-ui") {
    sendJsonError(403, "Forbidden");
    return false;
  }
  return true;
}

// ============================================================================
// Web handlers
// ============================================================================
void handleDashboard() {
  if (!requireAuth()) return;
  sendSecurityHeaders();
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  if (!requireAuth()) return;
  const char* level = "warn";
  const char* state = describeState(&level);
  char s1[200], s2[200], er[120];
  jsonEscape(setupMode ? "" : cfg.staSsid, s1, sizeof(s1));
  jsonEscape(currentApSsid(), s2, sizeof(s2));
  jsonEscape(lastError, er, sizeof(er));
  char bn[64];
  jsonEscape(cfg.brand, bn, sizeof(bn));
  char buf[1400];
  snprintf(buf, sizeof(buf),
    "{\"mode\":\"%s\",\"state\":\"%s\",\"level\":\"%s\",\"nat\":%s,\"staUp\":%s,"
    "\"staSsid\":\"%s\",\"staIp\":\"%s\",\"apSsid\":\"%s\",\"apIp\":\"%s\","
    "\"rssi\":%d,\"ch\":%u,\"cfgCh\":%u,\"clients\":%u,\"uptime\":%lu,"
    "\"heap\":%lu,\"minHeap\":%lu,\"reconn\":%lu,\"err\":\"%s\",\"memLow\":%s,"
    "\"brand\":\"%s\",\"accent\":\"%s\",\"theme\":%u,\"user\":\"%s\"}",
    setupMode ? "setup" : "repeater", state, level, natActive ? "true" : "false", status.staUp ? "true" : "false",
    s1, status.staIp, s2, status.apIp,
    status.rssi, (unsigned)status.channel, (unsigned)cfg.apChannel, (unsigned)status.clients, (unsigned long)status.uptime,
    (unsigned long)status.heap, (unsigned long)status.minHeap, (unsigned long)reconnectCount, er,
    status.memLow ? "true" : "false", bn, cfg.accent, (unsigned)cfg.theme, cfg.adminUser);
  sendJson(200, buf);
}

void handleSystemInfo() {
  if (!requireAuth()) return;
  char buf[640];
  snprintf(buf, sizeof(buf),
    "{\"fw\":\"%s\",\"chip\":\"%s rev %u\",\"cores\":%u,\"mhz\":%u,\"flashKB\":%lu,\"psramKB\":%lu,"
    "\"sketchKB\":%lu,\"sdk\":\"%s\",\"core\":\"%d.%d.%d\",\"staMac\":\"%s\",\"apMac\":\"%s\","
    "\"reset\":\"%s\",\"minHeap\":%lu}",
    FW_VERSION, ESP.getChipModel(), (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(),
    (unsigned)ESP.getCpuFreqMHz(), (unsigned long)(ESP.getFlashChipSize() / 1024),
    (unsigned long)(ESP.getPsramSize() / 1024), (unsigned long)(ESP.getSketchSize() / 1024),
    ESP.getSdkVersion(), ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
    WiFi.macAddress().c_str(), WiFi.softAPmacAddress().c_str(), resetReasonText(),
    (unsigned long)ESP.getMinFreeHeap());
  sendJson(200, buf);
}

// GET /api/scan[?start=1]  -> {"scanning":bool,"networks":[{ssid,rssi,ch,sec}...]}
void handleScan() {
  if (!requireAuth()) return;
  if (server.hasArg("start")) startScan();
  sendSecurityHeaders();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);               // chunked: no big String needed
  server.send(200, "application/json", "");
  server.sendContent(scanRunning ? "{\"scanning\":true,\"networks\":[" : "{\"scanning\":false,\"networks\":[");
  char esc[200], line[300];
  for (uint8_t i = 0; i < scanCount; i++) {
    jsonEscape(scanCache[i].ssid, esc, sizeof(esc));
    snprintf(line, sizeof(line), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%u,\"sec\":%u}",
             i ? "," : "", esc, (int)scanCache[i].rssi, (unsigned)scanCache[i].ch, (unsigned)scanCache[i].sec);
    server.sendContent(line);
  }
  server.sendContent("]}");
  server.sendContent("");                                        // end of chunked body
}

// ---- form field parsers: return nullptr on success, else an error message ----
static const char* readUpstream(Config& c, bool keepIfBlank) {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  bool open   = (server.arg("open") == "1");
  if (!validSsid(ssid)) return "Invalid upstream SSID (1-32 characters)";
  const SavedNet* sn = findSaved(ssid.c_str());
  if (open) {
    c.staPass[0] = 0;
  } else if (pass.length() == 0 && keepIfBlank && strcmp(ssid.c_str(), c.staSsid) == 0 && c.staPass[0]) {
    // unchanged SSID + blank password = keep the stored password
  } else if (pass.length() == 0 && keepIfBlank && sn && sn->pass[0]) {
    strlcpy(c.staPass, sn->pass, sizeof(c.staPass));            // blank password + saved network = reuse it
  } else if (!validWpaPass(pass)) {
    return "Upstream password must be 8-63 printable characters";
  } else {
    strlcpy(c.staPass, pass.c_str(), sizeof(c.staPass));
  }
  strlcpy(c.staSsid, ssid.c_str(), sizeof(c.staSsid));
  return nullptr;
}

static const char* readRepeater(Config& c, bool keepIfBlank) {
  String ssid = server.arg("apssid");
  String pass = server.arg("appass");
  String adm  = server.arg("adminpass");
  String ch   = server.arg("channel");
  if (!validSsid(ssid)) return "Invalid repeater SSID (1-32 characters)";
  if (!(pass.length() == 0 && keepIfBlank)) {
    if (!validWpaPass(pass)) return "Repeater password must be 8-63 printable characters";
    strlcpy(c.apPass, pass.c_str(), sizeof(c.apPass));
  }
  if (!(adm.length() == 0 && keepIfBlank)) {
    if (!validAdminPass(adm)) return "Admin password must be 8-32 printable characters";
    strlcpy(c.adminPass, adm.c_str(), sizeof(c.adminPass));
  }
  if (ch.length()) {
    if (ch.length() > 2) return "Invalid channel";
    for (size_t i = 0; i < ch.length(); i++) if (ch[i] < '0' || ch[i] > '9') return "Invalid channel";
    int v = ch.toInt();
    if (v < 0 || v > 11) return "Channel must be Auto or 1-11";
    c.apChannel = (uint8_t)v;
  }
  String usr = server.arg("adminuser");
  if (usr.length()) {
    if (!validUser(usr)) return "Username must be 3-16 characters: letters, digits, dot, dash or underscore";
    strlcpy(c.adminUser, usr.c_str(), sizeof(c.adminUser));
  }
  strlcpy(c.apSsid, ssid.c_str(), sizeof(c.apSsid));
  return nullptr;
}

static void commitConfig(const Config& c) {
  const char* err = configError(c);
  if (err) { sendJsonError(400, err); return; }
  if (!saveConfig(c)) { sendJsonError(500, "Could not save configuration (storage error)"); return; }
  if (c.staSsid[0]) rememberNetwork(c.staSsid, c.staPass);
  LOG("WEB", "Configuration saved, restarting");
  sendJson(200, "{\"ok\":true,\"msg\":\"Saved. The ESP32 is restarting...\"}");
  scheduleRestart(1500);
}

// POST /api/wifi : change upstream network (dashboard, repeater mode)
void handleWiFiSettings() {
  if (!requireAuth() || !requireXhr()) return;
  Config c = cfg;
  const char* err = readUpstream(c, true);
  if (err) { sendJsonError(400, err); return; }
  commitConfig(c);
}

// POST /api/ap : change repeater SSID/password/channel/admin password
void handleApSettings() {
  if (!requireAuth() || !requireXhr()) return;
  Config c = cfg;
  const char* err = readRepeater(c, true);
  if (err) { sendJsonError(400, err); return; }
  commitConfig(c);
}

// POST /api/setup : first-time configuration (setup mode only)
void handleSetup() {
  if (!setupMode) { sendJsonError(403, "Setup is already complete"); return; }
  if (!requireXhr()) return;
  Config c;
  memset(&c, 0, sizeof(c));
  applyUiDefaults(c);
  const char* err = readUpstream(c, false);
  if (!err) err = readRepeater(c, false);
  if (err) { sendJsonError(400, err); return; }
  commitConfig(c);
}

void handleRestart() {
  if (!requireAuth() || !requireXhr()) return;
  LOG("WEB", "Restart requested");
  sendJson(200, "{\"ok\":true,\"msg\":\"Restarting...\"}");
  scheduleRestart(1000);
}

void handleFactoryReset() {
  if (!requireAuth() || !requireXhr()) return;
  LOG("WEB", "Factory reset requested");
  factoryReset();
  sendJson(200, "{\"ok\":true,\"msg\":\"Configuration erased. The ESP32 is restarting into setup mode...\"}");
  scheduleRestart(1000);
}

// ---- Saved networks ----
// GET /api/saved -> {"saved":[{"i":0,"ssid":"..","sec":1,"cur":true}...]}   (no passwords)
void handleSavedList() {
  if (!requireAuth()) return;
  sendSecurityHeaders();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"saved\":[");
  char esc[200], line[300];
  for (uint8_t i = 0; i < savedCount; i++) {
    jsonEscape(saved[i].ssid, esc, sizeof(esc));
    bool cur = !setupMode && strcmp(saved[i].ssid, cfg.staSsid) == 0;
    snprintf(line, sizeof(line), "%s{\"i\":%u,\"ssid\":\"%s\",\"sec\":%u,\"cur\":%s}",
             i ? "," : "", (unsigned)i, esc, saved[i].pass[0] ? 1u : 0u, cur ? "true" : "false");
    server.sendContent(line);
  }
  server.sendContent("]}");
  server.sendContent("");
}

static int savedIndexArg() {
  String v = server.arg("i");
  if (v.length() != 1 || v[0] < '0' || v[0] > '9') return -1;
  int i = v[0] - '0';
  return (i < savedCount) ? i : -1;
}

// POST /api/saved/use : switch the upstream to a saved network (restarts)
void handleSavedUse() {
  if (!requireAuth() || !requireXhr()) return;
  int i = savedIndexArg();
  if (i < 0) { sendJsonError(400, "Unknown saved network"); return; }
  Config c = cfg;
  strlcpy(c.staSsid, saved[i].ssid, sizeof(c.staSsid));
  strlcpy(c.staPass, saved[i].pass, sizeof(c.staPass));
  commitConfig(c);
}

// POST /api/saved/del : forget a saved network (not the one in use)
void handleSavedDel() {
  if (!requireAuth() || !requireXhr()) return;
  int i = savedIndexArg();
  if (i < 0) { sendJsonError(400, "Unknown saved network"); return; }
  if (strcmp(saved[i].ssid, cfg.staSsid) == 0) { sendJsonError(400, "This network is in use. Connect to another one first."); return; }
  for (int k = i; k < (int)savedCount - 1; k++) saved[k] = saved[k + 1];
  savedCount--;
  memset(&saved[savedCount], 0, sizeof(SavedNet));
  saveSaved();
  sendJson(200, "{\"ok\":true,\"msg\":\"Network forgotten\"}");
}

// POST /api/account : change admin username / password (needs the current password; no restart)
void handleAccount() {
  if (!requireAuth() || !requireXhr()) return;
  String cur = server.arg("curpass"), usr = server.arg("user"), np = server.arg("newpass");
  if (!ctEq(cur.c_str(), cfg.adminPass)) { sendJsonError(403, "Current password is wrong"); return; }
  Config c = cfg;
  if (usr.length()) {
    if (!validUser(usr)) { sendJsonError(400, "Username must be 3-16 characters: letters, digits, dot, dash or underscore"); return; }
    strlcpy(c.adminUser, usr.c_str(), sizeof(c.adminUser));
  }
  if (np.length()) {
    if (!validAdminPass(np)) { sendJsonError(400, "New password must be 8-32 printable characters"); return; }
    strlcpy(c.adminPass, np.c_str(), sizeof(c.adminPass));
  }
  const char* err = configError(c);
  if (err) { sendJsonError(400, err); return; }
  if (!saveConfig(c)) { sendJsonError(500, "Could not save configuration (storage error)"); return; }
  cfg = c;
  LOG("WEB", "Admin account updated");
  sendJson(200, "{\"ok\":true,\"msg\":\"Account updated. Sign in again with the new details.\"}");
}

// POST /api/brand : panel name, accent color, theme (or reset=1). No restart.
void handleBrand() {
  if (!requireAuth() || !requireXhr()) return;
  Config c = cfg;
  if (server.arg("reset") == "1") {
    strlcpy(c.brand, DEFAULT_BRAND, sizeof(c.brand));
    strlcpy(c.accent, DEFAULT_ACCENT, sizeof(c.accent));
    c.theme = 0;
  } else {
    String b = server.arg("brand"), a = server.arg("accent"), t = server.arg("theme");
    if (!validPrintable(b, 1, 24))   { sendJsonError(400, "Panel name must be 1-24 printable characters"); return; }
    if (!validAccent(a.c_str()))     { sendJsonError(400, "Accent must be a color like #2f9bff"); return; }
    if (t.length() != 1 || t[0] < '0' || t[0] > '2') { sendJsonError(400, "Invalid theme"); return; }
    strlcpy(c.brand, b.c_str(), sizeof(c.brand));
    strlcpy(c.accent, a.c_str(), sizeof(c.accent));
    c.theme = (uint8_t)(t[0] - '0');
  }
  const char* err = configError(c);
  if (err) { sendJsonError(400, err); return; }
  if (!saveConfig(c)) { sendJsonError(500, "Could not save configuration (storage error)"); return; }
  cfg = c;
  char bn[64], buf[200];
  jsonEscape(cfg.brand, bn, sizeof(bn));
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"msg\":\"Saved\",\"brand\":\"%s\",\"accent\":\"%s\",\"theme\":%u}",
           bn, cfg.accent, (unsigned)cfg.theme);
  sendJson(200, buf);
}

// Captive-portal probes (Android, iOS/macOS, Windows, Kindle...) and unknown URLs.
static void redirectToPortal() {
  char url[40];
  snprintf(url, sizeof(url), "http://%s/", WiFi.softAPIP().toString().c_str());
  server.sendHeader("Location", url, true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}
void handleCaptive() {
  if (setupMode) redirectToPortal();
  else server.send(404, "text/plain", "Not found");
}
void handleNotFound() {
  if (setupMode) redirectToPortal();
  else server.send(404, "text/plain", "Not found");
}

void startWebServer() {
  static const char* hdrs[] = { "X-Requested-With" };
  server.collectHeaders(hdrs, 1);
  server.on("/",           HTTP_GET,  handleDashboard);
  server.on("/api/status", HTTP_GET,  handleStatus);
  server.on("/api/system", HTTP_GET,  handleSystemInfo);
  server.on("/api/scan",   HTTP_GET,  handleScan);
  server.on("/api/wifi",   HTTP_POST, handleWiFiSettings);
  server.on("/api/ap",     HTTP_POST, handleApSettings);
  server.on("/api/setup",  HTTP_POST, handleSetup);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/factory", HTTP_POST, handleFactoryReset);
  server.on("/api/saved",     HTTP_GET,  handleSavedList);
  server.on("/api/saved/use", HTTP_POST, handleSavedUse);
  server.on("/api/saved/del", HTTP_POST, handleSavedDel);
  server.on("/api/account",   HTTP_POST, handleAccount);
  server.on("/api/brand",     HTTP_POST, handleBrand);
  static const char* probes[] = {
    "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
    "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt",
    "/fwlink", "/kindle-wifi/wifiredirect.html"
  };
  for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) server.on(probes[i], HTTP_ANY, handleCaptive);
  server.onNotFound(handleNotFound);
  server.begin();
  LOG("WEB", "HTTP server listening on port 80 (%s)", setupMode ? "setup, no login" : "login required");
}

// ============================================================================
// Periodic services called from loop()
// ============================================================================
static void serviceButton(uint32_t now) {
  if (restartPending) return;
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!btnDownAt) btnDownAt = now ? now : 1;
    else if (now - btnDownAt >= RESET_HOLD_MS) {
      LOG("BOOT", "Reset button held for %lu s - factory reset", (unsigned long)(RESET_HOLD_MS / 1000));
      factoryReset();
      scheduleRestart(300);
    }
  } else btnDownAt = 0;
}

static void serviceLog() {
  if (setupMode) {
    LOG("AP", "Setup mode | clients %u | heap %lu", (unsigned)status.clients, (unsigned long)status.heap);
  } else if (status.staUp) {
    LOG("WIFI", "Upstream \"%s\" OK | IP %s | RSSI %d dBm | ch %u | AP clients %u | heap %lu (min %lu) | reconnects %lu",
        cfg.staSsid, status.staIp, status.rssi, (unsigned)status.channel, (unsigned)status.clients,
        (unsigned long)status.heap, (unsigned long)status.minHeap, (unsigned long)reconnectCount);
  } else {
    LOG("WIFI", "Upstream \"%s\" DOWN (%s) | AP clients %u | heap %lu | reconnects %lu",
        cfg.staSsid, (const char*)lastError, (unsigned)status.clients, (unsigned long)status.heap,
        (unsigned long)reconnectCount);
  }
}

// ============================================================================
// Arduino entry points
// ============================================================================
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { yield(); }          // bounded wait for USB-CDC
  LOG("BOOT", "ESP32-S3 Wi-Fi Repeater v%s", FW_VERSION);
  LOG("BOOT", "Chip %s rev %u, %u cores @ %u MHz, free heap %lu, reset reason: %s",
      ESP.getChipModel(), (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(),
      (unsigned)ESP.getCpuFreqMHz(), (unsigned long)ESP.getFreeHeap(), resetReasonText());

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  loadConfig();

  WiFi.persistent(false);                 // credentials live only in our own NVS namespace
  WiFi.setAutoReconnect(false);           // reconnect is handled by checkWiFiConnection()
  WiFi.onEvent(handleWiFiEvents);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_AP_STA);                 // STA + AP on the single radio (STA also does scans in setup mode)
  WiFi.setSleep(false);                   // no modem sleep: lower latency / better AP throughput
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);   // pick the strongest AP when several share the SSID

  startAccessPoint();
  startWebServer();
  updateStatus();
  LOG("BOOT", "Mode: %s", setupMode ? "SETUP" : "REPEATER");
  // The first upstream connection attempt is started by checkWiFiConnection() in loop().
}

void loop() {
  const uint32_t now = millis();

  server.handleClient();
  if (setupMode) dnsServer.processNextRequest();

  pollScan();
  checkWiFiConnection();
  serviceNat();
  if (!apOk && (int32_t)(now - apRetryAt) >= 0) startAccessPoint();
  serviceButton(now);

  if (now - lastStatusMs >= STATUS_INTERVAL_MS) { lastStatusMs = now; updateStatus(); }
  if (now - lastLogMs    >= LOG_INTERVAL_MS)    { lastLogMs = now;    serviceLog();  }

  if (restartPending && (int32_t)(now - restartAt) >= 0) {
    LOG("BOOT", "Restarting...");
    Serial.flush();
    ESP.restart();
  }

  // Cooperative 2 ms yield (not a blocking wait): lets the IDLE task run so the
  // task watchdog is fed, and keeps CPU use sane while Wi-Fi/lwIP tasks work.
  vTaskDelay(pdMS_TO_TICKS(2));
}

// ============================================================================
// Web UI (single page, served from flash; all dynamic text uses textContent)
// ============================================================================
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>Repeater</title>
<style>
:root{--bg:#0d1117;--sb:#0a0e13;--pn:#151b23;--ln:#232c38;--tx:#e6edf3;--mu:#8793a2;--ac:#2f9bff;--ink:#fff;--ok:#3ecf8e;--wa:#f2b13c;--er:#ff6b6b}
@media(prefers-color-scheme:light){:root:not([data-theme=dark]){--bg:#f3f5f8;--sb:#fff;--pn:#fff;--ln:#dde3ea;--tx:#111b27;--mu:#5b6877}}
:root[data-theme=light]{--bg:#f3f5f8;--sb:#fff;--pn:#fff;--ln:#dde3ea;--tx:#111b27;--mu:#5b6877}
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--tx);font:14.5px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",sans-serif;-webkit-font-smoothing:antialiased}
button{font:inherit}
.app{display:grid;grid-template-columns:240px minmax(0,1fr);min-height:100vh}
aside{background:var(--sb);border-right:1px solid var(--ln);padding:22px 14px;position:sticky;top:0;height:100vh;display:flex;flex-direction:column;gap:24px}
.brand{display:flex;align-items:center;gap:10px;padding:0 8px;font-weight:650;font-size:16px;letter-spacing:-.01em;min-width:0}
.brand .bn{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.mark{width:26px;height:26px;fill:none;stroke:var(--ac);stroke-width:2.2;stroke-linecap:round;flex:none}.mark circle{fill:var(--ac);stroke:none}
nav{display:flex;flex-direction:column;gap:2px}
nav button{display:flex;align-items:center;gap:12px;background:none;border:0;color:var(--mu);padding:10px 12px;border-radius:8px;font-weight:500;cursor:pointer;text-align:left}
nav button:hover{color:var(--tx);background:rgba(128,140,155,.12)}
nav button.on{color:var(--tx);background:rgba(128,140,155,.16);background:color-mix(in srgb,var(--ac) 16%,transparent)}
nav button.on .ic{stroke:var(--ac)}
.ic{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;flex:none}
.side-st{margin-top:auto;padding:11px 12px;border:1px solid var(--ln);border-radius:10px;font-size:13px}
.badge{display:inline-flex;align-items:center;gap:8px;font-weight:550;min-width:0}
.dot{width:9px;height:9px;border-radius:50%;background:var(--mu);flex:none}
.badge.ok .dot{background:var(--ok)}.badge.warn .dot{background:var(--wa)}.badge.err .dot{background:var(--er)}
.mtop{display:none;align-items:center;justify-content:space-between;gap:10px;padding:12px 16px;border-bottom:1px solid var(--ln);background:var(--sb);position:sticky;top:0;z-index:5}
.mtop .badge{font-size:12.5px}
main{padding:30px 34px 60px;max-width:1000px;width:100%}
h2.pt{font-size:22px;margin:0 0 4px;letter-spacing:-.02em}
.sub{color:var(--mu);margin:0 0 22px}
.pn{background:var(--pn);border:1px solid var(--ln);border-radius:10px;padding:18px 20px;margin-bottom:16px;max-width:640px}
.pn.wide{max-width:none}
.pn h3{margin:0 0 2px;font-size:15px}
.d{color:var(--mu);font-size:13px;margin:0 0 10px}
.banner{margin:0 0 16px;padding:11px 14px;border-radius:10px;border:1px solid var(--wa);background:rgba(242,177,60,.12);color:var(--wa)}
.chain{display:grid;grid-template-columns:minmax(0,1fr) 56px minmax(0,1fr) 56px minmax(0,1fr);align-items:center}
.node{padding:14px 16px;border:1px solid var(--ln);border-radius:10px;background:var(--bg);min-width:0}
.node.me{border-color:var(--ac)}
.node .k{display:block;color:var(--mu);font-size:12.5px}
.node b{display:block;font-size:17px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin:1px 0}
.node .s{color:var(--mu);font-size:13px;display:flex;align-items:center;gap:8px}
.lk{height:0;border-top:2px solid var(--ln)}
.lk.ok{border-top-color:var(--ok)}.lk.off{border-top-style:dashed;border-top-color:var(--er)}
.sig{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.sig i{width:3px;border-radius:1px;background:var(--ln)}
.sig i:nth-child(1){height:4px}.sig i:nth-child(2){height:7px}.sig i:nth-child(3){height:10px}.sig i:nth-child(4){height:14px}
.sig[data-q="1"] i:nth-child(-n+1),.sig[data-q="2"] i:nth-child(-n+2),.sig[data-q="3"] i:nth-child(-n+3),.sig[data-q="4"] i{background:var(--ok)}
.sig[data-q="1"] i:nth-child(1){background:var(--wa)}
.kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(270px,1fr));gap:0 32px;margin-top:6px}
.kv div{display:flex;justify-content:space-between;gap:14px;padding:9px 0;border-bottom:1px solid var(--ln)}
.kv span{color:var(--mu)}
.kv b{font-weight:550;font-variant-numeric:tabular-nums;text-align:right;overflow-wrap:anywhere}
label{display:block;margin:14px 0 6px;font-weight:550;font-size:13.5px}
.chk{display:flex;gap:9px;align-items:center;font-weight:400;margin-top:12px}
input,select{width:100%;padding:10px 12px;border-radius:8px;border:1px solid var(--ln);background:var(--bg);color:var(--tx);font:inherit}
.chk input{width:auto}
input[type=color]{width:44px;height:34px;padding:2px;cursor:pointer}
input:focus-visible,select:focus-visible,button:focus-visible{outline:2px solid var(--ac);outline-offset:1px}
.pw{position:relative}.pw input{padding-right:70px}
.eye{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:0;color:var(--mu);padding:6px 8px;cursor:pointer;border-radius:6px}
.eye:hover{color:var(--tx)}
.btn{padding:10px 18px;border:1px solid var(--ac);border-radius:8px;background:var(--ac);color:var(--ink);font-weight:600;cursor:pointer}
.btn.sec{background:transparent;border-color:var(--ln);color:var(--tx)}
.btn.dng{background:var(--er);border-color:var(--er);color:#fff}
.btn.sm{padding:6px 12px;font-size:13px}
.row{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}
.li{display:flex;align-items:center;gap:12px;padding:10px 12px;border:1px solid var(--ln);border-radius:8px;margin-top:8px;background:var(--bg)}
button.li{width:100%;text-align:left;color:var(--tx);cursor:pointer}
button.li:hover{border-color:var(--ac)}
.li .nm{flex:1;min-width:0;font-weight:550;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.li .mt{color:var(--mu);font-size:12.5px;white-space:nowrap}
.tag{font-size:12px;font-weight:600;padding:2px 9px;border-radius:99px;background:rgba(62,207,142,.15);color:var(--ok)}
.sw{display:flex;gap:10px;flex-wrap:wrap}
.sw button{width:30px;height:30px;border-radius:50%;border:2px solid transparent;cursor:pointer;box-shadow:inset 0 0 0 2px var(--pn)}
.sw button.on{border-color:var(--tx)}
.seg{display:inline-flex;border:1px solid var(--ln);border-radius:8px;overflow:hidden}
.seg button{background:none;border:0;color:var(--mu);padding:8px 18px;cursor:pointer}
.seg button.on{background:var(--ac);color:var(--ink)}
#toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%);padding:11px 18px;border-radius:10px;background:var(--tx);color:var(--bg);font-weight:550;box-shadow:0 10px 32px rgba(0,0,0,.35);max-width:92vw;z-index:30;text-align:center}
#toast.err{background:var(--er);color:#fff}
@media(max-width:820px){
 .app{grid-template-columns:minmax(0,1fr)}
 aside{position:fixed;top:auto;bottom:0;left:0;right:0;height:auto;flex-direction:row;padding:6px 6px calc(6px + env(safe-area-inset-bottom,0px));border-right:0;border-top:1px solid var(--ln);z-index:10;gap:0}
 aside .brand,aside .side-st{display:none}
 nav{flex-direction:row;width:100%}
 nav button{flex:1;flex-direction:column;gap:3px;font-size:11.5px;padding:7px 2px;align-items:center;text-align:center}
 .mtop{display:flex}
 main{padding:20px 16px 100px}
 .chain{grid-template-columns:minmax(0,1fr)}
 .lk{height:22px;width:0;border-top:0;border-left:2px solid var(--ln);margin:0 auto}
 .lk.ok{border-left-color:var(--ok)}.lk.off{border-left-style:dashed;border-left-color:var(--er)}
 #toast{bottom:84px}
}
</style></head><body>
<div class="app">
<aside>
<div class="brand"><svg class="mark" viewBox="0 0 24 24"><path d="M3.5 9.5a12 12 0 0 1 17 0M7 13a7 7 0 0 1 10 0"/><circle cx="12" cy="17.5" r="1.8"/></svg><span class="bn">Repeater</span></div>
<nav id="nav">
<button data-p="dash" class="on" type="button"><svg class="ic" viewBox="0 0 24 24"><path d="M3 13h8V3H3zM13 21h8V11h-8zM13 3v6h8V3zM3 21h8v-6H3z"/></svg>Dashboard</button>
<button data-p="wifi" type="button"><svg class="ic" viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1"/></svg>Wi-Fi</button>
<button data-p="ap" type="button"><svg class="ic" viewBox="0 0 24 24"><circle cx="12" cy="12" r="2"/><path d="M7.8 7.8a6 6 0 0 0 0 8.4M16.2 7.8a6 6 0 0 1 0 8.4M4.9 4.9a10 10 0 0 0 0 14.2M19.1 4.9a10 10 0 0 1 0 14.2"/></svg>Repeater</button>
<button data-p="admin" type="button"><svg class="ic" viewBox="0 0 24 24"><path d="M12 3l8 3v6c0 4.5-3.2 8-8 9-4.8-1-8-4.5-8-9V6z"/></svg>Admin</button>
<button data-p="sys" type="button"><svg class="ic" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/></svg>System</button>
</nav>
<div class="side-st"><span class="badge"><i class="dot"></i><span class="bt">Connecting…</span></span></div>
</aside>
<div>
<div class="mtop"><div class="brand"><svg class="mark" viewBox="0 0 24 24"><path d="M3.5 9.5a12 12 0 0 1 17 0M7 13a7 7 0 0 1 10 0"/><circle cx="12" cy="17.5" r="1.8"/></svg><span class="bn">Repeater</span></div><span class="badge"><i class="dot"></i><span class="bt">…</span></span></div>
<main>
<div id="setupHead" class="setonly" hidden><h2 class="pt">Set up your repeater</h2><p class="sub">Choose your router, name the repeater, then create the admin login.</p></div>
<div id="banner" class="banner" hidden></div>

<section id="dash" class="pg">
<h2 class="pt">Dashboard</h2><p class="sub">Live status, refreshed every 2 seconds.</p>
<div class="pn wide"><div class="chain">
<div class="node"><span class="k">Upstream router</span><b id="d_up">–</b><span class="s"><span class="sig" id="d_sig" data-q="0"><i></i><i></i><i></i><i></i></span><span id="d_rssi">–</span></span></div>
<div class="lk" id="lk1"></div>
<div class="node me"><span class="k">This repeater</span><b id="d_ap">–</b><span class="s" id="d_apip">–</span></div>
<div class="lk" id="lk2"></div>
<div class="node"><span class="k">Connected devices</span><b id="d_cl">0</b><span class="s">on the repeater network</span></div>
</div></div>
<div class="pn wide"><h3>Details</h3><div class="kv" id="kv"></div></div>
</section>

<section id="wifi" class="pg" hidden>
<h2 class="pt">Wi-Fi</h2><p class="sub">The network this repeater extends. The ESP32-S3 supports 2.4 GHz only.</p>
<div class="pn repo"><h3>Saved networks</h3><p class="d">Passwords stay on the device and are never shown. Pick one to switch.</p><div id="savedList"></div></div>
<div class="pn"><h3>Connect to a network</h3>
<div class="row" style="margin-top:8px"><button class="btn sec sm" id="rescan" type="button">Scan again</button></div>
<div id="nets"></div>
<label for="ssid">Network name</label><input id="ssid" maxlength="32" autocomplete="off" autocapitalize="off">
<label for="pass">Password</label><div class="pw"><input id="pass" type="password" maxlength="63" autocomplete="new-password"><button class="eye" data-for="pass" type="button">Show</button></div>
<label class="chk"><input type="checkbox" id="open">Open network, no password</label>
<p class="d repo" style="margin-top:10px">Leave the password blank to use the one already saved for this network.</p>
<div class="row repo"><button class="btn" id="saveWifi" type="button">Save and restart</button></div>
</div>
</section>

<section id="ap" class="pg" hidden>
<h2 class="pt">Repeater network</h2><p class="sub">The network your devices join.</p>
<div class="pn">
<label for="ap_ssid">Network name</label><input id="ap_ssid" maxlength="32" value="ESP32-S3-Repeater" autocomplete="off">
<label for="ap_pass">Password (8–63 characters)</label><div class="pw"><input id="ap_pass" type="password" maxlength="63" autocomplete="new-password"><button class="eye" data-for="ap_pass" type="button">Show</button></div>
<label for="ap_ch">Preferred channel</label><select id="ap_ch"><option value="0">Auto (follow upstream)</option></select>
<p class="d" style="margin-top:8px">With one radio, the repeater uses the router's channel while connected. This applies only when the upstream is offline.</p>
<p class="d repo">Leave the password blank to keep the saved one.</p>
<div class="row repo"><button class="btn" id="saveAp" type="button">Save and restart</button></div>
</div>
<div class="pn setonly" hidden><h3>Admin login</h3><p class="d">You will use this to open this panel later.</p>
<label for="ap_user">Username</label><input id="ap_user" maxlength="16" value="admin" autocomplete="username" autocapitalize="off">
<label for="ap_adm">Password (8–32 characters)</label><div class="pw"><input id="ap_adm" type="password" maxlength="32" autocomplete="new-password"><button class="eye" data-for="ap_adm" type="button">Show</button></div>
<label for="ap_adm2">Repeat password</label><input id="ap_adm2" type="password" maxlength="32" autocomplete="new-password">
</div>
</section>

<section id="admin" class="pg" hidden>
<h2 class="pt">Admin</h2><p class="sub">Sign-in details and how this panel looks.</p>
<div class="pn"><h3>Account</h3><p class="d">You will be asked to sign in again after a change.</p>
<label for="a_user">Username</label><input id="a_user" maxlength="16" autocomplete="username" autocapitalize="off">
<label for="a_cur">Current password</label><div class="pw"><input id="a_cur" type="password" maxlength="32" autocomplete="current-password"><button class="eye" data-for="a_cur" type="button">Show</button></div>
<label for="a_new">New password (8–32 characters)</label><div class="pw"><input id="a_new" type="password" maxlength="32" autocomplete="new-password"><button class="eye" data-for="a_new" type="button">Show</button></div>
<label for="a_new2">Repeat new password</label><input id="a_new2" type="password" maxlength="32" autocomplete="new-password">
<p class="d" style="margin-top:8px">Leave both new password fields blank to change only the username.</p>
<div class="row"><button class="btn" id="saveAcc" type="button">Update account</button></div>
</div>
<div class="pn"><h3>Appearance</h3><p class="d">Changes preview instantly. Save to keep them.</p>
<label for="b_name">Panel name</label><input id="b_name" maxlength="24" autocomplete="off">
<label>Accent color</label><div class="sw" id="sw"></div>
<label class="chk"><input type="color" id="b_col">Custom color</label>
<label>Theme</label><div class="seg" id="seg"><button data-t="0" type="button">Auto</button><button data-t="1" type="button">Dark</button><button data-t="2" type="button">Light</button></div>
<div class="row"><button class="btn" id="saveUi" type="button">Save appearance</button><button class="btn sec" id="resetUi" type="button">Reset to default</button></div>
</div>
</section>

<section id="sys" class="pg" hidden>
<h2 class="pt">System</h2><p class="sub">Hardware, firmware and maintenance.</p>
<div class="pn wide"><h3>Information</h3><div class="kv" id="sysKv"></div></div>
<div class="pn"><h3>Maintenance</h3>
<p class="d">Factory reset erases all saved networks and settings and returns to setup mode. Holding the BOOT button for 10 seconds does the same.</p>
<div class="row" style="margin-top:8px"><button class="btn sec" id="btnRestart" type="button">Restart</button><button class="btn dng" id="btnFactory" type="button">Factory reset</button></div>
</div>
</section>

<div id="setupBar" hidden><div class="row" style="margin-top:4px"><button class="btn" id="saveSetup" type="button">Save and start repeater</button></div></div>
</main>
</div>
</div>
<div id="toast" hidden></div>
<script>
const $=i=>document.getElementById(i);
const h=(t,c,x)=>{const e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e};
const PG=['dash','wifi','ap','admin','sys'];
let mode='',tt,ui={brand:'Repeater',accent:'#2f9bff',theme:0};

function toast(m,ok){const t=$('toast');t.textContent=m;t.className=ok?'':'err';t.hidden=false;clearTimeout(tt);tt=setTimeout(()=>{t.hidden=true},ok?4500:7000)}
async function api(u,o){const r=await fetch(u,o);let d={};try{d=await r.json()}catch(e){}
 if(!r.ok||d.ok===false)throw new Error(d.error||('HTTP '+r.status));return d}
const post=(u,d)=>api(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Requested-With':'repeater-ui'},body:new URLSearchParams(d)});
async function act(u,d,msg,after){try{const r=await post(u,d);toast(msg||r.msg,1);if(after)after(r)}catch(e){toast(e.message,0)}}

const fmtUp=s=>{const d=Math.floor(s/86400),H=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+H+'h '+m+'m '+(s%60)+'s'};
const qual=r=>r>=-55?'Excellent':r>=-67?'Good':r>=-75?'Fair':'Weak';
const q4=r=>r>=-55?4:r>=-67?3:r>=-75?2:1;
const sig=r=>{const s=h('span','sig');s.dataset.q=q4(r);for(let i=0;i<4;i++)s.append(h('i'));return s};
function kvFill(el,rows){el.textContent='';rows.forEach(r=>{const d=h('div');d.append(h('span',0,r[0]),h('b',0,String(r[1])));el.append(d)})}

function ink(c){const n=parseInt(c.slice(1),16),r=n>>16,g=(n>>8)&255,b=n&255;return(.299*r+.587*g+.114*b)>150?'#0b1220':'#fff'}
function applyUi(u){const r=document.documentElement;r.style.setProperty('--ac',u.accent);r.style.setProperty('--ink',ink(u.accent));
 r.dataset.theme=['auto','dark','light'][u.theme]||'auto';document.title=u.brand;document.querySelectorAll('.bn').forEach(e=>{e.textContent=u.brand})}
function syncUi(){document.querySelectorAll('#sw button').forEach(b=>b.classList.toggle('on',b.dataset.c===ui.accent.toLowerCase()));
 $('b_col').value=ui.accent;document.querySelectorAll('#seg button').forEach(b=>b.classList.toggle('on',+b.dataset.t===ui.theme))}
function fillBrand(){$('b_name').value=ui.brand;syncUi()}

['#2f9bff','#14b8a6','#8b5cf6','#f97316','#ec4899','#22c55e'].forEach(c=>{const b=h('button');b.type='button';b.dataset.c=c;b.style.background=c;b.setAttribute('aria-label','Accent '+c);
 b.onclick=()=>{ui.accent=c;applyUi(ui);syncUi()};$('sw').append(b)});
document.querySelectorAll('#seg button').forEach(b=>b.onclick=()=>{ui.theme=+b.dataset.t;applyUi(ui);syncUi()});
$('b_name').oninput=()=>{ui.brand=$('b_name').value||'Repeater';applyUi(ui)};
$('b_col').oninput=()=>{ui.accent=$('b_col').value.toLowerCase();applyUi(ui);syncUi()};
$('saveUi').onclick=()=>act('/api/brand',{brand:$('b_name').value,accent:ui.accent,theme:ui.theme},'Appearance saved');
$('resetUi').onclick=()=>act('/api/brand',{reset:'1'},'Default appearance restored',r=>{ui={brand:r.brand,accent:r.accent,theme:r.theme};applyUi(ui);fillBrand()});

document.querySelectorAll('.eye').forEach(b=>b.onclick=()=>{const i=$(b.dataset.for),s=i.type==='password';i.type=s?'text':'password';b.textContent=s?'Hide':'Show'});

const sel=$('ap_ch');for(let i=1;i<=11;i++){const o=h('option',0,'Channel '+i);o.value=i;sel.append(o)}

function show(p){PG.forEach(x=>{$(x).hidden=x!==p});document.querySelectorAll('#nav button').forEach(b=>b.classList.toggle('on',b.dataset.p===p));
 if(p==='wifi'){scan(1);loadSaved()}if(p==='sys')sysinfo();window.scrollTo(0,0)}
document.querySelectorAll('#nav button').forEach(b=>b.onclick=()=>show(b.dataset.p));

async function scan(start){const n=$('nets');if(start)n.textContent='Scanning…';
 try{const d=await api('/api/scan'+(start?'?start=1':''));
  if(d.scanning){setTimeout(()=>{if(!$('wifi').hidden)scan(0)},1500);return}
  n.textContent='';if(!d.networks.length){n.append(h('p','d','No networks found. Scan again.'));return}
  d.networks.forEach(x=>{const b=h('button','li');b.type='button';
   b.append(sig(x.rssi),h('span','nm',x.ssid),h('span','mt',(x.sec?'Secured · ':'Open · ')+x.rssi+' dBm · ch '+x.ch));
   b.onclick=()=>{$('ssid').value=x.ssid;$('open').checked=!x.sec;$('pass').focus()};n.append(b)})
 }catch(e){n.textContent='Scan failed: '+e.message}}
$('rescan').onclick=()=>scan(1);

async function loadSaved(){const l=$('savedList');
 try{const d=await api('/api/saved');l.textContent='';
  if(!d.saved.length){l.append(h('p','d','Nothing saved yet. Networks you connect to appear here.'));return}
  d.saved.forEach(x=>{const r=h('div','li');r.append(h('span','nm',x.ssid),h('span','mt',x.sec?'Secured':'Open'));
   if(x.cur){r.append(h('span','tag','In use'))}else{
    const u=h('button','btn sm','Connect');u.type='button';
    u.onclick=()=>{if(confirm('Switch to "'+x.ssid+'"? The repeater restarts.'))act('/api/saved/use',{i:x.i},'Switching network. Reconnect in about 20 seconds.')};r.append(u);
    const f=h('button','btn sec sm','Forget');f.type='button';
    f.onclick=()=>{if(confirm('Forget "'+x.ssid+'"?'))act('/api/saved/del',{i:x.i},'Network forgotten',loadSaved)};r.append(f)}
   l.append(r)})
 }catch(e){l.textContent=e.message}}

async function sysinfo(){try{const d=await api('/api/system');
 kvFill($('sysKv'),[['Firmware',d.fw],['Chip',d.chip+', '+d.cores+' cores @ '+d.mhz+' MHz'],['Flash',(d.flashKB/1024).toFixed(0)+' MB'],
  ['PSRAM',d.psramKB?(d.psramKB/1024).toFixed(0)+' MB':'none'],['Sketch size',d.sketchKB+' KB'],['Arduino core',d.core],['ESP-IDF',d.sdk],
  ['Upstream MAC',d.staMac],['Repeater MAC',d.apMac],['Last reset',d.reset],['Lowest free memory',(d.minHeap/1024).toFixed(0)+' KB']])
 }catch(e){toast(e.message,0)}}

async function refresh(){
 try{const s=await api('/api/status');
  if(s.mode==='setup'&&mode!=='setup')initSetup();
  if(!window.__ui){window.__ui=1;ui={brand:s.brand,accent:s.accent,theme:s.theme};applyUi(ui);fillBrand();
   $('a_user').value=s.user;$('ap_ch').value=s.cfgCh;$('ap_ssid').value=s.apSsid;$('ssid').value=s.staSsid}
  document.querySelectorAll('.badge').forEach(b=>{b.className='badge '+s.level;b.querySelector('.bt').textContent=s.state});
  $('d_up').textContent=s.staSsid||'–';$('d_sig').dataset.q=s.staUp?q4(s.rssi):0;
  $('d_rssi').textContent=s.staUp?s.rssi+' dBm · '+qual(s.rssi):'Not connected';
  $('d_ap').textContent=s.apSsid;$('d_apip').textContent=s.apIp;$('d_cl').textContent=s.clients;
  $('lk1').className='lk '+(s.staUp?'ok':'off');$('lk2').className='lk '+(s.clients>0?'ok':'');
  kvFill($('kv'),[['Status',s.state],['Upstream address',s.staIp],['Internet sharing (NAT)',s.nat?'On':'Off'],['Repeater address',s.apIp],['Channel',s.ch],
   ['Uptime',fmtUp(s.uptime)],['Free memory',Math.round(s.heap/1024)+' KB (lowest '+Math.round(s.minHeap/1024)+' KB)'],['Reconnect attempts',s.reconn]]);
  const m=[];if(s.err)m.push(s.err);if(s.memLow)m.push('Low memory: '+Math.round(s.heap/1024)+' KB free');
  const bn=$('banner');bn.hidden=!m.length;bn.textContent=m.join(' | ');
 }catch(e){document.querySelectorAll('.badge').forEach(b=>{b.className='badge err';b.querySelector('.bt').textContent='Offline'})}}

function initSetup(){mode='setup';
 document.querySelectorAll('.repo').forEach(e=>{e.hidden=true});document.querySelectorAll('.setonly').forEach(e=>{e.hidden=false});
 $('nav').hidden=true;document.querySelector('.side-st').hidden=true;
 PG.forEach(x=>{$(x).hidden=true});$('wifi').hidden=false;$('ap').hidden=false;$('setupBar').hidden=false;scan(1)}

$('saveWifi').onclick=()=>act('/api/wifi',{ssid:$('ssid').value,pass:$('pass').value,open:$('open').checked?'1':'0'},'Saved. The repeater is restarting.');
$('saveAp').onclick=()=>act('/api/ap',{apssid:$('ap_ssid').value,appass:$('ap_pass').value,channel:$('ap_ch').value},'Saved. Rejoin "'+$('ap_ssid').value+'" in a few seconds.');
$('saveSetup').onclick=()=>{
 if($('ap_adm').value!==$('ap_adm2').value){toast('Admin passwords do not match',0);return}
 act('/api/setup',{ssid:$('ssid').value,pass:$('pass').value,open:$('open').checked?'1':'0',apssid:$('ap_ssid').value,appass:$('ap_pass').value,
  adminuser:$('ap_user').value,adminpass:$('ap_adm').value,channel:$('ap_ch').value},
  'Saved. Restarting. Join "'+$('ap_ssid').value+'", open http://192.168.50.1/ and sign in as '+$('ap_user').value+'.')};
$('saveAcc').onclick=()=>{
 if($('a_new').value!==$('a_new2').value){toast('New passwords do not match',0);return}
 act('/api/account',{user:$('a_user').value,curpass:$('a_cur').value,newpass:$('a_new').value},null,()=>{setTimeout(()=>location.reload(),1500)})};
$('btnRestart').onclick=()=>{if(confirm('Restart the ESP32 now?'))act('/api/restart',{})};
$('btnFactory').onclick=()=>{if(confirm('Erase ALL settings and saved networks, and return to setup mode?'))act('/api/factory',{})};

refresh();setInterval(refresh,2000);
</script></body></html>
)HTML";
