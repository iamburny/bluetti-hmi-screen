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

// Refresh period while connected. ~15 register reads/poll at ~100-150ms each
// (40ms BLE_GAP_MS + notify round-trip) costs ~1.5-2.25s active time, so 3s
// leaves comfortable slack for the occasional retry while updating far more
// often than the old 15s. No WiFi/BLE coexistence to worry about in this
// build (BLE has the radio to itself), so there's no reason to poll slower.
#define POLL_MS 3000
#define RETRY_MS 4000   // retry period after a failure / reconnect
#define DEVNAME_PREFIX "EL300"

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
// single notification can be lost under WiFi/BLE coexistence. Returns the word
// count placed in out[], or -1.
// Small gap before each BLE command. Spacing transactions improves reliability
// with the Elite 300 under WiFi/BLE coexistence (also noticeably fewer connect
// retries).
#define BLE_GAP_MS 40

static int readRegs(uint16_t addr, uint16_t qty, uint16_t* out, int maxw) {
  if (!g_wr) return -1;
  vTaskDelay(BLE_GAP_MS / portTICK_PERIOD_MS);
  for (int attempt = 0; attempt < 3; attempt++) {
    std::vector<uint8_t> cmd = g_crypt.readCommand(addr, qty);
    if (cmd.empty()) return -1;
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
    vTaskDelay(BLE_GAP_MS / portTICK_PERIOD_MS);
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

  if (readRegs(102, 1, w, 1) != 1) {
    BDBG("[bluetti] SoC read failed; dropping link\n");
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
  if (readRegs(103, 1, w, 1) == 1) tmp.gridConnected = (w[0] != 0); // confirmed, see power.h
  tmp.charging = (tmp.dcInW + tmp.acInW) > (tmp.dcOutW + tmp.acOutW);
  tmp.whRemaining = 0;
  tmp.fetchedMs = millis();

  portENTER_CRITICAL(&g_mux);
  power = tmp;
  portEXIT_CRITICAL(&g_mux);
  g_state = BTC_ONLINE;  // got real data — now the link is "online" to the UI
  BDBG("[bluetti] SoC=%d%% DCout=%d ACout=%d DCin=%d ACin=%d AC%s DC%s\n",
                tmp.soc, tmp.dcOutW, tmp.acOutW, tmp.dcInW, tmp.acInW,
                tmp.acOn ? "on" : "off", tmp.dcOn ? "on" : "off");
  return true;
}

static void bluettiTask(void*) {
  for (;;) {
    bool ok = poll();
    // Sleep until the next refresh, or wake early when a write is requested.
    xSemaphoreTake(g_wake, (ok ? POLL_MS : RETRY_MS) / portTICK_PERIOD_MS);
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
