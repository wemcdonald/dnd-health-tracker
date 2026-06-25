"""Supervisor: boot, mode selection, render thread, and recovery.

Replaces the Go ``cmd/healthbar`` entrypoint + ``internal/app`` supervisor and
the systemd ``Restart=always`` unit. On a microcontroller there is no init
system, so reliability is built in here:

  - The LED render loop runs on **core 1** via ``_thread`` so animations stay
    smooth even while a blocking TLS fetch occupies core 0.
  - A hardware ``WDT`` (fed by the render thread) reboots a fully-wedged board.
  - ``run()`` is wrapped so any unhandled exception triggers ``machine.reset()``.
  - On sustained WiFi loss the supervisor resets, which re-runs network
    selection and drops into the setup portal if nothing connects.

Two mutually-exclusive modes keep peak RAM low (only one is ever resident):
  RUN  : STA connected + character configured -> TLS poll loop, no web server.
  SETUP: otherwise -> AP + captive portal + config UI, no TLS client.
"""

import gc

import anim
import config
import ddb
import leds
import portal
import wifi

DATA_DIR = "/data"   # littlefs path on the device
WDT_TIMEOUT_MS = 8000
OFFLINE_RESET_SECONDS = 90
WS_SAFETY_NET_SECONDS = 30  # relaxed poll interval while the websocket is live


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

def choose_mode(connected, has_character):
    """RUN only when we have both a network and a character to poll."""
    return "run" if (connected and has_character) else "setup"


# ----- render thread (core 1) ---------------------------------------------

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


# ----- run modes ----------------------------------------------------------

async def _run_mode(dev, engine, net):
    import uasyncio
    engine.set_status(anim.ONLINE)

    def on_hp(snap):
        engine.set_health(anim.Health(snap[0], snap[1], snap[2]))

    def on_status(online):
        engine.set_status(anim.ONLINE if online else anim.OFFLINE)

    cookie = config.cobalt_cookie(DATA_DIR)
    use_ws = bool(dev.game_id and dev.user_id and cookie)
    fast = dev.poll_seconds
    slow = max(dev.poll_seconds, WS_SAFETY_NET_SECONDS)

    event = uasyncio.Event() if use_ws else None
    poller = ddb.Poller(
        lambda cid: ddb.fetch_hp(cid), dev.character_id,
        on_hp=on_hp, on_status=on_status,
        interval=fast, event=event,
    )
    uasyncio.create_task(poller.run())

    if use_ws:
        # Live push: relax polling to a safety-net while the socket is healthy;
        # snap back to responsive polling if it drops (incl. on MemoryError).
        import auth
        import ws as wsmod

        def on_ws_state(connected):
            poller.interval = slow if connected else fast
            poller.nudge()  # re-evaluate the current wait immediately

        authr = auth.CookieAuth(cookie)
        listener = wsmod.WSListener(
            authr.get_token, dev.game_id, dev.user_id,
            on_nudge=poller.nudge, on_state=on_ws_state,
        )
        uasyncio.create_task(listener.run())

    bad_since = None
    while True:
        await uasyncio.sleep(5)
        gc.collect()
        if net.is_online():
            bad_since = None
        else:
            if bad_since is None:
                bad_since = _monotonic()
            elif _monotonic() - bad_since > OFFLINE_RESET_SECONDS:
                _reset()  # reboot -> reconnect, or fall into setup if it fails


async def _setup_mode(engine, net):
    engine.set_status(anim.CONNECTING)
    p = portal.Portal(net, DATA_DIR, reset=_reset)
    await p.serve()


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
    import uasyncio

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
    if nets and dev.character_id:
        connected = net.connect_known(nets, timeout=15) is not None

    if choose_mode(connected, bool(dev.character_id)) == "run":
        uasyncio.run(_run_mode(dev, engine, net))
    else:
        uasyncio.run(_setup_mode(engine, net))


if __name__ == "__main__":
    run()
