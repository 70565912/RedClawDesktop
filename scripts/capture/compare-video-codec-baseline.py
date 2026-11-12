"""Compare an H.264 and an HEVC Host baseline report produced by run-video-link-baseline.ps1.

Both reports must already contain analysis.json (analyze-video-link-baseline.py).
The comparison refuses mislabeled runs: every encoder start recorded in
host-video-codec.txt must carry the expected codec, and both plans must use the
same geometry/network contract and workload. Visual quality is not measured here.
"""
import argparse
import json
import re
import statistics
from pathlib import Path

SCENES = ("Static", "Dynamic", "DynamicLog")
load = lambda p: json.loads(p.read_text(encoding="utf-8-sig"))


def negotiated_codecs(report):
    path = report / "host-video-codec.txt"
    if not path.exists():
        raise ValueError(f"{report.name}: host-video-codec.txt is missing; rerun the baseline with this script version")
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    codecs = [m[1] for m in (re.search(r"Runtime video codec negotiated .*?\bcodec=(\w+)", l) for l in lines) if m]
    renegotiations = sum("Runtime video codec renegotiation" in l for l in lines)
    return codecs, renegotiations


def window_kbps(window):
    kinds = window["frame_kind"]
    total = sum(kinds[k]["wire_bytes"]["n"] * (kinds[k]["wire_bytes"]["mean"] or 0) for k in ("keyframe", "ordinary"))
    seconds = window["host_trace_seconds"]
    return total * 8 / 1000 / seconds if seconds > 0 else None


def median_or_none(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def scene_metrics(analysis):
    result = {}
    for scene in SCENES:
        windows = [w for w in analysis["windows"] if w["window"].rsplit("-", 1)[0] == scene]
        result[scene] = {
            "windows": len(windows),
            "wire_kbps": median_or_none([window_kbps(w) for w in windows]),
            "keyframe_wire_bytes_mean": median_or_none([w["frame_kind"]["keyframe"]["wire_bytes"]["mean"] for w in windows]),
            "ordinary_wire_bytes_mean": median_or_none([w["frame_kind"]["ordinary"]["wire_bytes"]["mean"] for w in windows]),
            "sent_fps": median_or_none([w["sent_trace_fps"] for w in windows]),
            "encode_p95_ms": median_or_none([w["stage_ms"]["encode_ms"]["p95"] for w in windows]),
            "frame_age_p95_ms": median_or_none([w["stage_ms"]["frame_age_last_send_ms"]["p95"] for w in windows]),
            "host_cpu_percent_one_core": median_or_none(
                [w["processes"]["runtime"]["cpu_percent_one_core"] for w in windows]),
        }
    return result


def compare(h264_report, hevc_report):
    problems = []
    labels = {}
    for name, report, expected in (("h264", h264_report, "h264"), ("hevc", hevc_report, "hevc")):
        codecs, renegotiations = negotiated_codecs(report)
        labels[name] = {"negotiated": codecs, "renegotiations": renegotiations}
        if not codecs:
            problems.append(f"{name}: no 'Runtime video codec negotiated' line was recorded")
        elif any(c != expected for c in codecs):
            problems.append(f"{name}: expected only codec={expected}, saw {codecs}")
        if renegotiations:
            problems.append(f"{name}: {renegotiations} mid-run codec renegotiation(s) mix codecs inside the report")
    h264_plan, hevc_plan = [load(r / "plan.json") for r in (h264_report, hevc_report)]
    if h264_plan["contract"] != hevc_plan["contract"]:
        problems.append("plan contracts differ (geometry, log visibility, network or diagnostic interval)")
    for field in ("repetitions", "scenarios", "random_seed"):
        if field not in h264_plan or field not in hevc_plan:
            problems.append(f"plan workload is missing {field}")
        elif h264_plan[field] != hevc_plan[field]:
            problems.append(f"plan workloads differ in {field}")
    metrics = {name: scene_metrics(load(r / "analysis.json"))
               for name, r in (("h264", h264_report), ("hevc", hevc_report))}
    ratios = {}
    for scene in SCENES:
        ratios[scene] = {}
        for key, base in metrics["h264"][scene].items():
            other = metrics["hevc"][scene][key]
            if key != "windows" and base and other is not None:
                ratios[scene][key] = other / base
    return {
        "schema": "redclaw.video-codec-baseline-comparison.v1",
        "ok": not problems,
        "problems": problems,
        "codec_labels": labels,
        "metrics": metrics,
        "hevc_over_h264": ratios,
        "notes": [
            "HEVC targets are configured at 70% of the H.264 budget, so a wire_kbps ratio near 0.70 is expected by construction.",
            "The decision evidence is whether picture quality stays comparable at that bitrate; judge it visually or with a separate PSNR/VMAF capture.",
            "Wire bytes include fragment headers. Host-only metrics; no cross-machine timestamp subtraction.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("h264_report", type=Path)
    parser.add_argument("hevc_report", type=Path)
    parser.add_argument("--output", type=Path, help="Defaults to <hevc_report>/codec-comparison.json")
    args = parser.parse_args()
    result = compare(args.h264_report, args.hevc_report)
    output = args.output or args.hevc_report / "codec-comparison.json"
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps({"ok": result["ok"], "problems": result["problems"],
                      "hevc_over_h264": result["hevc_over_h264"]}, indent=2, ensure_ascii=False))
    raise SystemExit(0 if result["ok"] else 1)


if __name__ == "__main__":
    main()
