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
  "<html><body><form method='POST' action='/save'>"
  "<input name='ssid' placeholder='WLAN SSID'><br>"
  "<input name='pass' placeholder='WLAN Passwort' type='password'><br><br>"
  "<button type='submit'>Speichern</button>"
  "</form></body></html>");
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

static void startAccessPoint() {
  const char* ap_ssid = "LeviCube-Setup";

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(ap_ssid);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);

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
