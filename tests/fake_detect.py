#!/usr/bin/env python3
"""A detector that answers without a model, so the protocol is testable.

Reports one 'person' in the middle of every frame, at a score taken from
the environment.  The point is the framing - one frame in, exactly 480
bytes out - which is the part that can silently desynchronise.
"""
import os
import struct
import sys

ROWS, COLUMNS = 20, 6


def main():
    geometry = "640x360"
    for index, argument in enumerate(sys.argv):
        if argument == "--geometry" and index + 1 < len(sys.argv):
            geometry = sys.argv[index + 1]
    width, _, height = geometry.partition("x")
    frame_bytes = int(width) * int(height) * 4
    score = float(os.environ.get("FAKE_DETECT_SCORE", "0.9"))
    class_id = float(os.environ.get("FAKE_DETECT_CLASS", "0"))

    while True:
        remaining = frame_bytes
        while remaining > 0:
            chunk = sys.stdin.buffer.read(remaining)
            if not chunk:
                return 0
            remaining -= len(chunk)
        rows = [0.0] * (ROWS * COLUMNS)
        rows[0] = class_id
        rows[1] = score
        rows[2], rows[3], rows[4], rows[5] = 0.25, 0.25, 0.75, 0.75
        sys.stdout.buffer.write(struct.pack("<%df" % (ROWS * COLUMNS), *rows))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    sys.exit(main())
