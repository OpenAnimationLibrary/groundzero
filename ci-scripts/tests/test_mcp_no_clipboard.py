"""Prove that no worker source reaches the system clipboard, which column.duplicate promises."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

SOURCES = Path(__file__).resolve().parents[2] / "toonz" / "sources" / "toonz"
CLIPBOARD_USE = re.compile(
    r"QClipboard|QApplication::clipboard|QGuiApplication::clipboard|setMimeData|QMimeData|"
    r"->clipboard\(|\bclipboard\(\)"
)


def code_lines(path: Path) -> list[tuple[int, str]]:
    """The lines of a source file that are not comments and hold no string literal text."""
    lines = []
    for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        stripped = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line).split("//")[0]
        if stripped.strip():
            lines.append((number, stripped))
    return lines


class ClipboardTest(unittest.TestCase):
    def test_the_worker_sources_never_use_the_system_clipboard(self) -> None:
        paths = sorted(SOURCES.glob("automation*.cpp")) + sorted(SOURCES.glob("automation*.h"))
        self.assertGreater(len(paths), 40)
        found = [
            f"{path.name}:{number}"
            for path in paths
            for number, line in code_lines(path)
            if CLIPBOARD_USE.search(line)
        ]
        self.assertEqual(found, [])

    def test_the_check_sees_a_use_and_ignores_text(self) -> None:
        self.assertTrue(CLIPBOARD_USE.search("QApplication::clipboard()->setText(x);"))
        self.assertTrue(CLIPBOARD_USE.search("auto* data = new QMimeData;"))
        directory = Path(__file__).resolve().parent
        sample = directory / "_clipboard_sample.tmp"
        sample.write_text('f("The clipboard is not used.");\n// QClipboard\nint x = 1;\n')
        self.addCleanup(sample.unlink)
        self.assertEqual(code_lines(sample), [(1, 'f("");'), (3, "int x = 1;")])


if __name__ == "__main__":
    unittest.main()
