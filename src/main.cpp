#include <Arduino.h>
#include <EEPROM.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiUdp.h>
#elif defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiUdp.h>
#endif

const int EEPROM_SIZE = 512;

// Boot button / factory reset settings
const int BOOT_BUTTON_PIN = 0; // change if your board uses a different pin
const unsigned long BOOT_BUTTON_DEBOUNCE_MS = 50;
const unsigned long BOOT_BUTTON_WINDOW_MS = 5000; // time window to count presses
const int BOOT_BUTTON_REQUIRED = 5; // required presses to trigger factory reset


void initPersistent() {
#if defined(ESP8266) || defined(ESP32)
  EEPROM.begin(EEPROM_SIZE);
#endif
}

// Helper: parse dotted IP string to IPAddress (returns 0.0.0.0 on failure)
static IPAddress parseIP(const String& s) {
  int parts[4] = {0,0,0,0};
  int idx = 0;
  String cur = "";
  for (unsigned int i = 0; i <= s.length(); ++i) {
    if (i == s.length() || s[i] == '.') {
      if (cur.length() > 0 && idx < 4) {
        parts[idx++] = cur.toInt();
      }
      cur = "";
    } else {
      cur += s[i];
    }
  }
  if (idx < 4) return IPAddress(0,0,0,0);
  return IPAddress(parts[0], parts[1], parts[2], parts[3]);
}

bool persistSave(const String& key, const String& value) {
  if (key.length() == 0) return false;
  int addr = 0;
  while (addr < EEPROM_SIZE) {
    if (EEPROM.read(addr) == 0xFF) break;
    while (addr < EEPROM_SIZE && EEPROM.read(addr) != 0) addr++;
    if (addr >= EEPROM_SIZE) return false;
    addr++;
    while (addr < EEPROM_SIZE && EEPROM.read(addr) != 0) addr++;
    if (addr >= EEPROM_SIZE) return false;
    addr++;
  }
  int need = key.length() + 1 + value.length() + 1;
  if (addr + need > EEPROM_SIZE) return false;
  for (size_t i = 0; i < key.length(); ++i) EEPROM.write(addr++, key[i]);
  EEPROM.write(addr++, 0);
  for (size_t i = 0; i < value.length(); ++i) EEPROM.write(addr++, value[i]);
  EEPROM.write(addr++, 0);
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
  return true;
}

String persistRead(const String& key) {
  int addr = 0;
  while (addr < EEPROM_SIZE) {
    if (EEPROM.read(addr) == 0xFF) break;
    String k = "";
    while (addr < EEPROM_SIZE) {
      char c = EEPROM.read(addr++);
      if (c == 0) break;
      k += c;
    }
    String v = "";
    while (addr < EEPROM_SIZE) {
      char c = EEPROM.read(addr++);
      if (c == 0) break;
      v += c;
    }
    if (k == key) return v;
  }
  return String();
}

static bool apModeActive = false;

#if defined(ESP8266)
static ESP8266WebServer server(80);
#elif defined(ESP32)
static WebServer server(80);
#endif
static DNSServer dnsServer;
const byte DNS_PORT = 53;
static IPAddress apIP(192,168,4,1);

// UDP discovery
static WiFiUDP udp;
const unsigned int UDP_DISCOVERY_PORT = 4267;

static void startUDPListener() {
  if (udp.begin(UDP_DISCOVERY_PORT)) {
    Serial.print("UDP discovery listener started on port ");
    Serial.println(UDP_DISCOVERY_PORT);
  } else {
    Serial.println("UDP discovery listener failed to start");
  }
}

// Boot button runtime state
static bool _lastButtonState = true;
static unsigned long _lastButtonChange = 0;
static bool _lastPressedHandled = false;
static int _bootPressCount = 0;
static unsigned long _firstBootPressTime = 0;

static void handleRoot() {
  // Scan nach Netzwerken (blockierend) und Duplikate filtern
  String current = persistRead("wifi-ssid");
  int n = WiFi.scanNetworks();
  String options = "";
  for (int i = 0; i < n; ++i) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    // nur gegenüber früheren Einträgen prüfen — erstes Vorkommen behalten
    for (int j = 0; j < i; ++j) {
      if (WiFi.SSID(j) == s) { dup = true; break; }
    }
    if (dup) continue;
    options += "<option value='" + s + "'";
    if (s == current) options += " selected";
    options += ">" + s + "</option>";
  }
  if (options.length() == 0) {
    options = "<option value='' disabled selected>Keine Netzwerke gefunden</option>";
  }
  // statische IP-Vorgaben aus Persistent
  String useStatic = persistRead("wifi-static");
  String ipVal = persistRead("wifi-ip");
  String gwVal = persistRead("wifi-gateway");
  String nmVal = persistRead("wifi-netmask");

  server.send(200, "text/html",
  "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<style>"
  "html,body{height:100%;margin:0}body{display:flex;align-items:center;justify-content:center;"
  "background:linear-gradient(135deg,#36CAFF11,#E566FF11);font-family:Arial,Helvetica,sans-serif}"
  ".card{background:#fff;padding:24px;border-radius:12px;box-shadow:0 6px 20px rgba(0,0,0,0.12);width:360px;max-width:90%}"
  "h1{margin:0 0 12px;font-size:20px;color:#36CAFF;text-align:center}"
  "select,input{width:100%;padding:10px;border:1px solid #e6e6e6;border-radius:8px;margin:8px 0;box-sizing:border-box}"
  "button{width:100%;padding:12px;border:none;border-radius:8px;background:linear-gradient(90deg,#36CAFF,#E566FF);color:#fff;font-weight:600;cursor:pointer}"
  "button:active{opacity:0.95}"
  "</style></head><body>"
  "<div class='card'>"
  "<h1>WLAN einrichten</h1>"
  "<form method='POST' action='/save'>"
  "<select name='ssid'>" + options + "</select><br>"
  "<input name='pass' placeholder='WLAN Passwort' type='password'><br>"
  "<label style='display:block;margin-top:8px'><input type='checkbox' name='use_static'" + (useStatic=="1"?" checked":"") + "> Statische IP verwenden</label>"
  "<input name='ip' placeholder='IP (z.B. 192.168.1.50)' value='" + ipVal + "'><br>"
  "<input name='gateway' placeholder='Gateway (z.B. 192.168.1.1)' value='" + gwVal + "'><br>"
  "<input name='netmask' placeholder='Netzmaske (z.B. 255.255.255.0)' value='" + nmVal + "'><br><br>"
  "<button type='submit'>Speichern</button>"
  "</form></div></body></html>"
  );
}

static void handleSave() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  persistSave("wifi-ssid", s);
  persistSave("wifi-password", p);

  // Static IP fields
  String useStatic = server.hasArg("use_static") ? server.arg("use_static") : String();
  if (useStatic.length() > 0) persistSave("wifi-static", "1"); else persistSave("wifi-static", "0");
  String ip = server.arg("ip");
  String gw = server.arg("gateway");
  String nm = server.arg("netmask");
  persistSave("wifi-ip", ip);
  persistSave("wifi-gateway", gw);
  persistSave("wifi-netmask", nm);

  server.send(200, "text/plain", "OK, reboot...");
  delay(800);
  ESP.restart();
}

static void handleFactoryReset() {
  Serial.println("Factory Reset: Lösche persistente Daten...");
  server.send(200, "text/plain", "Factory Reset wird ausgef&uuml;hrt, reboot...");
  // perform immediate reset (will reboot)
  for (int i = 0; i < EEPROM_SIZE; ++i) {
    EEPROM.write(i, 0xFF);
  }
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
  delay(800);
  ESP.restart();
}

// Shortcut to perform factory reset without HTTP response
static void performFactoryResetImmediate() {
  Serial.println("Factory Reset: Lösche persistente Daten (button)...");
  for (int i = 0; i < EEPROM_SIZE; ++i) {
    EEPROM.write(i, 0xFF);
  }
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
  delay(800);
  ESP.restart();
}

// API: health check
static void handleApiHealth() {
  String ip = apModeActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String mac = "";
#if defined(ESP8266) || defined(ESP32)
  mac = WiFi.macAddress();
#endif
  unsigned long uptime = millis() / 1000;
  int freeHeap = 0;
  int heapFrag = -1;
#if defined(ESP8266) || defined(ESP32)
  freeHeap = ESP.getFreeHeap();
  #if defined(ESP8266)
    heapFrag = ESP.getHeapFragmentation();
  #endif
#endif

  bool connected = false;
  String currentSsid = "";
  int rssi = 0;
  if (WiFi.status() == WL_CONNECTED) {
    connected = true;
    currentSsid = WiFi.SSID();
    rssi = WiFi.RSSI();
  }

  int apClients = 0;
#if defined(ESP8266)
  apClients = WiFi.softAPgetStationNum();
#elif defined(ESP32)
  // On ESP32 the same API is available
  apClients = WiFi.softAPgetStationNum();
#endif

  String json = "{";
  json += "\"status\":\"ok\",";
  json += "\"uptime_s\":" + String(uptime) + ",";
  json += "\"ap_mode\":" + String(apModeActive ? "true" : "false") + ",";
  json += "\"connected\":" + String(connected ? "true" : "false") + ",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"ssid\":\"" + currentSsid + "\",";
  json += "\"rssi_dbm\":" + String(rssi) + ",";
  json += "\"ap_clients\":" + String(apClients) + ",";
  json += "\"mac\":\"" + mac + "\",";
  json += "\"free_heap\":" + String(freeHeap);
  if (heapFrag >= 0) json += ",\"heap_frag_percent\":" + String(heapFrag);
  json += "}";

  server.send(200, "application/json", json);
}

static void startAccessPoint() {
  const char* ap_ssid = "LeviCube-Setup";

  Serial.println("Starte Access Point (AP+STA)...");
  // kombinierten Modus aktivieren, damit SSID-Scan im AP-Betrieb möglich ist
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(ap_ssid);

  dnsServer.start(DNS_PORT, "*", apIP);
  // start UDP discovery listener so mobile app can find device
  startUDPListener();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/factoryReset", handleFactoryReset);
  server.on("/api/health", handleApiHealth);

  server.on("/generate_204", [](){ server.send(204, "text/plain", ""); });
  server.on("/hotspot-detect.html", [](){ handleRoot(); });
  server.onNotFound([](){ handleRoot(); });

  server.begin();
  apModeActive = true;
  Serial.println("Access Point gestartet.");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

// neue Funktion: Startet einfachen Webserver im Station-Modus (liefert /api)
static void startStationServices() {
  // /api -> OK als plain text
  server.on("/api", [](){
    server.send(200, "text/plain", "OK");
  });
  // factory reset auch im Station-Modus verfügbar
  server.on("/api/factoryReset", handleFactoryReset);
  server.on("/api/health", handleApiHealth);
  // optional: keep other handlers minimal
  server.begin();
  // start UDP discovery listener in station mode as well
  startUDPListener();
  Serial.println("Webserver im Station-Modus gestartet.");
  Serial.print("Station IP: ");
  Serial.println(WiFi.localIP());
}

static bool tryConnectWithTimeout(const String& ssid, const String& pass, unsigned long timeoutMs) {
#if defined(ESP8266) || defined(ESP32)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  // If a static IP is configured, apply it before connecting
  String useStatic = persistRead("wifi-static");
  if (useStatic == "1") {
    String ip = persistRead("wifi-ip");
    String gw = persistRead("wifi-gateway");
    String nm = persistRead("wifi-netmask");
    IPAddress lip = parseIP(ip);
    IPAddress lgw = parseIP(gw);
    IPAddress lnm = parseIP(nm);
    if (lip != IPAddress(0,0,0,0) && lgw != IPAddress(0,0,0,0) && lnm != IPAddress(0,0,0,0)) {
      WiFi.config(lip, lgw, lnm);
      Serial.print("Benutze statische IP: "); Serial.println(lip);
    }
  }
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(250);
  }
  
#endif
  return false;
}

static void connectWiFiWithFallback() {
  String ssid = persistRead("wifi-ssid");
  String password = persistRead("wifi-password");

  if (ssid.length() == 0) {
    startAccessPoint();
    return;
  }

  if (tryConnectWithTimeout(ssid, password, 8000)) {
    Serial.println("Mit WLAN verbunden.");
    Serial.print("Station IP: ");
    Serial.println(WiFi.localIP());
    apModeActive = false;
    // DNS nur stoppen, wenn evtl. aktiv (sicherheitsmaßnahme)
    dnsServer.stop();
    startStationServices();
    return;
  }
  startAccessPoint();
}

void setup() {
  initPersistent();
  Serial.begin(115200);
  delay(100);
  // Boot button input
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  connectWiFiWithFallback();
}

void loop() {
  // Boot button handling: detect quick successive presses to trigger factory reset
  bool state = digitalRead(BOOT_BUTTON_PIN);
  unsigned long now = millis();
  if (state != _lastButtonState) {
    _lastButtonChange = now;
    _lastButtonState = state;
  }
  // pressed when LOW (INPUT_PULLUP)
  if (!state && (now - _lastButtonChange) > BOOT_BUTTON_DEBOUNCE_MS && !_lastPressedHandled) {
    // first press in sequence
    if (_firstBootPressTime == 0 || (now - _firstBootPressTime) > BOOT_BUTTON_WINDOW_MS) {
      _firstBootPressTime = now;
      _bootPressCount = 0;
    }
    _bootPressCount++;
    _lastPressedHandled = true;
    Serial.print("Boot button press count: "); Serial.println(_bootPressCount);
    if (_bootPressCount >= BOOT_BUTTON_REQUIRED) {
      performFactoryResetImmediate();
    }
  }
  if (state && _lastPressedHandled) {
    _lastPressedHandled = false; // release
  }
  if (apModeActive) {
    dnsServer.processNextRequest();
  }
  // UDP discovery responder: check for incoming packets and reply with our IP
  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char incoming[256];
    int len = udp.read(incoming, 255);
    if (len > 0) incoming[len] = 0;
    IPAddress remote = udp.remoteIP();
    unsigned int rport = udp.remotePort();
    String replyIP = apModeActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    Serial.print("UDP discovery packet from "); Serial.print(remote); Serial.print(":"); Serial.println(rport);
    Serial.print("Replying with IP: "); Serial.println(replyIP);
    udp.beginPacket(remote, rport);
    udp.print(replyIP);
    udp.endPacket();
  }
  // Webserver-Client immer bedienen (AP oder STA)
  server.handleClient();
}
