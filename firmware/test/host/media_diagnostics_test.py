"""Metadata privacy and bounded aggregation tests; no hardware or Provider access."""

import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "media_diagnostics", Path(__file__).resolve().parents[2] / "tools/media_diagnostics.py")
diagnostics = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(diagnostics)


class MediaDiagnosticsTest(unittest.TestCase):
    def test_unknown_events_and_fields_are_not_retained(self):
        self.assertIsNone(diagnostics.parse_line("event=secret token=private"))
        result = diagnostics.parse_line("event=playback_queue request=012345abcdef depth=4 token=private text=secret")
        self.assertEqual(result, {"event": "playback_queue", "request": "012345abcdef", "depth": 4})

    def test_values_cannot_smuggle_raw_ids_or_text(self):
        result = diagnostics.parse_line("event=playback_queue request=r_private result=secret depth=secret task=private")
        self.assertEqual(result, {"event": "playback_queue"})

    def test_terminal_color_uptime_and_numeric_results(self):
        result = diagnostics.parse_line("\x1b[0;32mI (321) terminal: event=audio_playback type=1 result=0 frames=42\x1b[0m\n")
        self.assertEqual(result["uptime_ms"], 321)
        self.assertEqual(result["result"], 0)
        self.assertEqual(result["frames"], 42)

    def test_host_result_enum_and_capability(self):
        result = diagnostics.parse_line("event=speech_request_finished capability=tts result=timeout elapsed_ms=30")
        self.assertEqual(result["result"], "timeout")
        self.assertEqual(result["capability"], "tts")

    def test_oversized_number_and_line_rejected(self):
        result = diagnostics.parse_line("event=playback_queue depth=" + "9" * 21)
        self.assertNotIn("depth", result)
        self.assertIsNone(diagnostics.parse_line("event=playback_queue " + "x" * 4096))

    def test_summary_reports_observation_not_acceptance(self):
        report = diagnostics.Summary()
        for uptime, free in ((10, 100), (20, 80), (1, 110)):
            report.observe({"event": "runtime_resource_snapshot", "uptime_ms": uptime, "internal_free_bytes": free})
        result = report.result()
        self.assertEqual(result["observed_uptime_resets"], 1)
        self.assertEqual(result["metrics"]["runtime_resource_snapshot.all.internal_free_bytes"]["min"], 80)
        self.assertEqual(result["acceptance"], "manual_verification_required")

    def test_many_request_ids_do_not_expand_summary(self):
        report = diagnostics.Summary()
        for index in range(10000):
            report.observe({"event": "playback_queue", "request": f"{index:012x}", "depth": 1})
        self.assertEqual(len(report.metrics), 1)
        self.assertEqual(len(report.counts), 1)

    def test_collection_filters_overlong_remainder_and_creates_private_files(self):
        text = "x" * 4100 + " event=playback_queue depth=99\n"
        text += "event=playback_queue depth=2 token=private\n"
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            diagnostics.collect(io.StringIO(text), destination)
            events = (destination / "events.jsonl").read_text()
            self.assertNotIn("private", events)
            self.assertNotIn("99", events)
            self.assertEqual(json.loads(events)["depth"], 2)
            self.assertEqual((destination / "events.jsonl").stat().st_mode & 0o777, 0o600)
            self.assertEqual((destination / "summary.json").stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                diagnostics.collect(io.StringIO(""), destination)


if __name__ == "__main__":
    unittest.main()
