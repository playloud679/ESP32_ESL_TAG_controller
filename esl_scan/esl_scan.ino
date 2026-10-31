#include <BLEDevice.h>
#include <BLEScan.h>

void setup() {
  Serial.begin(115200);
  delay(1500);
  BLEDevice::init("");
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
}

void loop() {
  BLEScanResults *res = BLEDevice::getScan()->start(5, false);
  for (int i = 0; i < res->getCount(); i++) {
    BLEAdvertisedDevice d = res->getDevice(i);
    String m = d.getManufacturerData();
    String name = d.getName();
    bool esl = m.length() >= 2 && (uint8_t)m[0] == 0x53 && (uint8_t)m[1] == 0x50;
    if (!esl && name.indexOf("ESL") < 0 && name.indexOf("94") < 0) continue;
    Serial.printf("DEV %s rssi=%d name='%s' msd=", d.getAddress().toString().c_str(), d.getRSSI(), name.c_str());
    for (int k = 0; k < m.length(); k++) Serial.printf("%02X", (uint8_t)m[k]);
    Serial.println();
  }
  Serial.println("--");
  BLEDevice::getScan()->clearResults();
}
