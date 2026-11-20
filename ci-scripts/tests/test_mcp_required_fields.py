"""Prove the required-field proof does not depend on the order the sources are read in."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_schema_required as required

HOME = """
QJsonValue handler(Context& context) {
  QJsonObject result{{"own", 1}};
  result.insert("shared", helper());
  return result;
}
"""
WRITES_KEY = 'QJsonObject helper() { return {{"key", 1}, {"both", 2}}; }\n'
LACKS_KEY = 'QJsonObject helper() { return {{"other", 1}, {"both", 2}}; }\n'
OVERRIDE = 'QJsonObject helper() const override { return {{"key", 1}}; }\n'


def proven(*texts: str) -> set[str]:
    required._CACHE.clear()
    files = [(text, required.masked(text)) for text in texts]
    return required.unconditional(files, "handler", 0)


class DefinitionLookup(unittest.TestCase):
    def test_a_name_defined_in_several_files_proves_only_shared_keys(self) -> None:
        self.assertEqual(proven(HOME, WRITES_KEY, LACKS_KEY), {"own", "shared", "both"})

    def test_the_proof_is_the_same_in_every_file_order(self) -> None:
        self.assertEqual(proven(HOME, WRITES_KEY, LACKS_KEY), proven(HOME, LACKS_KEY, WRITES_KEY))

    def test_a_definition_in_the_home_file_is_the_one_called(self) -> None:
        self.assertEqual(proven(HOME + WRITES_KEY, LACKS_KEY), {"own", "shared", "key", "both"})

    def test_a_definition_marked_override_is_found(self) -> None:
        self.assertEqual(proven(HOME + OVERRIDE, LACKS_KEY), {"own", "shared", "key"})


TABLE = 'Handlers table{{"tool.read", tableHandler}};\n' + (
    'QJsonValue tableHandler(Context& c) { return QJsonObject{{"a", 1}, {"b", 2}}; }\n'
)
DISPATCH = 'if (method == "tool.read") return dispatched(context, params);\n' + (
    'QJsonValue dispatched(Context& c, const QJsonObject& p) { return QJsonObject{{"a", 1}}; }\n'
)


class Registrations(unittest.TestCase):
    def found(self, *texts: str) -> set[str]:
        required._CACHE.clear()
        sources = {Path(f"automation_{index}.cpp"): text for index, text in enumerate(texts)}
        return required.proven(sources)["tool.read"][1]

    def test_a_tool_registered_twice_proves_only_what_both_write(self) -> None:
        self.assertEqual(self.found(TABLE, DISPATCH), {"a"})
        self.assertEqual(self.found(DISPATCH, TABLE), {"a"})


if __name__ == "__main__":
    unittest.main()
