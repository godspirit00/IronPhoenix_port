import tempfile
import unittest
from pathlib import Path

from perf_report import summarize


class PerfReportTests(unittest.TestCase):
    def test_hle_path_and_cumulative_coverage_survive_window_filter(self):
        result = self.report(
            "[ 1 100] HLE UP bridge: host draw packets (state/resources remain on existing frontend)\n"
            "[ 1 1100] perf: 60 fps\n"
            "[ 1 2100] HLE UP: calls 100 submitted 90 consumed 80 fallback 10 verified 0\n",
            start=0.5, end=1)
        self.assertEqual(result["hle_up_mode"], "host draw packets")
        self.assertEqual(result["hle_up_cumulative"], {
            "calls":100, "submitted":90, "consumed":80, "fallback":10, "verified":0})

    def test_vulkan_backend_is_distinct_from_gl_presenter(self):
        result = self.report(
            "[ 1 100] OpenGL: NVIDIA presenter\n"
            "[ 1 200] Renderer active: Vulkan\n"
            "[ 1 1100] perf: 60 fps\n")
        self.assertEqual(result["backend"], "Vulkan")
        self.assertEqual(result["renderer"], "NVIDIA presenter")

    def report(self, text, start=0, end=1):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "runtime.log"
            log.write_text(text, encoding="utf-8")
            return summarize(log, start, end)

    def test_rollover_and_nested_phase_totals(self):
        result = self.report(
            "[ 1 999500] OpenGL: test renderer\n"
            "[ 1 0] gpu time: draw_begin 2ms/100 textures 1ms/100\n"
            "[ 1 0] perf: 60 fps\n"
            "[ 1 500] perf: 80 fps\n")
        self.assertEqual(result["log_duration_seconds"], 1)
        self.assertEqual(result["fps_samples"]["mean"], 70)
        self.assertEqual(result["cpu_phase_us_per_call"], {"draw_begin": 20, "textures": 10})
        self.assertEqual(result["cpu_phase_totals_ms"], {"draw_begin": 2, "textures": 1})
        self.assertFalse(result["warnings"])

    def test_wrong_route_is_rejected(self):
        result = self.report(
            "[ 1 100] OpenGL: test\n"
            "[ 1 200] perf: 500 fps\n"
            "[ 1 1100] HalReturnToFirmware(2)\n", start=2, end=3)
        self.assertIsNone(result["fps_samples"])
        self.assertEqual(len(result["warnings"]), 3)

    def test_measurement_window_excludes_menu(self):
        result = self.report(
            "[ 1 100] starting\n"
            "[ 1 200] perf: 500 fps\n"
            "[ 1 2100] gpu time: draw_begin 4ms/200\n"
            "[ 1 2100] perf: 70 fps\n"
            "[ 1 3100] perf: 60 fps\n", start=2, end=3)
        self.assertEqual(result["fps_sample_count"], 2)
        self.assertEqual(result["fps_samples"]["mean"], 65)

    def test_validation_failure_outside_window_is_reported(self):
        result = self.report(
            "[ 1 100] surface snapshot check 1: 10x10 format 8058 MISMATCH\n"
            "[ 1 1100] gpu time: draw_begin 4ms/200\n"
            "[ 1 1100] perf: 70 fps\n", start=0.5, end=1)
        self.assertEqual(result["snapshot_validation_checks"], 1)
        self.assertTrue(any("validation failure" in warning for warning in result["warnings"]))

    def test_gpu_samples_are_weighted_and_windowed(self):
        result = self.report(
            "[ 1 100] gpu draw sample: 100.000 us/draw (10 draws, 1/64 sampling)\n"
            "[ 1 600] gpu draw sample: 2.000 us/draw (10 draws, 1/64 sampling)\n"
            "[ 1 1100] gpu draw sample: 4.000 us/draw (30 draws, 1/64 sampling)\n",
            start=0.5, end=1)
        self.assertEqual(result["gpu_draw_sample"], {"samples": 40, "mean_us": 3.5})

    def test_fps_only_run_without_phase_instrumentation(self):
        result = self.report("[ 1 100] starting\n[ 1 1100] perf: 60 fps\n")
        self.assertFalse(result["warnings"])
        self.assertIsNone(result["draw_calls"])
        self.assertEqual(result["cpu_phase_us_per_draw"], {})


if __name__ == "__main__":
    unittest.main()
