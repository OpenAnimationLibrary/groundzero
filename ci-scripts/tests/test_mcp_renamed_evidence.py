"""Prove the evidence renaming keeps every entry and leaves no name the upload refuses."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_evidence_names as evidence


class RenameRefused(unittest.TestCase):
    def setUp(self) -> None:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)

    def names(self) -> list[str]:
        return sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*"))

    def test_refused_names_are_encoded_at_every_depth(self) -> None:
        nested = self.root / "case-1" / "line\nbreak" / "a:b"
        nested.mkdir(parents=True)
        (nested / 'say "hi"?.png').write_bytes(b"png")
        evidence.rename_refused(self.root)
        self.assertEqual(
            self.names(),
            [
                "case-1",
                "case-1/line%0Abreak",
                "case-1/line%0Abreak/a%3Ab",
                "case-1/line%0Abreak/a%3Ab/say %22hi%22%3F.png",
            ],
        )
        self.assertEqual((self.root / self.names()[-1]).read_bytes(), b"png")

    def test_plain_names_are_left_alone(self) -> None:
        (self.root / "worker.stderr.log").write_text("log")
        self.assertEqual(evidence.rename_refused(self.root), [])
        self.assertEqual(self.names(), ["worker.stderr.log"])

    def test_a_taken_name_stops_the_renaming(self) -> None:
        (self.root / "x\ny").write_text("first")
        (self.root / "x%0Ay").mkdir()
        with self.assertRaises(FileExistsError):
            evidence.rename_refused(self.root)


if __name__ == "__main__":
    unittest.main()
