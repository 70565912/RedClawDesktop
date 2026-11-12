"""Offline checks for the H.264/HEVC baseline comparison; no desktop load."""
import importlib.util
import json
import tempfile
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "codec_comparison", Path(__file__).with_name("compare-video-codec-baseline.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def window(name, keyframe_bytes, ordinary_bytes):
    dist = lambda n, mean: {"n": n, "mean": mean}
    return {"window": name, "host_trace_seconds": 10.0, "sent_trace_fps": 30.0,
            "frame_kind": {"keyframe": {"wire_bytes": dist(1, keyframe_bytes)},
                           "ordinary": {"wire_bytes": dist(299, ordinary_bytes)}},
            "stage_ms": {"encode_ms": {"p95": 4.0}, "frame_age_last_send_ms": {"p95": 20.0}},
            "processes": {"runtime": {"cpu_percent_one_core": 50.0}}}


def write_report(root, codec_lines, keyframe_bytes, ordinary_bytes, contract=None, **workload):
    root.mkdir()
    plan = {"contract": contract or {"encode_width": 1920},
            "repetitions": 1, "scenarios": ["Dynamic"], "random_seed": 2700}
    plan.update(workload)
    (root / "plan.json").write_text(json.dumps(plan))
    (root / "analysis.json").write_text(json.dumps(
        {"windows": [window("Dynamic-1", keyframe_bytes, ordinary_bytes)]}))
    (root / "host-video-codec.txt").write_text("\n".join(codec_lines))
    return root


NEGOTIATED = "[t] Runtime video codec negotiated role=host codec={} reason=x preference=auto"


class CompareTests(unittest.TestCase):
    def test_reports_ratio_for_correctly_labeled_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            h264 = write_report(Path(tmp) / "a", [NEGOTIATED.format("h264")], 100000, 20000)
            hevc = write_report(Path(tmp) / "b", [NEGOTIATED.format("hevc")], 70000, 14000)
            result = comparison.compare(h264, hevc)
            self.assertTrue(result["ok"], result["problems"])
            self.assertAlmostEqual(result["hevc_over_h264"]["Dynamic"]["wire_kbps"], 0.7)
            self.assertAlmostEqual(result["hevc_over_h264"]["Dynamic"]["keyframe_wire_bytes_mean"], 0.7)
            self.assertNotIn("Static", [k for k, v in result["hevc_over_h264"].items() if v])

    def test_refuses_fallback_or_renegotiated_hevc_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            h264 = write_report(Path(tmp) / "a", [NEGOTIATED.format("h264")], 1, 1)
            hevc = write_report(Path(tmp) / "b", [
                NEGOTIATED.format("hevc"),
                "[t] Runtime video codec renegotiation role=host from=hevc reason=peer_cannot_decode_hevc",
                NEGOTIATED.format("h264")], 1, 1)
            result = comparison.compare(h264, hevc)
            self.assertFalse(result["ok"])
            self.assertEqual(len(result["problems"]), 2)

    def test_refuses_different_contracts_and_missing_codec_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            h264 = write_report(Path(tmp) / "a", [], 1, 1)
            hevc = write_report(Path(tmp) / "b", [NEGOTIATED.format("hevc")], 1, 1,
                                contract={"encode_width": 1280})
            problems = comparison.compare(h264, hevc)["problems"]
            self.assertTrue(any("no 'Runtime video codec negotiated'" in p for p in problems))
            self.assertTrue(any("contracts differ" in p for p in problems))

    def test_refuses_unmatched_workload_fields(self):
        for field, value in (("repetitions", 2), ("scenarios", ["Static"]), ("random_seed", 2701)):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as tmp:
                h264 = write_report(Path(tmp) / "a", [NEGOTIATED.format("h264")], 1, 1)
                hevc = write_report(Path(tmp) / "b", [NEGOTIATED.format("hevc")], 1, 1,
                                    **{field: value})
                result = comparison.compare(h264, hevc)
                self.assertFalse(result["ok"])
                self.assertEqual(result["problems"], [f"plan workloads differ in {field}"])


if __name__ == "__main__":
    unittest.main()
