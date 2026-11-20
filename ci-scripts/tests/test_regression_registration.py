"""Every CTest registration is run by the regression runner or has a stated reason.

ci-scripts/run_regression_tests.py runs the tests registered with the CTest
label "regression" and every test whose name starts with "ui_". A test that is
neither is silently left out of those runs, so each registered name must be
labelled or listed in ci-scripts/regression_exempt.txt with the reason. This
reads the CMake files, so it needs no build.
"""
import unittest
from fnmatch import fnmatchcase
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXEMPT = ROOT / "ci-scripts" / "regression_exempt.txt"
LABEL = "regression"
UI_PREFIX = "ui_"


def cmake_calls(text):
    """Yield (command, arguments) for each command call; arguments are words."""
    i, size = 0, len(text)
    while i < size:
        if text[i] == "#":
            while i < size and text[i] != "\n":
                i += 1
            continue
        start = i
        while i < size and (text[i].isalnum() or text[i] == "_"):
            i += 1
        name_end = i
        while i < size and text[i] in " \t":
            i += 1
        if name_end > start and i < size and text[i] == "(":
            command = text[start:name_end].lower()
            words, word, depth, quote, i = [], "", 1, False, i + 1
            while i < size and depth:
                char = text[i]
                if quote:
                    if char == "\\":
                        word += text[i + 1]
                        i += 1
                    elif char == '"':
                        quote = False
                    else:
                        word += char
                elif char == '"':
                    quote = True
                elif char == "#":
                    while i < size and text[i] != "\n":
                        i += 1
                    continue
                elif char == "(":
                    depth += 1
                    word += char
                elif char == ")":
                    depth -= 1
                    if depth:
                        word += char
                elif char.isspace():
                    if word:
                        words.append(word)
                    word = ""
                else:
                    word += char
                i += 1
            if word:
                words.append(word)
            yield command, words
        elif i == start:
            i += 1


def cmake_files():
    toonz = ROOT / "toonz"
    return sorted(list(toonz.rglob("CMakeLists.txt")) + list(toonz.rglob("*.cmake")))


def registrations():
    """Return (registered test names, names labelled regression, labelled calls)."""
    registered, labelled = {}, set()
    for path in cmake_files():
        for command, words in cmake_calls(path.read_text(encoding="utf-8")):
            if command == "add_test":
                if words[0] != "NAME":
                    raise AssertionError(f"{path}: add_test without NAME: {words[:2]}")
                registered[words[1]] = path
            elif command == "set_tests_properties" and "PROPERTIES" in words:
                split = words.index("PROPERTIES")
                properties = words[split + 1:]
                if "LABELS" in properties:
                    value = properties[properties.index("LABELS") + 1]
                    if LABEL in value.split(";"):
                        labelled.update(words[:split])
    return registered, labelled


def exemptions():
    """Return {pattern: reason} from regression_exempt.txt."""
    result = {}
    for number, line in enumerate(EXEMPT.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        pattern, _, reason = line.strip().partition(" ")
        if not reason.strip():
            raise AssertionError(f"{EXEMPT.name}:{number}: {pattern} has no reason")
        if pattern in result:
            raise AssertionError(f"{EXEMPT.name}:{number}: {pattern} is listed twice")
        result[pattern] = reason.strip()
    return result


def exempt_by(name, patterns):
    return [pattern for pattern in patterns if fnmatchcase(name, pattern)]


class RegressionRegistrationTest(unittest.TestCase):
    def test_every_registered_test_is_run_or_exempt_with_a_reason(self):
        registered, labelled = registrations()
        patterns = exemptions()
        loose = sorted(name for name in registered
                       if name not in labelled and not name.startswith(UI_PREFIX)
                       and not exempt_by(name, patterns))
        self.assertEqual(loose, [], (
            "Registered tests the regression runner does not run. Add LABELS "
            f"{LABEL} to their set_tests_properties, or list them with a reason in "
            f"ci-scripts/{EXEMPT.name}: " + ", ".join(loose)))

    def test_labels_name_registered_tests(self):
        registered, labelled = registrations()
        self.assertEqual(sorted(labelled - set(registered)), [])
        self.assertTrue(labelled, "no test is labelled for the regression runner")

    def test_exemptions_are_current_and_do_not_hide_run_tests(self):
        registered, labelled = registrations()
        for pattern in exemptions():
            matches = [name for name in registered if fnmatchcase(name, pattern)]
            self.assertTrue(matches, f"{pattern} matches no registered test: remove it")
            run = [name for name in matches
                   if name in labelled or name.startswith(UI_PREFIX)]
            self.assertEqual(run, [], f"{pattern} is exempt but the runner runs {run}")


if __name__ == "__main__":
    unittest.main()
