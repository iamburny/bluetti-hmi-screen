"""Tkinter control panel for the fake Elite 300.

Sliders drive the registers the HMI polls. Controls the HMI writes (output
toggles, ECO, charge limit, ...) update here when the write commits, so a tap
on the screen shows up on the PC.
"""
import queue
import time
import tkinter as tk
from tkinter import ttk

from ble_server import BleServer
from registers import (
    AC_ECO, AC_IN_W, AC_OUT_DV, AC_OUT_FREQ_DHZ, AC_OUT_W,
    CHARGE_LIMIT, CHARGE_MODE, CTRL_AC, CTRL_DC, DC_ECO, DC_IN_W, DC_OUT_W,
    AC_ECO_HOURS, AC_ECO_MIN_W, AC_OUT_ENERGY, DC_ECO_HOURS, DC_ECO_MIN_W,
    GRID_CHARGE_A, GRID_CHG_ENERGY, POWER_LIFT, PV_CHG_ENERGY, SCREEN_TIMEOUT,
    SOC, SOC_HIGH, SOC_LOW, SOC_SET_LOW, TIME_REMAINING, WORK_MODE, Registers)

CHARGE_MODES = [("Standard", 0), ("Silent", 1), ("Turbo", 2), ("Custom", 4)]
TIMEOUTS = [("30 s", 2), ("1 min", 3), ("5 min", 4), ("Never", 5)]
WORK_MODES = [("Customised", 1), ("PV priority", 2), ("Standard", 3), ("Time control", 4)]
PRESETS = {
    "Idle": {DC_OUT_W: 0, AC_OUT_W: 0, DC_IN_W: 0, AC_IN_W: 0},
    "Mains charging": {DC_OUT_W: 0, AC_OUT_W: 0, DC_IN_W: 0, AC_IN_W: 800},
    "AC load": {DC_OUT_W: 0, AC_OUT_W: 500, DC_IN_W: 0, AC_IN_W: 0},
    "Charging + load": {DC_OUT_W: 0, AC_OUT_W: 500, DC_IN_W: 0, AC_IN_W: 1300},
    "DC phone charge": {DC_OUT_W: 18, AC_OUT_W: 0, DC_IN_W: 0, AC_IN_W: 0},
}


class App:
    def __init__(self, root):
        self.root = root
        root.title("Bluetti Elite 300 simulator")
        root.geometry("820x800")
        self.regs = Registers()
        self.events = queue.Queue()
        self.regs.on_commit = lambda addr, value: self.events.put(
            ("commit", (addr, value)))
        self._updating = False
        self._last_tick = time.monotonic()
        self.vars = {}

        self.status = tk.StringVar(value="starting")
        ttk.Label(root, textvariable=self.status, font=("Segoe UI", 12, "bold")
                  ).pack(anchor="w", padx=10, pady=(8, 0))

        body = ttk.Frame(root)
        body.pack(fill="both", expand=True, padx=10, pady=6)
        left = ttk.LabelFrame(body, text="Live values")
        right = ttk.LabelFrame(body, text="Settings the HMI writes")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
        right.grid(row=0, column=1, sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=1)

        self._slider(left, "State of charge %", SOC, 0, 100)
        self._slider(left, "Time remaining min", TIME_REMAINING, 0, 4000)
        self._slider(left, "DC out W", DC_OUT_W, 0, 3000)
        self._slider(left, "AC out W", AC_OUT_W, 0, 3000)
        self._slider(left, "DC in W", DC_IN_W, 0, 3000)
        self._slider(left, "AC in W", AC_IN_W, 0, 3000)
        self._slider(left, "AC out voltage", AC_OUT_DV, 0, 2600, scale=0.1, digits=1)
        self._slider(left, "AC frequency Hz", AC_OUT_FREQ_DHZ, 450, 650, scale=0.1, digits=1)

        self._check(right, "AC output", CTRL_AC)
        self._check(right, "DC output", CTRL_DC)
        self._check(right, "AC ECO", AC_ECO)
        self._check(right, "DC ECO", DC_ECO)
        self._check(right, "Power lifting", POWER_LIFT)
        self._choice(right, "Charge mode", CHARGE_MODE, CHARGE_MODES)
        self._slider(right, "Grid charge A", GRID_CHARGE_A, 1, 15)
        self._slider(right, "Charge limit %", CHARGE_LIMIT, 20, 100, encode=lambda p: p << 8,
                     decode=lambda w: w >> 8)
        self._choice(right, "Screen timeout", SCREEN_TIMEOUT, TIMEOUTS)

        # Lifetime counters: they build up from the power flows; typing a
        # value (Enter or leaving the field) sets the counter.
        energy = ttk.LabelFrame(right, text="Lifetime energy kWh")
        energy.pack(fill="x", padx=6, pady=(8, 4))
        self.energy = {}
        for label, addr in (("Grid charging", GRID_CHG_ENERGY),
                            ("Solar / DC charging", PV_CHG_ENERGY),
                            ("AC output", AC_OUT_ENERGY)):
            row = ttk.Frame(energy)
            row.pack(fill="x", padx=6, pady=2)
            ttk.Label(row, text=label, width=18).pack(side="left")
            var = tk.StringVar()
            entry = ttk.Entry(row, textvariable=var, width=10)
            entry.pack(side="left")
            entry.bind("<Return>", lambda _e, a=addr: self._on_energy(a, leave=True))
            entry.bind("<FocusOut>", lambda _e, a=addr: self._on_energy(a))
            self.energy[addr] = (var, entry)
        self._energy_shown = {}  # text last written to each entry
        self._refresh_energy()

        # Settings the HMI's ECO & Limits page shows read-only.
        eco = ttk.LabelFrame(left, text="ECO & limits (HMI shows read-only)")
        eco.pack(fill="x", padx=6, pady=(8, 4))
        self._slider(eco, "AC ECO hours", AC_ECO_HOURS, 1, 4)
        self._slider(eco, "AC ECO min W", AC_ECO_MIN_W, 0, 100)
        self._slider(eco, "DC ECO hours", DC_ECO_HOURS, 1, 4)
        self._slider(eco, "DC ECO min W", DC_ECO_MIN_W, 0, 100)
        self._slider(eco, "SoC low %", SOC_LOW, 0, 100)
        self._slider(eco, "SoC high %", SOC_HIGH, 0, 100)
        self._choice(eco, "Working mode", WORK_MODE, WORK_MODES)
        self._raw_entry(eco, "SoC set low (raw)", SOC_SET_LOW)

        bar = ttk.Frame(root)
        bar.pack(fill="x", padx=10)
        for name in PRESETS:
            ttk.Button(bar, text=name, command=lambda n=name: self._preset(n)
                       ).pack(side="left", padx=(0, 4))

        opts = ttk.Frame(root)
        opts.pack(fill="x", padx=10, pady=6)
        self.simulate = tk.BooleanVar()
        ttk.Checkbutton(opts, text="Simulate battery", variable=self.simulate,
                        command=self._set_simulate).pack(side="left")
        ttk.Label(opts, text="Commit delay s").pack(side="left", padx=(12, 4))
        self.delay = tk.DoubleVar(value=self.regs.commit_delay)
        ttk.Spinbox(opts, from_=0, to=5, increment=0.1, width=5, textvariable=self.delay,
                    command=self._set_delay).pack(side="left")
        self.delay.trace_add("write", lambda *_: self._set_delay())
        ttk.Button(opts, text="Disconnect client", command=self._disconnect
                   ).pack(side="right")

        self.log = tk.Text(root, height=10, state="disabled", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        self.ble = BleServer(self.regs, self.events)
        try:
            self.ble.start()
        except Exception as exc:
            self.status.set("BLE failed")
            self._append(f"BLE failed: {exc}")
            self.ble = None
        self._refresh_all()
        root.after(100, self._poll)
        root.protocol("WM_DELETE_WINDOW", self._close)

    def _slider(self, parent, label, addr, lo, hi, scale=1, digits=0,
                encode=None, decode=None):
        row = ttk.Frame(parent)
        row.pack(fill="x", padx=6, pady=2)
        ttk.Label(row, text=label, width=18).pack(side="left")
        var = tk.DoubleVar(value=self._shown(addr, scale, decode))
        shown = tk.StringVar(value=f"{var.get():.{digits}f}")
        var.trace_add("write", lambda *_: shown.set(f"{var.get():.{digits}f}"))
        self.vars[addr] = (var, scale, digits, encode, decode)
        scale_w = ttk.Scale(row, from_=lo, to=hi, variable=var,
                            command=lambda _v, a=addr: self._on_slider(a))
        scale_w.pack(side="left", fill="x", expand=True)
        ttk.Label(row, textvariable=shown, width=7).pack(side="left")

    def _check(self, parent, label, addr):
        var = tk.IntVar(value=self.regs.get(addr))
        self.vars[addr] = var
        ttk.Checkbutton(parent, text=label, variable=var,
                        command=lambda a=addr: self._on_check(a)
                        ).pack(anchor="w", padx=8, pady=1)

    def _choice(self, parent, label, addr, options):
        row = ttk.Frame(parent)
        row.pack(fill="x", padx=6, pady=2)
        ttk.Label(row, text=label, width=16).pack(side="left")
        names = [n for n, _ in options]
        var = tk.StringVar()
        self.vars[addr] = (var, dict(options), {v: n for n, v in options})
        box = ttk.Combobox(row, textvariable=var, values=names, state="readonly", width=12)
        box.pack(side="left")
        box.bind("<<ComboboxSelected>>", lambda _e, a=addr: self._on_choice(a))
        self._show_choice(addr)

    def _raw_entry(self, parent, label, addr):
        """A register typed as a raw 0-65535 number (Enter or leaving applies)."""
        row = ttk.Frame(parent)
        row.pack(fill="x", padx=6, pady=2)
        ttk.Label(row, text=label, width=18).pack(side="left")
        var = tk.StringVar(value=str(self.regs.get(addr)))
        entry = ttk.Entry(row, textvariable=var, width=8)
        entry.pack(side="left")

        def apply(_e=None):
            try:
                text = var.get().strip().lower()
                value = int(text, 16) if text.startswith("0x") else int(text)
                self.regs.set(addr, max(0, min(0xFFFF, value)))
            except ValueError:
                pass
            var.set(str(self.regs.get(addr)))

        entry.bind("<Return>", apply)
        entry.bind("<FocusOut>", apply)
        self.vars[addr] = ("raw", var, entry)

    def _shown(self, addr, scale, decode):
        raw = self.regs.get(addr)
        if decode:
            raw = decode(raw)
        return raw * scale

    def _on_slider(self, addr):
        if self._updating:
            return
        var, scale, _digits, encode, _decode = self.vars[addr]
        raw = int(round(var.get() / scale))
        self.regs.set(addr, encode(raw) if encode else raw)

    def _on_check(self, addr):
        if not self._updating:
            self.regs.set(addr, 1 if self.vars[addr].get() else 0)

    def _on_choice(self, addr):
        if self._updating:
            return
        var, by_name, _by_value = self.vars[addr]
        if var.get() in by_name:
            self.regs.set(addr, by_name[var.get()])

    def _show_choice(self, addr):
        var, _by_name, by_value = self.vars[addr]
        var.set(by_value.get(self.regs.get(addr), ""))

    def _refresh_all(self):
        self._updating = True
        try:
            for addr, spec in self.vars.items():
                if isinstance(spec, tuple) and spec[0] == "raw":
                    continue  # typed by hand; nothing else changes it
                if isinstance(spec, tk.IntVar):
                    spec.set(1 if self.regs.get(addr) else 0)
                elif isinstance(spec[0], tk.StringVar):
                    self._show_choice(addr)
                else:
                    var, scale, digits, _encode, decode = spec
                    var.set(round(self._shown(addr, scale, decode), digits))
        finally:
            self._updating = False

    def _on_energy(self, addr, leave=False):
        var, _entry = self.energy[addr]
        # Only apply text the user actually changed: a FocusOut (e.g. alt-tab)
        # on an untouched field would otherwise write a stale value back.
        if var.get() != self._energy_shown.get(addr):
            try:
                self.regs.set_energy(addr, float(var.get()))
            except ValueError:
                pass
        if leave:
            self.root.focus_set()  # Enter leaves the field so it updates live again
        self._refresh_energy()

    def _refresh_energy(self):
        try:
            focused = self.root.focus_get()
        except KeyError:  # raised while a Combobox dropdown is open
            focused = None
        for addr, (var, entry) in self.energy.items():
            if entry is not focused:  # don't overwrite what's being typed
                # Truncate to tenths, as the HMI does with the 0.1 kWh register.
                text = f"{int(self.regs.get_energy(addr) * 10) / 10:.1f}"
                var.set(text)
                self._energy_shown[addr] = text

    def _preset(self, name):
        for addr, value in PRESETS[name].items():
            self.regs.set(addr, value)
        self._refresh_all()
        self._append(f"preset: {name}")

    def _set_simulate(self):
        self.regs.simulate_battery = self.simulate.get()

    def _set_delay(self):
        try:
            self.regs.commit_delay = max(0.0, float(self.delay.get()))
        except (tk.TclError, ValueError):
            pass

    def _disconnect(self):
        if self.ble:
            self.ble.disconnect()

    def _append(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        self.log.see("end")
        if int(self.log.index("end-1c").split(".")[0]) > 200:
            self.log.delete("1.0", "2.0")
        self.log.configure(state="disabled")

    def _poll(self):
        now = time.monotonic()
        self.regs.tick(now - self._last_tick)
        self._last_tick = now
        committed = False
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "status":
                    self.status.set(payload)
                elif kind == "log":
                    self._append(payload)
                elif kind == "commit":
                    addr, value = payload
                    self._append(f"committed {addr} = {value}")
                    committed = True
        except queue.Empty:
            pass
        if committed or self.regs.simulate_battery:
            self._refresh_all()
        self._refresh_energy()
        self.root.after(100, self._poll)

    def _close(self):
        if self.ble:
            self.ble.stop()
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
