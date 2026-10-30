"""CI-only signing inventory/order tests; deployment performs real verification."""
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import sign_macos_deployment as signing


class SigningInventory(unittest.TestCase):
    def test_inside_out_and_alias_deduplication(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "Fixture.app"
            framework = app / "Contents/Frameworks/Example.framework"
            binary = framework / "Versions/A/Example"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(bytes.fromhex("cffaedfe") + b"fixture")
            (framework / "Example").symlink_to("Versions/A/Example")
            plugin = app / "Contents/PlugIns/imageformats/original.dylib"
            plugin.parent.mkdir(parents=True)
            plugin.write_bytes(bytes.fromhex("cffaedfe") + b"fixture")
            (plugin.parent / "data.json").write_text("{}")
            targets = signing.signing_targets(app)
            self.assertEqual(set(targets), {framework, binary, plugin})
            self.assertLess(targets.index(binary), targets.index(framework))

    def test_external_alias_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / "Fixture.app"
            frameworks = app / "Contents/Frameworks"
            frameworks.mkdir(parents=True)
            outside = Path(directory) / "outside.dylib"
            outside.write_bytes(bytes.fromhex("cffaedfe"))
            (frameworks / "escape.dylib").symlink_to(outside)
            with self.assertRaises(RuntimeError):
                signing.signing_targets(app)

    def test_only_explicitly_unsigned_code_is_treated_as_unsigned(self):
        with patch.object(signing.subprocess, "run") as command:
            command.return_value.returncode = 1
            command.return_value.stderr = b"code object is not signed at all"
            self.assertFalse(signing.has_signature(Path("Fixture.app")))
            command.return_value.stderr = b"invalid signature format"
            with self.assertRaises(RuntimeError):
                signing.has_signature(Path("Fixture.app"))

    def test_outer_signature_is_verified_without_resigning(self):
        with patch.object(signing, "has_signature", return_value=True), patch.object(signing, "run") as run:
            signing.verify_app(Path("Fixture.app"))
            run.assert_called_once_with("/usr/bin/codesign", "--verify", "--strict", "--verbose=2", "Fixture.app")

    def test_dependencies_preserve_metadata_without_helper_entitlements(self):
        with patch.object(signing.subprocess, "run") as subprocess_run, patch.object(signing, "run") as run:
            subprocess_run.return_value.returncode = 0
            signing.sign_existing(Path("original.dylib"), "-")
            calls = [call.args for call in run.call_args_list]
            self.assertIn("--preserve-metadata=identifier,entitlements,flags,runtime", calls[0])
            self.assertNotIn("--entitlements", calls[0])
            self.assertNotIn("--deep", calls[0])
            self.assertEqual(calls[1][:3], ("/usr/bin/codesign", "--verify", "--strict"))


if __name__ == "__main__":
    unittest.main()
