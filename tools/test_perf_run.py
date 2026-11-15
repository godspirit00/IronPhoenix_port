"""Reject invalid diagnostic runs before starting the game or writing artifacts."""
import subprocess
import sys
import unittest
from pathlib import Path


class PerfRunArgumentsTests(unittest.TestCase):
    def reject(self, *args):
        result = subprocess.run(
            [sys.executable, str(Path(__file__).with_name('perf_run.py')),
             '--tag', 'argument-validation-only', *args],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn('Running PID', result.stdout)
        return result.stderr

    def test_window_must_end_before_timeout(self):
        self.assertIn('invalid measurement interval', self.reject('--end', '200'))

    def test_sampling_profile_must_finish_before_timeout(self):
        self.assertIn('finish before', self.reject('--profile', '170,30'))

    def test_sampling_profile_requires_integer_pair(self):
        self.assertIn('START,SECONDS', self.reject('--profile', '120:30'))
