"""Desktop demo: drive the animation engine into the ANSI sim with scripted HP.

    cd firmware && python3 demo.py [num_frames]

Plays boot -> connecting -> live HP, then scripts damage / heal / low-HP / temp
events so the animations can be eyeballed without hardware. Mirrors the Go
`--demo` flag. Runs forever by default, or for `num_frames` frames if given.
"""

import sys
import time

import anim
import config
import leds

# (seconds_from_start, kind, args)
SCRIPT = [
    (1.3, "status", anim.ONLINE),
    (1.4, "health", (30, 30, 0)),    # full
    (3.0, "health", (12, 30, 0)),    # damage
    (5.0, "health", (12, 30, 8)),    # gain temp HP
    (7.0, "health", (28, 30, 0)),    # heal
    (9.0, "health", (5, 30, 0)),     # low HP -> heartbeat pulse
    (11.0, "status", anim.OFFLINE),  # lost connection -> breathing
    (13.0, "status", anim.ONLINE),
]


def main():
    max_frames = int(sys.argv[1]) if len(sys.argv) > 1 else None
    theme = config.load_theme("data")
    dev = config.load_device("data")
    engine = anim.Engine(theme, dev.num_leds)
    strip = leds.SimStrip(dev.num_leds, dev.brightness)
    frame = anim.new_frame(dev.num_leds)
    period = 1.0 / theme.fps

    start = time.monotonic()
    last = start
    idx = 0
    frames = 0
    try:
        while max_frames is None or frames < max_frames:
            now = time.monotonic()
            elapsed = now - start
            while idx < len(SCRIPT) and elapsed >= SCRIPT[idx][0]:
                _, kind, arg = SCRIPT[idx]
                if kind == "status":
                    engine.set_status(arg)
                else:
                    engine.set_health(anim.Health(*arg))
                idx += 1
            engine.render(frame, now - last)
            last = now
            strip.render(frame)
            frames += 1
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    finally:
        strip.close()


if __name__ == "__main__":
    main()
