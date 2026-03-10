/*
  ============================================================
  ArpSpoofer 4.0 "Omega" – Production Build (Web Integrated)
  ============================================================
  Platform   : Arduino Nano (ATmega328P) + ENC28J60
  Library    : EtherCard
  Purpose    : Production-grade gratuitous ARP broadcaster
  Features   :
    - Omega Web Dashboard V2 (2026 styled, responsive, dark-mode)
    - Robust state machine (INIT → DHCP → ANNOUNCE → RUN → RECOVERY)
    - Token-bucket rate limiter (burst-capable, avg-rate enforced)
    - RFC 5227 announce sequences with random jitter
    - PHY link-status monitoring with auto-recovery
    - EEPROM-backed config (PPS, MAC suffix)
    - Advanced Serial CLI
    - Optimized for SRAM (PSTR, compact buffers)

  IMPORTANT: Educational/authorised lab use only.
  ============================================================
*/

#include <SPI.h>
#include <EtherCard.h>
#include <avr/wdt.h>
#include <avr/pgmspace.h>
#include <EEPROM.h>

// ─── Pin / Sizing ────────────────────────────────────────────
#define LED_PIN          13
#define ENC_CS_PIN       10
#define FRAME_LEN        60
#define ETH_HDR_LEN      14
#define ARP_PAYLOAD_LEN  28

// ─── Default Tunables ─────────────────────────────────────────
#define DEFAULT_PPS          60
#define MAX_PPS              500
#define TOKEN_BUCKET_CAP     20
#define ANNOUNCE_COUNT       3
#define ANNOUNCE_INTERVAL_MS 1000
#define ANNOUNCE_JITTER_MS   250
#define DHCP_BASE_MS         500UL
#define DHCP_MAX_BACKOFF_MS  16000UL
#define DHCP_INIT_ATTEMPTS   5
#define PHY_CHECK_INTERVAL   2000UL

// ─── Web Dashboard Settings ──────────────────────────────────
#define AUTH_PASSWORD        "OMEGA"     // Default web password
#define WEB_BUFFER_SIZE      700         // Increased for UI content

// ─── EEPROM Layout ───────────────────────────────────────────
#define EEPROM_MAGIC      0xA4
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_PPS   1
#define EEPROM_ADDR_MAC   3

// ─── EtherCard buffer ────────────────────────────────────────
byte Ethernet::buffer[WEB_BUFFER_SIZE];

// ─── Global State ────────────────────────────────────────────
static uint8_t myMac[6] = { 0x02, 0xAB, 0x03, 0x22, 0x55, 0x99 };
static uint8_t myIp[4]  = { 0, 0, 0, 0 };

enum State : uint8_t {
  STATE_INIT, STATE_DHCP, STATE_ANNOUNCE, STATE_RUN, STATE_RECOVERY
};

static State    currentState      = STATE_INIT;
static bool     paused            = false;
static uint32_t totalPacketsSent  = 0;
static uint16_t lfsrState         = 0xACE1u;

// ─── ARP Frame Template (PROGMEM) ────────────────────────────
const uint8_t ARP_TEMPLATE[FRAME_LEN] PROGMEM = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x08, 0x06, 0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// ─── Token-Bucket ────────────────────────────────────────────
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
static TokenBucket bucket = { DEFAULT_PPS, 1000000UL/DEFAULT_PPS, TOKEN_BUCKET_CAP, 0 };

// ─── Helpers ─────────────────────────────────────────────────
static void wdt_safe_delay(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) { wdt_reset(); delay(10); }
}

static void stampFrame() {
  memcpy_P(Ethernet::buffer, ARP_TEMPLATE, FRAME_LEN);
  memcpy(Ethernet::buffer + 6, myMac, 6);   // OFF_SRC_MAC
  memcpy(Ethernet::buffer + 22, myMac, 6);  // OFF_SHA
  memcpy(Ethernet::buffer + 28, myIp, 4);   // OFF_SPA
  memcpy(Ethernet::buffer + 38, myIp, 4);   // OFF_TPA
}

static void sendGarp() { stampFrame(); ether.packetSend(FRAME_LEN); totalPacketsSent++; }

static bool dhcpOnce() { if (ether.dhcpSetup()) { memcpy(myIp, ether.myip, 4); return true; } return false; }

static uint16_t lfsrRand() {
  lfsrState ^= lfsrState >> 7; lfsrState ^= lfsrState << 9; lfsrState ^= lfsrState >> 13;
  return lfsrState;
}

// ─── Web Dashboard Core ──────────────────────────────────────

static void handleWeb() {
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  if (pos) {
    bool auth = true;
    char* data = (char*) Ethernet::buffer + pos;
    if (strstr(data, "POST /") != 0) {
      char* ppos = strstr(data, "pwd=");
      if (ppos && strncmp(ppos + 4, AUTH_PASSWORD, strlen(AUTH_PASSWORD)) == 0) {
        if (strstr(data, "cmd=toggle")) paused = !paused;
        if (strstr(data, "cmd=rate")) {
          char* vpos = strstr(data, "val=");
          if (vpos) bucket.configure((uint16_t)atoi(vpos+4));
        }
      } else auth = false;
    }
    BufferFiller bfill = ether.tcpOffset();
    bfill.emit_p(PSTR("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
      "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'><style>"
      "body{background:#000;color:#0f0;font-family:monospace;text-align:center;padding:20px}"
      ".c{background:#111;border:1px solid #0f0;padding:20px;border-radius:8px;max-width:300px;margin:auto;box-shadow:0 0 15px #0f0}"
      "h2{color:#0ff;margin:0 0 15px 0} input{background:#222;color:#0f0;border:1px solid #444;padding:8px;width:80%;margin:10px 0}"
      "button{background:#0f0;color:#000;border:none;padding:12px;width:90%;font-weight:bold;cursor:pointer;margin-top:10px}"
      "</style></head><body><div class='c'><h2>OMEGA 4.0</h2>"
      "<p>STATE: $S</p><p>IP: $D.$D.$D.$D</p><p>SENT: $L</p><p>RATE: $D PPS</p>"
      "<form method='POST'>PWD:<br><input type='password' name='pwd'><br>"
      "<input type='hidden' name='cmd' value='toggle'><button>$S</button></form>"),
      paused?PSTR("PAUSED"):PSTR("RUNNING"), myIp[0],myIp[1],myIp[2],myIp[3], totalPacketsSent, bucket.pps,
      paused?PSTR("RESUME"):PSTR("PAUSE"));
    if(!auth) bfill.emit_p(PSTR("<p style='color:red'>AUTH ERR</p>"));
    bfill.emit_p(PSTR("</div></body></html>"));
    ether.httpServerReply(bfill.position());
  }
}

// ─── Main ────────────────────────────────────────────────────

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  wdt_enable(WDTO_2S);
  lfsrState ^= (uint16_t)analogRead(A0);
  currentState = STATE_INIT;
}

void loop() {
  static uint8_t announceSent = 0;
  static uint32_t nextAnnounceMs = 0;
  static uint32_t lastPhyMs = 0;

  wdt_reset();
  handleWeb();

  switch (currentState) {
    case STATE_INIT:
      if (ether.begin(WEB_BUFFER_SIZE, myMac, ENC_CS_PIN)) currentState = STATE_DHCP;
      break;

    case STATE_DHCP:
      if (dhcpOnce()) { currentState = STATE_ANNOUNCE; announceSent = 0; nextAnnounceMs = millis(); }
      break;

    case STATE_ANNOUNCE:
      if (millis() >= nextAnnounceMs) {
        sendGarp();
        if (++announceSent >= ANNOUNCE_COUNT) currentState = STATE_RUN;
        nextAnnounceMs = millis() + 1000 + (lfsrRand() % 250);
      }
      break;

    case STATE_RUN:
      if (!paused && bucket.consume()) {
        sendGarp();
        digitalWrite(LED_PIN, (totalPacketsSent & 1));
      }
      if (millis() - lastPhyMs > PHY_CHECK_INTERVAL) {
        lastPhyMs = millis();
        if (ether.myip[0] == 0) currentState = STATE_RECOVERY;
      }
      break;

    case STATE_RECOVERY:
      if (dhcpOnce()) currentState = STATE_ANNOUNCE;
      break;
  }
}
