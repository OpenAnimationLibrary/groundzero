"""Original paced timer acquisitions from pinned serialization metadata only."""
import json
from sensor_fixtures import trace


def prepare(root, resources, preset_writer, group):
    plans = []
    if group == 'airbrushtimed':
        for rate in (5, 20):
            for ignore in (False, True):
                for moving in (False, True):
                    for duration in (250000, 1000000):
                        for enabled in (False, True):
                            plans.append((duration, moving, {
                                'PaintOpSettings/isAirbrushing': str(enabled).lower(),
                                'PaintOpSettings/rate': str(rate),
                                'PaintOpSettings/ignoreSpacing': str(ignore).lower()}, 'airbrush'))
        # Off-boundary independent repeats distinguish timer phase/jitter from
        # deterministic deposit equations. Existing cases keep their identities.
        for rate in (5, 20):
            for duration in (375000, 1125000):
                for repeat in range(3):
                    for enabled in (False, True):
                        plans.append((duration, False, {
                            'PaintOpSettings/isAirbrushing': str(enabled).lower(),
                            'PaintOpSettings/rate': str(rate),
                            'PaintOpSettings/ignoreSpacing': 'false'}, 'airbrush-off-boundary-' + str(repeat)))
    elif group == 'timedsensors':
        for sensor in ('time', 'speed'):
            for duration in (250000, 1000000):
                for periodic in ((False, True) if sensor == 'time' else (False,)):
                    for enabled in (False, True):
                        curve = '0,0.25;1,1;'
                        attrs = (' duration="1000" periodic="' + str(int(periodic)) + '"') if sensor == 'time' else ''
                        plans.append((duration, True, {
                            'PressureSize': str(enabled).lower(), 'SizeUseCurve': 'true',
                            'SizeUseSameCurve': 'true', 'SizeValue': '1', 'SizecurveMode': '0',
                            'SizecommonCurve': curve,
                            'SizeSensor': '<params id="' + sensor + '"' + attrs + '><curve>' + curve + '</curve></params>'}, sensor))
        for duration in (1250000, 2250000):
            for periodic in (False, True):
                for enabled in (False, True):
                    curve = '0,0.25;1,1;'
                    plans.append((duration, True, {
                        'PressureSize': str(enabled).lower(), 'SizeUseCurve': 'true',
                        'SizeUseSameCurve': 'true', 'SizeValue': '1', 'SizecurveMode': '0',
                        'SizecommonCurve': curve,
                        'SizeSensor': '<params id="time" duration="1000" periodic="' + str(int(periodic)) + '"><curve>' + curve + '</curve></params>'}, 'time-beyond-period'))
        for count in (17, 65):
            for duration in (250000, 1000000):
                for enabled in (False, True):
                    curve = '0,0.25;1,1;'
                    plans.append((duration, True, {
                        'PressureSize': str(enabled).lower(), 'SizeUseCurve': 'true',
                        'SizeUseSameCurve': 'true', 'SizeValue': '1', 'SizecurveMode': '0',
                        'SizecommonCurve': curve,
                        'SizeSensor': '<params id="speed"><curve>' + curve + '</curve></params>'}, 'speed-cadence-' + str(count)))
        # Causal clock controls: identical real pacing with independently
        # scaled device timestamps and shifted device epoch. No inferred clock.
        for duration in (1250000, 2250000):
            for periodic in (False, True):
                for scale, offset in ((1, 0), (2, 0), (2, 1000000)):
                    curve = '0,0.25;1,1;'
                    plans.append((duration, True, {
                        'PressureSize': 'true', 'SizeUseCurve': 'true',
                        'SizeUseSameCurve': 'true', 'SizeValue': '1', 'SizecurveMode': '0',
                        'SizecommonCurve': curve,
                        'SizeSensor': '<params id="time" duration="1000" periodic="' + str(int(periodic)) + '"><curve>' + curve + '</curve></params>'},
                        'time-clock-' + str(scale) + '-' + str(offset)))
    else:
        raise RuntimeError('Unknown paced acquisition group')
    names, cases = [], []
    for duration, moving, options, target in plans:
        name = 'GZ Original dry ' + group + ' ' + str(len(cases)).zfill(3)
        events = trace('east') if moving else trace('east')[:1]
        if target.startswith('speed-cadence-'):
            count = int(target.rsplit('-', 1)[1])
            events = [{**events[0], 'x': 896 + 256 * i // (count - 1)} for i in range(count)]
        for i, event in enumerate(events):
            event['timeUs'] = round(i * duration / (len(events) - 1)) if moving else 0
        overrides = {'PaintOpAction': '1', 'OpacityValue': '0.1', 'FlowValue': '1',
                     'PressureFlow': 'false', 'FlowUseCurve': 'false',
                     'PaintOpSettings/isAirbrushing': 'false', 'PaintOpSettings/rate': '20',
                     'PaintOpSettings/ignoreSpacing': 'false',
                     'PaintOpSettings/updateSpacingBetweenDabs': 'false', **options}
        (resources / 'paintoppresets' / (name + '.kpp')).write_bytes(
            preset_writer(name, 'dry', False, overrides, 16, 1, 0))
        names.append(name)
        cases.append({'preset': name, 'variant': 'paced acquisition', 'diameter': 16, 'spacing': 1,
                      'pressure': [1, 1], 'opacity': .1, 'flow': 1, 'start': [events[0]['x'], events[0]['y']],
                      'end': [events[-1]['x'], events[-1]['y']], 'background': 'transparent',
                      'overrides': overrides, 'expectUnchanged': False, 'delivery': 'Qt tablet events',
                      'canvasCenter': [1024, 1024], 'events': events,
                      'timedProbe': {'durationUs': duration, 'moving': moving, 'target': target,
                                     'rect': [864, 992, 320, 64],
                                     'maxLatenessUs': 20000 if duration in (375000, 1125000, 1250000, 2250000) or target.startswith('speed-cadence-') else 250000,
                                     'qualification': 'paced source acquisition; no physical tablet or source parity claim'}})
        if target.startswith('time-clock-'):
            scale, offset = map(int, target[len('time-clock-'):].split('-'))
            cases[-1]['timedProbe'].update(deviceTimestampScale=scale,
                                           deviceTimestampOffsetMs=offset)
    (root / 'output' / 'fixtures.json').write_text(json.dumps({
        'version': '5.3.4.1', 'group': group, 'presets': names, 'cases': cases,
        'license': 'CC0-1.0', 'width': 2048, 'height': 2048,
        'input': 'Qt5 tablet events paced by real monotonic elapsed time; timers remain serviced'}, indent=2))
