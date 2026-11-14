#!/usr/bin/env python3
"""Summarize a runtime log over an explicit elapsed-time window (no dependencies).

Example: python tools/perf_report.py build/ironphoenix.log --start 120 --end 180
The phase timers measure CPU wall time, including driver waits, not GPU time.
"""
import argparse
import json
import re
from pathlib import Path


def summarize(path, start, end):
    origin = previous = None
    elapsed = 0
    fps = []
    phases = {}
    renderer = None
    exit_at = None
    copy_checks = 0
    validation_failed = False
    gpu_samples = 0
    gpu_draw_us = 0.0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stamp = re.match(r"\[\s*\d+\s+(\d+)\]", line)
        if not stamp:
            continue
        tick = int(stamp[1])
        if origin is None:
            origin = previous = tick
        # Runtime logs truncate GetTickCount modulo 1,000,000 ms.
        delta = tick - previous
        if delta < -500000:
            delta += 1000000
        # Threads can emit a timestamp obtained before the preceding log line.
        if delta >= 0:
            elapsed += delta / 1000
            previous = tick
        if "OpenGL:" in line:
            renderer = line.split("OpenGL:", 1)[1].strip()
        if "HalReturnToFirmware" in line and exit_at is None:
            exit_at = elapsed
        if "surface snapshot check" in line:
            copy_checks += 1
        if "MISMATCH" in line or "FATAL:" in line:
            validation_failed = True
        if elapsed < start or elapsed > end:
            continue
        gpu = re.search(r"gpu draw sample: ([\d.]+) us/draw \((\d+) draws", line)
        if gpu:
            samples = int(gpu[2])
            gpu_samples += samples
            gpu_draw_us += float(gpu[1]) * samples
        if "gpu time:" in line:
            for name, ms, calls in re.findall(r"(\w+) ([\d.]+)ms/(\d+)", line):
                entry = phases.setdefault(name, [0.0, 0])
                entry[0] += float(ms)
                entry[1] += int(calls)
        match = re.search(r"perf: (\d+) fps", line)
        if match:
            fps.append(int(match[1]))
    warnings = []
    if validation_failed:
        warnings.append("Runtime log contains a validation failure.")
    if elapsed < end - 1:
        warnings.append(f"Log ends at {elapsed:.1f}s, before the requested window ends.")
    if exit_at is not None:
        warnings.append(f"Game returned to firmware at {exit_at:.1f}s.")
    draws = phases.get("draw_begin", [0, 0])[1]
    if not fps or (not draws and not any(fps)):
        warnings.append("No rendered frames in the requested measurement window.")
    return {
        "log": str(path), "renderer": renderer,
        "window_seconds": [start, end], "log_duration_seconds": round(elapsed, 3),
        "fps_sample_count": len(fps),
        "fps_samples": {"min": min(fps), "mean": round(sum(fps) / len(fps), 2),
                        "max": max(fps)} if fps else None,
        "cpu_phase_us_per_call": {
            name: round(ms * 1000 / calls, 3) for name, (ms, calls) in phases.items() if calls
        },
        "cpu_phase_us_per_draw": {
            name: round(ms * 1000 / draws, 3) for name, (ms, _) in phases.items()
        } if draws else {},
        "draw_calls": draws if phases else None,
        "snapshot_validation_checks": copy_checks, "warnings": warnings,
        "gpu_draw_sample": {"samples": gpu_samples,
                            "mean_us": round(gpu_draw_us / gpu_samples, 3)} if gpu_samples else None,
        "notes": ["FPS samples are periodic, smoothed counters, not individual frame times.",
                  "CPU phase timers nest; do not add them. Millisecond log values are rounded.",
                  "GPU samples span flush_draw commands, including uploads/dependencies and possible idle gaps; they are not full-frame GPU time.",
                  "Verify the map and combat visually; telemetry cannot establish the scene."],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--start", type=float, required=True)
    parser.add_argument("--end", type=float, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 0 <= args.start < args.end:
        parser.error("require 0 <= start < end")
    report = summarize(args.log, args.start, args.end)
    result = json.dumps(report, indent=2, ensure_ascii=False)
    if args.output:
        args.output.write_text(result + "\n", encoding="utf-8")
    print(result)
    return 1 if report["warnings"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
