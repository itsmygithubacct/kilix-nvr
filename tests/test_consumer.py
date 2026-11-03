"""C raw scores and policy agree with the provider's Python policy."""
from pathlib import Path
import struct
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'third_party/kilix-sound-detect/tools'))
from kilix_sound_common import CLASSES, thresholds

subprocess.run([sys.executable, '-B', 'tests/fixture_providers.py', 'prepare'], cwd=root, check=True)
result = subprocess.run([str(root/'build/test-consumer')], cwd=root, capture_output=True, text=True)
print(result.stdout, end='')
print(result.stderr, file=sys.stderr, end='')
assert result.returncode == 0
expected = thresholds(root/'build/thresholds.txt', .6)
rows = [line.split() for line in result.stdout.splitlines() if line.startswith('policy ')]
assert [row[1] for row in rows] == CLASSES
for _, name, raw, policy in rows:
    assert float(raw) == .5
    assert struct.pack('<f', float(policy)) == struct.pack('<f', expected[name])
print('ok C/Python float32 policy and raw-score agreement')
