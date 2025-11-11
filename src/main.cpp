#include <Arduino.h>
#include <EEPROM.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#elif defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#endif

const int EEPROM_SIZE = 512;

void initPersistent() {
#if defined(ESP8266) || defined(ESP32)
  EEPROM.begin(EEPROM_SIZE);
#endif
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

static void handleRoot() {
  server.send(200, "text/html",
  "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<style>"
  "html,body{height:100%;margin:0}body{display:flex;align-items:center;justify-content:center;"
  "background:linear-gradient(135deg,#36CAFF11,#E566FF11);font-family:Arial,Helvetica,sans-serif}"
  ".card{background:#fff;padding:24px;border-radius:12px;box-shadow:0 6px 20px rgba(0,0,0,0.12);width:360px;max-width:90%}"
  "h1{margin:0 0 12px;font-size:20px;color:#36CAFF;text-align:center}"
  "input{width:100%;padding:10px;border:1px solid #e6e6e6;border-radius:8px;margin:8px 0;box-sizing:border-box}"
  "button{width:100%;padding:12px;border:none;border-radius:8px;background:linear-gradient(90deg,#36CAFF,#E566FF);color:#fff;font-weight:600;cursor:pointer}"
  "button:active{opacity:0.95}"
  "</style></head><body>"
  "<div class='card'>"
  "<h1>WLAN einrichten</h1>"
  "<form method='POST' action='/save'>"
  "<input name='ssid' placeholder='WLAN SSID'><br>"
  "<input name='pass' placeholder='WLAN Passwort' type='password'><br><br>"
  "<button type='submit'>Speichern</button>"
  "</form></div></body></html>"
  );
}

static void handleSave() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  persistSave("wifi-ssid", s);
  persistSave("wifi-password", p);
  server.send(200, "text/plain", "OK, reboot...");
  delay(800);
  ESP.restart();
}

static void handleFactoryReset() {
  Serial.println("Factory Reset: Lösche persistente Daten...");
  server.send(200, "text/plain", "Factory Reset wird ausgef&uuml;hrt, reboot...");
  // EEPROM vollständig zurücksetzen
  for (int i = 0; i < EEPROM_SIZE; ++i) {
    EEPROM.write(i, 0xFF);
  }
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
  delay(800);
  ESP.restart();
}

static void startAccessPoint() {
  const char* ap_ssid = "LeviCube-Setup";

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(ap_ssid);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/factoryReset", handleFactoryReset);

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
  // optional: keep other handlers minimal
  server.begin();
  Serial.println("Webserver im Station-Modus gestartet.");
  Serial.print("Station IP: ");
  Serial.println(WiFi.localIP());
}

static bool tryConnectWithTimeout(const String& ssid, const String& pass, unsigned long timeoutMs) {
#if defined(ESP8266) || defined(ESP32)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
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
  connectWiFiWithFallback();
}

void loop() {
  if (apModeActive) {
    dnsServer.processNextRequest();
  }
  // Webserver-Client immer bedienen (AP oder STA)
  server.handleClient();
}
