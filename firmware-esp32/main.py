"""Supervisor: boot, mode selection, render thread, poll loop, sleep, OTA.

ESP32 port of the Pico supervisor, retargeted at the thin-poller architecture:
the device fetches a precomputed line from the server over plain HTTP and drives
the LEDs; it does no D&D Beyond / TLS / HP math itself.

Reliability (there is no init system on a microcontroller):
  - The LED render loop runs on a second thread (a FreeRTOS task) so animations
    stay smooth while a blocking HTTP poll is in flight.
  - A hardware ``WDT`` (fed by the render thread) reboots a fully-wedged board.
  - ``run()`` wraps everything so any unhandled exception triggers a reset.
  - Sustained offline -> reset (re-selects networks, or drops into setup).

Two mutually-exclusive modes keep peak RAM low:
  RUN  : STA connected + slug configured -> poll loop.
  SETUP: otherwise -> AP + captive portal + config UI.

Power is handled in hardware: the bar runs off a standalone LiPo charger board
whose switched OUTPUT feeds the device, so a plain power switch gives true-off
(zero draw) while USB still charges the cell upstream. No on-device sleep needed.

OTA: while online, periodically checks the server manifest and self-updates via
the ESP32 dual-app partitions (see ota.py). The first good poll after boot marks
the running image valid, so a bad OTA rolls back on the next reset.
"""

import gc

import anim
import config
import leds
import ota
import poll
import wifi

DATA_DIR = "/data"
WDT_TIMEOUT_MS = 8000
OFFLINE_RESET_SECONDS = 90
OTA_CHECK_EVERY_S = 3600   # check for a firmware update at most this often


# ----- platform helpers (degrade gracefully off-device) -------------------

def _monotonic():
    try:
        import time
        return time.ticks_ms() / 1000
    except (ImportError, AttributeError):
        import time
        return time.monotonic()


def _sleep(d):
    import time
    time.sleep(d)


def _start_thread(fn, args):
    try:
        import _thread
        _thread.start_new_thread(fn, args)
    except ImportError:
        import threading
        threading.Thread(target=fn, args=args, daemon=True).start()


def _make_wdt():
    try:
        import machine
        return machine.WDT(timeout=WDT_TIMEOUT_MS)
    except (ImportError, ValueError):
        return None


def _reset():
    try:
        import machine
        machine.reset()
    except ImportError:
        raise SystemExit("reset requested")


# ----- mode selection (pure, testable) ------------------------------------

def choose_mode(connected, has_slug):
    """RUN only when we have both a network and a slug to poll."""
    return "run" if (connected and has_slug) else "setup"


# ----- render thread ------------------------------------------------------

def _render_loop(engine, strip, fps, wdt, stop):
    frame = anim.new_frame(len(strip) if hasattr(strip, "__len__") else engine.n)
    period = 1.0 / fps if fps > 0 else 1.0 / 30
    last = _monotonic()
    while not stop[0]:
        now = _monotonic()
        dt = now - last
        last = now
        if dt < 0 or dt > 1:
            dt = period
        engine.render(frame, dt)
        try:
            strip.render(frame)
        except Exception:
            pass
        if wdt:
            wdt.feed()
        _sleep(period)


# ----- run mode (poll loop + OTA) -----------------------------------------

def _run_mode(dev, engine, net):
    engine.set_status(anim.ONLINE)
    last_ota = 0.0
    offline_since = None
    image_confirmed = False

    while True:
        data = poll.fetch(dev)
        now = _monotonic()

        if data is None:
            engine.set_status(anim.OFFLINE)
            if offline_since is None:
                offline_since = now
            elif now - offline_since > OFFLINE_RESET_SECONDS:
                _reset()  # reboot -> reconnect, or fall into setup if it fails
        else:
            offline_since = None
            cur, mx, temp, age = data
            engine.set_health(anim.Health(cur, mx, temp))
            engine.set_status(anim.ONLINE)
            if not image_confirmed:
                ota.mark_valid()  # a good poll proves this image is healthy
                image_confirmed = True
            if (now - last_ota) > OTA_CHECK_EVERY_S:
                last_ota = now
                try:
                    manifest, available = ota.check(dev)
                    if available and ota.apply_update(dev, manifest):
                        ota.reboot()
                except Exception:
                    pass  # OTA is best-effort; never let it wedge the bar

        gc.collect()
        _sleep(dev.poll_seconds)


# ----- setup mode ---------------------------------------------------------

def _setup_mode(engine, net):
    import uasyncio
    import portal
    engine.set_status(anim.CONNECTING)
    p = portal.Portal(net, DATA_DIR, reset=_reset)
    uasyncio.run(p.serve())


# ----- entrypoint ----------------------------------------------------------

def run():
    try:
        _run()
    except Exception as e:
        try:
            import sys
            sys.print_exception(e)
        except Exception:
            pass
        _reset()  # crash recovery: reboot and start over


def _run():
    dev = config.load_device(DATA_DIR)
    theme = config.load_theme(DATA_DIR)
    engine = anim.Engine(theme, dev.num_leds)
    strip = leds.make_strip(dev.num_leds, dev.gpio_pin, dev.brightness)

    wdt = _make_wdt()
    stop = [False]
    _start_thread(_render_loop, (engine, strip, theme.fps, wdt, stop))

    net = wifi.NetManager()
    engine.set_status(anim.CONNECTING)

    connected = False
    nets = config.load_wifi(DATA_DIR)
    if nets and dev.slug:
        connected = net.connect_known(nets, timeout=15) is not None

    if choose_mode(connected, bool(dev.slug)) == "run":
        _run_mode(dev, engine, net)
    else:
        _setup_mode(engine, net)


if __name__ == "__main__":
    run()
