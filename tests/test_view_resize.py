"""A real PTY changes size after view starts; observe its graphics packets."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class ResizeTests(unittest.TestCase):
    def test_view_grows_and_shrinks_without_restarting_camera(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "rtsp/config"
            config.mkdir(mode=0o700, parents=True)
            config.parent.chmod(0o700)
            camera = "resize-test-" + str(os.getpid())
            path = config / "cameras.conf"
            path.write_text(f'[camera "{camera}"]\nmain = rtsp://camera.invalid/stream\n')
            path.chmod(0o600)
            fake = root / "ffmpeg"
            fake.write_text('''#!/usr/bin/env python3
import os,sys,time
if '-version' in sys.argv:
    print('libavformat 61.0.0'); raise SystemExit(0)
if '-vn' in sys.argv:
    raise SystemExit(1)
with open(os.environ['VIEW_STARTS'],'a') as log: log.write(str(os.getpid())+'\\n')
while True:
    sys.stdout.buffer.write(bytes(640*360*4)); sys.stdout.buffer.flush()
    time.sleep(.1)
''')
            fake.chmod(0o700)
            env = {**os.environ, "KILIX_RTSP_HOME": str(config.parent),
                   "KILIX_NVR_HOME": str(root / "nvr"), "KILIX_RTSP_FFMPEG": str(fake),
                   "KILIX_SOUND_CLASSIFIER": "/bin/false", "VIEW_STARTS": str(root / "starts"),
                   "KITTYFB_SKIP_PROBE": "1", "KITTYFB_TRANSPORT": "inline"}
            subprocess.run([BINARY, "add", camera], env=env, check=True, capture_output=True)
            master, slave = pty.openpty()

            def size(columns, rows):
                fcntl.ioctl(slave, termios.TIOCSWINSZ,
                            struct.pack("HHHH", rows, columns, columns * 10, rows * 20))

            size(80, 24)
            process = subprocess.Popen([BINARY, "view", camera, "--no-detect", "--seconds", "15"],
                env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True)

            def frame_at(width, height):
                output = bytearray()
                deadline = time.monotonic() + 6
                pattern = rb'\x1b_Ga=T,[^;]*s=' + str(width).encode() + rb',v=' + str(height).encode() + rb'[,;]'
                while time.monotonic() < deadline:
                    if select.select([master], [], [], .1)[0]:
                        output.extend(os.read(master, 65536))
                        if re.search(pattern, output):
                            return
                        if len(output) > 131072:
                            del output[:-65536]
                self.fail(f"No graphics frame at {width}x{height}; process={process.poll()}")

            try:
                # The framebuffer reserves one terminal row for the prompt.
                frame_at(800, 460)
                size(192, 54)
                process.send_signal(signal.SIGWINCH)
                frame_at(1920, 1060)
                size(96, 30)
                process.send_signal(signal.SIGWINCH)
                frame_at(960, 580)
                self.assertEqual(len((root / "starts").read_text().splitlines()), 1)
                os.write(master, b"q")
                deadline = time.monotonic() + 5
                while process.poll() is None and time.monotonic() < deadline:
                    if select.select([master], [], [], .1)[0]:
                        os.read(master, 65536)
                self.assertEqual(process.poll(), 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                process.wait()
                os.close(master)
                os.close(slave)


if __name__ == "__main__":
    unittest.main()
