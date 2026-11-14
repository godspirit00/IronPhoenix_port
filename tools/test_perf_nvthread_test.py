from pathlib import Path
from types import SimpleNamespace
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import perf_nvthread_test as nvtest


class NvThreadRestorationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.file_patch = patch.object(nvtest, "__file__", str(self.root / "tools/perf_nvthread_test.py"))
        self.file_patch.start()
        self.probe_patch = patch.object(nvtest.subprocess, "check_output", return_value="application_profile=0 named_profile=0\n")
        self.probe_patch.start()
        self.print_patch = patch("builtins.print")
        self.print_patch.start()

    def tearDown(self):
        self.print_patch.stop()
        self.probe_patch.stop()
        self.file_patch.stop()
        self.directory.cleanup()

    def assert_restored(self):
        self.assertIn("Restored original absence: True", (self.root / "scratch/perf/nv-thread-test-state.txt").read_text())

    def test_existing_profile_is_never_modified(self):
        with patch.object(nvtest.subprocess, "check_output", return_value="application_profile=1 named_profile=1"), patch.object(nvtest.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                nvtest.main()
            run.assert_not_called()

    def test_running_game_prevents_driver_changes(self):
        with patch.object(nvtest.subprocess, "check_output", return_value='"ironphoenix.exe","123"'), patch.object(nvtest.subprocess, "run") as run:
            with self.assertRaisesRegex(SystemExit, "Close the running game"):
                nvtest.main()
            run.assert_not_called()

    def test_setting_failure_still_cleans_up(self):
        with patch.object(nvtest.subprocess, "run", side_effect=[subprocess.CalledProcessError(1, "set"), SimpleNamespace(returncode=0)]) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                nvtest.main()
            self.assertEqual(run.call_args_list[-1].args[0][-1], "delete")
            self.assert_restored()

    def test_benchmark_failure_restores_original_state(self):
        with patch.object(nvtest.subprocess, "run", side_effect=[SimpleNamespace(returncode=0), OSError("launch failed"), SimpleNamespace(returncode=0)]):
            with self.assertRaises(OSError):
                nvtest.main()
            self.assert_restored()

    def test_cleanup_failure_is_not_reported_as_success(self):
        with patch.object(nvtest.subprocess, "run", side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(returncode=0), SimpleNamespace(returncode=1)]):
            with self.assertRaisesRegex(SystemExit, "restoration failed"):
                nvtest.main()


if __name__ == "__main__":
    unittest.main()
