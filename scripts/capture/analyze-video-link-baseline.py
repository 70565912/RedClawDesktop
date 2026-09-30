"""Recompute local Host baseline statistics; no peer clock subtraction."""
import csv
import argparse
import hashlib
import json
import math
import re
import statistics
from datetime import datetime, timedelta, timezone
from pathlib import Path

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


def send_attribution(rows):
    """Exclusive wait reasons and pacer accounting, successful frames only."""
    waits = ("token_wait", "in_flight_wait", "buffered_wait", "channel_wait")
    components = (*waits, "send_call", "transport_state", "callback")
    sent = [r for r in rows if r["outcome"] == 1]
    result = {"denominator": "successfully sent traced frames of each kind",
              "outcomes": {str(i): sum(r["outcome"] == i for r in rows) for i in (1, 2, 3)},
              "kinds": {}}
    for name, keyframe in (("keyframe", 1), ("ordinary", 0)):
        selected = [r for r in sent if r["keyframe"] == keyframe]
        durations = {k: [] for k in (*components, "queue", "pacer", "age_last_send",
                                      "wait_elapsed", "wait_overshoot", "unattributed")}
        for r in selected:
            times = [r[k] for k in ("capture_ready_us", "encode_begin_us", "encode_end_us",
                                   "enqueued_us", "pacer_begin_us", "first_send_us",
                                   "last_send_us", "finish_us")]
            if times[0] <= 0 or times != sorted(times):
                raise ValueError(f"Invalid sent-frame timing: {r['frame_id']}")
            if sum(r[k + "_us"] for k in waits) != r["wait_elapsed_us"]:
                raise ValueError(f"Wait attribution does not balance: {r['frame_id']}")
            if r["sent_fragments"] != r["fragment_count"]:
                raise ValueError(f"Incomplete sent-frame fragments: {r['frame_id']}")
            pacer = r["finish_us"] - r["pacer_begin_us"]
            residual = pacer - sum(r[k + "_us"] for k in components)
            if residual < 0 or r["wait_overshoot_us"] > r["wait_elapsed_us"]:
                raise ValueError(f"Overlapping/invalid pacer accounting: {r['frame_id']}")
            values = {k: r[k + "_us"] for k in components}
            values.update(queue=r["pacer_begin_us"] - r["enqueued_us"], pacer=pacer,
                          age_last_send=r["last_send_us"] - r["capture_ready_us"],
                          wait_elapsed=r["wait_elapsed_us"], wait_overshoot=r["wait_overshoot_us"],
                          unattributed=residual)
            for k, value in values.items():
                durations[k].append(value / 1000)
        totals = {k: sum(v) for k, v in durations.items()}
        result["kinds"][name] = {
            "frames": len(selected), "sent_frame_ratio": len(selected) / len(sent) if sent else None,
            "wire_bytes": distribution([r["wire_bytes"] for r in selected]),
            "duration_ms": {k: distribution(v) for k, v in durations.items()},
            "total_ms": totals,
            "pacer_share_percent": {k: 100 * totals[k] / totals["pacer"] if totals["pacer"] else None
                                    for k in (*components, "unattributed")},
            # Overshoot is already in a reason's wait time, never an extra component.
            "overshoot_share_of_wait_percent": (100 * totals["wait_overshoot"] / totals["wait_elapsed"]
                                                if totals["wait_elapsed"] else None),
            "frames_with_wait_overshoot": sum(v > 0 for v in durations["wait_overshoot"]),
        }
    return result


def summarize_window(path, log_timezone):
    with (path / "host-frames.csv").open(encoding="utf-8-sig", newline="") as f:
        header = f.readline().strip()
        rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]
    if not header.startswith(("# redclaw.host-frame-trace.v1 ", "# redclaw.host-frame-trace.v2 ")):
        raise ValueError(f"Unsupported trace schema: {path.name}")
    timing = parse_fields(header)
    seconds = (timing["ended_us"] - timing["started_us"]) / 1e6
    if seconds <= 0 or any(v < 0 for r in rows for v in r.values()):
        raise ValueError(f"Invalid trace duration or negative field: {path.name}")
    ids = [(r["capture_generation"], r["frame_id"]) for r in rows]
    if len(ids) != len(set(ids)) or any(r["outcome"] not in (1, 2, 3) or r["keyframe"] not in (0, 1)
                                      or not timing["started_us"] <= r["encode_begin_us"] < timing["ended_us"]
                                      for r in rows):
        raise ValueError(f"Invalid trace identity, outcome or admission window: {path.name}")
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
            "pacing_kbps": sorted({r["pacing_kbps"] for r in rows}),
            "send_attribution": send_attribution(rows),
            "trace_sha256": hashlib.sha256((path / "host-frames.csv").read_bytes()).hexdigest(),
            "scene": load(path / "scene.json") if (path / "scene.json").exists() else None,
            "host_arm": load(path / "host-arm.json"),
            "log_replay_delta": last["local_log"]["replay"]["lines"] - first["local_log"]["replay"]["lines"],
            "hidden_mirror_refresh_delta": last["local_log"]["mirror_refreshes"] - first["local_log"]["mirror_refreshes"]}


def attribution_summary(windows):
    result = {}
    for scene in ("Static", "Dynamic", "DynamicLog"):
        selected = [w for w in windows if w["window"].rsplit("-", 1)[0] == scene]
        result[scene] = {}
        for kind in ("keyframe", "ordinary"):
            metrics = {}
            for window in selected:
                data = window["send_attribution"]["kinds"][kind]
                values = {"frames": data["frames"], "sent_frame_ratio": data["sent_frame_ratio"],
                          "wire_bytes_mean": data["wire_bytes"]["mean"],
                          "overshoot_share_of_wait_percent": data["overshoot_share_of_wait_percent"]}
                for name, dist in data["duration_ms"].items():
                    for stat in ("mean", "p95", "p99"):
                        values[f"{name}_{stat}_ms"] = dist[stat]
                for name, share in data["pacer_share_percent"].items():
                    values[f"{name}_pacer_share_percent"] = share
                for name, value in values.items():
                    if value is not None:
                        metrics.setdefault(name, []).append(value)
            result[scene][kind] = {name: {"n": len(values), "median": statistics.median(values),
                                        "min": min(values), "max": max(values)}
                                   for name, values in metrics.items()}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report_directory", type=Path)
    parser.add_argument("--output", type=Path, help="Separate output file; defaults to report_directory/analysis.json")
    parser.add_argument("--log-utc-offset-minutes", type=int, default=480,
                        help="UTC offset of local timestamps in Host logs (default: 480)")
    args = parser.parse_args()
    root = args.report_directory
    output = args.output or root / "analysis.json"
    log_timezone = timezone(timedelta(minutes=args.log_utc_offset_minutes))
    windows = [summarize_window(p, log_timezone) for p in sorted(root.iterdir()) if p.is_dir() and (p / "host-frames.csv").exists()]
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
    result = {"schema": "redclaw.video-link-baseline-analysis.v3", "scope": "local Host only",
              "log_utc_offset_minutes": args.log_utc_offset_minutes,
              "contract": load(root / "plan.json")["contract"],
              "percentile_method": "nearest rank", "cpu_unit": "100 percent equals one logical core",
              "notes": ["Stage summary windows overlap status boundaries; weighted values are supporting diagnostics.",
                        "v1 capture copy weights use attempts minus timeouts only when capture failure delta is zero; v2 uses explicit successful capture count.",
                        "Static zero-frame windows have no per-frame latency quantiles.",
                        "Send attribution excludes failed/cancelled frames; outcome counts are retained.",
                        "Wait reasons are exclusive in channel/in-flight/buffered/token priority order, not independent causal estimates.",
                        "Overshoot is included in reason wait time; it is not additive. Overshoot frame count is not timeout/wakeup count.",
                        "probe_wait_us is unpopulated in trace v1; zero does not demonstrate absence of probe effects.",
                        "Unattributed pacer time includes packetization, locks, scheduling and other uninstrumented work.",
                        "Wire bytes include fragment headers and are not pure codec payload bytes.",
                        "No cross-machine timestamp subtraction or remote GUI dispatch inference."],
              "windows": windows, "three_run_summary": aggregate,
              "send_attribution_summary": attribution_summary(windows)}
    output.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(aggregate, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
