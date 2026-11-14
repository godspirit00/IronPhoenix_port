#!/usr/bin/env python3
"""Run one isolated scripted performance check; preserve log, screenshots, and report.

Windows: python tools/perf_run.py --tag baseline --no-fast-state
Run sequentially with a warm shader cache. Screenshots add overhead; use
--shots 0 for timing runs after visually confirming the route.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from perf_report import summarize


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--exe", type=Path, default=root / "build/ironphoenix.exe")
    parser.add_argument("--script", type=Path, default=root / "tools/inputs/buddha_cave.txt")
    parser.add_argument("--seconds", type=float, default=185)
    parser.add_argument("--start", type=float, default=120)
    parser.add_argument("--scale", type=int, default=3)
    parser.add_argument("--hz", type=int, default=120)
    parser.add_argument("--shots", type=float, default=5)
    parser.add_argument("--no-fast-state", action="store_true")
    parser.add_argument("--no-fast-uniforms", action="store_true")
    parser.add_argument("--no-fast-copy", action="store_true")
    parser.add_argument("--no-fast-vertex", action="store_true")
    parser.add_argument("--gpu-timing", action="store_true")
    parser.add_argument("--no-cpu-timing", action="store_true")
    parser.add_argument("--gl-debug", action="store_true")
    args = parser.parse_args()
    if not 0 <= args.start < args.seconds or args.scale < 1 or args.hz < 1 or args.shots < 0:
        parser.error("invalid measurement interval, scale, refresh rate, or screenshot interval")
    if not args.tag or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in args.tag):
        parser.error("tag must contain only letters, digits, '-' or '_'")
    out = root / "scratch/perf" / args.tag
    out.mkdir(parents=True, exist_ok=False)
    exe = args.exe.resolve()
    executable_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    work = exe.parent
    env = os.environ.copy()
    if "XB_NULL_DRAW" in env:
        parser.error("unset XB_NULL_DRAW before benchmarking rendered gameplay")
    for name, enabled in (("XB_NO_FAST_STATE", args.no_fast_state),
                          ("XB_NO_FAST_UNIFORMS", args.no_fast_uniforms),
                          ("XB_NO_FAST_COPY", args.no_fast_copy),
                          ("XB_NO_FAST_VERTEX", args.no_fast_vertex),
                          ("XB_GPU_TIMING", args.gpu_timing),
                          ("XB_CPU_TIMING", not args.no_cpu_timing),
                          ("XB_GL_DEBUG", args.gl_debug)):
        env.pop(name, None)
        if enabled:
            env[name] = "1"
    command = [str(exe), "--game", str(root / "extracted"), "--save", str(root / "build/save"),
               "--hz", str(args.hz), "--scale", str(args.scale), "--novsync", "--input-only",
               "--input", args.script.read_text(encoding="utf-8").strip()]
    if args.shots:
        command += ["--shot-every", str(args.shots)]
    began = time.time()
    timed_out = False
    startup = None
    if os.name == "nt":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with (out / "console.log").open("w", encoding="utf-8") as console:
        proc = subprocess.Popen(command, cwd=work, env=env, stdout=console, stderr=console, startupinfo=startup)
        print(f"Running PID {proc.pid}; artifacts: {out}", flush=True)
        try:
            proc.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.terminate()
            proc.wait(timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
    log = out / "ironphoenix.log"
    shutil.copy2(work / "ironphoenix.log", log)
    for shot in (work / "shots").glob("*.bmp"):
        if shot.stat().st_mtime >= began:
            (out / "shots").mkdir(exist_ok=True)
            shutil.copy2(shot, out / "shots" / shot.name)
    report = summarize(log, args.start, args.seconds - 1)
    if not timed_out and proc.returncode:
        report["warnings"].append(f"Game exited with code {proc.returncode} before timeout.")
    console_text = (out / "console.log").read_text(encoding="utf-8", errors="replace")
    if any(word in console_text for word in ("MISMATCH", "Assertion failed", "Assertion failure", "FATAL:")):
        report["warnings"].append("Runtime output contains a validation failure; inspect console.log.")
    report["configuration"] = {"scale": args.scale, "hz": args.hz, "screenshots_seconds": args.shots,
                               "fast_state": not args.no_fast_state,
                               "fast_uniforms": not args.no_fast_uniforms, "gl_debug": args.gl_debug,
                               "fast_copy": not args.no_fast_copy,
                               "fast_vertex": not args.no_fast_vertex, "gpu_timing": args.gpu_timing,
                               "cpu_timing": not args.no_cpu_timing,
                               "exclusive_script": True, "executable": str(exe)}
    report["configuration"]["executable_sha256"] = executable_hash
    report["configuration"]["script_sha256"] = hashlib.sha256(args.script.read_bytes()).hexdigest()
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    return 1 if report["warnings"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
