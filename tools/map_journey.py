"""Generate and assess a continuous offline map-transition session."""
import argparse
import json
from pathlib import Path
from map_routes import MAPS, route
from perf_report import summarize

DEATHMATCH = ['summoning-altar', 'yama-hall', 'buddha-cave', 'great-wall',
              'temple-of-paganism', 'dragon-valley', 'training-square',
              'thousand-bone-tomb', 'castle-entrance', 'apparatus-maze',
              'electric-shock-chamber']

def journey(names):
    if not names or any(MAPS[n][1] != 0 for n in names):
        raise ValueError('journey currently supports deathmatch maps')
    initial = route(names[0])
    presses = [(float(t), b) for item in initial.strip().split(',')
               for t, b in [item.split(':') ] if float(t) < 145]
    phases = [dict(map=names[0], scene=MAPS[names[0]][0], start=115, end=144)]
    combat = ['up', 'x', 'left', 'y', 'up', 'a', 'right', 'x', 'b', 'down']
    quit_at = 145
    for name in names[1:]:
        # Pause, select Quit Game, confirm, returning to Game Mode.
        presses += [(quit_at, 'start'), (quit_at+1, 'up'), (quit_at+2, 'a'),
                    (quit_at+3, 'up'), (quit_at+3.5, 'left'), (quit_at+4, 'a')]
        presses += [(quit_at+10, 'a')]
        presses += [(quit_at+11+i*.4, 'down') for i in range(MAPS[name][2])]
        presses += [(quit_at+t, 'a') for t in (17, 23, 29, 35)]
        start = quit_at+45
        presses += [(start+i*.5, combat[i % len(combat)]) for i in range(60)]
        phases.append(dict(map=name, scene=MAPS[name][0], start=start, end=start+29))
        quit_at += 80
    return ','.join(f'{t:g}:{b}' for t, b in sorted(presses))+'\n', phases

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--results', type=Path)
    args = parser.parse_args()
    if args.log:
        phases = json.loads(args.metadata.read_text())
        rows = []
        for phase in phases:
            report = summarize(args.log, phase['start'], phase['end'])
            prior = [e for e in report['scene_events'] if e['seconds'] <= phase['start']]
            if not prior or prior[-1]['asset'] != phase['scene']+'.scn':
                report['warnings'].append('Expected scene was not active before this window')
            if (report.get('fps_samples') or {}).get('min', 0) < 5:
                report['warnings'].append('FPS below the 5 fps functional regression floor')
            rows.append(dict(**phase, fps=report['fps_samples'], warnings=report['warnings']))
        result_file = args.results or args.log.parent / 'map_journey.results.json'
        result_file.write_text(json.dumps(rows, indent=2)+'\n')
        print(json.dumps(rows, indent=2))
        return int(any(row['warnings'] for row in rows))
    if not args.output:
        parser.error('--output is required when generating')
    text, phases = journey(DEATHMATCH)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding='ascii')
    args.metadata.write_text(json.dumps(phases, indent=2)+'\n')
    print(f'Run for {phases[-1]["end"]+11} seconds; {len(phases)} map windows')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
