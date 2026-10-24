"""Fresh installs and service failures, isolated from the user's data and bus."""
import json
import os
from pathlib import Path
import shlex
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls.jsonl"
        stub = self.bin / "systemctl"
        stub.write_text('''#!/usr/bin/env python3
import json, os, sys
with open(os.environ['SERVICE_CALLS'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
if 'kilix-nvr-prune.timer' in sys.argv and os.environ.get('FAIL_TIMER') in sys.argv:
    sys.exit(1)
''')
        stub.chmod(0o700)
        self.env = {"HOME": str(self.home), "PATH": str(self.bin) + ":/usr/bin:/bin",
                    "LANG": "C.UTF-8", "SERVICE_CALLS": str(self.calls)}

    def run_nvr(self, *args, binary=BINARY):
        return subprocess.run([str(binary), *args], env=self.env,
                              capture_output=True, text=True, timeout=10)

    def prepare_existing_directories(self):
        (self.home / ".local/gpu_terminal").mkdir(parents=True)
        (self.home / ".config/systemd/user").mkdir(parents=True)

    def test_empty_home_can_open_policy_store(self):
        result = self.run_nvr("cameras")
        self.assertEqual(result.returncode, 0, result.stderr)
        store = self.home / ".local/gpu_terminal/kilix-nvr/state/cameras.db"
        self.assertTrue(store.is_file())
        self.assertEqual(store.parent.stat().st_mode & 0o777, 0o700)

    def test_nested_override_is_created(self):
        root = self.root / "new/recordings/nvr"
        self.env["KILIX_NVR_HOME"] = str(root)
        result = self.run_nvr("cameras")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((root / "state/cameras.db").is_file())

    def test_relative_override_does_not_silently_use_default_store(self):
        self.prepare_existing_directories()
        self.env["KILIX_NVR_HOME"] = "relative-store"
        result = self.run_nvr("cameras")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.home / ".local/gpu_terminal/kilix-nvr").exists())

    def test_non_directory_parent_is_rejected(self):
        (self.home / ".local").write_text("keep this")
        result = self.run_nvr("cameras")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.home / ".local").read_text(), "keep this")

    def test_grid_refuses_a_policy_list_larger_than_its_capacity(self):
        result = self.run_nvr("cameras")
        self.assertEqual(result.returncode, 0, result.stderr)
        with sqlite3.connect(self.home / ".local/gpu_terminal/kilix-nvr/state/cameras.db") as db:
            db.executemany("INSERT INTO camera (name) VALUES (?)",
                           [(f"sample-{i:03}",) for i in range(65)])
        result = self.run_nvr("view", "--no-detect", "--seconds", "1",
                              "--render", str(self.root / "unused.ppm"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("too many cameras", result.stderr)
        self.assertFalse((self.root / "unused.ppm").exists())

    def test_service_creates_missing_configuration_parents(self):
        # Separate the service-directory case from first-use data initialization.
        (self.home / ".local/gpu_terminal").mkdir(parents=True)
        result = self.run_nvr("service", "install")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.home / ".config/systemd/user/kilix-nvr.service").is_file())

    def test_retention_failure_is_not_reported_as_success(self):
        self.prepare_existing_directories()
        for action in ("enable", "start"):
            with self.subTest(action=action):
                self.env["FAIL_TIMER"] = action
                self.calls.unlink(missing_ok=True)
                result = self.run_nvr("service", "install")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("retention", result.stderr)
                calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
                self.assertNotIn(["--user", "restart", "kilix-nvr.service"], calls)

    def test_service_preserves_roots_for_recording_and_pruning(self):
        self.prepare_existing_directories()
        nvr = self.root / "custom nvr%root"
        nvr.mkdir()
        rtsp = self.root / "custom camera root"
        rtsp.mkdir()
        self.env.update(KILIX_NVR_HOME=str(nvr), KILIX_RTSP_HOME=str(rtsp))
        detector = '/usr/bin/example-detector --label "sample label" --cache %local'
        self.env["KILIX_OBJECT_DETECTOR"] = detector
        self.env["GPU_TERMINAL_DATA_HOME"] = str(self.root / "data root")
        self.env["KILIX_YOLOX_DIR"] = str(self.root / "detector runtime")
        self.env["KILIX_YOLOX_MODEL"] = "yolox_tiny"
        result = self.run_nvr("service", "install")
        self.assertEqual(result.returncode, 0, result.stderr)
        env_file = nvr / "state/service.env"
        self.assertTrue(env_file.is_file())
        self.assertEqual(env_file.stat().st_mode & 0o777, 0o600)
        values = dict(item.split("=", 1) for item in shlex.split(env_file.read_text()))
        self.assertEqual(values["KILIX_NVR_HOME"], str(nvr))
        self.assertEqual(values["KILIX_RTSP_HOME"], str(rtsp))
        self.assertEqual(values["KILIX_OBJECT_DETECTOR"], detector)
        for key in ("GPU_TERMINAL_DATA_HOME", "KILIX_YOLOX_DIR", "KILIX_YOLOX_MODEL"):
            self.assertEqual(values[key], self.env[key])
        for name in ("kilix-nvr.service", "kilix-nvr-prune.service"):
            unit = (self.home / ".config/systemd/user" / name).read_text()
            self.assertIn('EnvironmentFile=' + str(env_file).replace('%', '%%') + '\n', unit)
        # Use the delivered environment to access the same policy store.
        self.env.update(values)
        self.assertEqual(self.run_nvr("add", "sample-hall").returncode, 0)
        self.assertIn("sample-hall", self.run_nvr("cameras").stdout)

    def test_invalid_environment_preserves_previous_private_snapshot(self):
        self.prepare_existing_directories()
        result = self.run_nvr("service", "install")
        self.assertEqual(result.returncode, 0, result.stderr)
        env_file = self.home / ".local/gpu_terminal/kilix-nvr/state/service.env"
        original = env_file.read_bytes()
        self.calls.unlink()
        self.env["KILIX_OBJECT_DETECTOR"] = "bad\ncommand"
        result = self.run_nvr("service", "install")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(env_file.read_bytes(), original)
        self.assertFalse(self.calls.exists())

    def test_executable_with_spaces_is_quoted(self):
        self.prepare_existing_directories()
        binary = self.root / "program files/nvr"
        binary.parent.mkdir()
        shutil.copy2(BINARY, binary)
        result = self.run_nvr("service", "install", binary=binary)
        self.assertEqual(result.returncode, 0, result.stderr)
        unit = (self.home / ".config/systemd/user/kilix-nvr.service").read_text()
        self.assertIn('ExecStart="' + str(binary) + '" run', unit)

    @unittest.skipUnless(shutil.which("systemd-analyze"), "systemd parser unavailable")
    def test_generated_units_pass_the_systemd_parser_without_warnings(self):
        self.prepare_existing_directories()
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        self.env["XDG_RUNTIME_DIR"] = str(runtime)
        binary = self.root / "program %$files/nvr"
        binary.parent.mkdir()
        shutil.copy2(BINARY, binary)
        self.env["KILIX_NVR_HOME"] = str(self.root / "data %files/nvr")
        config = self.home / ".local/gpu_terminal/kilix/config"
        config.mkdir(parents=True)
        (config / "kilix.env").write_text("KILIX_OBJECT_DETECTOR=/bin/false\n")
        result = self.run_nvr("service", "install", binary=binary)
        self.assertEqual(result.returncode, 0, result.stderr)
        directory = self.home / ".config/systemd/user"
        units = [str(directory / name) for name in
                 ("kilix-nvr.service", "kilix-nvr-prune.service", "kilix-nvr-prune.timer")]
        result = subprocess.run(["systemd-analyze", "--user", "verify", *units],
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main()
