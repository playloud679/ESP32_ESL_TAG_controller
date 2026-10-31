/* Poster "synthwave" single shot su ESL NEMR (BWR 400x300) via BLE da ESP32-C3.
 * Disegna una sola volta data e ora e le invia al tag, poi resta fermo.
 * Protocollo e formato come esl_clock: FEF1 comandi, FEF2 dati, piani per righe.
 * Ora: da seriale con "T<epoch locale>\n" entro 5 s dall'avvio, altrimenti
 * ora di compilazione.
 */

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>

static const char *TARGET_ADDR = "ff:ff:94:20:55:69";
static const uint16_t W = 400, H = 300;
static const int HORIZON = 200;
static const int SUN_X = 200, SUN_R = 92;

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
enum { WHITE, BLACK, RED };

static void plot(int x, int y, uint8_t c) {
  cBlack.drawPixel(x, y, c == BLACK);
  cRed.drawPixel(x, y, c == RED);
}

static void hline(int x0, int x1, int y, uint8_t c) {
  for (int x = x0; x <= x1; x++)
    plot(x, y, c);
}

static const uint8_t BAYER[8][8] = {
  { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
  {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
  { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
  {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

static bool dither(int x, int y, float level) {
  return BAYER[y & 7][x & 7] < level * 64;
}

static float sunDist(int x, int y) {
  return sqrtf((float) (x - SUN_X) * (x - SUN_X) + (float) (y - HORIZON) * (y - HORIZON));
}

/* cielo: nero in alto che sfuma verso l'orizzonte, alone rosso attorno al sole */
static void drawSky() {
  for (int y = 0; y < HORIZON; y++) {
    float level = 0.92f * powf(1.0f - (float) y / HORIZON, 1.5f);
    for (int x = 0; x < W; x++) {
      float d = sunDist(x, y);
      float glow = (d - SUN_R) / 30.0f;
      if (glow >= 0 && glow < 1 && dither(x + 3, y + 5, 0.75f * (1 - glow)))
        plot(x, y, RED);
      else
        plot(x, y, dither(x, y, level) ? BLACK : WHITE);
    }
  }
}

static void sparkle(int x, int y, int arm) {
  for (int i = -arm; i <= arm; i++) {
    plot(x + i, y, WHITE);
    plot(x, y + i, WHITE);
  }
  for (int i = -arm / 3; i <= arm / 3; i++) {
    plot(x + i, y + i, WHITE);
    plot(x + i, y - i, WHITE);
  }
}

static void drawStars() {
  uint32_t seed = 0x5EED1234;
  auto rnd = [&seed](int n) {
    seed = seed * 1664525u + 1013904223u;
    return (int) ((seed >> 8) % n);
  };
  for (int i = 0; i < 90; i++) {
    int x = rnd(W), y = rnd(110);
    if (sunDist(x, y) < SUN_R + 40)
      continue;
    plot(x, y, WHITE);
    if (rnd(4) == 0) {
      plot(x + 1, y, WHITE);
      plot(x, y + 1, WHITE);
      plot(x + 1, y + 1, WHITE);
    }
  }
  sparkle(48, 26, 6);
  sparkle(352, 40, 7);
  sparkle(300, 14, 4);
  sparkle(96, 70, 4);
}

/* sole pieno con tagli orizzontali sempre piu' larghi verso l'orizzonte */
static void drawSun() {
  for (int y = HORIZON - SUN_R; y < HORIZON; y++) {
    int half = (int) sqrtf((float) SUN_R * SUN_R - (float) (HORIZON - y) * (HORIZON - y));
    bool cut = false;
    int rel = y - (HORIZON - 56);
    if (rel >= 0)
      cut = (rel % 11) < 1 + rel / 11;
    hline(SUN_X - half, SUN_X + half, y, cut ? WHITE : RED);
  }
}

static int ridge(const int (*pts)[2], int n, int x) {
  for (int i = 0; i + 1 < n; i++)
    if (x >= pts[i][0] && x <= pts[i + 1][0]) {
      int x0 = pts[i][0], y0 = pts[i][1], x1 = pts[i + 1][0], y1 = pts[i + 1][1];
      return y0 + (y1 - y0) * (x - x0) / (x1 - x0 ? x1 - x0 : 1);
    }
  return HORIZON;
}

static void drawRange(const int (*pts)[2], int n) {
  for (int x = pts[0][0]; x <= pts[n - 1][0]; x++) {
    int top = ridge(pts, n, x);
    for (int y = top; y < HORIZON; y++)
      plot(x, y, BLACK);
    plot(x, top, RED);
    plot(x, top + 1, RED);
  }
}

static void drawMountains() {
  static const int left[][2] = {
    {0, 150}, {20, 138}, {42, 158}, {70, 128}, {96, 160}, {112, 152}, {136, 182}, {150, 200}
  };
  static const int right[][2] = {
    {250, 200}, {266, 180}, {290, 150}, {306, 162}, {334, 124}, {362, 158}, {382, 142}, {399, 152}
  };
  drawRange(left, sizeof(left) / sizeof(left[0]));
  drawRange(right, sizeof(right) / sizeof(right[0]));
}

/* pavimento nero con griglia rossa in prospettiva */
static void drawFloor() {
  cBlack.fillRect(0, HORIZON, W, H - HORIZON, 1);
  cRed.fillRect(0, HORIZON, W, H - HORIZON, 0);
  cRed.fillRect(0, HORIZON, W, 3, 1);
  for (int k = 1;; k++) {
    int y = HORIZON + 3 + 3 * k * k;
    if (y >= H)
      break;
    cRed.fillRect(0, y, W, k >= 4 ? 2 : 1, 1);
  }
  for (int i = -14; i <= 14; i++) {
    int xb = SUN_X + i * 46;
    int xs = SUN_X + (xb - SUN_X) * 4 / 100;
    cRed.drawLine(xs, HORIZON + 4, xb, H - 1, 1);
    if (abs(i) >= 2)
      cRed.drawLine(xs + (i > 0 ? 1 : -1), HORIZON + 4, xb + (i > 0 ? 1 : -1), H - 1, 1);
  }
}

static void textBounds(const GFXfont *f, uint8_t size, const char *s, int16_t *x1,
                       int16_t *y1, uint16_t *w, uint16_t *h) {
  cBlack.setFont(f);
  cBlack.setTextSize(size);
  cBlack.getTextBounds(s, 0, 0, x1, y1, w, h);
}

static void printAt(const GFXfont *f, uint8_t size, const char *s, int x, int y, uint8_t c) {
  cBlack.setFont(f);
  cBlack.setTextSize(size);
  cBlack.setTextColor(c == BLACK);
  cBlack.setCursor(x, y);
  cBlack.print(s);
  cRed.setFont(f);
  cRed.setTextSize(size);
  cRed.setTextColor(c == RED);
  cRed.setCursor(x, y);
  cRed.print(s);
}

/* ora grande nera con contorno bianco spesso, sopra il sole */
static void drawTime(const char *hhmm) {
  int16_t x1, y1;
  uint16_t w, h;
  textBounds(&FreeSansBold24pt7b, 2, hhmm, &x1, &y1, &w, &h);
  int x = (W - w) / 2 - x1, y = 10 - y1;
  for (int dy = -4; dy <= 4; dy++)
    for (int dx = -4; dx <= 4; dx++)
      if (dx * dx + dy * dy <= 16)
        printAt(&FreeSansBold24pt7b, 2, hhmm, x + dx, y + dy, WHITE);
  printAt(&FreeSansBold24pt7b, 2, hhmm, x, y, BLACK);
}

/* targa con la data: nero, bordo rosso, testo bianco */
static void drawDate(const char *s) {
  const GFXfont *f = &FreeSansBold12pt7b;
  int16_t x1, y1;
  uint16_t w, h;
  textBounds(f, 1, s, &x1, &y1, &w, &h);
  if (w > 330) {
    f = &FreeSansBold9pt7b;
    textBounds(f, 1, s, &x1, &y1, &w, &h);
  }
  int bw = w + 32, bh = h + 18;
  int bx = (W - bw) / 2, by = 266 - bh / 2;
  cBlack.fillRect(bx, by, bw, bh, 1);
  cRed.fillRect(bx, by, bw, bh, 0);
  for (int i = 0; i < 2; i++)
    cRed.drawRect(bx + i, by + i, bw - 2 * i, bh - 2 * i, 1);
  cRed.drawRect(bx + 4, by + 4, bw - 8, bh - 8, 1);
  printAt(f, 1, s, (W - w) / 2 - x1, by + (bh - h) / 2 - y1, WHITE);
}

static void render(const struct tm &t) {
  static const char *DAYS[] = {"DOMENICA", "LUNEDI", "MARTEDI", "MERCOLEDI",
                               "GIOVEDI", "VENERDI", "SABATO"};
  static const char *MONTHS[] = {"GENNAIO", "FEBBRAIO", "MARZO", "APRILE", "MAGGIO",
                                 "GIUGNO", "LUGLIO", "AGOSTO", "SETTEMBRE", "OTTOBRE",
                                 "NOVEMBRE", "DICEMBRE"};
  char hhmm[6], date[40];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &t);
  snprintf(date, sizeof(date), "%s %d %s %d", DAYS[t.tm_wday], t.tm_mday,
           MONTHS[t.tm_mon], t.tm_year + 1900);

  drawSky();
  drawStars();
  drawSun();
  drawMountains();
  drawFloor();
  drawTime(hhmm);
  drawDate(date);

  /* BW 1=bianco, rosso 1=rosso; sotto il rosso il BW resta bianco */
  memset(frame, 0, sizeof(frame));
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint32_t ofs = y * ROW_BYTES + x / 8;
      uint8_t bit = 0x80 >> (x % 8);
      bool red = cRed.getPixel(x, y);
      if (red)
        frame[RED_OFS + ofs] |= bit;
      if (red || !cBlack.getPixel(x, y))
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
static bool sent = false;

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
  if (sent) {
    delay(1000);
    return;
  }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  render(t);
  Serial.printf("invio poster: %02d:%02d %02d/%02d/%04d\n", t.tm_hour, t.tm_min,
                t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
  sent = updateTag();
  if (sent)
    Serial.println("fatto, nessun altro aggiornamento");
  else
    delay(5000);
}
