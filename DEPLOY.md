# DEPLOY.md — Flashing guide

How to build, flash, and monitor the firmware. All commands run from the
**repo root** in PowerShell.

---

## 1. One-time setup

### 1a. PlatformIO CLI
`pio` lives at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` and is already on
PATH via the PowerShell profile. Verify:

```powershell
pio --version
```

### 1b. ⚠️ REQUIRED on Windows: force UTF-8 output
esptool 5.x prints a Unicode progress bar. The default Windows console codepage
(cp1252) can't encode it, so **`pio ... -t upload` crashes mid-write with
`UnicodeEncodeError: 'charmap' codec`** — which leaves the board half-flashed and
black. Set these **once per terminal session** before any flashing:

```powershell
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
```

(Equivalent alternative: run `chcp 65001` to switch the console to UTF-8.) If you
ever see the `charmap` traceback, this is why — set the vars and re-flash.

---

## 2. Flash & monitor

Guition JC3248W535 (ESP32-S3). Auto-enters the bootloader over USB-C
(USB-Serial/JTAG); no buttons needed.

```powershell
# build + flash + monitor
pio run -e jc3248w535 -t upload -t monitor

# build only
pio run -e jc3248w535
```

If more than one board is plugged in, list ports and pass one explicitly:

```powershell
pio device list
pio run -e jc3248w535 -t upload -t monitor --upload-port COM7 --monitor-port COM7
```

---

## 3. Monitoring notes

- Serial is **115200**. The monitor uses `monitor_speed` from `platformio.ini`;
  if you open a bare monitor, pass `-b 115200`.
- **Native-USB boards re-enumerate on reset.** A monitor opened *after* boot
  misses the startup logs and may show nothing. Use a combined
  `-t upload -t monitor` so the monitor attaches across the reset, or just
  unplug/replug to watch a fresh boot.

```powershell
pio device monitor -p COM7 -b 115200
```

---

## 4. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Upload crashes with `UnicodeEncodeError: 'charmap'` | Windows cp1252 console can't print esptool's Unicode progress bar | Set `$env:PYTHONIOENCODING="utf-8"` / `$env:PYTHONUTF8="1"` (§1b) |
| Upload hangs on **"Connecting……"** for minutes | USB-Serial/JTAG bridge wedged (often after an interrupted flash) | **Unplug/replug** the USB cable; or BOOT-hold → tap RST → release. Don't run two `pio upload`s at once |
| Screen **black** after a successful flash | The previous flash was interrupted and left the board half-written / held in reset | Reflash cleanly (with the UTF-8 fix); a black screen *during* flashing is normal (download mode) |
| Monitor shows **nothing / garbage** | Wrong baud, or native-USB re-enumerated on reset | Use `-b 115200`; prefer combined `-t upload -t monitor` to catch boot |

---

## 5. Host unit tests (no hardware)

```powershell
pio test -e native
```

Needs host gcc/g++ on PATH. CI runs these on every push.
