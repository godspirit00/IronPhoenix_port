#!/usr/bin/env python3
"""Temporary NVIDIA per-application threading test; always restore prior absence.

Refuses to run if either an application profile or the named profile already
exists. No global setting is changed. Requires the compiled build/nvprofile.exe.
"""
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    helper = root / "build/nvprofile.exe"
    clean = "application_profile=0 named_profile=0"
    def probe():
        return subprocess.check_output([str(helper), "probe"], text=True).strip()

    tasks = subprocess.check_output(["tasklist", "/fi", "imagename eq ironphoenix.exe", "/fo", "csv", "/nh"], text=True)
    if '"ironphoenix.exe"' in tasks.lower():
        raise SystemExit("Close the running game before the temporary driver test; no settings were changed.")
    if probe() != clean:
        raise SystemExit("Existing NVIDIA profile found; no settings were changed.")
    saved = root / "scratch/perf/nv-thread-test-state.txt"
    saved.parent.mkdir(parents=True, exist_ok=True)
    saved.write_text("Before: " + clean + "\n", encoding="utf-8")
    result = 1
    try:
        subprocess.run([str(helper), "set", "off"], check=True)
        with saved.open("a", encoding="utf-8") as state:
            state.write("Temporary ironphoenix.exe profile: threaded optimization off\n")
        result = subprocess.run([sys.executable, str(root / "tools/perf_run.py"),
                                 "--tag", "nv-thread-off", "--no-cpu-timing",
                                 "--hz", "60", "--shots", "0"], cwd=root).returncode
    finally:
        # The initial probe established that this named profile belongs to this
        # test. Remove it even if launching/benchmarking fails partway through.
        restored = subprocess.run([str(helper), "delete"]).returncode == 0 and probe() == clean
        with saved.open("a", encoding="utf-8") as state:
            state.write("Restored original absence: " + str(restored) + "\n")
        if not restored:
            raise SystemExit("NVIDIA profile restoration failed; inspect the saved state and run nvprofile delete.")
        print("Original NVIDIA application-profile state restored.", flush=True)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
