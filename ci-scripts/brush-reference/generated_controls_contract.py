"""Pure original-fixture and source-readback contract for generated controls."""
import json
import math
from pathlib import Path
import sys
from xml.etree.ElementTree import ParseError, TreeBuilder, XMLParser

COUNTS = {"generated-transfer": 216, "generated-geometry": 52, "generated-sensors": 84}
PENDING = frozenset(("generated-geometry", "generated-sensors"))


def status(group):
    if group not in COUNTS:
        raise ValueError("Unknown generated control contract group")
    return "open-domain-acquisition-only" if group in PENDING else "shared-pixel-transfer-candidate"


def validate_manifest(fixtures):
    group = fixtures.get("group")
    implementation = status(group)
    names, cases = fixtures.get("presets"), fixtures.get("cases")
    if (fixtures.get("version") != "5.3.4.1" or not isinstance(names, list) or
            not isinstance(cases, list) or len(names) != COUNTS[group] or len(cases) != len(names)):
        raise ValueError("Incomplete generated control acquisition matrix")
    for index, (name, case) in enumerate(zip(names, cases)):
        expected = "GZ Original " + group + " " + str(index).zfill(3)
        if not isinstance(case, dict):
            raise ValueError("Generated control case must be an object")
        declaration = case.get("generatedControlProbe", {})
        if (type(declaration) is not dict or set(declaration) != {
                "objective", "candidateExpected", "sourceParityQualified", "scope"}):
            raise ValueError("Unknown or incomplete generated control declaration")
        candidate = group not in PENDING
        if (name != expected or case.get("preset") != expected or
                declaration.get("candidateExpected") is not candidate or
                declaration.get("sourceParityQualified") is not False or
                declaration.get("scope") != ("shared Pixel transfer holdout" if candidate else "open generated domain obligation") or
                not isinstance(declaration.get("objective"), str) or
                not 1 <= len(declaration["objective"]) <= 128 or "sourceAdmission" in case):
            raise ValueError("Forged generated control identity or implementation declaration")
    return implementation


def closed_xml(value, context="source settings XML", limit=65536, allow_cdata=False):
    """Parse bounded settings XML with a closed lexical/namespace contract.

    XML declarations are document headers, not PI nodes. Actual Preset param
    CDATA is allowed only by the retained-preset caller; settings fragments keep
    the pre-existing plain-text curve representation.
    """
    if type(value) not in (str, bytes) or not 0 < len(value) <= limit:
        raise ValueError(context + ": missing or oversized XML")
    marker = b"<![CDATA[" if type(value) is bytes else "<![CDATA["
    if not allow_cdata and marker in value:
        raise ValueError(context + ": CDATA is not an authored settings fragment")

    class Builder(TreeBuilder):
        def __init__(self):
            super().__init__()
            self.depth = self.nodes = 0

        def start(self, tag, attributes):
            self.depth += 1
            self.nodes += 1
            if (self.depth > 32 or self.nodes > 4096 or len(attributes) > 64 or
                    "}" in tag or any("}" in key for key in attributes)):
                raise ValueError(context + ": settings XML structure or qualified name changed")
            return super().start(tag, attributes)

        def end(self, tag):
            self.depth -= 1
            return super().end(tag)

        def start_ns(self, prefix, uri):
            raise ValueError(context + ": namespace declarations are not authored settings")

        def doctype(self, name, pubid, system):
            raise ValueError(context + ": DTD/entities are not authored settings")

        def pi(self, target, text):
            raise ValueError(context + ": processing instructions are not authored settings")

        def comment(self, text):
            raise ValueError(context + ": comments are not authored settings")

    try:
        parser = XMLParser(target=Builder())
        parser.feed(value)
        root = parser.close()
    except (ParseError, TypeError, ValueError) as error:
        raise ValueError(context + ": " + str(error)) from error
    if any((element.tail or "").strip() for element in root.iter()):
        raise ValueError(context + ": non-whitespace XML tail changed")
    return root


def _same_xml(observed, expected):
    if "<!" in observed or "<!" in expected:
        return False
    def tree(element):
        return element.tag, element.attrib, (element.text or "").strip(), [tree(child) for child in element]
    try:
        return tree(closed_xml(observed, "Malformed generated Sensor XML")) == tree(closed_xml(expected, "Malformed generated Sensor XML"))
    except (ParseError, TypeError) as error:
        raise ValueError("Malformed generated Sensor XML") from error


def _same_brush(observed, expected):
    if not isinstance(observed, str) or not isinstance(expected, str) or len(observed) > 65536:
        return False
    if "<!" in observed or "<!" in expected:
        return False
    numeric = {"spacing", "angle", "randomness", "density", "diameter", "ratio",
               "hfade", "vfade", "spikes", "antialiasEdges"}
    def equivalent(first, second):
        if (first.tag != second.tag or first.attrib.keys() != second.attrib.keys() or
                (first.text or "").strip() != (second.text or "").strip() or len(first) != len(second)):
            return False
        for key, value in first.attrib.items():
            target = second.attrib[key]
            if key in numeric:
                a, b = float(value), float(target)
                if not math.isfinite(a) or not math.isfinite(b) or abs(a - b) > 1e-7:
                    return False
            elif key == "softness_curve":
                def points(text):
                    rows = text.split(";")
                    if rows and rows[-1] == "":
                        rows.pop()
                    if not 2 <= len(rows) <= 1024:
                        raise ValueError("Invalid generated source curve cardinality")
                    values = [tuple(map(float, row.split(","))) for row in rows]
                    if any(len(row) != 2 or not all(math.isfinite(v) for v in row) for row in values):
                        raise ValueError("Invalid generated source curve point")
                    return values
                if points(value) != points(target):
                    return False
            elif value != target:
                return False
        return all(equivalent(a, b) for a, b in zip(first, second))
    try:
        return equivalent(closed_xml(observed, "Malformed generated brush XML"), closed_xml(expected, "Malformed generated brush XML"))
    except (ParseError, TypeError) as error:
        raise ValueError("Malformed generated brush XML") from error


def strict_equal(first, second):
    """JSON equality without treating booleans as numeric versions/flags."""
    if type(first) is not type(second):
        return False
    if isinstance(first, dict):
        return first.keys() == second.keys() and all(strict_equal(first[key], second[key]) for key in first)
    if isinstance(first, list):
        return len(first) == len(second) and all(strict_equal(a, b) for a, b in zip(first, second))
    return first == second


def _validate_delivery(case, delivery):
    if delivery.get("mode") != case["delivery"]:
        raise ValueError("Generated control delivery API changed")
    if case["delivery"] == "Node.paintLine":
        points = [case["start"], case["end"]]
        if any(type(value) not in (int, float) or not math.isfinite(value) or value != int(value)
               for point in points for value in point):
            raise ValueError("Generated Node geometry is not integral")
        expected = [{"type": "line-start", "position": list(map(int, points[0])), "pressure": case["pressure"][0]},
                    {"type": "line-end", "position": list(map(int, points[1])), "pressure": case["pressure"][1]}]
        if not strict_equal(delivery.get("events"), expected):
            raise ValueError("Generated control Node endpoint/pressure call changed")
        return
    if case["delivery"] != "Qt tablet events":
        raise ValueError("Unknown generated control delivery API")
    contacts = [event for event in delivery.get("events", [])
                if type(event.get("type")) is int and event["type"] in (87, 92) and
                type(event.get("pressure")) in (int, float) and event["pressure"] > 0]
    authored = case.get("events", [])
    if not authored or len(contacts) != len(authored):
        raise ValueError("Generated control contact count differs")
    for wanted, actual in zip(authored, contacts):
        if len(actual.get("image", [])) != 2 or len(actual.get("tilt", [])) != 2:
            raise ValueError("Missing generated control contact coordinates")
        values = (*actual["image"], actual["pressure"], *actual["tilt"], actual.get("rotation"))
        expected = (wanted["x"], wanted["y"], wanted["pressure"], wanted["tiltX"], wanted["tiltY"], wanted["rotation"])
        if any(type(observed) not in (int, float) or not math.isfinite(observed) or
               abs(observed - target) > 1e-7 for observed, target in zip(values, expected)):
            raise ValueError("Generated delivered contact differs from authored values")
        clock = actual.get("timeMs")
        if type(clock) not in (int, float) or not math.isfinite(clock):
            raise ValueError("Invalid generated control device timestamp")
        if "timedProbe" not in case:
            delta = clock - contacts[0]["timeMs"]
            planned = (wanted["timeUs"] - authored[0]["timeUs"]) / 1000
            if abs(delta - planned) > .500001:
                raise ValueError("Generated synthetic device clock differs")


def validate_evidence(group, case, fields, delivery, evidence):
    expected = capture(group, case, fields, delivery)
    if not strict_equal(evidence, expected):
        raise ValueError("Generated control evidence differs from actual effective XML or delivery")
    return expected


def capture(group, case, fields, delivery):
    implementation = status(group)
    controls = {}
    if type(case.get("overrides")) is not dict or type(fields) is not dict or type(delivery) is not dict:
        raise ValueError("Generated controls, effective fields and delivery must be objects")
    if not _same_brush(fields.get("brush_definition"), case["overrides"].get("brush_definition")):
        raise ValueError("Actual effective generated brush differs from authored definition")
    for key, expected in case["overrides"].items():
        if key == "brush_definition":
            continue  # Bound above to the actual retained effective preset XML.
        actual = fields.get(key)
        if not isinstance(actual, str) or len(actual) > 65536:
            raise ValueError("Missing or oversized effective generated control: " + key)
        if key.endswith("Value") or key == "PaintOpAction":
            first, second = float(actual), float(expected)
            same = math.isfinite(first) and math.isfinite(second) and abs(first - second) <= 1e-7
        elif key.endswith("Sensor"):
            same = _same_xml(actual, expected)
        else:
            same = actual == expected
        if not same:
            raise ValueError("Effective generated control differs: " + key)
        controls[key] = actual
    _validate_delivery(case, delivery)
    return {"version": 1, "implementationStatus": implementation,
            "candidateExpected": group not in PENDING, "effectiveControls": controls,
            "deliveryInterface": delivery["mode"], "sourceReferenceVerified": False,
            "sourceSemanticallySupported": False, "sourceParityQualified": False}


def pending_report(root):
    root = Path(root)
    fixtures = json.loads((root / "output/fixtures.json").read_text())
    implementation = validate_manifest(fixtures)
    if fixtures["group"] not in PENDING:
        raise ValueError("An implemented candidate workload cannot become acquisition-only")
    oracle = json.loads((root / "output/oracle-status.json").read_text())
    records = oracle.get("records", [])
    if (oracle.get("status") != "complete" or oracle.get("version") != "5.3.4.1" or
            [row.get("preset") for row in records] != fixtures["presets"]):
        raise ValueError("Pending generated source acquisition is incomplete")
    for row in records:
        evidence = row.get("generatedControlEvidence", {})
        if (type(evidence) is not dict or type(evidence.get("version")) is not int or
                evidence["version"] != 1 or evidence.get("implementationStatus") != implementation or
                evidence.get("candidateExpected") is not False or
                any(evidence.get(key) is not False for key in (
                    "sourceReferenceVerified", "sourceSemanticallySupported", "sourceParityQualified"))):
            raise ValueError("Pending generated observations gained candidate eligibility")
    report = {"status": implementation, "group": fixtures["group"], "sourceCases": len(records),
              "candidateImplemented": False, "candidateCompared": False,
              "sourceReferenceVerified": False, "sourceSemanticallySupported": False,
              "remainingWork": sorted({case["generatedControlProbe"]["objective"] for case in fixtures["cases"]})}
    (root / "output/open-generated-domain.json").write_text(json.dumps(report, indent=2))
    print("::notice title=Open generated domain::" + json.dumps(report, separators=(",", ":")))


if __name__ == "__main__":
    pending_report(sys.argv[1])
