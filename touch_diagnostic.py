#!/usr/bin/env python3
"""Print raw events from the WaveShare touchscreen on a Linux Raspberry Pi."""

import glob
import os
import select
import struct
import sys


def find_touchscreen():
    candidates = []
    for name_path in glob.glob("/sys/class/input/event*/device/name"):
        try:
            with open(name_path, encoding="utf-8", errors="replace") as source:
                name = source.read().strip()
        except OSError:
            continue

        lowered = name.casefold()
        if "waveshare" in lowered or "touchscreen" in lowered:
            event = name_path.split("/")[-3]
            candidates.append(("waveshare" not in lowered, name, "/dev/input/" + event))

    if not candidates:
        print("No WaveShare touchscreen found. Available input devices:", file=sys.stderr)
        for name_path in glob.glob("/sys/class/input/event*/device/name"):
            try:
                with open(name_path, encoding="utf-8", errors="replace") as source:
                    print("  {} ({})".format(source.read().strip(), name_path.split("/")[-3]), file=sys.stderr)
            except OSError:
                pass
        return None

    candidates.sort()
    return candidates[0][1:]


def describe(event_type, code, value):
    if event_type == 1:
        name = {330: "BTN_TOUCH", 325: "BTN_TOOL_FINGER"}.get(code, "KEY_{}".format(code))
        if code == 330:
            value = "down" if value else "up"
        return "KEY {} {}".format(name, value)

    if event_type == 3:
        name = {
            0: "ABS_X", 1: "ABS_Y", 47: "ABS_MT_SLOT", 53: "ABS_MT_POSITION_X",
            54: "ABS_MT_POSITION_Y", 57: "ABS_MT_TRACKING_ID", 58: "ABS_MT_PRESSURE",
        }.get(code, "ABS_{}".format(code))
        return "ABS {} {}".format(name, value)

    return None


def main():
    device = find_touchscreen()
    if device is None:
        return 1

    name, path = device
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError as error:
        print("Cannot open {}: {}. Try sudo python3 touch_diagnostic.py".format(path, error), file=sys.stderr)
        return 1

    print("Listening to {} at {}. Tap or drag the touchscreen. Ctrl+C exits.".format(name, path), flush=True)
    event_struct = struct.Struct("@llHHi")
    try:
        while True:
            ready, _, _ = select.select([fd], [], [])
            if not ready:
                continue
            data = os.read(fd, event_struct.size * 64)
            for _, _, event_type, code, value in event_struct.iter_unpack(data):
                if event_type == 0 and code == 0:
                    print("  --- touch frame ---", flush=True)
                else:
                    message = describe(event_type, code, value)
                    if message:
                        print("  " + message, flush=True)
    except KeyboardInterrupt:
        print("Stopped.")
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
