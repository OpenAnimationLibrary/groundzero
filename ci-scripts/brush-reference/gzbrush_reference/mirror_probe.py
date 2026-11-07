"""Additional Mirror-axis checks atop the generic sensor/resource capture."""

def qualify(case, fields, evidence):
    for key, flag in (("HorizontalMirrorEnabled", "horizontal"), ("VerticalMirrorEnabled", "vertical")):
        expected = str(case["mirrorProbe"][flag]).lower()
        if fields.get(key) != expected:
            raise RuntimeError("Effective source mirror axis differs: " + key)
        evidence["effectiveControls"][key] = fields[key]
    evidence["mirrorAxisValuesVerified"] = True
    evidence["mirrorThresholdQualified"] = False
