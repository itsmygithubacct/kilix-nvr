"""The bundled detector delegates admission and preserves the binary stream."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DetectorLauncherTests(unittest.TestCase):
    def test_executes_only_the_checked_runtime_and_preserves_frames(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            command = root/'kilix'
            command.write_text(f'#!{sys.executable}\n'
                               'import json, os, sys\n'
                               'open(os.environ["CALL"], "w").write(json.dumps(sys.argv[1:]))\n'
                               'sys.stdout.buffer.write(sys.stdin.buffer.read())\n')
            command.chmod(0o700)
            call = root/'call.json'
            frame = bytes(range(256)) + b'\0' * 224
            result = subprocess.run([sys.executable, str(ROOT/'tools/kilix-look-detect'),
                                     '--geometry', '320x320', '--conf', '0.1'],
                                    env=dict(os.environ, PATH=str(root), CALL=str(call)),
                                    input=frame, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, frame)
            self.assertEqual(json.loads(call.read_text()),
                             ['yolox', 'run', '--geometry', '320x320', '--conf', '0.1'])

    def test_missing_host_reports_failure_without_fake_detections(self):
        with tempfile.TemporaryDirectory() as td:
            result = subprocess.run([sys.executable, str(ROOT/'tools/kilix-look-detect')],
                                    env=dict(os.environ, PATH=td), input=b'frame',
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 127)
            self.assertEqual(result.stdout, b'')
            self.assertIn(b'kilix install yolox', result.stderr)


if __name__ == '__main__':
    unittest.main()
