"""Exercise check through the executable, including child cleanup."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())


class CheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        config = self.root / "rtsp/config"
        config.mkdir(parents=True, mode=0o700)
        config.parent.chmod(0o700)
        self.camera = config / "cameras.conf"
        self.camera.write_text('[camera "doorway"]\nmain = rtsp://example:sample-pass@camera.invalid/stream\n')
        self.camera.chmod(0o600)
        fake = self.root / "ffmpeg"
        fake.write_text('''#!/usr/bin/env python3
import os,sys,time
from pathlib import Path
if '-version' in sys.argv:
    print('libavformat 61.0.0')
    raise SystemExit(0)
Path(os.environ['CHECK_PID']).write_text(str(os.getpid()))
mode=os.environ.get('CHECK_MODE','frames')
count=3 if mode=='frames' else 1 if mode=='stale' else 0
for _ in range(count):
    sys.stdout.buffer.write(bytes(320*180*4)); sys.stdout.buffer.flush()
    time.sleep(.05)
time.sleep(60)
''')
        fake.chmod(0o700)
        self.env = {**os.environ, "KILIX_RTSP_HOME": str(config.parent),
                    "KILIX_NVR_HOME": str(self.root / "nvr"),
                    "KILIX_RTSP_FFMPEG": str(fake),
                    "CHECK_PID": str(self.root / "child.pid")}

    def run_check(self, *args):
        result = subprocess.run([BINARY, "check", *args], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertNotIn("sample-pass", result.stdout + result.stderr)
        self.assertFalse((self.root / "nvr").exists(), "check created recorder state")
        pidfile = self.root / "child.pid"
        if pidfile.exists():
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pidfile.read_text()), 0)
        return result

    def test_three_fresh_frames_succeed_and_stop_decoder(self):
        result = self.run_check("doorway", "--seconds", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("fresh video received", result.stdout)

    def test_one_frame_or_silence_is_not_healthy(self):
        for mode in ("stale", "silence"):
            with self.subTest(mode=mode):
                self.env["CHECK_MODE"] = mode
                result = self.run_check("doorway", "--seconds", "1")
                self.assertEqual(result.returncode, 1)
                self.assertIn("no fresh video", result.stderr)

    def test_bad_configuration_and_arguments_do_not_start_decoder(self):
        for seconds in ("0", "61", "nan", "1x", "-1", "999999999999999999999"):
            self.assertEqual(self.run_check("doorway", "--seconds", seconds).returncode, 2)
        self.assertEqual(self.run_check("unknown").returncode, 1)
        self.camera.chmod(0o644)
        self.assertEqual(self.run_check("doorway").returncode, 1)
        self.assertFalse((self.root / "child.pid").exists())

    def test_interrupt_stops_decoder(self):
        self.env["CHECK_MODE"] = "silence"
        process = subprocess.Popen([BINARY, "check", "doorway"], env=self.env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / "child.pid").exists() and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertTrue((self.root / "child.pid").exists())
            process.send_signal(signal.SIGINT)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 130)
            with self.assertRaises(ProcessLookupError):
                os.kill(int((self.root / "child.pid").read_text()), 0)
        finally:
            if process.poll() is None:
                process.kill()
            process.communicate()


if __name__ == "__main__":
    unittest.main()
