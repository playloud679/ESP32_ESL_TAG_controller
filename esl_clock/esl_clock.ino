/* Orologio su ESL NEMR (BWR 250x128) via BLE da ESP32-C3.
 * Protocollo NEMR: servizio FEF0,
 * FEF1 = comandi (write + notify), FEF2 = dati immagine.
 * Ora: da seriale con "T<epoch locale>\n", altrimenti ora di compilazione.
 */

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <sys/time.h>
#include <time.h>

static const char *TARGET_ADDR = "ff:ff:94:20:55:69";
static const uint16_t W = 400, H = 300;   /* typ pixelSize=2 (typ2 dice 250x128, errato) */
static const uint32_t UPDATE_MINUTES = 1;
/* orientamento: da regolare se il testo appare specchiato/capovolto */
static const bool FLIP_X = false;
static const bool FLIP_Y = false;

static BLEUUID SERVICE_UUID("0000fef0-0000-1000-8000-00805f9b34fb");
static BLEUUID CMD_UUID("0000fef1-0000-1000-8000-00805f9b34fb");
static BLEUUID IMG_UUID("0000fef2-0000-1000-8000-00805f9b34fb");

static GFXcanvas1 cBlack(W, H);
static GFXcanvas1 cRed(W, H);
/* il tag legge per righe: 8 pixel per byte, bit7 = sinistra.
 * Dimensione inviata (H+7)/8*W per piano, accettata dal tag */
static const uint32_t ROW_BYTES = (W + 7) / 8;
static const uint32_t PLANE = (H + 7) / 8 * W;
static const uint32_t RED_OFS = ROW_BYTES * H;   /* il rosso segue subito il B/W */
static uint8_t frame[PLANE * 2];   /* piano B/W + piano rosso */

static volatile bool rxReady = false;
static uint8_t rxBuf[32];
static volatile size_t rxLen = 0;

static void onNotify(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  rxLen = len > sizeof(rxBuf) ? sizeof(rxBuf) : len;
  memcpy(rxBuf, data, rxLen);
  rxReady = true;
}

/* ---------------------------------------------------------------- ora */
static void setFromCompileTime() {
  static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  struct tm t = {};
  sscanf(__DATE__, "%3s %d %d", mon, &t.tm_mday, &t.tm_year);
  sscanf(__TIME__, "%d:%d:%d", &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_mon = (strstr(MONTHS, mon) - MONTHS) / 3;
  t.tm_year -= 1900;
  struct timeval tv = { mktime(&t), 0 };
  settimeofday(&tv, nullptr);
}

static bool pollSerialTime() {
  static String line;
  bool set = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 1 && line[0] == 'T') {
        struct timeval tv = { (time_t) atoll(line.c_str() + 1), 0 };
        settimeofday(&tv, nullptr);
        Serial.println("ora impostata da seriale");
        set = true;
      }
      line = "";
    } else {
      line += c;
    }
  }
  return set;
}

/* ------------------------------------------------------------ disegno */
static void centerText(GFXcanvas1 &c, const GFXfont *f, uint8_t size, const char *s,
                       int16_t baseline) {
  int16_t x1, y1;
  uint16_t w, h;
  c.setFont(f);
  c.setTextSize(size);
  c.getTextBounds(s, 0, baseline, &x1, &y1, &w, &h);
  c.setCursor((W - w) / 2 - x1, baseline);
  c.print(s);
}

static void render(const struct tm &t) {
  static const char *DAYS[] = {"Domenica", "Lunedi", "Martedi", "Mercoledi",
                               "Giovedi", "Venerdi", "Sabato"};
  char hhmm[6], date[12];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &t);
  strftime(date, sizeof(date), "%d/%m/%Y", &t);

  cBlack.fillScreen(0);
  cRed.fillScreen(0);
  /* banda rossa con giorno in bianco */
  cRed.fillRect(0, 0, W, 64, 1);
  cRed.setTextColor(0);
  centerText(cRed, &FreeSansBold24pt7b, 1, DAYS[t.tm_wday], 48);
  cBlack.setTextColor(1);
  centerText(cBlack, &FreeSansBold24pt7b, 2, hhmm, 185);
  centerText(cBlack, &FreeSansBold24pt7b, 1, date, 262);

  /* BW 1=bianco, rosso 1=rosso; sotto il rosso il BW resta bianco */
  memset(frame, 0, sizeof(frame));
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      int sx = FLIP_X ? W - 1 - x : x;
      int sy = FLIP_Y ? H - 1 - y : y;
      uint32_t ofs = y * ROW_BYTES + x / 8;
      uint8_t bit = 0x80 >> (x % 8);
      bool red = cRed.getPixel(sx, sy);
      if (red)
        frame[RED_OFS + ofs] |= bit;
      if (red || !cBlack.getPixel(sx, sy))
        frame[ofs] |= bit;
    }
  }
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
static int lastSlot = -1;

void setup() {
  Serial.begin(115200);
  setFromCompileTime();
  /* qualche secondo per ricevere l'ora dal PC */
  uint32_t t0 = millis();
  while (millis() - t0 < 5000 && !pollSerialTime())
    delay(10);
  BLEDevice::init("");
  BLEDevice::setMTU(247);
}

void loop() {
  if (pollSerialTime())
    lastSlot = -1;   /* forza aggiornamento */
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int slot = (t.tm_hour * 60 + t.tm_min) / UPDATE_MINUTES;
  if (slot != lastSlot) {
    render(t);
    Serial.printf("aggiorno: %02d:%02d %02d/%02d/%04d\n", t.tm_hour, t.tm_min,
                  t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    if (updateTag())
      lastSlot = slot;
    else
      delay(5000);
  }
  delay(200);
}
