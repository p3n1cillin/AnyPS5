import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location("benchmark_report", Path(__file__).parents[1] / "benchmark_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class BenchmarkReportTests(unittest.TestCase):
    def test_intervals_use_elapsed_time_and_keep_outputs_separate(self):
        text = "\n".join([
            "[FrameTiming] frame=1 output=1 endpoint=flip_complete",
            "[FrameTiming] frame=2 output=1 endpoint=flip_complete flip_interval_ms=10 render_fence_wait=999",
            "[FrameTiming] frame=3 output=1 endpoint=flip_complete flip_interval_ms=30",
            "[FrameTiming] frame=4 output=2 endpoint=flip_complete flip_interval_ms=100",
        ])
        metrics = report.frame_metrics(text)
        self.assertEqual(metrics["outputs"]["1"]["average_fps"], 50)
        self.assertEqual(metrics["outputs"]["1"]["p50_ms"], 20)
        self.assertEqual(metrics["outputs"]["2"]["average_fps"], 10)
        self.assertFalse(metrics["display_confirmed"])

    def test_missing_timings_does_not_infer_fps(self):
        self.assertEqual(report.frame_metrics("[present] 500 presents over 10 s")["outputs"], {})

    def test_rejects_duplicate_invalid_and_nonfinite_intervals(self):
        line = "[FrameTiming] frame=1 output=1 endpoint=flip_complete flip_interval_ms="
        for value in ("0", "-1", "nan", "inf"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                report.frame_metrics(line + value)
        with self.assertRaises(ValueError):
            report.frame_metrics(line + "10\n" + line + "10")

    def test_comparison_checks_settings_and_profiling(self):
        metadata = dict(game="title", version="1", driver="2", scenario="intro", settings={"resolution": "960x540"}, profiling={"gpu": True})
        baseline = {"metadata": metadata, "frames": {"endpoint": "flip_complete", "outputs": {"1": {"average_fps": 20}}}}
        current = {"metadata": dict(metadata), "frames": {"endpoint": "flip_complete", "outputs": {"1": {"average_fps": 30}}}}
        self.assertEqual(report.compare_reports(current, baseline)["average_fps_change_percent"]["1"], 50)
        current["metadata"]["profiling"] = {"gpu": False}
        comparison = report.compare_reports(current, baseline)
        self.assertFalse(comparison["compatible"])
        self.assertEqual(comparison["average_fps_change_percent"], {})

    def test_comparison_requires_frame_intervals(self):
        empty = {"metadata": {}, "frames": report.frame_metrics("")}
        comparison = report.compare_reports(empty, empty)
        self.assertFalse(comparison["compatible"])
        self.assertIn("No matching frame interval measurements", comparison["reasons"])

    def test_cache_totals_and_device_loss_are_reported(self):
        metrics = report.log_metrics("[shader-disk-cache] (10 s): totals: 7 hits, 9 misses, 3 writes\n[gpu-progress] range 2 program pending\nAGC graphics: Vulkan result -4")
        self.assertEqual(metrics["shader_cache_last_reported_totals"]["misses"], 9)
        self.assertTrue(metrics["device_loss_reported"])
        self.assertEqual(len(metrics["gpu_progress"]), 1)

    def test_telemetry_rejects_mixed_processes_and_bad_time_order(self):
        fields = ("cpuPercent", "workingSetMiB", "privateMiB", "systemAvailableMiB", "gpuPercent", "gpuMemoryPercent", "gpuMiB", "gpuWatts", "gpuCelsius")
        first = dict.fromkeys(fields, "10")
        first.update(utc="2026-10-09T00:00:00Z", pid="42")
        second = dict(first, utc="2026-10-09T00:00:05Z", gpuMiB="20")
        metrics = report.telemetry_metrics([first, second])
        self.assertEqual(metrics["duration_s"], 5)
        self.assertEqual(metrics["gpuMiB"]["max"], 20)
        with self.assertRaises(ValueError):
            report.telemetry_metrics([second, first])
        with self.assertRaises(ValueError):
            report.telemetry_metrics([first, dict(second, pid="99")])
        with self.assertRaises(ValueError):
            report.telemetry_metrics([first, dict(second, gpuPercent="nan")])


if __name__ == "__main__":
    unittest.main()
