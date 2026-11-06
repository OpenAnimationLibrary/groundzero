"""CI-only original scatter holdout authorship checks; no source renderer claims."""
import copy
import math
from pathlib import Path
import sys
import unittest
from xml.etree.ElementTree import fromstring

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scatter_holdout_fixtures import GROUPS, cases, validate_authorship


class ScatterHoldoutAuthorshipTest(unittest.TestCase):
    def test_every_configuration_keeps_independent_policy_and_false_claims(self):
        self.assertEqual(len(GROUPS), 10)
        for group in GROUPS:
            authored = list(cases(group))
            self.assertTrue(validate_authorship(group, authored))
            active = [c for c in authored if c['scatterHoldoutProbe']['enabled']]
            controls = [c for c in authored if not c['scatterHoldoutProbe']['enabled']]
            self.assertEqual((len(active), len(controls)), (100, 2))
            for row in authored:
                probe = row['scatterHoldoutProbe']
                self.assertFalse(probe['sourceProfileVerified'])
                self.assertFalse(probe['sourceRngLawIdentified'])
                self.assertFalse(probe['pixelParityClaimed'])
                self.assertFalse(probe['physicalTabletClaimed'])
                self.assertNotIn('sourceAdmission', row)
                self.assertNotIn('seed', row)
                self.assertEqual(row['overrides']['PressureScatter'], str(probe['enabled']).lower())
                mask = fromstring(row['overrides']['brush_definition']).find('MaskGenerator')
                self.assertEqual(float(mask.get('diameter')), row['diameter'])

    def test_static_dab_cells_and_capture_bounds_are_authored_without_clipping(self):
        for group in GROUPS:
            row = next(cases(group))
            probe = row['scatterHoldoutProbe']
            if 'pressureTrace' in probe:
                continue
            length = math.dist(row['start'], row['end'])
            interval = row['diameter'] * row['spacing']
            self.assertEqual(math.floor((length - .5) / interval) + 1, 32)
            left, top, width, height = probe['rect']
            reach = row['diameter'] * (probe['amount'] + .5)
            for x, y in (row['start'], row['end']):
                self.assertGreaterEqual(x - reach, left)
                self.assertLess(x + reach, left + width)
                self.assertGreaterEqual(y - reach, top)
                self.assertLess(y + reach, top + height)

    def test_unmeasured_scaling_and_pressure_are_not_marked_as_admitted(self):
        admitted = {'scatterholdout-origin-x', 'scatterholdout-origin-y', 'scatterholdout-west-xy'}
        for group in GROUPS:
            expected = 'current-static-component' if group in admitted else 'not-admitted-scaling-or-dynamics'
            self.assertTrue(all(c['scatterHoldoutProbe']['candidateScope'] == expected for c in cases(group)))

    def test_pressure_holdouts_use_actual_events_and_distinct_pressure_controls(self):
        profiles = {}
        for label in ('half', 'full', 'ramp'):
            row = next(cases('scatterholdout-pressure-' + label))
            self.assertEqual(row['delivery'], 'Qt tablet events')
            self.assertEqual(row['sensorProbe']['target'], 'Scatter')
            self.assertEqual(row['overrides']['ScatterUseCurve'], 'true')
            self.assertEqual(row['overrides']['ScattercommonCurve'], '0,0;1,1;')
            events = row['events']
            self.assertEqual(len(events), 33)
            self.assertEqual([row['start'], row['end']], [[events[0]['x'], events[0]['y']], [events[-1]['x'], events[-1]['y']]])
            self.assertTrue(all(a['timeUs'] < b['timeUs'] for a, b in zip(events, events[1:])))
            profiles[label] = [e['pressure'] for e in events]
        self.assertEqual(profiles['half'], [.5] * 33)
        self.assertEqual(profiles['full'], [1] * 33)
        self.assertEqual((profiles['ramp'][0], profiles['ramp'][-1]), (.25, 1))
        self.assertGreater(len(set(profiles['ramp'])), 2)

    def test_authorship_rejects_missing_replicate_and_mutated_delivery(self):
        group = 'scatterholdout-origin-x'
        authored = list(cases(group))
        for mutation in range(4):
            changed = copy.deepcopy(authored)
            if mutation == 0:
                changed.pop()
            if mutation == 1:
                changed[1]['scatterHoldoutProbe']['replicate'] = 0
            if mutation == 2:
                changed[0]['start'][0] += 1
            if mutation == 3:
                changed[0]['delivery'] = 'Qt tablet events'
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_authorship(group, changed)


if __name__ == '__main__':
    unittest.main()
