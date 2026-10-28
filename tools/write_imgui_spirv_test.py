#!/usr/bin/env python3
"""Test the --check logic of write_imgui_spirv.py with no shader compiler.

A fake subprocess.run stands in for slangc and spirv-val. It records each
command line and writes chosen bytes as the compiled module.
"""
import contextlib
import io
import re
import subprocess
import sys
import unittest
from pathlib import Path

# No __pycache__ in the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import write_imgui_spirv  # noqa: E402

MAGIC = 0x07230203


def header_arrays() -> dict[str, list[int]]:
    text = write_imgui_spirv.HEADER.read_text()
    arrays = {}
    for name, body in re.findall(r"static const uint32 (\w+)\[\] = \{(.*?)\};", text, re.S):
        arrays[name] = [int(word, 16) for word in re.findall(r"0x([0-9a-f]{8})u", body)]
    return arrays


def header_version() -> str:
    for line in write_imgui_spirv.HEADER.read_text().splitlines():
        if line.startswith(write_imgui_spirv.VERSION_PREFIX):
            return line[len(write_imgui_spirv.VERSION_PREFIX):]
    raise AssertionError("the header records no slangc version")


def to_bytes(words: list[int]) -> bytes:
    return b"".join(word.to_bytes(4, "little") for word in words)


def contains_run(command: list[str], run: list[str]) -> bool:
    return any(command[index:index + len(run)] == run for index in range(len(command) - len(run) + 1))


class FakeTools:
    """Answers -version with version, and writes modules[stage] to the path after -o."""

    def __init__(self, version: str, modules: dict[str, bytes]):
        self.version = version
        self.modules = modules
        self.commands: list[list[str]] = []

    def run(self, command, **kwargs):
        command = [str(part) for part in command]
        self.commands.append(command)
        if command[1:] == ["-version"]:
            return subprocess.CompletedProcess(command, 0, stdout=self.version, stderr="")
        if "-o" in command:
            stage = command[command.index("-stage") + 1]
            Path(command[command.index("-o") + 1]).write_bytes(self.modules[stage])
        return subprocess.CompletedProcess(command, 0, stdout="", stderr="")


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.original_run = write_imgui_spirv.subprocess.run
        arrays = header_arrays()
        self.matching = {
            "vertex": to_bytes(arrays["imgui_vertex_spirv"]),
            "fragment": to_bytes(arrays["imgui_fragment_spirv"]),
        }

    def tearDown(self):
        write_imgui_spirv.subprocess.run = self.original_run

    def check(self, tools: FakeTools) -> tuple[int, str]:
        write_imgui_spirv.subprocess.run = tools.run
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = write_imgui_spirv.main(["--check", "--slangc", "slangc", "--spirv-val", "spirv-val"])
        return result, output.getvalue()

    def test_a_fresh_compile_equal_to_the_header_passes(self):
        result, output = self.check(FakeTools(header_version(), self.matching))
        self.assertEqual(result, 0, output)

    def test_a_fresh_compile_that_differs_from_the_header_fails(self):
        modules = dict(self.matching, fragment=to_bytes([MAGIC, 0x00010500, 0, 1, 0]))
        result, output = self.check(FakeTools(header_version(), modules))
        self.assertEqual(result, 1)
        self.assertIn("FAIL:", output)

    def test_a_module_without_the_spirv_magic_is_rejected(self):
        modules = dict(self.matching, vertex=to_bytes([MAGIC ^ 1, 0x00010500, 0, 1, 0]))
        with self.assertRaises(SystemExit) as raised:
            self.check(FakeTools(header_version(), modules))
        self.assertIn("not a little-endian SPIR-V module", str(raised.exception.code))

    def test_a_module_whose_length_is_not_whole_words_is_rejected(self):
        modules = dict(self.matching, vertex=to_bytes([MAGIC, 0x00010500, 0, 1, 0]) + b"\x00")
        with self.assertRaises(SystemExit) as raised:
            self.check(FakeTools(header_version(), modules))
        self.assertIn("not a little-endian SPIR-V module", str(raised.exception.code))

    def test_another_slangc_version_skips(self):
        result, output = self.check(FakeTools(header_version() + ".1", self.matching))
        self.assertEqual(result, 77)
        self.assertIn("skip:", output)

    def test_the_commands_hold_every_flag_of_the_flags_file(self):
        tools = FakeTools(header_version(), self.matching)
        self.check(tools)
        flags = write_imgui_spirv.read_flags(write_imgui_spirv.FLAGS)
        compiles = [command for command in tools.commands if command[0] == "slangc" and "-o" in command]
        validations = [command for command in tools.commands if command[0] == "spirv-val"]
        self.assertEqual(len(compiles), len(write_imgui_spirv.STAGES))
        self.assertEqual(len(validations), len(write_imgui_spirv.STAGES))
        for command in compiles:
            self.assertTrue(contains_run(command, flags["spirv"]), command)
            self.assertTrue(contains_run(command, flags["common"]), command)
        for command in validations:
            self.assertTrue(contains_run(command, flags["spirv-val"]), command)


if __name__ == "__main__":
    unittest.main()
