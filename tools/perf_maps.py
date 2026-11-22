"""Sequential map regression matrix; preserves every per-map report and screenshot.

Example: python tools/perf_maps.py --tag map-check --maps yama-hall summoning-altar
This checks scene selection and catastrophic regressions, not a firm-60 target.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from map_routes import MAPS, route

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--maps', nargs='+', choices=MAPS, default=list(MAPS))
    parser.add_argument('--mode', choices=('off', '1', 'verify'), default='1')
    parser.add_argument('--seconds', type=int, default=155)
    parser.add_argument('--start', type=int, default=115)
    parser.add_argument('--minimum-fps', type=int, default=5)
    parser.add_argument('--shots', type=int, default=5, help='screenshot interval (each one stalls ~0.5-1 s at scale 3)')
    parser.add_argument('--texture-state-verify', action='store_true')
    parser.add_argument('--renderer', choices=('opengl', 'vulkan'), default='opengl')
    parser.add_argument('--exe', type=Path, help='executable (default: perf_run.py default, build/ironphoenix.exe)')
    parser.add_argument('--ordinary', action='store_true',
                        help='ordinary user settings: all HLE bridges off (ignores --mode)')
    args = parser.parse_args()
    if not args.tag or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.tag):
        parser.error('invalid tag')
    if not 100 <= args.start < args.seconds - 1 or args.minimum_fps < 1:
        parser.error('invalid gameplay window or minimum FPS')
    root = Path(__file__).resolve().parents[1]
    out = root / 'scratch/perf' / args.tag
    out.mkdir(parents=True, exist_ok=False)
    rows = []
    for name in args.maps:
        script = out / f'{name}.txt'
        script.write_text(route(name), encoding='ascii')
        tag = f'{args.tag}-{name}'
        command = [sys.executable, str(root/'tools/perf_run.py'), '--tag', tag,
                   '--script', str(script), '--expect-scene', MAPS[name][0],
                   '--renderer', args.renderer, *(['--exe', str(args.exe)] if args.exe else []),
                   *(['--hle-up', 'off', '--hle-indexed', 'off', '--hle-resources', 'off', '--hle-buffers', 'off']
                     if args.ordinary else
                     ['--hle-up', '1', '--hle-indexed', '1', '--hle-resources', '1', '--hle-buffers', args.mode]),
                   '--hz', '60', '--scale', '3', '--seconds', str(args.seconds),
                   '--start', str(args.start), '--shots', str(args.shots), '--no-cpu-timing']
        if args.texture_state_verify:
            command.append('--texture-state-verify')
        print(f'Testing {name} ({MAPS[name][0]})', flush=True)
        with (out / f'{name}-runner.log').open('w', encoding='utf-8') as log:
            completed = subprocess.run(command, cwd=root, stdout=log, stderr=log)
        report_file = root / 'scratch/perf' / tag / 'report.json'
        report = json.loads(report_file.read_text()) if report_file.exists() else {}
        fps = report.get('fps_samples') or {}
        warnings = list(report.get('warnings', []))
        if completed.returncode and not warnings:
            warnings.append(f'Runner exited with code {completed.returncode}')
        if fps.get('min', 0) < args.minimum_fps:
            warnings.append(f'FPS fell below functional threshold {args.minimum_fps}')
        row = dict(map=name, scene=MAPS[name][0], fps=fps, fps_interval=report.get('fps_interval'),
                   buffers=report.get('hle_buffers'), warnings=warnings,
                   texture_state=report.get('texture_state'),
                   executable_sha256=report.get('configuration', {}).get('executable_sha256'),
                   report=str(report_file))
        rows.append(row)
        (out/'summary.json').write_text(json.dumps(rows, indent=2)+'\n')
        print(json.dumps(row), flush=True)
    return int(any(row['warnings'] for row in rows))

if __name__ == '__main__':
    raise SystemExit(main())
