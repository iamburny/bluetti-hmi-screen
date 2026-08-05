#include "powerlog.h"
#include "power.h"
#include "sdcard.h"
#include <SD_MMC.h>

// 24 h at one sample per 15 s = 5760 samples * 12 B ~= 69 KB in PSRAM.
static const int RING_N = 5760;
// Deliberately independent of bluetti.cpp's POLL_MS (which is faster, for a
// responsive live display) -- this throttle just keeps the history chart's
// sample rate (and RING_N's 24h budget) stable regardless of how often the
// BLE task actually polls.
static const uint32_t SAMPLE_INTERVAL_MS = 15000;

static PwrSample *ring = nullptr;
static int head = 0;    // next write slot
static int count = 0;   // valid samples held
static const PwrSample ZERO = {};

static int16_t clamp16(int v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static void pushRing(const PwrSample &s) {
  ring[head] = s;
  head = (head + 1) % RING_N;
  if (count < RING_N) count++;
}

static void loadFromSd();  // restore recent history at boot (defined below)

void powerlog_init() {
  ring = (PwrSample *)ps_malloc((size_t)RING_N * sizeof(PwrSample));
  if (!ring) Serial.println("[powerlog] PSRAM alloc failed; history disabled");
  sd_begin();    // best-effort; logging still no-ops gracefully if absent
  loadFromSd();  // repopulate the ring from the SD log so the chart survives reboots
}

// Append one CSV row to /logs/power.csv. Best-effort: silently returns if the
// SD card is absent.
//
// The log file is kept OPEN across samples: re-opening with FILE_APPEND every
// 15 s seeks to the end of an ever-growing file, which got slower through the day
// and hitched the whole UI. We only (re)open once, on the first write.
static File g_logf;
static char g_logPath[40] = "";

static void logToSd(const PwrSample &s) {
  if (!sd_begin()) return;

  const char *path = "/logs/power.csv";

  if (strcmp(path, g_logPath) != 0) {  // first write
    if (g_logf) g_logf.close();
    SD_MMC.mkdir("/logs");
    bool isNew = !SD_MMC.exists(path);
    g_logf = SD_MMC.open(path, FILE_APPEND);
    strncpy(g_logPath, path, sizeof(g_logPath) - 1);
    g_logPath[sizeof(g_logPath) - 1] = '\0';
    if (g_logf && isNew)
      g_logf.println("epoch,iso,soc,dc_in,ac_in,dc_out,ac_out");
  }
  if (!g_logf) {
    g_logPath[0] = '\0';  // open failed (card pulled?) — retry next sample
    return;
  }
  g_logf.printf("%lu,%s,%d,%d,%d,%d,%d\n", (unsigned long)s.t, "", s.soc,
                s.dcIn, s.acIn, s.dcOut, s.acOut);
  g_logf.flush();  // persist without paying the open-seek cost each sample
}

void powerlog_tick() {
  if (!ring || !power_valid()) return;

  static uint32_t lastFetched = 0, lastSampleMs = 0;
  if (power.fetchedMs == lastFetched) return;        // no new poll yet
  uint32_t now = millis();
  if (lastSampleMs && now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastFetched = power.fetchedMs;
  lastSampleMs = now;

  PwrSample s;
  s.t = now / 1000;
  s.soc = clamp16(power.soc);
  s.dcIn = clamp16(power.dcInW);
  s.acIn = clamp16(power.acInW);
  s.dcOut = clamp16(power.dcOutW);
  s.acOut = clamp16(power.acOutW);

  pushRing(s);
  logToSd(s);
}

// Replay a CSV log file into the ring (oldest->newest). Does NOT re-log to SD.
static void replaySdFile(const char *path) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    char *p = (char *)line.c_str();
    if (*p < '0' || *p > '9') continue;  // skip the header / blank lines
    PwrSample s;
    s.t = strtoul(p, &p, 10);
    if (*p == ',') p++;
    while (*p && *p != ',') p++;  // skip the iso field
    if (*p == ',') p++;
    s.soc = (int16_t)strtol(p, &p, 10); if (*p == ',') p++;
    s.dcIn = (int16_t)strtol(p, &p, 10); if (*p == ',') p++;
    s.acIn = (int16_t)strtol(p, &p, 10); if (*p == ',') p++;
    s.dcOut = (int16_t)strtol(p, &p, 10); if (*p == ',') p++;
    s.acOut = (int16_t)strtol(p, &p, 10);
    pushRing(s);
  }
  f.close();
}

// Restore history from /logs/power.csv at boot. The ring keeps only the last
// RING_N samples (~24h).
static void loadFromSd() {
  if (!ring || !sd_begin()) return;
  if (SD_MMC.exists("/logs/power.csv")) replaySdFile("/logs/power.csv");
  Serial.printf("[powerlog] restored %d samples from SD\n", count);
}

int powerlog_count() { return count; }

const PwrSample &powerlog_at(int i) {
  if (!ring || i < 0 || i >= count) return ZERO;
  int idx = (head - count + i) % RING_N;
  if (idx < 0) idx += RING_N;
  return ring[idx];
}
