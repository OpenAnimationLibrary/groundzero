"""Contracts for isolated cache mutation and exact LeakSanitizer evidence."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import qualify_image_cache_lsan as qualifier


class CacheLeakQualification(unittest.TestCase):
    def test_mutation_changes_only_one_memory_destructor(self):
        memory = qualifier.MEMORY_DESTRUCTOR + qualifier.DELETE_BUILDER + "\n}"
        disk = "CompressedOnDiskCacheItem::~CompressedOnDiskCacheItem() {\n  delete m_builder;\n}"
        source = memory + "\n" + disk
        changed = qualifier.remove_builder_delete(source)
        self.assertEqual(changed, qualifier.MEMORY_DESTRUCTOR + "\n}\n" + disk)
        self.assertEqual(source.count("delete m_builder;"), 2)

    def test_mutation_rejects_missing_or_duplicate_matches(self):
        fixed = qualifier.MEMORY_DESTRUCTOR + qualifier.DELETE_BUILDER
        for source in ("", qualifier.MEMORY_DESTRUCTOR, fixed + fixed):
            with self.subTest(source=source), self.assertRaises(ValueError):
                qualifier.remove_builder_delete(source)

    def test_requires_exact_exit_and_direct_builder_stack(self):
        report = (
            qualifier.SUCCESS + "\nERROR: LeakSanitizer: detected memory leaks\n"
            "Direct leak of 1024 byte(s) in 128 object(s) allocated from:\n"
            "    #0 0x123 in operator new(unsigned long)\n"
            "    #1 0x456 in CompressedOnMemoryCacheItem::CompressedOnMemoryCacheItem()\n"
        )
        qualifier.require_result(23, report, mutated=True)
        for code, text in (
            (0, report),
            (1, report),
            (-11, report),
            (23, report.replace(qualifier.SUCCESS, "fixture did not finish")),
            (
                23,
                report.replace("CompressedOnMemoryCacheItem::CompressedOnMemoryCacheItem", "Qt"),
            ),
            (23, report.replace("Direct leak of", "Indirect leak of")),
            (23, qualifier.SUCCESS),
        ):
            with self.subTest(code=code, text=text), self.assertRaises(RuntimeError):
                qualifier.require_result(code, text, mutated=True)

    def test_fixed_result_rejects_a_report_even_with_zero_exit(self):
        qualifier.require_result(0, qualifier.SUCCESS, mutated=False)
        for code, text in (
            (23, qualifier.SUCCESS),
            (0, ""),
            (0, qualifier.SUCCESS + "\nLeakSanitizer: detected memory leaks"),
        ):
            with self.subTest(code=code, text=text), self.assertRaises(RuntimeError):
                qualifier.require_result(code, text, mutated=False)

    def test_reuses_build_options_but_not_original_output_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / "CMakeCache.txt"
            cache.write_text(
                "BUILD_RELIABILITY_TESTS:BOOL=ON\n"
                "BUILD_UI_PORT_TESTS:BOOL=ON\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++\n"
                "CMAKE_CXX_COMPILER_LAUNCHER:UNINITIALIZED=ccache\n"
                "CMAKE_CXX_FLAGS:STRING=-Werror=return-type\n"
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "Python3_EXECUTABLE:FILEPATH=/selected/python\n"
                "CMAKE_RUNTIME_OUTPUT_DIRECTORY:PATH=/original/bin\n"
                "OpenToonz_BINARY_DIR:STATIC=/original/build\n",
                encoding="utf-8",
            )
            options = qualifier.configure_options(cache)
            self.assertIn("-DCMAKE_CXX_FLAGS=-Werror=return-type", options)
            self.assertIn("-DBUILD_UI_PORT_TESTS=ON", options)
            self.assertIn("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", options)
            self.assertIn("-DPython3_EXECUTABLE=/selected/python", options)
            self.assertEqual(len(options), 7)
            cache.write_text("BUILD_RELIABILITY_TESTS:BOOL=OFF\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                qualifier.configure_options(cache)

    def test_disposable_sources_do_not_alias_mutated_or_fetched_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "original"
            build = root / "toonz/build"
            copied = Path(temporary) / "copy"
            for directory in (
                root / "toonz/sources/common",
                root / "toonz/cmake",
                root / "ci-scripts/seexpr",
                root / "thirdparty",
                root / "toonz/install",
                build / "_deps/spdlog-src",
            ):
                directory.mkdir(parents=True)
            original = root / "toonz/sources/common/cache.cpp"
            original.write_text("fixed", encoding="utf-8")
            dependency = build / "_deps/spdlog-src/LICENSE"
            dependency.write_text("license", encoding="utf-8")
            options = qualifier.copy_sources(root, build, copied)
            (copied / "toonz/sources/common/cache.cpp").write_text("mutant", encoding="utf-8")
            (copied / "dependencies/spdlog-src/LICENSE").write_text("copy", encoding="utf-8")
            self.assertEqual(original.read_text(encoding="utf-8"), "fixed")
            self.assertEqual(dependency.read_text(encoding="utf-8"), "license")
            self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", options)
            self.assertTrue((copied / "thirdparty").is_symlink())

    def test_timeout_kills_and_reaps_owned_process_group(self):
        process = Mock(pid=73)
        process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 1), -9]
        factory = Mock()
        factory.return_value.__enter__ = Mock(return_value=process)
        factory.return_value.__exit__ = Mock(return_value=False)
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "timeout.log"
            with (
                patch.object(qualifier.subprocess, "Popen", factory),
                patch.object(qualifier.os, "killpg") as kill,
            ):
                with self.assertRaisesRegex(RuntimeError, "exceeded"):
                    qualifier.run_logged(["fixture"], log, {}, 1)
                kill.assert_called_once_with(73, qualifier.signal.SIGKILL)
        self.assertEqual(process.wait.call_count, 2)

    def test_fixture_prefers_its_own_library_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "isolated/bin/timagecache_test"
            log = root / "normal.log"
            log.write_text(qualifier.SUCCESS, encoding="utf-8")
            environment = {
                "LD_LIBRARY_PATH": "/shipping/lib",
                "LSAN_OPTIONS": "detect_leaks=1",
            }
            with patch.object(qualifier, "run_logged", return_value=0) as run:
                qualifier.run_fixture(binary, log, environment, mutated=False)
            self.assertEqual(
                run.call_args.args[2]["LD_LIBRARY_PATH"],
                str(root / "isolated/lib/opentoonz"),
            )
            self.assertEqual(environment["LD_LIBRARY_PATH"], "/shipping/lib")


if __name__ == "__main__":
    unittest.main()
