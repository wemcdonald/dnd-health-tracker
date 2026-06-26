# Pico 2 W cold-boot hang — investigation findings (2026-06-25)

Status: **UNRESOLVED.** Flashing works; the firmware runs correctly when launched
from a connected REPL, but **hangs on a real (headless) cold boot** — no USB CDC,
no `healthbar-setup` AP. This doc captures everything learned so the next attempt
(or a C++ rewrite) doesn't have to re-derive it.

---

## TL;DR

- **Flashing the board is fully working** (MicroPython 1.28.0 + all modules + config). The original "flash it" task is done.
- The full SETUP code path (core-1 render thread + hardware WDT + AP + DNS + HTTP server) **runs flawlessly for 12s when launched from a warm REPL with a host attached.** The logic is sound.
- On a genuine cold boot (`boot.py` auto-runs `main.run()`), the board **hangs**: no USB, no AP. It does **not** reset-loop — the hardware WDT never fires.
- An attempted fix (cyw43 settle + scan/AP reorder) **did not work**. Root cause still not pinned.
- The blocker on faster diagnosis is **observability**: MicroPython gives no traceback on a hard fault/hang. A **picoprobe (SWD/gdb)** would show core 0's stuck frame instantly.

---

## Hardware / setup

- Board: **Raspberry Pi Pico 2 W**, RP2350, with cyw43 WiFi. BOOTSEL drive mounts as `/Volumes/RP2350`.
- Host: macOS. No LED strip wired during this session (bare Pico on USB) — irrelevant to the bug; `leds.make_strip` drives the GPIO fine with nothing attached.

## Flashing / deploy procedure that works

1. **MicroPython UF2**: `RPI_PICO2_W`, **ARM** build (family `rp2350-arm-s`), v1.28.0. From <https://micropython.org/download/RPI_PICO2_W/>. (RISC-V build is the wrong target.)
2. **Recovery / reflash recipe** (reliable) — board in BOOTSEL:
   ```sh
   picotool erase                  # wipes flash incl. littlefs (kills boot loops)
   picotool load  <mp.uf2>         # reflash MicroPython
   picotool reboot                 # boot into it
   ```
   `brew install picotool` (used v2.2.0).
3. **Deploy app**: `firmware/push.sh` (needs `mpremote`).

### Tooling gotchas (cost real time)

- **USB-CDC serial to this board is flaky under `mpremote` on this Mac.** Intermittent
  `OSError: [Errno 6] Device not configured` on the RTS toggle at connection close —
  which can *reset the board mid-operation*.
- Running `mpremote` under the default `python 3.13.5t` (**free-threaded**) build made it
  worse. Install under normal CPython:
  ```sh
  uv tool install --python /opt/homebrew/bin/python3.13 mpremote   # -> ~/.local/bin/mpremote
  ```
- **`picotool` (bulk USB) is 100% reliable here** — when serial misbehaves, fall back to picotool.
- **`push.sh` copies `boot.py` FIRST.** If a serial drop happens mid-push you get a
  runnable-but-incomplete app that boot-loops on the missing `/data` config. Safer order:
  **data + all modules first, `boot.py` LAST** — until `boot.py` exists the board can't
  auto-run the app, so any drop is harmless and you just retry.

---

## The bug

### Symptoms
- Cold boot (power cycle) → **no `/dev/cu.usbmodem*`, no `healthbar-setup` WiFi**, indefinitely.
- **Not a reset loop.** A reset loop with the guarded `boot.py` (below) would re-show the
  USB port every cycle; it doesn't. The board hangs.

### The decisive clue
The hardware WDT (`WDT_TIMEOUT_MS = 8000`) is fed **only** by the core-1 render thread.
The board hangs **without** the WDT ever firing ⇒ **core 1 is alive and looping**
(feeding the WDT) ⇒ **core 0 is blocked in a call that never returns.**

### What was ruled out
- **Not the LED/render math** — 10 inline frames OK; core-1 render thread alone OK for 6s.
- **Not core-1 threading per se** — render thread + real WDT + AP + DNS + HTTP server ran
  **12s clean** from a warm REPL (see `diag5` approach below).
- **Not USB-CDC `print()` blocking** — there are **zero `print()` calls** in the boot path
  (`boot.py`, `main.py`, `wifi.py`, `portal.py`, `config.py`, `anim.py` all 0).
- **Not a lock deadlock in `anim.Engine`** — `set_status`/`render` acquire→mutate→release
  with no nested re-acquire.

### The key unverified gap
**Every passing test was a *reimplementation* of the boot path (`diag5`), not the real
`main.run()` → `_run()` → `_setup_mode()` → `portal.serve()`.** And critically, all passing
tests ran with **cyw43 already warm** (initialized by earlier ops in the same power session)
and **with a host draining USB**. Cold cyw43 + headless was never successfully observed at a
breakpoint.

### Leading hypothesis (still unconfirmed)
Core 0 blocks inside a **cyw43 radio call at cold boot**. `Portal.__init__` does, in order,
`net.start_ap()` (first `WLAN(STA_IF)` = cyw43 firmware cold-load, then STA off → AP on) and
then `net.scan()` (STA back **on**, scanning **while the AP is active**). STA-scan-during-AP
on a freshly-loaded cyw43 is a known hang trigger. Core 0 parks there; core 1 keeps feeding
the WDT → indefinite hang, no AP, USB never serviced.

### Attempted fix — DID NOT WORK
Committed alongside this doc as an **unverified** mitigation (see the same commit's diff):
- `wifi._real_sta()`: added `time.sleep(0.5)` after first STA activation to let cyw43 cold-load settle.
- `portal.Portal.__init__`: reordered to `scan()` **before** `start_ap()` (scan while STA is the active iface, before switching to AP).

All 8 CPython unit tests still pass. **But a real cold boot still hangs** (no USB, no AP).
So cyw43-cold-init / scan-during-AP was at most a partial cause. These edits are defensible
hardening but are **unverified against the actual bug** — treat with suspicion.

---

## Diagnostic techniques that worked (reuse these)

- **Capture crashes to a flash file, read after** — survives the flaky serial and WDT resets.
- **NEVER write flash from core 1**, or from core 0 while core 1 is executing from flash —
  it **hard-faults RP2350** (XIP can't run while flash is being programmed). This bit us as a
  *diagnostic artifact* (diag3 died exactly on a core-1 `log()`). Pattern that works: stash
  exceptions in a **RAM list**, set the stop flag, let core 1 exit, **then** write flash from core 0.
- **Guarded `boot.py`** — `time.sleep(4)` before `import main` gives a window to `Ctrl-C` into
  the REPL at boot *before* the app saturates the cores. Lets you break a boot loop without
  BOOTSEL, and read a boot counter, etc.

## The decisive test that was NOT yet run
Run the **real `main.run()` from the REPL with a host attached** (push everything *except*
`boot.py` so the board sits at the REPL, then `import main; main.run()` while streaming). If it
hangs, **`Ctrl-C` prints the exact line core 0 is parked on.** This ends the guessing and was the
logical next step when the session paused.

---

## Recommendations / next tack

1. **Get a picoprobe** (a second Pico flashed as a CMSIS-DAP SWD probe, ~£4) → `gdb` with
   breakpoints and real stack traces. This single change would have shown core 0's stuck frame
   in ~30s instead of the ~6 BOOTSEL forensics cycles this took. Helps debug *either* the
   MicroPython firmware **or** a C++ rewrite.
2. **C++ / Pico SDK rewrite** (user is considering this):
   - *Won't* magically fix concurrency bugs — no GIL means you own all synchronization, and the
     dual-core flash constraint is hardware (use `flash_safe_execute` / multicore lockout).
   - *Will* remove GC pauses / heap fragmentation / `MemoryError` (the design doc's RAM anxiety),
     MicroPython-VM quirks, and — with picoprobe — give real debugging.
   - **Captive portal is the easy half**: `pico-examples/pico_w/wifi/access_point` is almost
     exactly SETUP mode (AP + DHCP server + DNS-answers-everything + hand-rolled HTTP form).
   - **TLS poll + websocket (RUN mode) is the hard half**: lwIP `altcp_tls` + bundled mbedTLS;
     more configurable / smaller than MicroPython `ssl`, so RAM math improves.

## Current board state
Left **hung** after the last cold-boot test, running the real (fixed-attempt) `boot.py`.
To get a clean board: BOOTSEL → `picotool erase` → reflash MicroPython → push modules+data
**without** `boot.py` (sits at REPL).
