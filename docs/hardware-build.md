# Hardware build — power & USB-C

How the physical LED health bar is powered and how the USB-C connector is wired,
for the current **Raspberry Pi Pico 2 W** build. This is the part of the build
that isn't captured elsewhere (the LED wiring lives in
[`../firmware/README.md`](../firmware/README.md#wiring)).

> **Provenance.** Synthesized from two design/research conversations (2026-09):
> [Pico power solutions](https://chatgpt.com/share/6aaf4567-5258-83e8-bb87-b3274518c576)
> and [Pico W USB-C charging](https://chatgpt.com/share/6aaf458a-5098-83e8-8512-0eaf63fb0035).
> Wiring and part *class* are settled; a few exact SKUs / the battery capacity
> are marked **TODO — confirm from the physical build**.

## Overview

The Pico 2 W only exposes **micro-USB** on-board and has no battery management.
The build adds two boards:

1. **Battery / charge / power-path:** a **Pimoroni Pico LiPo SHIM** — charges a
   single-cell LiPo and does proper power-path so the bar runs off USB *or*
   battery seamlessly.
2. **USB-C connector:** a small **USB-C breakout** wired to the Pico's underside
   USB test pads, so the finished unit has a modern USB-C port for both power and
   data (flashing / serial) instead of the bare micro-USB.

```
        USB-C breakout ──(VBUS 5V)──► Pico VBUS (pin 40) ──► Pico LiPo SHIM ──► LiPo cell
              │  (D+/D-)                                          │ (VSYS power-path)
              └──────────► Pico USB test pads (TP2/TP3)           ▼
                                                            Pico 2 W (VSYS) ──► WS2812B strip
```

## 1. Battery / charging — Pico LiPo SHIM

**Board:** Pimoroni **Pico LiPo SHIM** (e.g. pishop.us / Pimoroni). Solders onto
the Pico's underside pins. It:

- Charges a single-cell **LiPo/Li-Ion** using the Pico's **USB 5 V / VBUS** as
  the charge source (you plug USB into the Pico, not the SHIM).
- Feeds the battery into the Pico's **VSYS** rail, so the bar keeps running from
  LiPo when USB is unplugged.
- Provides **ideal-diode power-path / load-sharing**: USB/VBUS powers the Pico
  *while* the battery charges, instead of the Pico's load hanging in parallel
  across the charger.
- Includes **over-discharge and over-current protection** for the cell.

**Battery:** single-cell 3.7 V LiPo with a JST-PH connector. **TODO — capacity
(mAh) of the cell actually fitted.** Size it for the strip: 16 WS2812B LEDs near
full-white draw ~1 A, so runtime is short at full brightness — the firmware's
default brightness keeps typical draw well under that.

### Why not a bare TP4056 / MCP73831 board?

Considered and rejected. Cheap **TP4056** USB-C modules and **MCP73831**
(e.g. Adafruit Micro-Lipo) boards are *not* true power-path chargers: the load
sits in parallel with the battery, so running the Pico while charging steals
charge current and can confuse charge termination (the charger may report
"charging" indefinitely). They work in a pinch via the BAT/GND pins but aren't
correct for run-while-charging. A **power-path / load-sharing / PMIC** board is
required — the Pico LiPo SHIM is the compact, solder-on answer; a discrete
**BQ24074** is the equivalent if building on a custom PCB.

## 2. USB-C breakout → Pico 2 W

**Board:** a USB-C breakout exposing VBUS, GND, D+, D-, CC1, CC2 (e.g. an
Adafruit USB Type-C downstream breakout). **TODO — confirm exact breakout SKU.**

The Pico 2 W does **not** bring D+/D- out to castellated edge pins — they are on
the **underside test pads**. Wire to the pads, not to GPIO:

| USB-C breakout | Pico 2 W            | Notes                          |
|----------------|---------------------|--------------------------------|
| VBUS / 5 V     | **pin 40 (VBUS)**   | edge pin; feeds the SHIM       |
| GND            | **TP1** (or any GND)| USB shield ground              |
| D−             | **TP2** (USB DM)    | data minus                     |
| D+             | **TP3** (USB DP)    | data plus                      |
| CC1            | 5.1 kΩ → GND        | one resistor per CC pin        |
| CC2            | 5.1 kΩ → GND        | diverts a UFP into sink mode  |

### Gotchas

- **CC resistors:** each CC pin needs its **own** 5.1 kΩ pulldown to GND. Some
  breakouts include them already — check before adding duplicates.
- **Do not add USB series resistors.** The Pico 2 W already has the required
  **27 Ω** series resistors on D+/D-.
- **Route D+/D- short, paired, roughly equal length**, away from the
  switching regulator and the Wi-Fi antenna.
- **Don't drive both USB data paths at once.** With the USB-C breakout's data
  lines wired to TP2/TP3, avoid also plugging a cable into the on-board
  micro-USB for data at the same time — two hosts on one device's D+/D- is a
  conflict. (Power-only on VBUS is fine.)
- Raspberry Pi documents TP1/TP2/TP3 as the external USB ground / D− / D+.
  Confirm against the Pico 2 W datasheet before soldering, as Pico and Pico-W
  test-point layouts differ.

## Assembly order

1. Solder the **Pico LiPo SHIM** onto the Pico 2 W underside pins.
2. Wire the **USB-C breakout** VBUS/GND to VBUS(pin 40)/GND and D+/D- to
   TP3/TP2; add the two 5.1 kΩ CC pulldowns if the breakout lacks them.
3. Connect the **LiPo** to the SHIM's JST.
4. Wire the **WS2812B strip** per [`firmware/README.md`](../firmware/README.md#wiring)
   (DIN → GPIO18 through a level shifter; strip +5 V / common GND).
5. Flash and provision per the firmware README.

## Forward path: ESP32-C3 variant (candidate)

Under evaluation to collapse the Pico + LiPo SHIM + USB-C breakout stack. Baseline
(Pico) build above stays as-is.

- **Board:** Seeed XIAO ESP32-C3 — USB-C, external u.FL antenna. (It has onboard
  LiPo charging, but see the power note below for why we drive charging off-board.)
- **LED data pin:** **GPIO10 (pad D10)** — non-strapping, non-UART. Level shifter
  still needed (3.3 V→5 V), or run the strip at ~4.5 V.
- **Power the strip from battery voltage**, not the 3V3 rail (500 mA cap); keep
  brightness modest on battery. Common ground.
- **Power + switch (recommended):** a **standalone protected LiPo charger board**
  (e.g. TP4056 + DW01/FS8205) with separate `B±` (cell) and `OUT±` (device)
  terminals. Put a plain **SPST switch on `OUT+`** feeding the XIAO BAT pad + strip:

  ```
  LiPo ─(B±)─ charger board ─ USB-C in (charges the cell anytime)
                 OUT+ ─[ switch ]─ XIAO BAT+  &  strip +
                 OUT- ──────────── common GND
  ```

  Off = true zero draw (MCU + strip both cut); the charger is upstream on `B±`, so
  **USB still charges the cell while switched off**. Charge only via the charger
  board's USB-C; leave the XIAO's own USB unused for power. This is why the
  firmware needs **no deep sleep / strip-gate** — the switch does it in hardware,
  and it avoids the WS2812's ~10–16 mA idle that MCU sleep can't remove.
  - *Simpler alt:* use the XIAO's onboard charger + an inline SPST on the LiPo `+`
    lead. Works, but "off" then isolates the cell from the charger, so you must
    switch on + plug USB to charge.
- **Firmware:** [`firmware-esp32/`](../firmware-esp32/) (MicroPython) — thin
  poller + OTA. See its README for pins, power, OTA, and flashing.
- **Alternative considered:** SparkFun Qwiic Pocket Dev Board (ESP32-C6-MINI-1,
  1"×1", USB-C + JST + MCP73831 @214 mA). Works too; larger, C6 features unused.
