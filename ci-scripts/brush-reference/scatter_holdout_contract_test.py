"""CI-only focused scatter input, source readback, pending route and cache guards.

Synthetic cache rasters below validate storage contracts, never source renderer
fidelity. Original source observations and native stochastic gates stay separate.
"""
import base64
import copy
import hashlib
import json
from pathlib import Path
import random
import struct
import tempfile
import unittest
import zlib
from xml.etree.ElementTree import Element, SubElement, tostring, fromstring

import scatter_holdout_contract as contract
from scatter_holdout_fixtures import prepare
import oracle_cache as cache
from scatter_holdout_statistics import canonical_alpha


def manifest(group, root):
    (root / 'output').mkdir(parents=True)
    (root / 'paintoppresets').mkdir()
    prepare(root, root, lambda *args: b'original authored metadata', group)
    return json.loads((root / 'output/fixtures.json').read_text())


def fields(case):
    return {**case['overrides'], 'paintop': 'paintbrush', 'CompositeOp': 'normal', 'EraserMode': 'false',
            'OpacityValue': '1', 'FlowValue': '1', 'MergedPaint': 'false',
            'PaintOpSettings/isAirbrushing': 'false', 'PaintOpSettings/ignoreSpacing': 'false',
            'PaintOpSettings/updateSpacingBetweenDabs': 'false', 'KisPrecisionOption/AutoPrecisionEnabled': 'false',
            'KisPrecisionOption/precisionLevel': '5'}


def delivery(case):
    if case['delivery'] == 'Node.paintLine':
        return {'mode': 'Node.paintLine', 'physicalTabletClaimed': False, 'events': [
            {'type': 'line-start', 'position': case['start'], 'pressure': 1},
            {'type': 'line-end', 'position': case['end'], 'pressure': 1}]}
    def event(row, kind, pressure):
        return {'type': kind, 'image': [row['x'], row['y']], 'pressure': pressure,
                'tilt': [row['tiltX'], row['tiltY']], 'rotation': row['rotation'],
                'timeMs': 10000 + round(row['timeUs'] / 1000)}
    rows = case['events']
    events = [event(rows[0], 171, 0), event(rows[0], 87, 0)]
    events += [event(row, 92 if i == 0 else 87, row['pressure']) for i, row in enumerate(rows)]
    events += [event(rows[-1], 93, 0), event(rows[-1], 172, 0)]
    state = {'preset': case['preset'], 'size': case['diameter'], 'disablePressure': True}
    return {'mode': 'Qt tablet events', 'physicalTabletClaimed': False, 'events': events,
            'stateBeforeProximity': dict(state), 'stateAfterProximity': dict(state), 'stateBeforeContact': dict(state)}


def effective_xml(case, observed):
    root = Element('Preset', name=case['preset'], paintopid='paintbrush')
    for name, value in observed.items():
        SubElement(root, 'param', name=name, type='string').text = value
    return tostring(root, encoding='utf-8')


def evidence(group, case, observed=None, delivered=None):
    result = contract.controls(group, case, observed or fields(case), delivered or delivery(case))
    # One synthetic nonzero alpha is sufficient to validate exact byte/hash
    # contracts; it never represents an acquired or qualified source stroke.
    left, top, width, height = result['rect']
    pixels = bytearray(width * height * 4)
    pixels[(height // 2 * width + width // 2) * 4 + 3] = 255
    result.update(encoding='zlib-base64-bgra8-row-major', data=base64.b64encode(zlib.compress(pixels)).decode(),
                  sha256Pixels=hashlib.sha256(pixels).hexdigest(), nonzeroAlphaPixels=1)
    return result


def full_png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = (b'\0' + bytes(2048 * 4)) * 2048
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2048, 2048, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def acquisition_fixture(root, group):
    fixture = manifest(group, root)
    planned = list(fixture['presets'])
    random.Random(0x47494831).shuffle(planned)
    ordinals = {name: i for i, name in enumerate(planned)}
    image = full_png()
    rows = []
    for i, case in enumerate(fixture['cases']):
        stem = case['preset'].replace(' ', '_')
        observed, delivered = fields(case), delivery(case)
        xml = effective_xml(case, observed)
        count = 1 if case['scatterHoldoutProbe']['enabled'] else 2
        proof = evidence(group, case, observed, delivered)
        row = {**copy.deepcopy(case), 'intendedOrdinal': i, 'executionOrdinal': ordinals[case['preset']],
               'scatterHoldoutEvidence': proof, 'deliveryReport': delivered, 'sha256Pixels': 'b' * 64,
               'repeatSemantics': 'transparent-rgb-zero-v1', 'sha256RepeatPixels': 'd' * 64,
               'repeatSha256Pixels': ['b' * 64] * count, 'repeatSha256CanonicalPixels': ['d' * 64] * count,
               'repeatImages': [{'image': stem + '.repeat-1.png', 'sha256File': cache.digest(image)}] if count == 2 else [],
               'repeatDifferences': [{'hiddenRgbOnlyPixels': 0, 'visibleOrAlphaChangedPixels': 0}] * (count - 1),
               'image': stem + '.png', 'effectivePresetXml': stem + '.effective.xml',
               'effectivePresetSha256': cache.digest(xml), 'sourcePresetSha256': 'a' * 64,
               'profile': 'sRGB-elle-V2-srgbtrc.icc', 'changed': True,
               'effectiveView': {'diameter': case['diameter'], 'opacity': 1, 'flow': 1, 'rotation': 0,
                   'blend': 'normal', 'eraser': False, 'alphaLock': False, 'disablePressure': case['sourcePressureFlag']}}
        (root / 'output' / row['image']).write_bytes(image)
        (root / 'output' / row['effectivePresetXml']).write_bytes(xml)
        if count == 2:
            (root / 'output' / row['repeatImages'][0]['image']).write_bytes(image)
        rows.append(row)
    report = {'status': 'complete', 'version': '5.3.4.1', 'records': rows,
              'intendedPresetOrder': fixture['presets'], 'plannedExecutionOrder': planned, 'actualExecutionOrder': list(planned)}
    (root / 'output/oracle-status.json').write_bytes(cache.canonical(report))
    return fixture, report


class ScatterHoldoutContractTest(unittest.TestCase):
    def test_all_ten_complete_manifests_keep_exact_original_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            for group in contract.GROUPS:
                fixture = manifest(group, Path(directory) / group)
                self.assertEqual(contract.validate_manifest(fixture), contract.status(group))
                self.assertEqual(len(fixture['cases']), 102)
                self.assertIs(fixture['candidateExpected'], group in contract.CANDIDATE_GROUPS)
                for defect in ('truncated', 'typed-version', 'scope', 'source-claim', 'value', 'pressure-mode'):
                    changed = copy.deepcopy(fixture)
                    if defect == 'truncated': changed['presets'].pop(); changed['cases'].pop()
                    if defect == 'typed-version': changed['cases'][0]['scatterHoldoutProbe']['version'] = True
                    if defect == 'scope': changed['candidateExpected'] = not changed['candidateExpected']
                    if defect == 'source-claim': changed['sourceProfileVerified'] = True
                    if defect == 'value': changed['cases'][0]['overrides']['ScatterValue'] = '3'
                    if defect == 'pressure-mode': changed['cases'][0]['sourcePressureFlag'] = not changed['cases'][0]['sourcePressureFlag']
                    with self.subTest(group=group, defect=defect), self.assertRaises(ValueError):
                        contract.validate_manifest(changed)

    def test_controls_delivery_and_actual_retained_xml_bind_together(self):
        with tempfile.TemporaryDirectory() as directory:
            for group in ('scatterholdout-origin-x', 'scatterholdout-pressure-ramp'):
                fixture = manifest(group, Path(directory) / group)
                case = fixture['cases'][0]
                observed, sent = fields(case), delivery(case)
                proof = evidence(group, case, observed, sent)
                xml = effective_xml(case, observed)
                row = {**case, 'deliveryReport': sent, 'scatterHoldoutEvidence': proof,
                       'effectivePresetSha256': cache.digest(xml)}
                contract.validate_effective_preset(group, case, row, xml)
                # Krita stores param values in CDATA. Its decoded field content
                # must remain authoritative without rejecting legitimate CDATA.
                cdata = ('<Preset name="' + case['preset'] + '" paintopid="paintbrush">' +
                         ''.join('<param name="' + key + '" type="string"><![CDATA[' + value + ']]></param>'
                                 for key, value in observed.items()) + '</Preset>').encode()
                cdata_row = {**row, 'effectivePresetSha256': cache.digest(cdata)}
                contract.validate_effective_preset(group, case, cdata_row, cdata)
                for defect in ('evidence', 'xml', 'xml-and-evidence', 'brush', 'brush-tail', 'sensor-tail', 'namespace', 'duplicate', 'delivery'):
                    changed = copy.deepcopy(row)
                    actual = dict(observed)
                    data = xml
                    if defect == 'evidence': changed['scatterHoldoutEvidence']['version'] = True
                    if defect in ('xml', 'xml-and-evidence'):
                        actual['ScatterValue'] = '2'
                        data = effective_xml(case, actual)
                        changed['effectivePresetSha256'] = cache.digest(data)
                    if defect == 'xml-and-evidence': changed['scatterHoldoutEvidence']['effectiveControls']['ScatterValue'] = '2'
                    if defect == 'brush':
                        actual['brush_definition'] = actual['brush_definition'].replace('spacing="4"', 'spacing="1"')
                        data = effective_xml(case, actual); changed['effectivePresetSha256'] = cache.digest(data)
                    if defect in ('brush-tail', 'sensor-tail'):
                        key = 'brush_definition' if defect == 'brush-tail' else 'ScatterSensor'
                        # Change retained effective XML and copied evidence together.
                        tree = fromstring(actual[key]); tree[0].tail = 'unsupported'
                        actual[key] = tostring(tree, encoding='unicode')
                        if key == 'brush_definition': changed['scatterHoldoutEvidence']['effectiveBrushDefinition'] = actual[key]
                        else: changed['scatterHoldoutEvidence']['effectiveControls'][key] = actual[key]
                        data = effective_xml(case, actual); changed['effectivePresetSha256'] = cache.digest(data)
                    if defect == 'namespace':
                        data = xml.replace(b'<Preset ', b'<Preset xmlns="" '); changed['effectivePresetSha256'] = cache.digest(data)
                    if defect == 'duplicate':
                        data = xml.replace(b'</Preset>', b'<param name="ScatterValue">1</param></Preset>'); changed['effectivePresetSha256'] = cache.digest(data)
                    if defect == 'delivery': changed['deliveryReport']['events'][0]['pressure'] = .25
                    with self.subTest(group=group, defect=defect), self.assertRaises(ValueError):
                        contract.validate_effective_preset(group, case, changed, data)

    def test_actual_execution_and_complete_cache_repeat_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'source'
            fixture, report = acquisition_fixture(root, 'scatterholdout-origin-x')
            self.assertTrue(contract.validate_execution(fixture, report))
            files = cache.acquisition(root / 'output', fixture)
            self.assertEqual(len(files), 207)
            expected = {'schema': cache.VERSION, 'group': fixture['group'], 'applicationSha256': 'a' * 64}
            destination = Path(directory) / 'cache'
            cache.seal(root, destination, expected)
            for name in files:
                (root / 'output' / name).unlink()
            cache.restore(root, destination, expected)
            self.assertEqual(cache.parse((root / 'output/oracle-status.json').read_bytes()), report)
            for defect in ('count', 'not-random', 'actual', 'ordinal', 'records', 'disabled-repeat', 'pixels'):
                changed = copy.deepcopy(report)
                if defect == 'count': changed['records'].pop()
                if defect == 'not-random':
                    changed['plannedExecutionOrder'] = list(fixture['presets']); changed['actualExecutionOrder'] = list(fixture['presets'])
                if defect == 'actual': changed['actualExecutionOrder'].reverse()
                if defect == 'ordinal': changed['records'][0]['intendedOrdinal'] = False
                if defect == 'records': changed['records'].reverse()
                if defect == 'disabled-repeat':
                    row = changed['records'][-1]; row['repeatSha256Pixels'] = row['repeatSha256Pixels'][:1]
                    row['repeatSha256CanonicalPixels'] = row['repeatSha256CanonicalPixels'][:1]; row['repeatImages'] = []; row['repeatDifferences'] = []
                if defect == 'pixels': changed['records'][0]['scatterHoldoutEvidence']['sha256Pixels'] = 'f' * 64
                (root / 'output/oracle-status.json').write_bytes(cache.canonical(changed))
                with self.subTest(defect=defect), self.assertRaises(ValueError):
                    cache.acquisition(root / 'output', fixture)
            actual_xml = root / 'output' / report['records'][0]['effectivePresetXml']
            original_xml = actual_xml.read_bytes()
            altered = original_xml.replace(b'<param name="ScatterValue" type="string">1</param>', b'<param name="ScatterValue" type="string">2</param>')
            self.assertNotEqual(altered, original_xml)
            actual_xml.write_bytes(altered)
            forged = copy.deepcopy(report)
            forged['records'][0]['effectivePresetSha256'] = cache.digest(altered)
            forged['records'][0]['scatterHoldoutEvidence']['effectiveControls']['ScatterValue'] = '2'
            (root / 'output/oracle-status.json').write_bytes(cache.canonical(forged))
            with self.assertRaises(ValueError):
                cache.acquisition(root / 'output', fixture)

    def test_pending_pressure_route_never_creates_native_comparison(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'pressure'
            fixture, report = acquisition_fixture(root, 'scatterholdout-pressure-half')
            self.assertEqual(len(cache.acquisition(root / 'output', fixture)), 207)
            result = contract.pending_report(root)
            self.assertFalse(result['candidateCompared'])
            self.assertFalse(result['candidateImplemented'])
            self.assertFalse(result['sourceReferenceVerified'])
            self.assertEqual(result['sourceCases'], 102)
            self.assertTrue((root / 'output/open-scatter-domain.json').is_file())
            self.assertFalse((root / 'output/comparison.json').exists())
            (root / 'output/comparison.json').write_text('{}')
            with self.assertRaises(ValueError):
                contract.pending_report(root)

    def test_bounded_capture_rejects_malformed_and_concatenated_streams(self):
        case = next(contract.cases('scatterholdout-origin-x'))
        rect = case['scatterHoldoutProbe']['rect']
        pixels = bytes(rect[2] * rect[3] * 4)
        for compressed in (b'not-a-zlib-stream', zlib.compress(pixels[:-1]),
                           zlib.compress(pixels) + zlib.compress(b'extra'),
                           zlib.compress(pixels + b'extra')):
            item = {'rect': rect, 'encoding': 'zlib-base64-bgra8-row-major',
                    'data': base64.b64encode(compressed).decode()}
            with self.assertRaises(ValueError):
                contract.decode_capture(item, rect)

    def test_authored_origin_and_west_normalization_uses_fixed_coordinates(self):
        # Original authored path starts are known without inspecting paint.
        # Fixed off-center markers make accidental transpose/translation visible.
        for group, point, expected_point, layout in (
                ('scatterholdout-origin-x', (128, 128), (128, 128), 'east'),
                ('scatterholdout-origin-y', (135, 123), (135, 123), 'east'),
                ('scatterholdout-west-xy', (1120, 128), (159, 127), 'north'),
                ('scatterholdout-west-xy', (1113, 133), (166, 122), 'north')):
            case = next(contract.cases(group))
            alpha = bytearray(1280 * 256)
            alpha[point[1] * 1280 + point[0]] = 173
            item = {'rect': case['scatterHoldoutProbe']['rect'],
                    'encoding': 'zlib-base64-alpha8-row-major',
                    'data': base64.b64encode(zlib.compress(alpha)).decode()}
            normalized, observed_layout = canonical_alpha(item, case, 1)
            self.assertEqual(observed_layout, layout)
            self.assertEqual(normalized.count(173), 1)
            self.assertEqual(normalized[expected_point[1] * 1280 + expected_point[0]], 173)

    def test_unrelated_matrix_cannot_earn_holdout_repeat_exemption(self):
        fixture = {'version': '5.3.4.1', 'group': 'test', 'presets': ['arbitrary'],
                   'cases': [{'preset': 'arbitrary', 'scatterHoldoutProbe': {'enabled': True}}]}
        with self.assertRaises(ValueError):
            contract.validate_manifest(fixture)
        with tempfile.TemporaryDirectory() as directory:
            ready, report = acquisition_fixture(Path(directory) / 'ready', 'scatterholdout-origin-x')
            with self.assertRaises(ValueError):
                contract.pending_report(Path(directory) / 'ready')


if __name__ == '__main__':
    unittest.main()
