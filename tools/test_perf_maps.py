import unittest
from pathlib import Path
from map_routes import MAPS, route
from map_journey import journey

class MapRoutesTests(unittest.TestCase):
    def test_journey_separates_combat_from_quit_confirmation(self):
        text, phases = journey(['summoning-altar', 'yama-hall', 'buddha-cave'])
        presses = self.presses(text)
        self.assertEqual([p['scene'] for p in phases], ['sw05', 'se06', 'se08'])
        self.assertEqual([(p['start'], p['end']) for p in phases], [(115,144),(190,219),(270,299)])
        for quit_at in (145,225):
            self.assertEqual([b for t,b in presses if quit_at <= t < quit_at+5],
                             ['start','up','a','up','left','a'])

    def presses(self, text):
        return [(float(p.split(':')[0]), p.split(':')[1]) for p in text.strip().split(',')]

    def test_buddha_route_retains_validated_inputs(self):
        base = Path(__file__).with_name('inputs').joinpath('buddha_cave.txt').read_text()
        self.assertEqual(sorted(self.presses(base)), self.presses(route('buddha-cave')))

    def test_map_navigation_does_not_change_combat(self):
        base = self.presses(route('buddha-cave'))
        for name, (_, mode, position) in MAPS.items():
            with self.subTest(map=name):
                presses = self.presses(route(name))
                self.assertEqual([p for p in base if p[0] >= 78], [p for p in presses if p[0] >= 78])
                self.assertEqual(sum(68 <= t < 72 and b == 'down' for t, b in presses), mode)
                self.assertEqual(sum(73 <= t < 78 and b == 'down' for t, b in presses), position)

