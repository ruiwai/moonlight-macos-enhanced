#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Portable packaging tests: never invoke real Metal tools."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "metallibs", ROOT / "scripts/build_pyrowave_metallibs.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class MetallibBuildTests(unittest.TestCase):
    def test_exact_header_and_hash(self):
        header = builder.DEFAULT_SOURCE.read_bytes().decode("utf-8")
        sources = builder.extract_sources(header)
        self.assertEqual(set(sources), set(builder.SHADERS))
        for name, source in sources.items():
            expected = header.split(f'{name}[] = R"PYROWAVE_MSL(', 1)[1].split(
                ')PYROWAVE_MSL";', 1)[0]
            self.assertEqual(source.encode("utf-8"), expected.encode("utf-8"))
            self.assertTrue(source.startswith("\n"))
            digest = hashlib.sha256(b"pyrowave-metal3.0-fp32-v1\n" +
                                    expected.encode("utf-8")).hexdigest()
            self.assertEqual(builder.signature(source), digest)
            self.assertRegex(digest, r"^[0-9a-f]{64}$")
        self.assertNotEqual(builder.signature("x\r\n"), builder.signature("x\n"))

    def test_reject_missing_duplicate_and_bad_delimiters(self):
        header = builder.DEFAULT_SOURCE.read_bytes().decode("utf-8")
        for name in builder.SHADERS:
            for bad in (header.replace(name, "missing_name"),
                        header + f'\nstatic const char {name}[] = "duplicate";',
                        header + f'\nconst char *{name} = "duplicate";',
                        header.replace(f'{name}[] = R"PYROWAVE_MSL(',
                                       f'{name}[] = R"OTHER(')):
                with self.subTest(name=name, bad=bad[-80:]), self.assertRaises(ValueError):
                    builder.extract_sources(bad)
        for bad in (header.replace(')PYROWAVE_MSL";', ')OTHER";'),
                    header.replace(')PYROWAVE_MSL";', ')PYROWAVE_MSL" "concat";')):
            with self.assertRaises(ValueError):
                builder.extract_sources(bad)

    def test_extraction_failure_never_invokes_tools(self):
        header = builder.DEFAULT_SOURCE.read_bytes().decode("utf-8")
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / "bad.h", Path(tmp) / "output"
            for bad in ("missing definitions",
                        header.replace(')PYROWAVE_MSL"', ')OTHER"', 1),
                        header.replace(')PYROWAVE_MSL"', '', 1)):
                source.write_text(bad)
                with self.subTest(bad=bad[:80]), patch.object(builder.subprocess, "run") as run:
                    with self.assertRaises(ValueError):
                        builder.build(source, output, tmp)
                    run.assert_not_called()
                self.assertFalse(output.exists())

    def test_separate_commands_paths_and_publication(self):
        with tempfile.TemporaryDirectory(prefix="pyrowave test spaces ") as tmp:
            root = Path(tmp)
            source, output = root / "shader header.h", root / "app resources"
            source.write_bytes(builder.DEFAULT_SOURCE.read_bytes().replace(b"\n", b"\r\n"))
            sources = builder.extract_sources(source.read_bytes().decode("utf-8"))
            commands, compiled = [], {}

            def run(argv, **kwargs):
                self.assertEqual(kwargs, {"check": True})
                self.assertFalse(output.exists())  # No publication before BOTH links.
                commands.append(argv)
                if argv[3] == "metal":
                    msl = Path(argv[argv.index("-c") + 1])
                    compiled[msl.stem] = msl.read_bytes()
                    self.assertNotEqual(msl.parent, source.parent)
                Path(argv[argv.index("-o") + 1]).write_bytes(b"mock artifact")

            with patch.object(builder.subprocess, "run", side_effect=run):
                builder.main(["--source", str(source), "--output", str(output),
                              "--work", str(root)])
            self.assertEqual(len(commands), 4)
            for i, (name, stem) in enumerate(builder.SHADERS.items()):
                compile_argv, link_argv = commands[2*i:2*i+2]
                self.assertEqual(compile_argv[:8], ["xcrun", "-sdk", "macosx", "metal",
                    "-std=metal3.0", "-mmacosx-version-min=13.0", "-fno-fast-math", "-c"])
                self.assertEqual(len(compile_argv), 11)
                self.assertEqual(link_argv[:4], ["xcrun", "-sdk", "macosx", "metallib"])
                self.assertEqual(link_argv[4], compile_argv[-1])
                self.assertEqual(link_argv[-2], "-o")
                self.assertEqual(len(link_argv), 7)
                self.assertEqual(compiled[stem], sources[name].encode("utf-8"))
                self.assertEqual((output / (stem + ".metallib")).read_bytes(), b"mock artifact")
                self.assertEqual((output / (stem + ".sha256")).read_text(),
                                 builder.signature(sources[name]))
            self.assertEqual(len(list(output.iterdir())), 4)
            self.assertEqual(set(root.iterdir()), {source, output})  # Intermediates cleaned.

    def test_tool_failure_never_publishes(self):
        for failing_call in range(1, 5):
            for existing in (False, True):
                with self.subTest(call=failing_call, existing=existing), \
                        tempfile.TemporaryDirectory() as tmp:
                    output = Path(tmp) / "output"
                    if existing:
                        output.mkdir()
                        for stem in builder.SHADERS.values():
                            for ext in (".metallib", ".sha256"):
                                (output / (stem + ext)).write_bytes(b"previous")
                    calls = []

                    def run(argv, **kwargs):
                        self.assertEqual(kwargs, {"check": True})
                        calls.append(argv)
                        if len(calls) == failing_call:
                            raise subprocess.CalledProcessError(1, argv)
                        Path(argv[-1]).write_bytes(b"partial")

                    with patch.object(builder.subprocess, "run", side_effect=run):
                        with self.assertRaises(subprocess.CalledProcessError):
                            builder.build(builder.DEFAULT_SOURCE, output, tmp)
                    self.assertEqual(len(calls), failing_call)
                    self.assertEqual(output.exists(), existing)
                    if existing:
                        self.assertEqual(len(list(output.iterdir())), 4)
                        self.assertTrue(all(p.read_bytes() == b"previous" for p in output.iterdir()))


if __name__ == "__main__":
    unittest.main()
