"""Pure complete-input/readback contract for focused original scatter holdouts.

The three measured static configurations have a candidate; all scaling and
pressure configurations retain source evidence only and never gain admission.
"""
import base64
import copy
import hashlib
import json
import math
from pathlib import Path
import sys
import zlib
from xml.etree.ElementTree import Element, SubElement, tostring, fromstring, ParseError
try:
    from .generated_controls_contract import strict_equal, _same_xml, _same_brush, _validate_delivery, closed_xml
except ImportError:
    from generated_controls_contract import strict_equal, _same_xml, _same_brush, _validate_delivery, closed_xml

SAMPLE_POLICY = "100 independent enabled strokes; two independently repeated disabled controls; randomized execution bound to canonical identities"


def _brush(diameter, spacing):
    root = Element("Brush", type="auto_brush", BrushVersion="2", spacing=str(spacing),
                   angle="0", randomness="0", density="1", useAutoSpacing="0", autoSpacingCoeff="1")
    SubElement(root, "MaskGenerator", id="default", type="circle", diameter=str(diameter),
               ratio="1", hfade="1", vfade="1", antialiasEdges="0", spikes="2")
    return tostring(root, encoding="unicode")


def _common():
    return {"PressureSize": "false", "PressureRatio": "false", "PressureRotation": "false",
            "PressureSpacing": "false", "PressureScatter": "false", "PressureMirror": "false",
            "PressureOpacity": "false", "PressureFlow": "false", "Pressureh": "false",
            "Pressures": "false", "Pressurev": "false", "OpacityUseCurve": "false",
            "FlowUseCurve": "false", "Texture/Pattern/Enabled": "false", "PaintOpAction": "1",
            "Spacing/Isotropic": "false"}


def _trace(pressure):
    return [{"x": 896 + i * 8, "y": 1024,
             "pressure": pressure(i / 32) if callable(pressure) else pressure,
             "tiltX": 0, "tiltY": 0, "rotation": 0, "timeUs": round(i * 1000000 / 240)}
            for i in range(33)]


def specifications():
    east = {"diameter": 8, "spacing": 4, "amount": 1, "axes": "xy", "direction": "east",
            "start": [256, 1024], "end": [1249, 1024], "rect": [128, 896, 1280, 256], "nominalSlots": 32}
    def plan(name, **changes):
        return name, {**copy.deepcopy(east), **changes}
    return dict([
        plan("scatterholdout-origin-x", axes="x", start=[768, 256], end=[1761, 256], rect=[640, 128, 1280, 256]),
        plan("scatterholdout-origin-y", axes="y", start=[768, 256], end=[1761, 256], rect=[640, 128, 1280, 256]),
        plan("scatterholdout-west-xy", direction="west", start=[1632, 1024], end=[639, 1024], rect=[512, 896, 1280, 256]),
        plan("scatterholdout-half-xy", amount=.5),
        plan("scatterholdout-double-xy", amount=2, spacing=5, end=[1497, 1024], rect=[128, 896, 1536, 256]),
        plan("scatterholdout-small-xy", diameter=4, end=[753, 1024], rect=[128, 896, 768, 256]),
        plan("scatterholdout-large-xy", diameter=16, start=[32, 1024], end=[2017, 1024], rect=[0, 896, 2048, 256]),
        plan("scatterholdout-pressure-half", pressureTrace="half", start=[896, 1024], end=[1152, 1024],
             rect=[768, 896, 512, 256], nominalSlots=None),
        plan("scatterholdout-pressure-full", pressureTrace="full", start=[896, 1024], end=[1152, 1024],
             rect=[768, 896, 512, 256], nominalSlots=None),
        plan("scatterholdout-pressure-ramp", pressureTrace="ramp", start=[896, 1024], end=[1152, 1024],
             rect=[768, 896, 512, 256], nominalSlots=None),
    ])


GROUPS = tuple(specifications())


def cases(group):
    plans = specifications()
    if group not in plans:
        raise ValueError("Unknown authored scatter holdout")
    plan = plans[group]
    pressure = plan.get("pressureTrace")
    pressure_function = (.5 if pressure == "half" else 1 if pressure == "full" else
                         (lambda t: .25 + .75 * t)) if pressure else None
    for enabled, count in ((True, 100), (False, 2)):
        for replicate in range(count):
            fields = _common()
            cubic = "0,0;1,1;" if pressure else "0,1;1,1;"
            fields.update({"brush_definition": _brush(plan["diameter"], plan["spacing"]),
                           "PressureScatter": str(enabled).lower(), "ScatterValue": str(plan["amount"]),
                           "ScatterUseCurve": str(bool(pressure)).lower(), "ScatterUseSameCurve": "true",
                           "ScattercurveMode": "0", "ScattercommonCurve": cubic,
                           "ScatterSensor": '<params id="pressure"><curve>' + cubic + '</curve></params>',
                           "Scattering/AxisX": str("x" in plan["axes"]).lower(),
                           "Scattering/AxisY": str("y" in plan["axes"]).lower()})
            candidate_domain = (plan["diameter"] == 8 and plan["amount"] == 1 and
                                plan["spacing"] == 4 and not pressure)
            item = {"sourceEngine": "paintbrush", "variant": "baseline", "diameter": plan["diameter"],
                    "spacing": plan["spacing"], "opacity": 1, "flow": 1, "pressure": [1, 1],
                    "start": list(plan["start"]), "end": list(plan["end"]), "background": "transparent",
                    "foregroundRGBA8": [173, 91, 47, 255], "overrides": fields, "expectUnchanged": False,
                    "delivery": "Node.paintLine", "canvasCenter": [1024, 1024],
                    "scatterHoldoutProbe": {"version": 1, "configuration": group, **copy.deepcopy(plan),
                        "enabled": enabled, "replicate": replicate, "replicates": count, "bands": 3,
                        "physicalTabletClaimed": False, "sourceProfileVerified": False,
                        "sourceRngLawIdentified": False, "pixelParityClaimed": False,
                        "candidateScope": "current-static-component" if candidate_domain else "not-admitted-scaling-or-dynamics",
                        "qualification": "Original holdout authored for acquisition; no implementation admission or successful observation inferred"}}
            if pressure:
                events = _trace(pressure_function)
                item.update(delivery="Qt tablet events", events=events,
                            pressure=[events[0]["pressure"], events[-1]["pressure"]],
                            sensorProbe={"target": "Scatter", "sensor": "pressure", "enabled": enabled,
                                "label": "scatter holdout " + pressure, "isotropic": False,
                                "rect": plan["rect"], "verifyForeground": True,
                                "qualification": "Actual delivered QTabletEvent pressure and effective curve required; physical tablet and source normalization unqualified"})
            item["sourcePressureFlag"] = bool(pressure)
            yield item


def validate_authorship(group, authored):
    expected = list(cases(group))
    if authored != expected or len(authored) != 102:
        raise ValueError("Authored scatter holdout differs from its bounded original plan")
    active = [c for c in authored if c["scatterHoldoutProbe"]["enabled"]]
    controls = [c for c in authored if not c["scatterHoldoutProbe"]["enabled"]]
    if ({c["scatterHoldoutProbe"]["replicate"] for c in active} != set(range(100)) or
            {c["scatterHoldoutProbe"]["replicate"] for c in controls} != {0, 1}):
        raise ValueError("Incomplete independent holdout sample/control policy")
    return True


CANDIDATE_GROUPS = frozenset(('scatterholdout-origin-x', 'scatterholdout-origin-y', 'scatterholdout-west-xy'))
PENDING_GROUPS = frozenset(GROUPS) - CANDIDATE_GROUPS


def candidate_expected(group):
    if group not in GROUPS:
        raise ValueError('Unknown focused scatter holdout group')
    return group in CANDIDATE_GROUPS


def status(group):
    return 'measured-static-scatter-candidate' if candidate_expected(group) else 'open-scatter-domain-acquisition-only'


def name(group, index):
    return 'GZ Original dry ' + group + ' ' + str(index).zfill(3)


def validate_manifest(fixtures):
    group = fixtures.get('group')
    implementation = status(group)
    authored = list(cases(group))
    names = [name(group, i) for i in range(102)]
    if (fixtures.get('version') != '5.3.4.1' or fixtures.get('license') != 'CC0-1.0' or
            type(fixtures.get('width')) is not int or fixtures['width'] != 2048 or
            type(fixtures.get('height')) is not int or fixtures['height'] != 2048 or
            not strict_equal(fixtures.get('presets'), names) or
            type(fixtures.get('cases')) is not list or len(fixtures['cases']) != 102 or
            fixtures.get('randomizeExecution') is not True or fixtures.get('developmentAcquisition') is not True or
            fixtures.get('candidateExpected') is not candidate_expected(group) or
            fixtures.get('implementationStatus') != implementation or
            fixtures.get('stochasticSamplePolicy') != SAMPLE_POLICY or
            fixtures.get('sourceProfileVerified') is not False or fixtures.get('pixelParityClaimed') is not False):
        raise ValueError('Incomplete or relabeled focused scatter holdout acquisition')
    for i, expected in enumerate(authored):
        expected['preset'] = names[i]
        if not strict_equal(fixtures['cases'][i], expected):
            raise ValueError('Focused scatter holdout changed original inputs or implementation scope')
    return implementation


def validate_plan(fixtures, planned):
    validate_manifest(fixtures)
    names = fixtures['presets']
    if (type(planned) is not list or len(planned) != 102 or
            any(type(n) is not str for n in planned) or len(set(planned)) != 102 or set(planned) != set(names) or
            [n for n in planned if n in set(names[:100])] == names[:100]):
        raise ValueError('Focused scatter holdout execution is incomplete or not randomized')
    return {n: i for i, n in enumerate(planned)}


def validate_progress(fixtures, planned, actual):
    validate_plan(fixtures, planned)
    if type(actual) is not list or not 0 <= len(actual) <= 102 or actual != planned[:len(actual)]:
        raise ValueError('Focused scatter holdout actual execution differs from validated randomized plan')
    return True


def validate_execution(fixtures, report):
    order = validate_plan(fixtures, report.get('plannedExecutionOrder'))
    if (not strict_equal(report.get('intendedPresetOrder'), fixtures['presets']) or
            report.get('actualExecutionOrder') != report.get('plannedExecutionOrder') or
            type(report.get('records')) is not list or len(report['records']) != 102):
        raise ValueError('Focused scatter holdout actual 102-stroke acquisition is incomplete')
    for i, (case, row) in enumerate(zip(fixtures['cases'], report['records'])):
        if (type(row) is not dict or row.get('preset') != case['preset'] or
                type(row.get('intendedOrdinal')) is not int or row['intendedOrdinal'] != i or
                type(row.get('executionOrdinal')) is not int or row['executionOrdinal'] != order[case['preset']]):
            raise ValueError('Focused scatter holdout canonical record/execution identity changed')
        count = 1 if case['scatterHoldoutProbe']['enabled'] else 2
        repeated = row.get('repeatSha256Pixels')
        canonical = row.get('sha256RepeatPixels')
        if (type(repeated) is not list or len(repeated) != count or repeated[0] != row.get('sha256Pixels') or
                row.get('repeatSemantics') != 'transparent-rgb-zero-v1' or
                row.get('repeatSha256CanonicalPixels') != [canonical] * count or
                type(canonical) is not str or len(canonical) != 64 or
                any(type(value) is not str or len(value) != 64 for value in repeated) or
                type(row.get('repeatImages')) is not list or len(row['repeatImages']) != count - 1 or
                type(row.get('repeatDifferences')) is not list or len(row['repeatDifferences']) != count - 1 or
                any(type(d.get('visibleOrAlphaChangedPixels')) is not int or d['visibleOrAlphaChangedPixels'] != 0 for d in row['repeatDifferences'])):
            raise ValueError('Focused scatter repeat evidence changed its validated independent/control policy')
    return True


def _safe_xml(value, limit=65536, allow_cdata=False):
    try:
        closed_xml(value, "Focused scatter source XML", limit, allow_cdata)
    except ValueError:
        return False
    return True


def _contacts(case, delivery):
    if type(delivery.get('events')) is not list or any(type(event) is not dict for event in delivery['events']):
        raise ValueError('Focused scatter delivered events must be an explicit object list')
    _validate_delivery(case, delivery)
    if delivery.get('physicalTabletClaimed', False) is not False:
        raise ValueError('Synthetic scatter holdout cannot claim a physical tablet')
    if case['delivery'] == 'Node.paintLine':
        return delivery['events']
    events = delivery.get('events', [])
    types = [event.get('type') for event in events]
    if types != [171, 87, 92] + [87] * 32 + [93, 172]:
        raise ValueError('Focused scatter tablet enter/hover/contact/release sequence changed')
    if any(type(events[index].get('pressure')) not in (int, float) or events[index]['pressure'] != 0
           for index in (0, 1, -2, -1)):
        raise ValueError('Focused scatter non-contact event carried pressure')
    for phase in ('stateBeforeProximity', 'stateAfterProximity', 'stateBeforeContact'):
        observed = delivery.get(phase, {})
        if (observed.get('preset') != case['preset'] or observed.get('disablePressure') is not case['sourcePressureFlag'] or
                type(observed.get('size')) not in (int, float) or abs(observed['size'] - case['diameter']) > 1e-5):
            raise ValueError('Focused scatter tablet source state changed before contact')
    return events[2:-2]


def controls(group, case, fields, delivery):
    implementation = status(group)
    if type(fields) is not dict or type(delivery) is not dict:
        raise ValueError('Missing actual focused scatter fields or delivery')
    expected = case['overrides']
    brush_definition = fields.get('brush_definition')
    if (not _safe_xml(brush_definition) or not _same_brush(brush_definition, expected['brush_definition'])):
        raise ValueError('Focused scatter effective brush differs from authored XML')
    required = {k: v for k, v in expected.items() if k != 'brush_definition'}
    required.update(paintop='paintbrush', CompositeOp='normal', EraserMode='false', OpacityValue='1', FlowValue='1',
                    MergedPaint='false', **{'PaintOpSettings/isAirbrushing': 'false',
                      'PaintOpSettings/ignoreSpacing': 'false', 'PaintOpSettings/updateSpacingBetweenDabs': 'false',
                      'KisPrecisionOption/AutoPrecisionEnabled': 'false', 'KisPrecisionOption/precisionLevel': '5'})
    actual = {}
    for key, wanted in required.items():
        value = fields.get(key)
        if type(value) is not str or len(value) > 65536:
            raise ValueError('Missing focused scatter effective field: ' + key)
        if key.endswith('Sensor'):
            same = _safe_xml(value) and _same_xml(value, wanted)
        elif key.endswith('Value') or key in ('PaintOpAction', 'KisPrecisionOption/precisionLevel'):
            try:
                a, b = float(value), float(wanted)
                same = math.isfinite(a) and math.isfinite(b) and abs(a - b) <= 1e-7
            except (ValueError, TypeError):
                same = False
        else:
            same = value == wanted
        if not same:
            raise ValueError('Focused scatter effective control changed: ' + key)
        actual[key] = value
    contacts = _contacts(case, delivery)
    delivery_hash = hashlib.sha256(json.dumps(delivery, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()
    return {'version': 1, 'implementationStatus': implementation, 'candidateExpected': candidate_expected(group),
            'effectiveControls': actual, 'effectiveBrushDefinition': brush_definition,
            'deliveredContactEvents': contacts, 'deliveryInterface': delivery['mode'], 'deliverySha256': delivery_hash,
            'sourceReferenceVerified': False, 'sourceSemanticallySupported': False, 'sourceParityQualified': False,
            'sourceRngLawIdentified': False, 'pixelParityClaimed': False, 'physicalTabletClaimed': False,
            'rect': case['scatterHoldoutProbe']['rect'], 'exteriorAlphaChecked': True}


def capture(group, case, fields, delivery, raw, width):
    evidence = controls(group, case, fields, delivery)
    if type(width) is not int or width != 2048 or not isinstance(raw, (bytes, bytearray)) or len(raw) != 2048 * 2048 * 4:
        raise ValueError('Incomplete focused scatter full source raster')
    left, top, w, h = evidence['rect']
    for y in range(2048):
        row = raw[y * 2048 * 4:(y + 1) * 2048 * 4]
        outside = row[3::4] if y < top or y >= top + h else row[3:left * 4:4] + row[(left + w) * 4 + 3::4]
        if any(outside):
            raise ValueError('Focused scatter source alpha escaped the authored capture')
    pixels = b''.join(raw[(y * 2048 + left) * 4:(y * 2048 + left + w) * 4] for y in range(top, top + h))
    nonzero = sum(a != 0 for a in pixels[3::4])
    if not nonzero:
        raise ValueError('Focused scatter source unexpectedly produced no paint')
    evidence.update(encoding='zlib-base64-bgra8-row-major', data=base64.b64encode(zlib.compress(pixels)).decode(),
                    sha256Pixels=hashlib.sha256(pixels).hexdigest(), nonzeroAlphaPixels=nonzero)
    return evidence


def decode_capture(evidence, rect, channels=4):
    if (channels not in (1, 4) or type(rect) is not list or len(rect) != 4 or
            any(type(v) is not int for v in rect) or min(rect[:2]) < 0 or
            not 0 < rect[2] <= 2048 or not 0 < rect[3] <= 256 or
            rect[0] + rect[2] > 2048 or rect[1] + rect[3] > 2048):
        raise ValueError('Focused scatter crop exceeds authored source raster bounds')
    expected_encoding = 'zlib-base64-bgra8-row-major' if channels == 4 else 'zlib-base64-alpha8-row-major'
    data = evidence.get('data')
    size = rect[2] * rect[3] * channels
    if (evidence.get('rect') != rect or evidence.get('encoding') != expected_encoding or
            type(data) is not str or len(data) > size * 2 + 1024):
        raise ValueError('Focused scatter pixel contract changed')
    try:
        compressed = base64.b64decode(data, validate=True)
        if base64.b64encode(compressed).decode() != data:
            raise ValueError('Noncanonical focused scatter base64')
        stream = zlib.decompressobj()
        pixels = stream.decompress(compressed, size + 1)
    except (ValueError, zlib.error) as error:
        raise ValueError('Malformed focused scatter compressed raster') from error
    if len(pixels) != size or not stream.eof or stream.unused_data or stream.unconsumed_tail:
        raise ValueError('Incomplete or concatenated focused scatter raster')
    return pixels


def validate_evidence(group, case, fields, delivery, evidence):
    expected = controls(group, case, fields, delivery)
    keys = set(expected) | {'encoding', 'data', 'sha256Pixels', 'nonzeroAlphaPixels'}
    if type(evidence) is not dict or set(evidence) != keys or any(not strict_equal(evidence.get(k), v) for k, v in expected.items()):
        raise ValueError('Focused scatter evidence differs from actual fields, delivery or implementation scope')
    pixels = decode_capture(evidence, expected['rect'])
    count = sum(a != 0 for a in pixels[3::4])
    if (hashlib.sha256(pixels).hexdigest() != evidence.get('sha256Pixels') or
            type(evidence.get('nonzeroAlphaPixels')) is not int or evidence['nonzeroAlphaPixels'] != count or not count):
        raise ValueError('Focused scatter retained pixels do not match their complete readback')
    return expected


def validate_effective_preset(group, case, row, data):
    if not strict_equal({key: row.get(key) for key in case}, case):
        raise ValueError('Focused scatter case changed after original preparation')
    if not isinstance(data, bytes) or len(data) > 1024 * 1024 or not _safe_xml(data.decode(), 1024 * 1024, allow_cdata=True):
        raise ValueError('Unsafe focused scatter effective preset XML')
    if hashlib.sha256(data).hexdigest() != row.get('effectivePresetSha256'):
        raise ValueError('Focused scatter effective preset digest changed')
    try:
        root = fromstring(data)
    except (ParseError, TypeError) as error:
        raise ValueError('Malformed focused scatter effective preset') from error
    params = root.findall('param')
    names = [p.get('name') for p in params]
    if (root.tag != 'Preset' or root.get('paintopid') != 'paintbrush' or root.get('name') != case['preset'] or
            len(params) != len(root) or not 0 < len(params) <= 2048 or any(type(n) is not str or not 0 < len(n) <= 256 for n in names) or
            len(set(names)) != len(names) or (root.text or '').strip() or
            any(len(p) or (p.tail or '').strip() for p in params)):
        raise ValueError('Focused scatter actual effective preset identity or parameter structure changed')
    fields = {p.get('name'): p.text or '' for p in params}
    return validate_evidence(group, case, fields, row.get('deliveryReport'), row.get('scatterHoldoutEvidence'))


def pending_report(root):
    root = Path(root)
    if (root / 'output/comparison.json').exists():
        raise ValueError('Pending scatter source-only acquisition contains a native comparison')
    fixtures = json.loads((root / 'output/fixtures.json').read_text())
    implementation = validate_manifest(fixtures)
    if candidate_expected(fixtures['group']):
        raise ValueError('Measured scatter candidate cannot be converted to a source-only acquisition')
    report = json.loads((root / 'output/oracle-status.json').read_text())
    validate_execution(fixtures, report)
    if report.get('status') != 'complete' or report.get('version') != '5.3.4.1':
        raise ValueError('Pending focused scatter original source acquisition is incomplete')
    for case, row in zip(fixtures['cases'], report['records']):
        expected_name = case['preset'].replace(' ', '_') + '.effective.xml'
        if row.get('effectivePresetXml') != expected_name:
            raise ValueError('Pending scatter retained XML does not belong to its original case')
        validate_effective_preset(fixtures['group'], case, row, (root / 'output' / expected_name).read_bytes())
    result = {'status': implementation, 'group': fixtures['group'], 'sourceCases': 102,
              'independentEnabledStrokes': 100, 'independentlyRepeatedDisabledControls': 2,
              'candidateImplemented': False, 'candidateCompared': False, 'developmentAcquisition': True,
              'sourceReferenceVerified': False, 'sourceSemanticallySupported': False, 'sourceParityQualified': False,
              'sourceRngLawIdentified': False, 'pixelParityClaimed': False, 'physicalTabletClaimed': False,
              'remainingWork': fixtures['cases'][0]['scatterHoldoutProbe']['candidateScope']}
    (root / 'output/open-scatter-domain.json').write_text(json.dumps(result, indent=2))
    print('::notice title=Open scatter domain::' + json.dumps(result, separators=(',', ':')))
    return result


if __name__ == '__main__':
    pending_report(sys.argv[1])
