import tempfile
import unittest
from pathlib import Path

from perf_report import summarize


class PerfReportTests(unittest.TestCase):
    def test_scene_transition_timestamps_survive_clock_wrap(self):
        result = self.report(
            '[ 1 999500] Scene asset opened: se06.scn\n'
            '[ 1 500] Scene asset opened: sw05.scn\n'
            '[ 1 1500] perf: 60 fps\n')
        self.assertEqual(result['scene_events'], [dict(seconds=0.0, asset='se06.scn'),
                                                 dict(seconds=1.0, asset='sw05.scn')])

    def test_delayed_pre_wrap_line_does_not_jump_map_windows(self):
        result = self.report(
            '[ 1 999500] Scene asset opened: se06.scn\n'
            '[ 1 500] Scene asset opened: sw05.scn\n'
            '[ 2 999999] perf: 60 fps\n'
            '[ 1 1500] Scene asset opened: se08.scn\n')
        self.assertEqual(result['scene_events'][-1], dict(seconds=2.0, asset='se08.scn'))

    def test_scene_and_cache_budget_telemetry(self):
        result = self.report(
            "[ 1 100] Scene asset opened: se06.scn\n"
            "[ 1 1100] perf: 60 fps\n"
            "[ 1 2100] HLE buffers: binds 900 hits 800 uploads 100 bytes 4294967296 checks 900 readbacks 32 evictions 2 fallback 3 resident 65536 span_updates 50 span_bytes 12345 admission_fallback 5 compare_fallback 6 update_fallback 7 peak_admit 4194304 peak_compare 16777216 peak_update 1048576\n")
        self.assertEqual(result['scene_assets'], ['se06.scn'])
        self.assertEqual(result['hle_buffers']['admission_fallback'], 5)
        self.assertEqual(result['hle_buffers']['peak_compare'], 16777216)

    def test_owned_buffer_verification_preserves_large_byte_counts(self):
        result = self.report(
            "[ 1 100] HLE buffers mode: verify\n"
            "[ 1 1100] perf: 60 fps\n"
            "[ 1 2100] HLE buffers: binds 900 hits 800 uploads 100 bytes 4294967296 checks 900 readbacks 32 evictions 2 fallback 3 resident 65536 span_updates 50 span_bytes 12345\n")
        self.assertEqual(result["hle_buffers_mode"], "verify")
        self.assertEqual(result["hle_buffers"]["bytes"], 4294967296)
        self.assertEqual(result["hle_buffers"]["checks"], 900)
        self.assertEqual(result["hle_buffers"]["readbacks"], 32)
        self.assertEqual(result["hle_buffers"]["span_updates"], 50)

    def test_resource_modes_require_distinct_checked_and_applied_counts(self):
        result = self.report(
            "[ 1 100] HLE resources mode: 1\n"
            "[ 1 1100] perf: 60 fps\n"
            "[ 1 2100] HLE resources audit: register 5 free 6 streams 7 textures 8 targets 9 locks 10\n"
            "[ 1 2100] HLE vertex state: prepares 100 native 60 delegated 40 snapshots 80 checked 30 applied 50\n"
            "[ 1 2100] HLE resource identities: live 5 created 20 retired 2 evicted 0\n")
        self.assertEqual(result["hle_resources_mode"], "1")
        self.assertEqual(result["hle_vertex_state"]["applied"], 50)
        self.assertEqual(result["hle_vertex_state"]["checked"], 30)
        self.assertEqual(result["hle_resources_audit"]["locks"], 10)
        self.assertEqual(result["hle_resource_identities"]["evicted"], 0)
    def test_indexed_audit_distinguishes_api_calls_from_buffer_execution(self):
        result = self.report(
            "[ 1 100] HLE indexed mode: observe\n"
            "[ 1 1100] perf: 60 fps\n"
            "[ 1 2100] HLE indexed audit: indexed_calls 12 indices 50000 run_calls 90\n")
        self.assertEqual(result["hle_indexed_mode"], "observe")
        self.assertEqual(result["hle_indexed_audit"], {"indexed_calls":12,"indices":50000,"run_calls":90})

    def test_indexed_packet_counters_preserve_large_totals(self):
        result = self.report(
            "[ 1 100] HLE indexed mode: verify\n"
            "[ 1 1100] perf: 60 fps\n"
            "[ 1 2100] HLE indexed packets: submitted 100 consumed 0 fallback 4 verified 99 consumed_indices 0 verified_indices 4294967296\n")
        self.assertEqual(result["hle_indexed_packets"]["verified"], 99)
        self.assertEqual(result["hle_indexed_packets"]["verified_indices"], 4294967296)

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
