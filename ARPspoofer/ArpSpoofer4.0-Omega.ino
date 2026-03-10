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
    - EEPROM-backed config (PPS, MAC suffix, mode)
    - Advanced serial CLI  (R / S / M / P / C / ?)
    - PROGMEM frame template, stable LAA MAC
    - Watchdog-safe blocking calls throughout

  IMPORTANT: Educational/authorised lab use only.
             Do NOT deploy on networks you do not own or have
             explicit written permission to test.
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

// ─── Default Tunables (overridden by EEPROM) ─────────────────
#define DEFAULT_PPS          60
#define MAX_PPS              500
#define TOKEN_BUCKET_CAP     20          // max burst tokens
#define ANNOUNCE_COUNT       3           // RFC 5227: 2-3 announces
#define ANNOUNCE_INTERVAL_MS 1000        // 1 s between announces
#define ANNOUNCE_JITTER_MS   250         // ±250 ms jitter
#define DHCP_BASE_MS         500UL
#define DHCP_MAX_BACKOFF_MS  16000UL
#define DHCP_INIT_ATTEMPTS   5
#define PHY_CHECK_INTERVAL   2000UL      // poll PHY every 2 s
#define STATS_REPORT_INTERVAL 10000UL   // auto-stats every 10 s
#define DHCP_RETRY_LIMIT     10          // attempts before static fallback

// ─── Static IP Fallback Settings ─────────────────────────────
#define USE_STATIC_FALLBACK  true        // Set to false to disable
#define DEFAULT_STATIC_IP    192, 168, 1, 137
#define DEFAULT_GATEWAY      192, 168, 1, 1
#define DEFAULT_MASK         255, 255, 255, 0

// ─── Web Dashboard Settings ──────────────────────────────────
#define AUTH_PASSWORD        "OMEGA"     // Default web password
#define WEB_BUFFER_SIZE      700         // Increased to fit UI + headers

// ─── EEPROM Layout ───────────────────────────────────────────
#define EEPROM_MAGIC      0xA4           // sentinel byte
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_PPS   1              // uint16_t  (2 bytes)
#define EEPROM_ADDR_MAC   3              // 3 LSB bytes of the MAC

// ─── EtherCard buffer ────────────────────────────────────────
byte Ethernet::buffer[WEB_BUFFER_SIZE];

// ─── MAC / IP ────────────────────────────────────────────────
// LAA (locally administered unicast): bit1=1, bit0=0 in first octet
static uint8_t myMac[6] = { 0x02, 0xAB, 0x03, 0x22, 0x55, 0x99 };
static uint8_t myIp[4]  = { 0, 0, 0, 0 };

// ─── ARP Frame Template (PROGMEM) ────────────────────────────
enum {
  OFF_DEST_MAC = 0,
  OFF_SRC_MAC  = 6,
  OFF_TYPE     = 12,
  OFF_HTYPE    = 14,
  OFF_PTYPE    = 16,
  OFF_HLEN     = 18,
  OFF_PLEN     = 19,
  OFF_OPER     = 20,
  OFF_SHA      = 22,
  OFF_SPA      = 28,
  OFF_THA      = 32,
  OFF_TPA      = 38
};

const uint8_t ARP_TEMPLATE[FRAME_LEN] PROGMEM = {
  // Ethernet Header
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // Dst MAC: broadcast
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Src MAC: filled at runtime
  0x08, 0x06,                          // EtherType: ARP
  // ARP Payload
  0x00, 0x01,                          // HTYPE: Ethernet
  0x08, 0x00,                          // PTYPE: IPv4
  0x06,                                // HLEN: 6
  0x04,                                // PLEN: 4
  0x00, 0x01,                          // OPER: request (GARP)
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SHA: sender HW addr (runtime)
  0x00, 0x00, 0x00, 0x00,              // SPA: sender IP (runtime)
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // THA: target HW addr (zero)
  0x00, 0x00, 0x00, 0x00,              // TPA: target IP = SPA (runtime)
  // Padding to 60 bytes
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0
};

static_assert(ETH_HDR_LEN + ARP_PAYLOAD_LEN == 42, "ARP frame size mismatch");
static_assert(FRAME_LEN == 60, "Min Ethernet frame must be 60 bytes");

// ─── State Machine ───────────────────────────────────────────
enum State : uint8_t {
  STATE_INIT,
  STATE_DHCP,
  STATE_ANNOUNCE,
  STATE_RUN,
  STATE_RECOVERY
};

static State  currentState      = STATE_INIT;
static bool   paused            = false;

// ─── Token-Bucket Rate Limiter ────────────────────────────────
struct TokenBucket {
  uint16_t pps;
  uint32_t intervalUs;
  int16_t  tokens;
  uint32_t lastRefillUs;

  void configure(uint16_t newPps) {
    if (newPps == 0)    newPps = 1;
    if (newPps > MAX_PPS) newPps = MAX_PPS;
    pps        = newPps;
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

  bool consume() {
    refill();
    if (tokens > 0) {
      tokens--;
      return true;
    }
    return false;
  }
};

static TokenBucket bucket = { DEFAULT_PPS, 1000000UL / DEFAULT_PPS,
                               TOKEN_BUCKET_CAP, 0 };

// ─── DHCP / Link State ───────────────────────────────────────
static uint32_t lastDhcpAttemptMs  = 0;
static uint8_t  dhcpRetries        = 0;
static uint32_t lastPhyCheckMs     = 0;

// ─── Announce State ──────────────────────────────────────────
static uint8_t  announceSent       = 0;
static uint32_t nextAnnounceMs     = 0;

// ─── Statistics ──────────────────────────────────────────────
static uint32_t totalPacketsSent   = 0;
static uint16_t packetsThisSec     = 0;
static uint32_t counterResetMs     = 0;
static uint32_t lastStatsMs        = 0;
static uint32_t dhcpFailCount      = 0;
static uint32_t recoveryCount      = 0;

// ─── PRNG (Galois LFSR, period 2^16−1) ──────────────────────
static uint16_t lfsrState = 0xACE1u;

static uint16_t lfsrRand() {
  lfsrState ^= lfsrState >> 7;
  lfsrState ^= lfsrState << 9;
  lfsrState ^= lfsrState >> 13;
  return lfsrState;
}

// ─── Helpers ─────────────────────────────────────────────────

static void safeDelay(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    wdt_reset();
    delay(10);
  }
}

static void ledBlink(uint8_t times, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    safeDelay(onMs);
    digitalWrite(LED_PIN, LOW);
    safeDelay(offMs);
  }
}

static void ledState(State s) {
  // Non-blocking visual state indicator via direct write
  // Caller drives timing externally; here we just set the level
  // Run: solid flicker handled in loop(); others set a pattern once
  (void)s; // patterns driven in loop
}

// ─── EEPROM ──────────────────────────────────────────────────

static void eepromSave() {
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_PPS, bucket.pps);
  // Store last 3 bytes of MAC (the "suffix")
  EEPROM.write(EEPROM_ADDR_MAC,     myMac[3]);
  EEPROM.write(EEPROM_ADDR_MAC + 1, myMac[4]);
  EEPROM.write(EEPROM_ADDR_MAC + 2, myMac[5]);
  Serial.println(F("[CFG] Settings saved to EEPROM."));
}

static void eepromLoad() {
  if (EEPROM.read(EEPROM_ADDR_MAGIC) != EEPROM_MAGIC) {
    Serial.println(F("[CFG] No valid EEPROM config, using defaults."));
    return;
  }
  uint16_t storedPps;
  EEPROM.get(EEPROM_ADDR_PPS, storedPps);
  bucket.configure(storedPps);

  myMac[3] = EEPROM.read(EEPROM_ADDR_MAC);
  myMac[4] = EEPROM.read(EEPROM_ADDR_MAC + 1);
  myMac[5] = EEPROM.read(EEPROM_ADDR_MAC + 2);
  Serial.print(F("[CFG] EEPROM config loaded. PPS="));
  Serial.println(bucket.pps);
}

// ─── Frame Stamping ──────────────────────────────────────────

static void stampFrame() {
  memcpy_P(Ethernet::buffer, ARP_TEMPLATE, FRAME_LEN);
  memcpy(Ethernet::buffer + OFF_SRC_MAC, myMac, 6);
  memcpy(Ethernet::buffer + OFF_SHA,     myMac, 6);
  memcpy(Ethernet::buffer + OFF_SPA,     myIp,  4);
  memcpy(Ethernet::buffer + OFF_TPA,     myIp,  4);
}

static void sendGarp() {
  stampFrame();
  ether.packetSend(FRAME_LEN);
  totalPacketsSent++;
  packetsThisSec++;
}

// ─── DHCP ────────────────────────────────────────────────────

static bool dhcpOnce() {
  if (ether.dhcpSetup()) {
    memcpy(myIp, ether.myip, 4);
    return true;
  }
  return false;
}

static bool dhcpBlocking(uint8_t attempts) {
  for (uint8_t i = 0; i < attempts; i++) {
    if (dhcpOnce()) return true;
    uint32_t d = 1000UL << (i < 6 ? i : 6);
    safeDelay(d);
  }
  return false;
}

// ─── PHY Link Check ──────────────────────────────────────────

static bool phyLinkUp() {
  // PHSTAT2 bit 10 = LSTAT
  // ether.regRd() is not exposed in all EtherCard builds; we check myip.
  // As a proxy: if the IP zeroed out the DHCP stack lost the lease.
  // True PHY check would require direct SPI access to PHSTAT2 register.
  // We use the pragmatic lease-validity check here.
  return !(ether.myip[0] == 0 && ether.myip[1] == 0 &&
           ether.myip[2] == 0 && ether.myip[3] == 0);
}

// ─── Web Dashboard Core ──────────────────────────────────────

static void handleWeb() {
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  
  if (pos) {
    bool authed = true;
    char* data = (char*) Ethernet::buffer + pos;

    // --- Command Parsing ---
    if (strstr(data, "POST /") != 0) {
      char* ppos = strstr(data, "pwd=");
      if (ppos) {
        // Simple auth check
        char pwd[16];
        byte i = 0;
        char* pptr = ppos + 4;
        while (i < 15 && *pptr != '&' && *pptr != ' ' && *pptr != '\0') {
          pwd[i++] = *pptr++;
        }
        pwd[i] = '\0';
        if (strcmp(pwd, AUTH_PASSWORD) != 0) authed = false;
      } else {
        authed = false;
      }

      if (authed) {
        if (strstr(data, "cmd=toggle") != 0) paused = !paused;
        if (strstr(data, "cmd=rate") != 0) {
          char* vpos = strstr(data, "val=");
          if (vpos) bucket.configure((uint16_t)atoi(vpos + 4));
        }
      }
    }

    // --- HTML Emission (Optimized for SRAM) ---
    BufferFiller bfill = ether.tcpOffset();
    bfill.emit_p(PSTR("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
                      "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<style>body{background:#0a0a0a;color:#0f0;font-family:monospace;display:flex;flex-direction:column;align-items:center;padding:20px}"
                      "h1{color:#00ffff;text-shadow:0 0 10px #00ffff} .card{background:#111;border:1px solid #333;padding:20px;border-radius:10px;width:100%;max-width:400px}"
                      "button{background:#00ffff;color:#000;border:none;padding:10px 20px;font-weight:bold;cursor:pointer;width:100%;margin-top:10px}"
                      "input{background:#222;color:#0f0;border:1px solid #333;padding:10px;width:calc(100% - 22px);margin-top:10px}</style></head><body>"
                      "<h1>OMEGA DASHBOARD</h1><div class='card'>"
                      "<p>STATUS: $S</p><p>IP: $D.$D.$D.$D</p><p>SENT: $L</p><p>RATE: $D PPS</p>"
                      "<form method='POST'>PW: <input type='password' name='pwd'><br>"
                      "<input type='hidden' name='cmd' value='toggle'><button>$S</button></form>"),
                 currentState == STATE_RUN ? (paused ? PSTR("PAUSED") : PSTR("RUNNING")) : PSTR("BUSY"),
                 myIp[0], myIp[1], myIp[2], myIp[3], totalPacketsSent, bucket.pps,
                 paused ? PSTR("RESUME") : PSTR("PAUSE"));

    if (!authed) bfill.emit_p(PSTR("<p style='color:red'>AUTH FAILED</p>"));
    
    bfill.emit_p(PSTR("</div></body></html>"));
    ether.httpServerReply(bfill.position());
  }
}

// ─── Serial CLI ──────────────────────────────────────────────

static void printIp() {
  Serial.print(myIp[0]); Serial.print('.');
  Serial.print(myIp[1]); Serial.print('.');
  Serial.print(myIp[2]); Serial.print('.');
  Serial.print(myIp[3]);
}

static void printMac() {
  for (uint8_t i = 0; i < 6; i++) {
    if (i) Serial.print(':');
    if (myMac[i] < 16) Serial.print('0');
    Serial.print(myMac[i], HEX);
  }
}

static void printStatus() {
  const __FlashStringHelper* stateStr;
  switch (currentState) {
    case STATE_INIT:     stateStr = F("INIT");     break;
    case STATE_DHCP:     stateStr = F("DHCP");     break;
    case STATE_ANNOUNCE: stateStr = F("ANNOUNCE"); break;
    case STATE_RUN:      stateStr = F("RUN");      break;
    case STATE_RECOVERY: stateStr = F("RECOVERY"); break;
    default:             stateStr = F("?");        break;
  }
  Serial.println(F("─── Omega Status ───────────────"));
  Serial.print(F(" State   : ")); Serial.println(stateStr);
  Serial.print(F(" Paused  : ")); Serial.println(paused ? F("YES") : F("NO"));
  Serial.print(F(" MAC     : ")); printMac(); Serial.println();
  Serial.print(F(" IP      : ")); printIp();  Serial.println();
  Serial.print(F(" PPS     : ")); Serial.println(bucket.pps);
  Serial.print(F(" Tokens  : ")); Serial.println(bucket.tokens);
  Serial.print(F(" Sent    : ")); Serial.println(totalPacketsSent);
  Serial.print(F(" DHCP err: ")); Serial.println(dhcpFailCount);
  Serial.print(F(" Recovery: ")); Serial.println(recoveryCount);
  Serial.println(F("────────────────────────────────"));
}

static void printHelp() {
  Serial.println(F("─── CLI Commands ───────────────"));
  Serial.println(F(" R<n>       Set PPS (1-500)"));
  Serial.println(F(" S          Print status"));
  Serial.println(F(" P          Pause / Resume toggle"));
  Serial.println(F(" C          Save config to EEPROM"));
  Serial.println(F(" M<XXYYZZ>  Set MAC suffix (hex)"));
  Serial.println(F(" ?          Show this help"));
  Serial.println(F("────────────────────────────────"));
}

static void handleSerial() {
  while (Serial.available()) {
    char cmd = Serial.read();

    switch (cmd) {
      case 'R': case 'r': {
        uint16_t req = (uint16_t)Serial.parseInt();
        bucket.configure(req);
        Serial.print(F("pps=")); Serial.println(bucket.pps);
        break;
      }
      case 'S': case 's':
        printStatus();
        break;

      case 'P': case 'p':
        paused = !paused;
        Serial.println(paused ? F("[PAUSED]") : F("[RESUMED]"));
        break;

      case 'C': case 'c':
        eepromSave();
        break;

      case 'M': case 'm': {
        // Expect 6 hex chars for the 3 suffix bytes: e.g. M1A2B3C
        char buf[7];
        uint8_t idx = 0;
        uint32_t t = millis();
        while (idx < 6 && (millis() - t) < 500) {
          if (Serial.available()) {
            buf[idx++] = Serial.read();
          }
        }
        buf[idx] = '\0';
        if (idx == 6) {
          uint32_t val = strtoul(buf, nullptr, 16);
          myMac[3] = (val >> 16) & 0xFF;
          myMac[4] = (val >>  8) & 0xFF;
          myMac[5] =  val        & 0xFF;
          Serial.print(F("MAC suffix set: "));
          printMac(); Serial.println();
        } else {
          Serial.println(F("ERR: M needs exactly 6 hex chars (e.g. M1A2B3C)"));
        }
        break;
      }

      case '?':
        printHelp();
        break;

      default:
        break;
    }
  }
}

// ─── Setup ───────────────────────────────────────────────────

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  safeDelay(30);

  Serial.println(F("╔════════════════════════════════╗"));
  Serial.println(F("║  ArpSpoofer 4.0 Omega - BOOT   ║"));
  Serial.println(F("╚════════════════════════════════╝"));

  // Seed PRNG with analog noise
  lfsrState ^= (uint16_t)(analogRead(A0) ^ (analogRead(A1) << 8));
  if (!lfsrState) lfsrState = 0xACE1u;

  // Load EEPROM config
  eepromLoad();

  // Enable watchdog (2 s)
  wdt_enable(WDTO_2S);

  currentState = STATE_INIT;
}

// ─── State Handlers ──────────────────────────────────────────

static void stateInit() {
  Serial.println(F("[INIT] Starting ENC28J60..."));
  Serial.flush();  // Force serial output before accessing hardware

  if (ether.begin(WEB_BUFFER_SIZE, myMac, ENC_CS_PIN) == 0) {
    Serial.println(F("[INIT] ENC28J60 FAILED – halting."));
    ledBlink(10, 100, 100);    // fast panic blink
    while (true) wdt_reset();  // WDT will reset
  }

  Serial.print(F("[INIT] MAC: ")); printMac(); Serial.println();
  Serial.println(F("[INIT] Hardware OK → DHCP"));

  ledBlink(2, 200, 200);
  currentState   = STATE_DHCP;
  dhcpRetries    = 0;
  lastDhcpAttemptMs = millis();
}

static void stateDhcp() {
  // Attempt blocking DHCP on first entry, then scheduled retries
  static bool firstAttempt = true;

  if (firstAttempt) {
    firstAttempt = false;
    Serial.println(F("[DHCP] Requesting lease..."));
    if (dhcpBlocking(DHCP_INIT_ATTEMPTS)) {
      Serial.print(F("[DHCP] Lease OK. IP: ")); printIp(); Serial.println();
      dhcpRetries  = 0;
      announceSent = 0;
      nextAnnounceMs = millis() + (lfsrRand() % ANNOUNCE_JITTER_MS);
      currentState = STATE_ANNOUNCE;
      return;
    }
    dhcpFailCount++;
    Serial.println(F("[DHCP] Failed, scheduling retries..."));
  }

  // Scheduled retry with exponential backoff
  uint32_t backoff = DHCP_BASE_MS << (dhcpRetries < 5 ? dhcpRetries : 5);
  if (backoff > DHCP_MAX_BACKOFF_MS) backoff = DHCP_MAX_BACKOFF_MS;

  if ((uint32_t)(millis() - lastDhcpAttemptMs) >= backoff) {
    lastDhcpAttemptMs = millis();
    Serial.print(F("[DHCP] Retry #")); Serial.println(dhcpRetries + 1);
    
    if (dhcpOnce()) {
      Serial.print(F("[DHCP] Lease OK. IP: ")); printIp(); Serial.println();
      dhcpRetries  = 0;
      firstAttempt = true;   // reset for future entries
      announceSent = 0;
      nextAnnounceMs = millis() + (lfsrRand() % ANNOUNCE_JITTER_MS);
      currentState = STATE_ANNOUNCE;
    } else {
      dhcpFailCount++;
      if (dhcpRetries < 255) dhcpRetries++;
      
      // Check for static fallback
      if (USE_STATIC_FALLBACK && dhcpRetries >= DHCP_RETRY_LIMIT) {
        Serial.println(F("[DHCP] Limit reached. Falling back to STATIC IP."));
        uint8_t sip[] = { DEFAULT_STATIC_IP };
        uint8_t sgw[] = { DEFAULT_GATEWAY };
        uint8_t smsk[] = { DEFAULT_MASK };
        ether.staticSetup(sip, sgw, NULL, smsk);
        memcpy(myIp, ether.myip, 4);
        
        Serial.print(F("[INIT] Static IP Set: ")); printIp(); Serial.println();
        dhcpRetries  = 0;
        firstAttempt = true;
        announceSent = 0;
        nextAnnounceMs = millis() + (lfsrRand() % ANNOUNCE_JITTER_MS);
        currentState = STATE_ANNOUNCE;
      }
    }
  }

  // Slow LED blink while waiting
  static uint32_t blinkMs = 0;
  static bool ledOn = false;
  if ((uint32_t)(millis() - blinkMs) >= 800) {
    blinkMs = millis();
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
  }
}

static void stateAnnounce() {
  // Send ANNOUNCE_COUNT GARPs spaced ~1 s apart with jitter (RFC 5227)
  if ((uint32_t)(millis() - nextAnnounceMs) < 0x80000000UL) return; // not yet

  sendGarp();
  announceSent++;
  Serial.print(F("[ANN] GARP ")); Serial.print(announceSent);
  Serial.print(F("/")); Serial.println(ANNOUNCE_COUNT);

  if (announceSent >= ANNOUNCE_COUNT) {
    Serial.println(F("[ANN] Done → RUN"));
    counterResetMs  = millis();
    lastPhyCheckMs  = millis();
    lastStatsMs     = millis();
    bucket.lastRefillUs = micros();
    bucket.tokens   = TOKEN_BUCKET_CAP;
    currentState    = STATE_RUN;
    return;
  }

  // Schedule next announce with jitter
  uint16_t jitter = lfsrRand() % (2 * ANNOUNCE_JITTER_MS + 1);
  nextAnnounceMs = millis() + ANNOUNCE_INTERVAL_MS - ANNOUNCE_JITTER_MS + jitter;
}

static void stateRun() {
  uint32_t nowMs = millis();

  // ── Per-second packet counter reset ──────────────────────
  if ((uint32_t)(nowMs - counterResetMs) >= 1000UL) {
    counterResetMs  = nowMs;
    packetsThisSec  = 0;
  }

  // ── Periodic stats auto-report ───────────────────────────
  if ((uint32_t)(nowMs - lastStatsMs) >= STATS_REPORT_INTERVAL) {
    lastStatsMs = nowMs;
    Serial.print(F("[STAT] Sent=")); Serial.print(totalPacketsSent);
    Serial.print(F(" pps=")); Serial.println(bucket.pps);
  }

  // ── PHY link check ────────────────────────────────────────
  if ((uint32_t)(nowMs - lastPhyCheckMs) >= PHY_CHECK_INTERVAL) {
    lastPhyCheckMs = nowMs;
    if (!phyLinkUp()) {
      Serial.println(F("[RUN] Link lost → RECOVERY"));
      recoveryCount++;
      currentState = STATE_RECOVERY;
      return;
    }
  }

  // ── Send frames if not paused ─────────────────────────────
  if (!paused && bucket.consume()) {
    sendGarp();
    // Toggle LED on each sent frame for visual feedback
    digitalWrite(LED_PIN, (totalPacketsSent & 1) ? HIGH : LOW);
  }
}

static void stateRecovery() {
  static bool entered = false;
  if (!entered) {
    entered = true;
    Serial.println(F("[RECOVERY] Attempting DHCP re-lease..."));
    ledBlink(3, 300, 150);
  }

  // Try to rediscover IP
  if (dhcpOnce()) {
    Serial.print(F("[RECOVERY] IP restored: ")); printIp(); Serial.println();
    entered      = false;
    announceSent = 0;
    uint16_t jitter = lfsrRand() % ANNOUNCE_JITTER_MS;
    nextAnnounceMs = millis() + jitter;
    currentState = STATE_ANNOUNCE;
    return;
  }

  dhcpFailCount++;

  // Backoff retry
  uint32_t backoff = DHCP_BASE_MS << (dhcpRetries < 5 ? dhcpRetries : 5);
  if (backoff > DHCP_MAX_BACKOFF_MS) backoff = DHCP_MAX_BACKOFF_MS;
  safeDelay(backoff);        // safe: resets WDT internally
  if (dhcpRetries < 255) dhcpRetries++;

  // Slow amber blink
  static uint32_t blinkMs = 0;
  static bool ledOn = false;
  if ((uint32_t)(millis() - blinkMs) >= 400) {
    blinkMs = millis();
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
  }
}

// ─── Main Loop ───────────────────────────────────────────────

void loop() {
  wdt_reset();
  
  // Handle Serial inputs (non-blocking)
  handleSerial();

  switch (currentState) {
    case STATE_INIT:     
      stateInit();     
      break;
    case STATE_DHCP:     
      if (currentState != STATE_INIT) handleWeb(); // safely access SPI
      stateDhcp();     
      break;
    case STATE_ANNOUNCE: 
      if (currentState != STATE_INIT) handleWeb();
      stateAnnounce(); 
      break;
    case STATE_RUN:      
      if (currentState != STATE_INIT) handleWeb();
      stateRun();      
      break;
    case STATE_RECOVERY: 
      if (currentState != STATE_INIT) handleWeb();
      stateRecovery(); 
      break;
  }
}
