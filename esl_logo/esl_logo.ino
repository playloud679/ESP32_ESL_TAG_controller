/* Logo steo lab single shot su ESL NEMR (BWR 400x300) via BLE da ESP32-C3.
 * Il framebuffer e' in logo_frame.h, generato da logo2esl.py:
 *   python3 logo2esl.py <immagine> logo_frame.h anteprima.png
 * Protocollo come esl_clock: FEF1 comandi, FEF2 dati.
 */

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include "logo_frame.h"

static const char *TARGET_ADDR = "ff:ff:94:20:55:69";

static BLEUUID SERVICE_UUID("0000fef0-0000-1000-8000-00805f9b34fb");
static BLEUUID CMD_UUID("0000fef1-0000-1000-8000-00805f9b34fb");
static BLEUUID IMG_UUID("0000fef2-0000-1000-8000-00805f9b34fb");

static uint8_t frame[sizeof(LOGO_FRAME)];

static volatile bool rxReady = false;
static uint8_t rxBuf[32];
static volatile size_t rxLen = 0;

static void onNotify(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  rxLen = len > sizeof(rxBuf) ? sizeof(rxBuf) : len;
  memcpy(rxBuf, data, rxLen);
  rxReady = true;
}

/* ---------------------------------------------------------- protocollo */
static BLERemoteCharacteristic *cmdChr, *imgChr;

static int request(BLERemoteCharacteristic *chr, uint8_t *data, size_t len,
                   uint8_t *resp, size_t respMax, uint32_t waitMs) {
  rxReady = false;
  if (!chr->writeValue(data, len, chr->canWrite()))
    return -1;
  uint32_t t0 = millis();
  while (!rxReady) {
    if (millis() - t0 > waitMs)
      return -2;
    delay(5);
  }
  size_t n = rxLen < respMax ? rxLen : respMax;
  memcpy(resp, rxBuf, n);
  return n;
}

static int sendFrame(uint8_t *buf, uint32_t size) {
  uint8_t r[8];
  uint8_t q1[1] = {1};
  if (request(cmdChr, q1, 1, r, 8, 2000) < 3 || r[0] != 1)
    return -1;
  uint16_t blockSize = r[1] | (r[2] << 8);
  Serial.printf("blockSize=%u\n", blockSize);

  uint8_t q2[8] = {2, 0, 0, 0, 0, 0, 0, 0};
  memcpy(q2 + 1, &size, 4);
  if (request(cmdChr, q2, 8, r, 8, 2000) < 2 || r[0] != 2 || r[1] != 0)
    return -2;

  uint8_t q3[1] = {3};
  if (request(cmdChr, q3, 1, r, 8, 2000) < 2 || r[0] != 5 || r[1] != 0)
    return -3;

  uint32_t chunkSize = blockSize - 4;
  uint8_t pkt[512];
  uint32_t chunk = 0;
  for (;;) {
    uint32_t ofs = chunk * chunkSize;
    if (ofs >= size)
      return -5;
    uint32_t sz = min(chunkSize, size - ofs);
    memcpy(pkt, &chunk, 4);
    memcpy(pkt + 4, buf + ofs, sz);
    int n = -1;
    for (int i = 0; i < 3 && n < 2; i++)
      n = request(imgChr, pkt, sz + 4, r, 8, 10000);
    if (n < 2 || r[0] != 5)
      return -4;
    if (r[1] == 8)
      return 0;   /* trasferimento completato */
    if (n < 6)
      return -6;
    memcpy(&chunk, r + 2, 4);
  }
}

static bool updateTag() {
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  BLEScanResults *res = scan->start(10, false);
  BLEAdvertisedDevice *dev = nullptr;
  BLEAddress target(TARGET_ADDR);
  for (int i = 0; i < res->getCount(); i++) {
    BLEAdvertisedDevice d = res->getDevice(i);
    if (d.getAddress().equals(target)) {
      dev = new BLEAdvertisedDevice(d);
      break;
    }
  }
  scan->clearResults();
  if (!dev) {
    Serial.println("tag non trovato");
    return false;
  }

  bool ok = false;
  BLEClient *client = BLEDevice::createClient();
  if (client->connect(dev)) {
    Serial.printf("connesso, MTU=%u\n", client->getMTU());
    BLERemoteService *svc = client->getService(SERVICE_UUID);
    cmdChr = svc ? svc->getCharacteristic(CMD_UUID) : nullptr;
    imgChr = svc ? svc->getCharacteristic(IMG_UUID) : nullptr;
    if (cmdChr && imgChr) {
      cmdChr->registerForNotify(onNotify);
      imgChr->registerForNotify(onNotify);
      delay(300);   /* senza pausa la prima risposta va persa */
      uint32_t t0 = millis();
      int r = sendFrame(frame, sizeof(frame));
      Serial.printf("sendFrame=%d in %lu ms\n", r, millis() - t0);
      ok = r == 0;
    } else {
      Serial.println("servizio FEF0 non trovato");
    }
    client->disconnect();
  } else {
    Serial.println("connessione fallita");
  }
  delete client;
  delete dev;
  return ok;
}

/* ------------------------------------------------------------ main */
static bool sent = false;

void setup() {
  Serial.begin(115200);
  memcpy(frame, LOGO_FRAME, sizeof(frame));
  BLEDevice::init("");
  BLEDevice::setMTU(247);
}

void loop() {
  if (sent) {
    delay(1000);
    return;
  }
  Serial.println("invio logo");
  sent = updateTag();
  if (sent)
    Serial.println("fatto, nessun altro aggiornamento");
  else
    delay(5000);
}
