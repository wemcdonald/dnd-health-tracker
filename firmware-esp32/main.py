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
  RUN  : STA connected + slug configured (either local or pushed from the
         server) -> poll loop.
  SETUP: otherwise -> AP + captive portal + config UI. With WiFi saved and
         the portal idle for SETUP_IDLE_RESET_S, reboots to retry (router back
         up, or a slug assigned from the admin page meanwhile).

Power is handled in hardware: the bar runs off a standalone LiPo charger board
whose switched OUTPUT feeds the device, so a plain power switch gives true-off
(zero draw) while USB still charges the cell upstream. No on-device sleep needed.

OTA: while online, checks the server manifest (first good poll, then hourly) and
self-updates via the ESP32 dual-app partitions (see ota.py). The first server
answer after boot (health line or 404 unknown-slug) marks the running image
valid; an image that can't reach the server within PROBATION_S reboots, and the
bootloader rolls back. A version that rolled back is then skipped until a
different one is published (ota.note_boot / /data/ota.json).

Remote config: right after WiFi connects and every remote_config.CHECK_EVERY_S,
pulls this board's server-pushed config (see remote_config.py, device.py).
"""

import gc

import anim
import config
import device
import leds
import ota
import poll
import remote_config
import version
import wifi

DATA_DIR = "/data"
WDT_TIMEOUT_MS = 8000
OFFLINE_RESET_SECONDS = 90
OTA_CHECK_EVERY_S = 3600   # check for a firmware update at most this often
PROBATION_S = 180          # a new image that can't reach the server this long reboots (-> rollback)
SETUP_IDLE_RESET_S = 180   # setup portal idle this long with WiFi saved -> reboot and retry


# ----- platform helpers (degrade gracefully off-device) -------------------

def _monotonic():
    try:
        import time
        return time.ticks_ms() / 1000
    except (ImportError, AttributeError):
        import time
        return time.monotonic()


def _ticks():
    """Raw monotonic tick count (ms). Wraps at 2**30 ms (~12.4 days) on MicroPython."""
    import time
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.monotonic() * 1000)


def _elapsed_s(t0):
    """Seconds elapsed since tick t0 from _ticks(), correct across a ticks_ms() wrap."""
    import time
    try:
        return time.ticks_diff(_ticks(), t0) / 1000
    except AttributeError:
        return (_ticks() - t0) / 1000


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


def setup_idle_s(since_start_s, since_activity_s):
    """Seconds the setup portal has been idle: measured from the later of
    setup-mode start and the last real portal activity (None = none yet)."""
    if since_activity_s is None or since_activity_s > since_start_s:
        return since_start_s
    return since_activity_s


def setup_should_reset(has_nets, idle_s):
    """Leave setup mode by rebooting?

    Only when WiFi is saved (so a reboot can reconnect: a fresh board waiting
    for a slug from the admin page, or a router that was down) and the portal
    has been idle for SETUP_IDLE_RESET_S. An inactivity timer rather than a
    "was ever used" flag, so a phone that auto-joins the AP and auto-opens the
    page only delays the retry. With no saved WiFi the portal is the only way
    forward, so it serves forever.
    """
    return has_nets and idle_s > SETUP_IDLE_RESET_S


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


def _config_check(dev, net, ident, strip):
    """Pull server-pushed config; returns the (possibly new) Device. Never raises."""
    try:
        mac = device.mac_id()
    except Exception:
        return dev
    new = remote_config.check(dev, ident, mac=mac, fw_version=version.FIRMWARE_VERSION,
                              connected_ssid=net.connected_ssid(), local_ip=net.sta_ip(),
                              data_dir=DATA_DIR)
    if new is None:
        return dev
    try:
        strip.brightness = new.brightness  # NeoStrip reads this every frame
    except Exception:
        pass
    return new


# ----- run mode (poll loop + OTA) -----------------------------------------

def _run_mode(dev, engine, net, ident, strip):
    engine.set_status(anim.ONLINE)
    started = _ticks()
    last_ota = None      # None = never checked; check on the first good poll
    last_cfg = started   # _run just did a config check
    offline_since = None
    image_confirmed = False

    while True:
        data = poll.fetch(dev)

        if data is None:
            engine.set_status(anim.OFFLINE)
            if offline_since is None:
                offline_since = _ticks()
            elif _elapsed_s(offline_since) > OFFLINE_RESET_SECONDS:
                _reset()  # reboot -> reconnect, or fall into setup if it fails
        else:
            # The server answered (health line or 404 unknown-slug): network and
            # this image are fine.
            offline_since = None
            if not image_confirmed:
                ota.mark_valid()  # cancels rollback of a freshly-OTA'd image
                ota.note_confirmed(DATA_DIR)  # ...so the pending update took
                image_confirmed = True
            if data == poll.UNKNOWN_SLUG:
                engine.set_status(anim.OFFLINE)  # fix the slug in the admin page
            else:
                cur, mx, temp, age = data
                engine.set_health(anim.Health(cur, mx, temp))
                engine.set_status(anim.ONLINE)
            if last_ota is None or _elapsed_s(last_ota) > OTA_CHECK_EVERY_S:
                last_ota = _ticks()
                try:
                    manifest, available = ota.check(dev, DATA_DIR)
                    if available and ota.apply_update(dev, manifest, DATA_DIR):
                        ota.reboot()
                except Exception:
                    pass  # OTA is best-effort; never let it wedge the bar

        if not image_confirmed and _elapsed_s(started) > PROBATION_S:
            _reset()  # a pending-verify image that never reached the server rolls back

        if _elapsed_s(last_cfg) > remote_config.CHECK_EVERY_S:
            last_cfg = _ticks()
            dev = _config_check(dev, net, ident, strip)

        gc.collect()
        _sleep(dev.poll_seconds)


# ----- setup mode ---------------------------------------------------------

async def _setup_idle_watch(p, has_nets):
    """Reboot out of an idle setup portal (see setup_should_reset)."""
    import uasyncio
    started = _ticks()
    while True:
        await uasyncio.sleep(5)
        last = p.last_activity
        idle = setup_idle_s(_elapsed_s(started), None if last is None else _elapsed_s(last))
        if setup_should_reset(has_nets, idle):
            _reset()  # reconnect, re-check config (maybe a slug was assigned)


async def _setup(p, has_nets):
    import uasyncio
    uasyncio.create_task(_setup_idle_watch(p, has_nets))
    await p.serve()


def _setup_mode(engine, net, has_nets):
    import uasyncio
    import portal
    engine.set_status(anim.CONNECTING)
    p = portal.Portal(net, DATA_DIR, reset=_reset, ticks=_ticks)
    uasyncio.run(_setup(p, has_nets))


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
    ota.note_boot(DATA_DIR)  # a staged update we aren't running rolled back: skip it
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
    if nets:
        connected = net.connect_known(nets, timeout=15) is not None

    ident = device.load_identity(DATA_DIR)
    if connected:
        # Before the first poll: may assign a slug to a fresh board, so a new bar
        # only needs WiFi and the character is picked in the admin page.
        dev = _config_check(dev, net, ident, strip)

    if choose_mode(connected, bool(dev.slug)) == "run":
        _run_mode(dev, engine, net, ident, strip)
    else:
        # Re-read: a config check may have pushed networks since `nets` was loaded.
        _setup_mode(engine, net, bool(config.load_wifi(DATA_DIR)))


if __name__ == "__main__":
    run()
