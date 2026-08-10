#!/usr/bin/env python3
"""A sound classifier that answers without a model, for the protocol test."""
import os
import struct
import sys

ROWS, COLUMNS = 20, 6
WINDOW_BYTES = 16000 * 2


def main():
    score = float(os.environ.get("FAKE_LISTEN_SCORE", "0.8"))
    class_id = float(os.environ.get("FAKE_LISTEN_CLASS", "2"))
    while True:
        remaining = WINDOW_BYTES
        while remaining > 0:
            chunk = sys.stdin.buffer.read(remaining)
            if not chunk:
                return 0
            remaining -= len(chunk)
        rows = [0.0] * (ROWS * COLUMNS)
        rows[0], rows[1] = class_id, score
        sys.stdout.buffer.write(struct.pack("<%df" % (ROWS * COLUMNS), *rows))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    sys.exit(main())
