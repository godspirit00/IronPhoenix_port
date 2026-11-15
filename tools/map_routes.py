"""Generate offline map routes using normal game menus (no asset overrides)."""
from pathlib import Path
import argparse

# Mode menu position, map position. Scene IDs come from the supplied disc's
# SceneInfo; runtime scene telemetry and screenshots must confirm each route.
MAPS = {
    'great-wall': ('in02', 0, 0),
    'temple-of-paganism': ('in03', 0, 1),
    'dragon-valley': ('in04', 0, 2),
    'training-square': ('se01', 0, 3),
    'thousand-bone-tomb': ('se02', 0, 4),
    'castle-entrance': ('se03', 0, 5),
    'apparatus-maze': ('se04', 0, 6),
    'yama-hall': ('se06', 0, 7),
    'electric-shock-chamber': ('sw04', 0, 8),
    'summoning-altar': ('sw05', 0, 9),
    'buddha-cave': ('se08', 0, 10),
    'night-demon-warship': ('sw01', 1, 5),
    'training-square-night': ('se07', 1, 7),
    'red-vipers-den': ('in01', 4, 0),
}

def route(name, base=None):
    if base is None:
        base = Path(__file__).with_name('inputs').joinpath('buddha_cave.txt').read_text().strip()
    _, mode, position = MAPS[name]
    presses = [(float(p.split(':', 1)[0]), p.split(':', 1)[1]) for p in base.split(',')]
    presses = [(t, b) for t, b in presses if not 73 <= t < 78]
    presses += [(68 + i * .4, 'down') for i in range(mode)]
    presses += [(73 + i * .4, 'down') for i in range(position)]
    return ','.join(f'{t:g}:{b}' for t, b in sorted(presses)) + '\n'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', choices=MAPS)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(route(args.map), encoding='ascii')
    print(f'{args.map}: expected scene {MAPS[args.map][0]}.scn')

if __name__ == '__main__':
    main()
