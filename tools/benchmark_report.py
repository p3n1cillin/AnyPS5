import argparse
import csv
import datetime
import json
import math
from pathlib import Path
import re
import statistics


def finite_number(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"Nonfinite metric: {value}")
    return result


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def frame_metrics(text):
    outputs = {}
    seen = set()
    for line in text.splitlines():
        if not line.startswith("[FrameTiming] "):
            continue
        fields = dict(re.findall(r"([a-z_]+)=([^ ]+)", line))
        if fields.get("endpoint") != "flip_complete":
            raise ValueError("Unsupported frame timing endpoint")
        identity = (int(fields["output"]), int(fields["frame"]))
        if identity in seen:
            raise ValueError(f"Duplicate frame timing: {identity}")
        seen.add(identity)
        intervals = outputs.setdefault(identity[0], [])
        if "flip_interval_ms" not in fields:
            continue
        interval = finite_number(fields["flip_interval_ms"])
        if interval <= 0:
            raise ValueError("Frame intervals must be positive")
        intervals.append(interval)
    result = {}
    for output, intervals in outputs.items():
        if not intervals:
            continue
        mean = statistics.mean(intervals)
        result[str(output)] = {
            "intervals": len(intervals),
            "duration_s": sum(intervals) / 1000,
            "average_fps": 1000 / mean,
            "mean_ms": mean,
            "p50_ms": percentile(intervals, 0.50),
            "p90_ms": percentile(intervals, 0.90),
            "p95_ms": percentile(intervals, 0.95),
            "p99_ms": percentile(intervals, 0.99),
            "max_ms": max(intervals),
        }
    return {"endpoint": "flip_complete", "display_confirmed": False, "outputs": result}


def telemetry_metrics(rows):
    if not rows:
        return None
    times = [datetime.datetime.fromisoformat(row["utc"].replace("Z", "+00:00")) for row in rows]
    if any(at.tzinfo is None for at in times):
        raise ValueError("Telemetry timestamps require a timezone")
    if any(after <= before for before, after in zip(times, times[1:])):
        raise ValueError("Telemetry timestamps must increase")
    if len({row["pid"] for row in rows}) != 1:
        raise ValueError("Telemetry contains more than one process")
    result = {"samples": len(rows), "duration_s": (times[-1] - times[0]).total_seconds()}
    for field in ("cpuPercent", "workingSetMiB", "privateMiB", "systemAvailableMiB", "gpuPercent", "gpuMemoryPercent", "gpuMiB", "gpuWatts", "gpuCelsius"):
        values = [finite_number(row[field]) for row in rows]
        if any(value < 0 for value in values):
            raise ValueError(f"Negative telemetry metric: {field}")
        if field.endswith("Percent") and any(value > 100 for value in values):
            raise ValueError(f"Utilization exceeds 100 percent: {field}")
        result[field] = {"sample_mean": statistics.mean(values), "min": min(values), "max": max(values)}
    return result


def thread_metrics(rows):
    threads = {}
    for row in rows:
        value = finite_number(row["cpuCorePercent"])
        if value < 0:
            raise ValueError("Negative thread CPU utilization")
        threads.setdefault(row["thread"], []).append(value)
    result = [{"thread": thread, "samples": len(values), "sample_mean_core_percent": statistics.mean(values), "max_core_percent": max(values)} for thread, values in threads.items()]
    return sorted(result, key=lambda entry: entry["sample_mean_core_percent"], reverse=True)[:10]


def log_metrics(text):
    cache = re.findall(r"\[shader-disk-cache\].*?totals: (\d+) hits, (\d+) misses, (\d+) writes", text)
    progress = [line for line in text.splitlines() if "[gpu-progress]" in line]
    gpu = re.findall(r"\[gputime\] ([\d.]+) ms of GPU time in (\d+) batches over 10 s \(batch ([\d.]+) ms first to last command; classes ([\d.]+) ms", text)
    return {
        "shader_cache_last_reported_totals": dict(zip(("hits", "misses", "writes"), map(int, cache[-1]))) if cache else None,
        "gpu_timing_windows": [{"program_ms": finite_number(program), "batches": int(batches), "batch_span_ms": finite_number(batch), "class_ms": finite_number(classes)} for program, batches, batch, classes in gpu],
        "gpu_progress": progress[-30:],
        "device_loss_reported": "Vulkan result -4" in text,
        "fatal_reported": "FATAL:" in text,
    }


def compare_reports(current, baseline):
    reasons = []
    for field in ("game", "version", "driver", "scenario", "settings", "profiling"):
        present = field in current["metadata"] and field in baseline["metadata"]
        if not present or current["metadata"][field] != baseline["metadata"][field]:
            reasons.append(f"Metadata differs or is missing: {field}")
    for candidate in (current, baseline):
        if candidate["frames"].get("endpoint") != "flip_complete":
            reasons.append("Frame timing endpoint differs or is missing")
            break
    if not set(current["frames"]["outputs"]).intersection(baseline["frames"]["outputs"]):
        reasons.append("No matching frame interval measurements")
    result = {"compatible": not reasons, "reasons": reasons, "average_fps_change_percent": {}}
    if reasons:
        return result
    previous = baseline["frames"]["outputs"]
    for output, metrics in current["frames"]["outputs"].items():
        if output in previous:
            before = finite_number(previous[output]["average_fps"])
            if before <= 0:
                raise ValueError("Baseline FPS must be positive")
            result["average_fps_change_percent"][output] = 100 * (metrics["average_fps"] / before - 1)
    return result


def read_csv(path):
    with path.open(newline="", encoding="utf-8-sig") as file:
        return list(csv.DictReader(file))


def main():
    parser = argparse.ArgumentParser(description="Summarize AnyPS5 flip timings and captured telemetry; enable APS5_ENABLE_TIMING_LOG when building to record frame-timing.log.")
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--timings", type=Path)
    parser.add_argument("--telemetry", type=Path)
    parser.add_argument("--threads", type=Path)
    parser.add_argument("--log", type=Path, action="append", default=[])
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = {
            "metadata": json.loads(args.metadata.read_text(encoding="utf-8-sig")),
            "frames": frame_metrics(args.timings.read_text(encoding="utf-8-sig")) if args.timings else frame_metrics(""),
            "telemetry": telemetry_metrics(read_csv(args.telemetry)) if args.telemetry else None,
            "top_threads": thread_metrics(read_csv(args.threads)) if args.threads else [],
            "logs": log_metrics("\n".join(path.read_text(encoding="utf-8-sig", errors="replace") for path in args.log)),
        }
        if args.compare:
            report["comparison"] = compare_reports(report, json.loads(args.compare.read_text(encoding="utf-8-sig")))
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
