"""CI-only fixed diagnostic identity and public-node lifecycle checks."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock
from mask_repeat_contract import GROUPS, INDICES, contract, fresh_node


class MaskRepeatContractTest(unittest.TestCase):
    def test_fixed_matrix_preserves_baseline_inputs(self):
        source = Path(__file__).parent
        with tempfile.TemporaryDirectory() as directory:
            results = {}
            for group in ("maskwash", *GROUPS):
                root = Path(directory) / group
                subprocess.run([sys.executable, str(source / "prepare.py")],
                               env=dict(os.environ, BRUSH_REFERENCE_ROOT=str(root), BRUSH_REFERENCE_GROUP=group),
                               check=True, timeout=30, capture_output=True)
                results[group] = json.loads((root / "output/fixtures.json").read_text())
                configured = (root / "config/kritarc").read_text()
                self.assertEqual(configured.startswith("maxNumberOfThreads=1\n"),
                                 group.startswith("maskrepeat-single-"))
            baseline = results.pop("maskwash")
            self.assertEqual(len(baseline["cases"]), 156)
            self.assertNotIn("repeatDiagnostic", baseline)
            for group, diagnostic in results.items():
                self.assertEqual(diagnostic["repeatDiagnostic"], contract(group))
                self.assertEqual(diagnostic["repeatDiagnostic"]["repetitions"], 8)
                self.assertFalse(diagnostic["repeatDiagnostic"]["sourceParityQualified"])
                self.assertEqual(diagnostic["presets"], [baseline["presets"][i] for i in INDICES])
                for row, index in zip(diagnostic["cases"], INDICES):
                    self.assertEqual(row.pop("repeatDiagnostic"), contract(group))
                    self.assertEqual(row, baseline["cases"][index])
                for name in diagnostic["presets"]:
                    path = Path("data/krita/paintoppresets") / (name + ".kpp")
                    self.assertEqual((Path(directory) / group / path).read_bytes(),
                                     (Path(directory) / "maskwash" / path).read_bytes())
        self.assertIsNone(contract("maskwash"))
        self.assertIsNone(contract("maskrepeat-unknown"))

    def test_new_node_is_active_before_old_node_removal(self):
        events = []
        replacement = object()
        document, previous, root = Mock(), Mock(), Mock()
        document.waitForDone.side_effect = lambda: events.append("wait")
        document.createNode.side_effect = lambda *args: events.append("create") or replacement
        document.rootNode.return_value = root
        root.addChildNode.side_effect = lambda *args: events.append("add") or True
        document.setActiveNode.side_effect = lambda node: events.append("activate")
        document.activeNode.return_value = replacement
        previous.remove.side_effect = lambda: events.append("remove") or True
        self.assertIs(fresh_node(document, previous), replacement)
        self.assertEqual(events, ["wait", "create", "add", "activate", "remove", "wait"])

    def test_failed_node_creation_never_reuses_old_target(self):
        document, previous = Mock(), Mock()
        document.createNode.return_value = None
        with self.assertRaisesRegex(RuntimeError, "Cannot create"):
            fresh_node(document, previous)
        previous.remove.assert_not_called()
        document.setActiveNode.assert_not_called()


if __name__ == "__main__":
    unittest.main()
