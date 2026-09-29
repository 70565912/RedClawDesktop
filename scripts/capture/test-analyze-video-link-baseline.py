"""Offline arithmetic/regression checks for the Host trace report; no desktop load."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "baseline_analysis", Path(__file__).with_name("analyze-video-link-baseline.py"))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def sent_frame():
    return dict(frame_id=1, keyframe=1, outcome=1, wire_bytes=1000,
                capture_ready_us=1000, encode_begin_us=2000, encode_end_us=3000,
                enqueued_us=4000, pacer_begin_us=5000, first_send_us=7000,
                last_send_us=10000, finish_us=11000, sent_fragments=1, fragment_count=1,
                token_wait_us=2000, in_flight_wait_us=1000, buffered_wait_us=0,
                channel_wait_us=0, wait_elapsed_us=3000, wait_overshoot_us=500,
                send_call_us=1000, transport_state_us=200, callback_us=100)


class SendAttributionTests(unittest.TestCase):
    def test_accounting_uses_exclusive_reasons_and_does_not_add_overshoot(self):
        data = analysis.send_attribution([sent_frame()])["kinds"]["keyframe"]
        self.assertEqual(data["duration_ms"]["pacer"]["mean"], 6)
        self.assertEqual(data["duration_ms"]["queue"]["mean"], 1)
        self.assertEqual(data["duration_ms"]["age_last_send"]["mean"], 9)
        self.assertAlmostEqual(data["total_ms"]["unattributed"], 1.7)
        self.assertAlmostEqual(sum(data["pacer_share_percent"].values()), 100)
        self.assertAlmostEqual(data["overshoot_share_of_wait_percent"], 100 / 6)
        self.assertEqual(data["frames_with_wait_overshoot"], 1)

    def test_failed_frames_are_counted_without_polluting_success_latency(self):
        report = analysis.send_attribution([sent_frame(), dict(outcome=2, keyframe=0)])
        self.assertEqual(report["outcomes"], {"1": 1, "2": 1, "3": 0})
        self.assertEqual(report["kinds"]["keyframe"]["sent_frame_ratio"], 1)
        self.assertEqual(report["kinds"]["ordinary"]["frames"], 0)
        self.assertIsNone(report["kinds"]["ordinary"]["duration_ms"]["pacer"]["p95"])
        self.assertIsNone(analysis.send_attribution([])["kinds"]["keyframe"]["sent_frame_ratio"])

    def test_rejects_corrupt_accounting_and_incomplete_sent_frames(self):
        for field, value in (("wait_elapsed_us", 2999), ("finish_us", 8000),
                             ("sent_fragments", 0), ("send_call_us", 10000),
                             ("wait_overshoot_us", 3001)):
            with self.subTest(field=field):
                frame = sent_frame()
                frame[field] = value
                with self.assertRaises(ValueError):
                    analysis.send_attribution([frame])

    def test_three_run_summary_preserves_run_variation(self):
        windows = []
        for index, size in enumerate((100, 300, 2000), 1):
            frame = sent_frame()
            frame["wire_bytes"] = size
            windows.append(dict(window=f"Dynamic-{index}", send_attribution=analysis.send_attribution([frame])))
        summary = analysis.attribution_summary(windows)["Dynamic"]["keyframe"]
        self.assertEqual(summary["wire_bytes_mean"], dict(n=3, median=300, min=100, max=2000))
        self.assertEqual(analysis.distribution([1, 2, 100])["p95"], 100)


if __name__ == "__main__":
    unittest.main()
