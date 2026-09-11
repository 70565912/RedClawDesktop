"""Correlate local fixture runtime stages with native input receipts (same machine QPC)."""
import argparse
import json
import math
from pathlib import Path


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), **{name: values[math.ceil(len(values) * q) - 1]
            for name, q in (("p50_us", .5), ("p95_us", .95), ("p99_us", .99), ("max_us", 1))}}


def measure(report):
    host_generation = read(report / "host-injection-export.json")["result"]["generation"]
    host = read(report.parent / "host" / f"input-injections-{host_generation}.json")
    host.update(read(report.parent / "host" / host["stage_file"]))
    outputs = []
    for path in sorted(report.glob("*-input-window.json")):
        window = read(path)
        tag = path.name.removesuffix("-input-window.json")
        generation = read(report / f"{tag}-input-export.json")["result"]["generation"]
        controller = read(report.parent / "controller" / f"input-injections-{generation}.json")
        controller.update(read(report.parent / "controller" / controller["stage_file"]))
        stages = {}
        duplicates = 0
        for source in (controller, host):
            for s in source["stages"]:
                if s["sequence"]:
                    key = (s["sequence"], s["stage"])
                    duplicates += key in stages
                    stages[key] = s["begin_us"]
        rows = []
        invalid = 0
        for r in window["receipts"]:
            times = [stages.get((r["sequence"], stage)) for stage in range(8)]
            if any(t is None for t in times):
                invalid += 1
                continue
            local, send, locked, encoded, sent, received, enqueued, consumed = times
            parts = dict(gui_to_runtime=local-r["sent_us"], local_dispatch=send-local,
                         send_lock=locked-send, encode=encoded-locked, send_call=sent-encoded,
                         send_to_peer=received-encoded, peer_enqueue=enqueued-received,
                         host_queue=consumed-enqueued, host_to_inject=r["host_begin_us"]-consumed,
                         injection=r["host_end_us"]-r["host_begin_us"],
                         injection_to_receipt=r["received_us"]-r["host_begin_us"])
            # Peer receive can precede send-call return; injection receipt can precede
            # SendInput return. These intervals intentionally start at call BEGIN.
            invalid += any(v < 0 for v in parts.values())
            rows.append(dict(sequence=r["sequence"], kind=r["kind"], sent_us=r["sent_us"],
                             latency_us=r["receive_latency_us"], parts_us=parts, stage_times_us=times))
        loop = {}
        loop_names = ("sleep", "before_local_input", "local_input", "before_host_input",
                      "host_input", "agent_pump", "after_agent", "maintenance", "adaptation",
                      "other_signaling", "dht", "negotiation", "periodic_stats", "loop_tail",
                      "log_lock_ge_1ms", "output_write_ge_1ms", "file_write_ge_1ms", "flush_ge_1ms",
                      "dht_listen_port_ge_1ms")
        for role, source in (("controller", controller), ("host", host)):
            samples = [s for s in source["stages"] if not s["sequence"]
                       and window["start_us"] <= s["end_us"] < window["end_us"]]
            loop[role] = {name: {
                "wall": distribution(s["end_us"]-s["begin_us"] for s in samples if s["stage"] == i),
                "cpu": distribution(s["cpu_us"] for s in samples if s["stage"] == i),
                "non_execution": distribution(max(0, s["end_us"]-s["begin_us"]-s["cpu_us"])
                                              for s in samples if s["stage"] == i),
            } for i, name in enumerate(loop_names, 8)}
            loop[role]["longest"] = sorted(samples, key=lambda s: s["end_us"]-s["begin_us"], reverse=True)[:15]
            loop[role]["crossing_begin"] = sum(s["begin_us"] < window["start_us"] for s in samples)
        summary = {
            "schema": "redclaw.qa-input-stages.v1", "window": tag,
            "valid": bool(window["valid"] and not invalid and not duplicates and rows
                          and not host["stage_overflow"] and not controller["stage_overflow"]),
            "missing_or_invalid": invalid, "duplicate_stages": duplicates,
            "overflow": {"host": host["stage_overflow"], "controller": controller["stage_overflow"]},
            "parts": {key: distribution(r["parts_us"][key] for r in rows) for key in (rows[0]["parts_us"] if rows else {})},
            "loop": loop, "slowest": sorted(rows, key=lambda r: r["latency_us"], reverse=True)[:25], "receipts": rows,
        }
        output = report / f"{tag}-input-stages.json"
        output.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        outputs.append({k: summary[k] for k in ("window", "valid", "missing_or_invalid", "parts")})
    return outputs


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    result = measure(args.report)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result and all(r["valid"] for r in result) else 1)
