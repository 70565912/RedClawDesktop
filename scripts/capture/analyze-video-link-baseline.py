"""Recompute local Host baseline statistics; no peer clock subtraction."""
import csv
import argparse
import json
import math
import re
import statistics
from datetime import datetime, timedelta, timezone
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("report_directory", type=Path)
parser.add_argument("--log-utc-offset-minutes", type=int, default=480,
                    help="UTC offset of local timestamps in Host logs (default: 480)")
args = parser.parse_args()
root = args.report_directory
log_timezone = timezone(timedelta(minutes=args.log_utc_offset_minutes))
load = lambda p: json.loads(p.read_text(encoding="utf-8-sig"))


def distribution(values):
    values = sorted(values)
    if not values:
        return {"n": 0, "mean": None, "p50": None, "p95": None, "p99": None, "max": None}
    quantile = lambda p: values[max(0, math.ceil(len(values) * p) - 1)]
    return {"n": len(values), "mean": statistics.mean(values), "p50": quantile(.50),
            "p95": quantile(.95), "p99": quantile(.99), "max": values[-1]}


def parse_fields(line):
    return {k: float(v) for k, v in re.findall(r"\b(\w+)=(-?\d+(?:\.\d+)?)(?=\s|$)", line)}


def summarize_window(path):
    with (path / "host-frames.csv").open(encoding="utf-8-sig", newline="") as f:
        header = f.readline().strip()
        rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]
    timing = parse_fields(header)
    seconds = (timing["ended_us"] - timing["started_us"]) / 1e6
    snapshots = [load(path / "start.json")] + [load(p) for p in sorted(path.glob("sample-*.json"))]
    first, last = snapshots[0], snapshots[-1]
    first_time = datetime.fromisoformat(first["observed_utc"])
    last_time = datetime.fromisoformat(last["observed_utc"])
    observed_seconds = (last_time - first_time).total_seconds()
    counter_names = ("captured", "encoded", "transmitted", "synthetic", "capture_failures",
                     "encode_failures", "transmit_failures")
    counters = {k: last["status"]["stream"][k] - first["status"]["stream"][k] for k in counter_names}
    processes = {}
    for role in ("app", "runtime"):
        pid = first["status"][role + "_pid"]
        series = [next(p for p in s["processes"] if p["id"] == pid) for s in snapshots]
        processes[role] = {
            "pid": pid,
            "cpu_percent_one_core": (series[-1]["cpu_seconds"] - series[0]["cpu_seconds"]) / observed_seconds * 100,
            "working_set_mib_min": min(p["working_set_bytes"] for p in series) / 2**20,
            "working_set_mib_max": max(p["working_set_bytes"] for p in series) / 2**20,
            "private_mib_min": min(p["private_bytes"] for p in series) / 2**20,
            "private_mib_max": max(p["private_bytes"] for p in series) / 2**20,
        }
    pairs = {
        "capture_call_ms": ("capture_begin_us", "capture_end_us"),
        "capture_publication_ms": ("capture_end_us", "capture_ready_us"),
        "latest_frame_wait_ms": ("capture_ready_us", "encode_begin_us"),
        "encode_ms": ("encode_begin_us", "encode_end_us"),
        "send_queue_wait_ms": ("enqueued_us", "pacer_begin_us"),
        "pacer_ms": ("pacer_begin_us", "finish_us"),
        "frame_age_first_send_ms": ("capture_ready_us", "first_send_us"),
        "frame_age_last_send_ms": ("capture_ready_us", "last_send_us"),
    }
    stages = {name: distribution([(r[b] - r[a]) / 1000 for r in rows if 0 < r[a] <= r[b]])
              for name, (a, b) in pairs.items()}
    waits = ("token_wait_us", "in_flight_wait_us", "buffered_wait_us", "channel_wait_us",
             "probe_wait_us", "send_call_us", "wait_overshoot_us")
    kinds = {}
    for name, keyframe in (("keyframe", 1), ("ordinary", 0)):
        selected = [r for r in rows if r["keyframe"] == keyframe]
        kinds[name] = {"n": len(selected), "wire_bytes": distribution([r["wire_bytes"] for r in selected]),
                       "waits_ms": {k.removesuffix("_us"): distribution([r[k] / 1000 for r in selected]) for k in waits}}
    summary_lines = (path / "host-stage-summaries.txt").read_text(encoding="utf-8-sig").splitlines()
    stage_rows, config_rows = [], []
    for line in summary_lines:
        match = re.match(r"\[([^]]+)\]", line)
        if not match:
            continue
        timestamp = datetime.fromisoformat(match[1]).replace(tzinfo=log_timezone)
        if not first_time <= timestamp <= last_time:
            continue
        if "Runtime host stream stage" in line:
            row = parse_fields(line)
            # v1 did not print captured_delta. In these failure-free windows,
            # successful captures equal attempts minus timeouts. Never infer
            # this denominator for a window containing capture failures.
            if "capture_frames" not in row and counters["capture_failures"] == 0:
                row["capture_frames"] = row["capture_attempts"] - row["capture_timeouts_delta"]
            stage_rows.append(row)
        if "Runtime host stream diagnostics" in line:
            config_rows.append(parse_fields(line))
    weighted = {}
    for metric, denominator in (("avg_capture_copy_ms", "capture_frames"),
                                ("avg_capture_wait_ms", "capture_attempts"),
                                ("avg_input_prep_ms", "encode_attempts"),
                                ("avg_encode_total_ms", "encode_attempts")):
        valid = [r for r in stage_rows if metric in r and r.get(denominator, 0) > 0]
        weight = sum(r[denominator] for r in valid)
        weighted[metric] = {"windows": len(valid), "denominator": denominator, "count": weight,
                            "weighted_mean": sum(r[metric] * r[denominator] for r in valid) / weight if weight else None}
    gui = load(path / "gui-export.json")["result"]["measurement"]
    return {"window": path.name, "host_trace_seconds": seconds, "status_seconds": observed_seconds,
            "frames": len(rows), "sent_trace_fps": sum(r["outcome"] == 1 for r in rows) / seconds,
            "trace_overflow": timing["overflow"], "gui_trace_overflow": gui["overflow"],
            "trace_unsent": sum(r["outcome"] != 1 for r in rows),
            "capture_generations": sorted({r["capture_generation"] for r in rows}),
            "geometries": sorted({(r["width"], r["height"]) for r in rows}),
            "keyframe_ratio": sum(r["keyframe"] for r in rows) / len(rows) if rows else None,
            "target_fps": sorted({r["target_fps"] for r in rows}),
            "configured_fps": sorted({r["encoder_configured_fps"] for r in config_rows}),
            "counters": counters, "processes": processes, "stage_ms": stages,
            "frame_kind": kinds, "weighted_stage_summaries": weighted,
            "scene": load(path / "scene.json") if (path / "scene.json").exists() else None,
            "host_arm": load(path / "host-arm.json"),
            "log_replay_delta": last["local_log"]["replay"]["lines"] - first["local_log"]["replay"]["lines"],
            "hidden_mirror_refresh_delta": last["local_log"]["mirror_refreshes"] - first["local_log"]["mirror_refreshes"]}


windows = [summarize_window(p) for p in sorted(root.iterdir()) if p.is_dir() and (p / "host-frames.csv").exists()]
aggregate = {}
for scene in ("Static", "Dynamic", "DynamicLog"):
    selected = [w for w in windows if w["window"].rsplit("-", 1)[0] == scene]
    metrics = {"sent_fps": [w["sent_trace_fps"] for w in selected],
               "gui_cpu_percent_one_core": [w["processes"]["app"]["cpu_percent_one_core"] for w in selected],
               "host_cpu_percent_one_core": [w["processes"]["runtime"]["cpu_percent_one_core"] for w in selected],
               "encode_p95_ms": [w["stage_ms"]["encode_ms"]["p95"] for w in selected],
               "frame_age_p95_ms": [w["stage_ms"]["frame_age_last_send_ms"]["p95"] for w in selected]}
    aggregate[scene] = {}
    for metric, values in metrics.items():
        valid = [v for v in values if v is not None]
        aggregate[scene][metric] = {"n": len(valid), "median": statistics.median(valid) if valid else None,
                                    "min": min(valid) if valid else None, "max": max(valid) if valid else None}
result = {"schema": "redclaw.video-link-baseline-analysis.v2", "scope": "local Host only",
          "log_utc_offset_minutes": args.log_utc_offset_minutes,
          "percentile_method": "nearest rank", "cpu_unit": "100 percent equals one logical core",
          "notes": ["Stage summary windows overlap status boundaries; weighted values are supporting diagnostics.",
                    "v1 capture copy weights use attempts minus timeouts only when capture failure delta is zero; v2 uses explicit successful capture count.",
                    "Static zero-frame windows have no per-frame latency quantiles.",
                    "No cross-machine timestamp subtraction or remote GUI dispatch inference."],
          "windows": windows, "three_run_summary": aggregate}
(root / "analysis.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
print(json.dumps(aggregate, ensure_ascii=False, indent=2))
