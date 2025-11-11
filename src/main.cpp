#include <Arduino.h>
#include <EEPROM.h>

const int EEPROM_SIZE = 512; // anpassen nach Bedarf

void initPersistent() {
#if defined(ESP8266) || defined(ESP32)
  EEPROM.begin(EEPROM_SIZE);
#endif
}

// Speichert Key und Value als: key\0value\0 (anhängen). Keine Null-Bytes im Text erlaubt.
bool persistSave(const String& key, const String& value) {
  if (key.length() == 0) return false;
  int addr = 0;
  // Ende finden
  while (addr < EEPROM_SIZE) {
    if (EEPROM.read(addr) == 0xFF) break;
    while (addr < EEPROM_SIZE && EEPROM.read(addr) != 0) addr++; // key
    if (addr >= EEPROM_SIZE) return false;
    addr++; // skip '\0'
    while (addr < EEPROM_SIZE && EEPROM.read(addr) != 0) addr++; // value
    if (addr >= EEPROM_SIZE) return false;
    addr++; // skip '\0'
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
  return String(); // leer = nicht gefunden
}

void setup() {
  Serial.begin(115200);
  initPersistent();

  // kurzes Beispiel
  persistSave("name", "LeviCube");
  String val = persistRead("name");
  Serial.println(val);
}

void loop() {
  // nichts
}