"""CI-only bounds and identity contracts; mocks do not establish Lab fidelity."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("kernel_convergence_probe",
    Path(__file__).parent / "gzbrush_reference" / "kernel_convergence_probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def fixtures():
    cases = []
    for count in (25, 75):
        for complement in (False, True):
            cases.append({"preset": "Original mask " + str(count) + " " + str(complement),
                          "wetPattern": "opaque-black-white-points", "diameter": 64,
                          "overrides": {"SmudgeRadiusValue": "100"},
                          "wetKernelOffset": count, "wetKernelComplement": complement,
                          "wetKernelPoints": [[i % 10 - 5, i // 10 - 5] for i in range(count)]})
    return {"version": "5.3.4.1", "group": "wetkernelcount", "cases": cases}


class FakeApp:
    def colorDepths(self, model):
        return ["U8", "U16", "F32"]

    def profiles(self, model, depth):
        return [probe.PROFILE] if model == "RGBA" else ["Original mock Lab"]

    def version(self):
        return "5.3.4.1 (mock contract only)"


class FakeColor:
    def __init__(self, model, depth, profile):
        self.model, self.depth, self.profile = model, depth, profile or "Original mock Lab"
        self.values = [0., 0., 0., 1.]

    def colorModel(self): return self.model
    def colorDepth(self): return self.depth
    def colorProfile(self): return self.profile
    def setComponents(self, values): self.values = list(values)
    def components(self): return list(self.values)
    def componentsOrdered(self):
        return self.values[:3][::-1] + self.values[3:] if self.model == "RGBA" else list(self.values)

    def setColorSpace(self, model, depth, profile):
        self.model, self.depth, self.profile = model, depth, profile
        return True

    def toXML(self):
        if self.model == "RGBA": return "<Color><RGB/></Color>"
        # Deliberately simple fake conversion; contract checks use observed XML,
        # not an implementation of a claimed source color transform.
        return '<Color><Lab L="%s" a="%s" b="%s"/></Color>' % tuple(v * 100 for v in self.values[:3])


class ConvergenceContract(unittest.TestCase):
    def test_bounded_original_masks_and_hypothesis_tracks(self):
        plans = probe.plans(fixtures())
        self.assertEqual(len(plans), 544)
        self.assertEqual({p["pair"] for p in plans},
                         {"gray", "red-green", "blue-yellow", "near-lightness"})
        self.assertEqual({p["baseCandidate"] for p in plans}, {99, 100})
        self.assertEqual({p["pointCount"] for p in plans}, {25, 75})
        self.assertEqual({p["complement"] for p in plans}, {False, True})
        self.assertEqual(max(p["countCandidate"] for p in plans), 356)
        self.assertTrue(all(all(0 <= v <= 1 for v in p["inputRGB"]) for p in plans))
        for mutation in ("missing", "duplicate", "coordinates", "baseline"):
            bad = copy.deepcopy(fixtures())
            if mutation == "missing": bad["cases"].pop()
            if mutation == "duplicate": bad["cases"][1] = copy.deepcopy(bad["cases"][0])
            if mutation == "coordinates": bad["cases"][0]["wetKernelPoints"][0] = [99, 0]
            if mutation == "baseline": bad["version"] = "other"
            with self.assertRaises(ValueError): probe.plans(bad)

    def test_observed_conversion_identity_and_batch_linkage(self):
        report = probe.capture(FakeApp(), FakeColor, fixtures())
        self.assertEqual(len(report["records"]), 2176)
        first = report["records"][0]
        self.assertIsNone(first["previousCountCandidate"])
        self.assertIsNone(first["deltaLab76FromPreviousBatch"])
        for mutation in ("missing", "count", "source", "lab", "batch", "runtime"):
            bad = copy.deepcopy(report)
            if mutation == "missing": bad["records"].pop()
            if mutation == "count": bad["records"][0]["countCandidate"] += 1
            if mutation == "source": bad["records"][0]["source"]["depth"] = "F32"
            if mutation == "lab": bad["records"][0]["physicalLab"][0] = float("nan")
            if mutation == "batch": bad["records"][2]["previousCountCandidate"] = 0
            if mutation == "runtime": bad["runtimeVersion"] = ""
            with self.assertRaises(ValueError): probe.validate(bad, fixtures())

    def test_unavailable_lab_depth_remains_explicit(self):
        class IntegerOnly(FakeApp):
            def colorDepths(self, model):
                return ["U8", "U16"]
        report = probe.capture(IntegerOnly(), FakeColor, fixtures())
        self.assertEqual(len(report["records"]), 1088)
        self.assertEqual(report["labProfiles"]["F32"], {"supported": False})

    def test_physical_channels_require_valid_public_lab_xml(self):
        self.assertEqual(probe.physical_lab('<Color><Lab L="50" a="-12" b="3"/></Color>'), [50, -12, 3])
        for xml in ('<Color><RGB/></Color>', '<Color><Lab L="nan" a="0" b="0"/></Color>',
                    '<!DOCTYPE Color><Color><Lab L="0" a="0" b="0"/></Color>'):
            with self.assertRaises(ValueError): probe.physical_lab(xml)


if __name__ == "__main__":
    unittest.main()
