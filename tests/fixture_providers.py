#!/usr/bin/env python3
"""Deterministic wire fixtures; no model or external input."""
import struct
import sys
import wave
from pathlib import Path

mode = sys.argv[1]
if mode == 'prepare':
    Path('build').mkdir(exist_ok=True)
    with wave.open('build/integration.wav', 'wb') as out:
        out.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
        out.writeframes(struct.pack('<h', 64) * (16000 * 3))
    Path('build/thresholds.txt').write_text('dog=0.75\nknock=0.25\n')
    sys.exit(0)
size = 32000 if mode in ('sound', 'policy', 'sound-fail') else 320*320*4
while True:
    frame = sys.stdin.buffer.read(size)
    if len(frame) != size:
        break
    if mode.endswith('fail'):
        print('intentional fixture provider failure', file=sys.stderr)
        sys.exit(23)
    if mode == 'sound':
        rows = [[i, .5 + .05*i, 0, 0, 0, 0] for i in range(9)]
    elif mode == 'policy':
        rows = [[i, .5, 0, 0, 0, 0] for i in range(9)]
        rows += [[99, .99, 0, 0, 0, 0]]
    else:
        rows = [[16, .75, .2, .2, .8, .8], [56, .99, .1, .1, .9, .9],
                [0, .1, .1, .1, .9, .9]]
    rows += [[0]*6 for _ in range(20-len(rows))]
    sys.stdout.buffer.write(struct.pack('<120f', *(x for row in rows for x in row)))
    sys.stdout.buffer.flush()
