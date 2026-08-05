#include "bluetti.h"
#include "power.h"
#include "settings.h"
#include "bluetti_crypt.h"
#include "logic/modbus.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>

// Set to 1 for verbose BLE/handshake/telemetry logging over serial.
#define BLUETTI_DEBUG 0
#if BLUETTI_DEBUG
#define BDBG(...) Serial.printf(__VA_ARGS__)
#else
#define BDBG(...) \
  do {            \
  } while (0)
#endif

// BLE service/characteristics (Bluetti vendor service).
static const uint16_t SVC_UUID = 0xFF00;
static const uint16_t NOTIFY_UUID = 0xFF01;  // device -> app
static const uint16_t WRITE_UUID = 0xFF02;   // app -> device

// Refresh period while connected -- runtime-adjustable so it can be tuned
// against the link-health counters on the Diagnostics page (see bluetti.h).
// NOTE the real limit is the poll DURATION (~17 sequential reads, each a 40ms
// BLE_GAP_MS plus a notify round-trip, so ~1.5-2.5s); setting the interval
// below that just polls back-to-back rather than going faster.
#define POLL_MS_DEFAULT 3000
#define POLL_MS_MIN 250
#define POLL_MS_MAX 30000
static volatile uint32_t g_pollMs = POLL_MS_DEFAULT;

#define RETRY_MS 4000   // retry period after a failure / reconnect
#define DEVNAME_PREFIX "EL300"

// Link-health counters (see bluetti.h). Written only by the BLE task.
static BluettiStats g_stats = {};
static uint16_t g_pollReads = 0, g_pollRetries = 0, g_pollFailures = 0;

const BluettiStats &bluetti_stats() { return g_stats; }
uint32_t bluetti_poll_ms() { return g_pollMs; }

void bluetti_stats_reset() {
  g_stats.polls = g_stats.retries = g_stats.failures = g_stats.linkDrops = 0;
  g_stats.avgPollMs = 0;
}

void bluetti_set_poll_ms(uint32_t ms) {
  if (ms < POLL_MS_MIN) ms = POLL_MS_MIN;
  if (ms > POLL_MS_MAX) ms = POLL_MS_MAX;
  g_pollMs = ms;
}

static const uint16_t REG_CTRL_AC = 2011;
static const uint16_t REG_CTRL_DC = 2012;

// Frames pushed from the NimBLE notify callback to the poller task.
struct Frame {
  uint16_t len;
  uint8_t data[256];
};
static QueueHandle_t g_q = nullptr;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
struct PendWrite {
  uint16_t addr;
  uint16_t val;
};
static QueueHandle_t g_writeQ = nullptr;  // queued register writes (FC6)
static SemaphoreHandle_t g_wake = nullptr;
static volatile uint32_t g_releaseUntil = 0;  // millis() until which to stay off
static volatile BluettiConn g_state = BTC_OFFLINE;  // BLE link state (status bar)

// Persistent connection state (handshake done once, reused for all ops).
static NimBLEClient* g_client = nullptr;
static NimBLERemoteCharacteristic* g_wr = nullptr;
static BluettiCrypt g_crypt;

static void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len,
                     bool) {
  if (!g_q || len == 0 || len > sizeof(((Frame*)0)->data)) return;
  Frame f;
  f.len = (uint16_t)len;
  memcpy(f.data, data, len);
  xQueueSend(g_q, &f, 0);
}

static bool waitFrame(Frame& f, uint32_t ms) {
  return xQueueReceive(g_q, &f, ms / portTICK_PERIOD_MS) == pdTRUE;
}

static void drainQueue() {
  Frame junk;
  while (xQueueReceive(g_q, &junk, 0) == pdTRUE) {
  }
}

static void dropLink() {
  if (g_client) {
    if (g_client->isConnected()) g_client->disconnect();
    NimBLEDevice::deleteClient(g_client);
    g_client = nullptr;
  }
  g_wr = nullptr;
  g_state = BTC_OFFLINE;
}

// Resolve the Elite 300's address: a saved MAC, else a 5s scan by name prefix.
static NimBLEAddress resolveAddr() {
  if (strlen(settings.bluettiMac) >= 17)
    return NimBLEAddress(std::string(settings.bluettiMac), BLE_ADDR_PUBLIC);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(5000, false);
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    if (String(d->getName().c_str()).startsWith(DEVNAME_PREFIX))
      return d->getAddress();
  }
  return NimBLEAddress();
}

// Connect + run the encrypted handshake. Leaves g_client/g_wr/g_crypt ready.
static bool connectAndHandshake() {
  dropLink();
  g_state = BTC_CONNECTING;
  NimBLEAddress addr = resolveAddr();
  if (addr == NimBLEAddress()) {
    BDBG("[bluetti] device not found\n");
    g_state = BTC_OFFLINE;
    return false;
  }
  g_client = NimBLEDevice::createClient();
  // 30-50ms interval, 0 latency, and a short 4s supervision timeout. The short
  // timeout is the key bit: after an ungraceful reboot/crash the Bluetti (single
  // BLE client) otherwise holds our stale link for its full default timeout
  // (~20s) before freeing up, which is the "takes ages to reconnect" symptom.
  // With 4s it drops the dead link quickly so we reconnect on the next retry.
  g_client->setConnectionParams(24, 40, 0, 400);
  if (!g_client->connect(addr)) {
    BDBG("[bluetti] connect failed\n");
    dropLink();
    return false;
  }
  NimBLERemoteService* svc = g_client->getService(NimBLEUUID(SVC_UUID));
  if (!svc) {
    dropLink();
    return false;
  }
  g_wr = svc->getCharacteristic(NimBLEUUID(WRITE_UUID));
  NimBLERemoteCharacteristic* nt = svc->getCharacteristic(NimBLEUUID(NOTIFY_UUID));
  if (!g_wr || !nt) {
    dropLink();
    return false;
  }
  g_crypt.reset();
  drainQueue();
  if (!nt->subscribe(true, notifyCB)) {
    dropLink();
    return false;
  }

  uint32_t t0 = millis();
  while (!g_crypt.isReady() && millis() - t0 < 12000) {
    Frame f;
    if (!waitFrame(f, 800)) continue;
    std::vector<uint8_t> reply, plain;
    int r = g_crypt.handle(f.data, f.len, reply, plain);
    if (r == 1) {
      // A write can legitimately fail mid-handshake if the peripheral drops
      // the link (common with battery-powered stations disconnecting
      // between polls). Bail out now instead of sitting through the rest of
      // the 12s window waiting on a reply to a message that was never sent.
      if (!g_wr->writeValue(reply.data(), reply.size(), true)) {
        BDBG("[bluetti] handshake write failed (link dropped?)\n");
        break;
      }
    } else if (r == 2)
      break;
    else if (r < 0)
      break;
  }
  if (!g_crypt.isReady()) {
    BDBG("[bluetti] handshake failed\n");
    dropLink();
    return false;
  }
  BDBG("[bluetti] secure link established\n");
  // Stay BTC_CONNECTING until the first successful telemetry read (in poll());
  // the link being up isn't useful to the UI until real data arrives.
  vTaskDelay(200 / portTICK_PERIOD_MS);  // let the device settle
  return true;
}

static bool ensureConnected() {
  if (g_client && g_client->isConnected() && g_crypt.isReady()) return true;
  return connectAndHandshake();
}

// Read a register block (Modbus FC3); retries the request a few times because a
// single notification can be lost. Returns the word count placed in out[], or -1.
//
// Small gap before each BLE command: the Elite 300 is unreliable with
// back-to-back commands, and spacing them also cut connect retries. But it is
// the DOMINANT cost of a poll -- measured 17 reads in ~1020-1320ms total, of
// which 40ms x 17 = 680ms is this delay alone. So it's runtime-tunable
// alongside the poll interval; lowering it is the only way past the ~1.1s
// floor without cutting reads. Watch the retry/failure counters when you do.
#define BLE_GAP_MS_DEFAULT 40
#define BLE_GAP_MS_MIN 0
#define BLE_GAP_MS_MAX 80
static volatile uint32_t g_gapMs = BLE_GAP_MS_DEFAULT;

uint32_t bluetti_gap_ms() { return g_gapMs; }
void bluetti_set_gap_ms(uint32_t ms) {
  if (ms > BLE_GAP_MS_MAX) ms = BLE_GAP_MS_MAX;
  g_gapMs = ms;
}

static int readRegs(uint16_t addr, uint16_t qty, uint16_t* out, int maxw) {
  if (!g_wr) return -1;
  g_pollReads++;
  if (g_gapMs) vTaskDelay(g_gapMs / portTICK_PERIOD_MS);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {  // this read needed more than one go
      g_pollRetries++;
      g_stats.retries++;
    }
    std::vector<uint8_t> cmd = g_crypt.readCommand(addr, qty);
    if (cmd.empty()) break;
    drainQueue();
    if (!g_wr->writeValue(cmd.data(), cmd.size(), true)) continue;
    uint32_t t0 = millis();
    while (millis() - t0 < 1200) {
      Frame f;
      if (!waitFrame(f, 1200 - (millis() - t0))) break;
      std::vector<uint8_t> reply, plain;
      if (g_crypt.handle(f.data, f.len, reply, plain) != 3) continue;
      int words = modbus_parse_read(plain.data(), plain.size(), qty, out, maxw);
      if (words >= 0) return words;  // valid, matching FC3 response
    }
  }
  g_pollFailures++;
  g_stats.failures++;
  BDBG("[bluetti] read %u: no response\n", addr);
  return -1;
}

// Write a register (Modbus FC6) and confirm it. Sends the command (spaced from
// other BLE traffic), waits for the echo, then reads the register back until it
// reports the new value — the Elite 300 commits ~0.4s after the echo. Returns
// true once confirmed (resends once if not). The UI no longer updates
// optimistically, so a control reflects the change only after this confirms.
// NOTE: call this directly, never inside a BDBG()/macro argument — in release
// builds BDBG() compiles to nothing and the call would be silently dropped.
static bool writeReg(uint16_t addr, uint16_t val) {
  if (!g_wr) return false;
  for (int attempt = 0; attempt < 2; attempt++) {
    std::vector<uint8_t> cmd = g_crypt.writeCommand(addr, val);
    if (cmd.empty()) return false;
    // Writes always keep the full default gap -- they're rare, user-initiated,
    // and must not fail, so they don't ride the tuned-down read gap.
    vTaskDelay(BLE_GAP_MS_DEFAULT / portTICK_PERIOD_MS);
    drainQueue();
    if (!g_wr->writeValue(cmd.data(), cmd.size(), true)) continue;
    // Consume the echo (receipt ack); the value commits a fraction of a second
    // later, so confirm by reading it back. Measured commit ~0.4s.
    uint32_t t0 = millis();
    while (millis() - t0 < 1000) {
      Frame f;
      if (!waitFrame(f, 1000 - (millis() - t0))) break;
      std::vector<uint8_t> reply, plain;
      if (g_crypt.handle(f.data, f.len, reply, plain) == 3 &&
          modbus_is_write_echo(plain.data(), plain.size()))
        break;
    }
    for (int rb = 0; rb < 8; rb++) {
      vTaskDelay(300 / portTICK_PERIOD_MS);
      uint16_t cur;
      if (readRegs(addr, 1, &cur, 1) == 1 && cur == val) return true;
    }
    BDBG("[wr] addr=%u=%u not confirmed; resend (attempt %d)\n", addr, val, attempt);
  }
  return false;
}

// ===== Diagnostics: live register change detection =========================
// See the header for why this exists. Ranges mirror the readable windows from
// docs/BLUETTI.md's wide sweep (940 registers total).
struct DiagRange { uint16_t start, end; };
static const DiagRange DIAG_RANGES[] = {
    {0, 20},     {100, 200},   {700, 760},   {1100, 1180}, {1200, 1340},
    {1400, 1470},{1500, 1560}, {1600, 1610}, {2000, 2090}, {2200, 2280},
    {2400, 2450},{2500, 2540}, {3000, 3030}, {3500, 3550}, {3600, 3660},
};
static const int DIAG_NRANGES = sizeof(DIAG_RANGES) / sizeof(DIAG_RANGES[0]);
#define DIAG_NREGS 940  // must equal the sum of the ranges above

// Registers measured (this session) to drift continuously -- live watts,
// volts, currents, frequencies, and monotonic counters. Filtered out so a
// genuine state flip isn't buried under analogue noise. Deliberately does NOT
// include 103/124/156/161, which are state-ish and worth watching.
static bool diagIsNoisy(uint16_t a) {
  switch (a) {
    case 100: case 101: case 102: case 104: case 105:
    case 140: case 141: case 142: case 143: case 144:
    case 145: case 146: case 147: case 148:
    case 149:  // high word of the 148/149 signed 32-bit pair: flips
               // 0 <-> 65535 (sign extension) on every direction change
    case 167: case 188:
    case 1301: case 1313: case 1314: case 1315:
    case 1400:  // DC output W (the x10-address mirror of reg 140)
    case 1420: case 1430: case 1431: case 1432:
    case 1500: case 1509: case 1510: case 1511: case 1512:
    case 2003:
      return true;
    default:
      return false;
  }
}

#define DIAG_NCHANGES 24
static volatile bool g_diagOn = false;
static uint16_t *g_diagSnap = nullptr;   // baseline values, parallel to the ranges
static bool g_diagHaveSnap = false;
static RegChange g_diagChanges[DIAG_NCHANGES];
static int g_diagHead = 0;   // next write slot
static int g_diagCount = 0;
static uint32_t g_diagScanMs = 0;
static int g_diagScanned = 0;
static const RegChange DIAG_ZERO = {};

void bluetti_diag_enable(bool on) { g_diagOn = on; }
bool bluetti_diag_enabled() { return g_diagOn; }

void bluetti_diag_reset() {
  g_diagHaveSnap = false;
  g_diagHead = 0;
  g_diagCount = 0;
}

int bluetti_diag_count() { return g_diagCount; }

const RegChange &bluetti_diag_at(int i) {
  if (i < 0 || i >= g_diagCount) return DIAG_ZERO;
  // Newest first: walk backwards from the most recent write.
  int idx = (g_diagHead - 1 - i) % DIAG_NCHANGES;
  if (idx < 0) idx += DIAG_NCHANGES;
  return g_diagChanges[idx];
}

uint32_t bluetti_diag_scan_ms() { return g_diagScanMs; }
int bluetti_diag_scanned() { return g_diagScanned; }

static void diagNote(uint16_t addr, uint16_t oldV, uint16_t newV) {
  g_diagChanges[g_diagHead] = {addr, oldV, newV, millis()};
  g_diagHead = (g_diagHead + 1) % DIAG_NCHANGES;
  if (g_diagCount < DIAG_NCHANGES) g_diagCount++;
}

// Sweep every readable register, diffing against the baseline. First run just
// captures the baseline. Block reads of 10 to keep it reasonably quick.
static void diagScan() {
  if (!g_diagSnap) {
    g_diagSnap = (uint16_t *)ps_malloc(DIAG_NREGS * sizeof(uint16_t));
    if (!g_diagSnap) return;  // no PSRAM -> diagnostics silently unavailable
    g_diagHaveSnap = false;
  }
  uint32_t t0 = millis();
  bool baseline = !g_diagHaveSnap;
  int slot = 0, ok = 0;
  uint16_t buf[10];
  for (int r = 0; r < DIAG_NRANGES; r++) {
    for (uint16_t addr = DIAG_RANGES[r].start; addr < DIAG_RANGES[r].end;
         addr += 10) {
      uint16_t qty = (uint16_t)min(10, (int)(DIAG_RANGES[r].end - addr));
      int n = readRegs(addr, qty, buf, 10);
      for (uint16_t i = 0; i < qty; i++) {
        if ((int)i < n) {
          uint16_t a = addr + i, v = buf[i];
          if (!baseline && g_diagSnap[slot + i] != v && !diagIsNoisy(a))
            diagNote(a, g_diagSnap[slot + i], v);
          g_diagSnap[slot + i] = v;
          ok++;
        }
      }
      slot += qty;
    }
  }
  g_diagHaveSnap = true;
  g_diagScanned = ok;
  g_diagScanMs = millis() - t0;
}

static bool poll() {
  // Released for the phone app: stay disconnected until the window expires.
  if (millis() < g_releaseUntil) {
    dropLink();
    return false;
  }
  if (!ensureConnected()) return false;

  // Apply any queued register writes. writeReg() confirms each by reading it
  // back, so the telemetry read below reflects the committed values and the UI
  // (no longer optimistic) settles to the confirmed state.
  PendWrite pw;
  while (xQueueReceive(g_writeQ, &pw, 0) == pdTRUE) {
    bool ok = writeReg(pw.addr, pw.val);  // MUST be evaluated outside BDBG():
    BDBG("[bluetti] write %u=%u %s\n", pw.addr, pw.val, ok ? "ok" : "FAIL");
    (void)ok;
  }

  PowerData tmp = {};
  tmp.status = PWR_OK;
  uint16_t w[8];

  // Time the telemetry reads (the tuning-relevant part -- queued writes above
  // are user-initiated and would skew the figure).
  uint32_t pollT0 = millis();
  g_pollReads = g_pollRetries = g_pollFailures = 0;

  if (readRegs(102, 1, w, 1) != 1) {
    BDBG("[bluetti] SoC read failed; dropping link\n");
    g_stats.linkDrops++;
    dropLink();  // force a fresh connection next cycle
    return false;
  }
  tmp.soc = bluetti_clamp_soc(w[0]);
  if (readRegs(104, 1, w, 1) == 1) tmp.ttfMin = w[0];
  int n = readRegs(140, 8, w, 8);
  if (n >= 7) {
    tmp.dcOutW = w[0];  // 140
    tmp.acOutW = w[2];  // 142
    tmp.dcInW = w[4];   // 144
    tmp.acInW = w[6];   // 146
  }
  if (readRegs(REG_CTRL_AC, 1, w, 1) == 1) tmp.acOn = (w[0] != 0);
  if (readRegs(REG_CTRL_DC, 1, w, 1) == 1) tmp.dcOn = (w[0] != 0);
  if (readRegs(156, 1, w, 1) == 1) tmp.tempC = w[0];     // battery temp degC
  if (readRegs(1431, 1, w, 1) == 1) tmp.acOutDV = w[0];  // AC out V x10
  if (readRegs(2020, 1, w, 1) == 1) tmp.chargeMode = w[0];  // 0/1/2/4 mode
  if (readRegs(2214, 1, w, 1) == 1) tmp.gridChargeA = w[0]; // custom grid A
  if (readRegs(2017, 1, w, 1) == 1) tmp.acEco = (w[0] != 0);
  if (readRegs(2014, 1, w, 1) == 1) tmp.dcEco = (w[0] != 0);
  if (readRegs(2021, 1, w, 1) == 1) tmp.powerLift = (w[0] != 0);
  if (readRegs(2083, 1, w, 1) == 1) tmp.chargeLimit = w[0] >> 8;  // % in hi byte
  if (readRegs(2067, 1, w, 1) == 1) tmp.screenTimeout = w[0];
  if (readRegs(1500, 1, w, 1) == 1) tmp.acOutFreqDHz = w[0];
  // reg 161 bit1 = AC input present (bit0 = AC output active). See power.h.
  if (readRegs(161, 1, w, 1) == 1) tmp.gridConnected = (w[0] & 0x02) != 0;
  // reg 148 is signed (two's complement): negative = charging. See power.h.
  if (readRegs(148, 1, w, 1) == 1) tmp.netBatteryW = (int16_t)w[0];
  tmp.charging = (tmp.dcInW + tmp.acInW) > (tmp.dcOutW + tmp.acOutW);
  tmp.whRemaining = 0;
  tmp.fetchedMs = millis();

  portENTER_CRITICAL(&g_mux);
  power = tmp;
  portEXIT_CRITICAL(&g_mux);
  g_state = BTC_ONLINE;  // got real data — now the link is "online" to the UI

  // Record this poll's cost + error counts for the Diagnostics page.
  g_stats.lastPollMs = millis() - pollT0;
  g_stats.lastReads = g_pollReads;
  g_stats.lastRetries = g_pollRetries;
  g_stats.lastFailures = g_pollFailures;
  g_stats.polls++;
  g_stats.avgPollMs = g_stats.avgPollMs
                          ? (g_stats.avgPollMs * 7 + g_stats.lastPollMs) / 8
                          : g_stats.lastPollMs;

  BDBG("[bluetti] SoC=%d%% DCout=%d ACout=%d DCin=%d ACin=%d AC%s DC%s "
       "poll=%lums r=%u/%u\n",
                tmp.soc, tmp.dcOutW, tmp.acOutW, tmp.dcInW, tmp.acInW,
                tmp.acOn ? "on" : "off", tmp.dcOn ? "on" : "off",
                (unsigned long)g_stats.lastPollMs, g_pollRetries, g_pollReads);
  // Diagnostics sweep (only while the Diagnostics page is open -- it's slow).
  if (g_diagOn) diagScan();
  return true;
}

static void bluettiTask(void*) {
  for (;;) {
    bool ok = poll();
    // Sleep until the next refresh, or wake early when a write is requested.
    xSemaphoreTake(g_wake, (ok ? g_pollMs : RETRY_MS) / portTICK_PERIOD_MS);
  }
}

void bluetti_write_reg(uint16_t addr, uint16_t val) {
  PendWrite pw{addr, val};
  if (g_writeQ) xQueueSend(g_writeQ, &pw, 0);
  if (g_wake) xSemaphoreGive(g_wake);  // apply immediately on the next cycle
}

void bluetti_set_output(bool ac, bool on) {
  bluetti_write_reg(ac ? REG_CTRL_AC : REG_CTRL_DC, on ? 1 : 0);
}

void bluetti_release(uint32_t seconds) {
  g_releaseUntil = millis() + seconds * 1000;
  if (g_wake) xSemaphoreGive(g_wake);  // act on it now (disconnect or resume)
}

uint32_t bluetti_release_remaining() {
  uint32_t now = millis();
  return g_releaseUntil > now ? (g_releaseUntil - now) / 1000 : 0;
}

BluettiConn bluetti_state() { return g_state; }

void bluetti_begin() {
  g_q = xQueueCreate(8, sizeof(Frame));
  g_writeQ = xQueueCreate(8, sizeof(PendWrite));
  g_wake = xSemaphoreCreateBinary();
  NimBLEDevice::init("");
  NimBLEDevice::setPower(9);
  // 20KB stack: mbedTLS EC operations (ECDH/ECDSA on P-256) are stack-heavy.
  xTaskCreatePinnedToCore(bluettiTask, "bluetti", 20480, nullptr, 1, nullptr, 0);
}
