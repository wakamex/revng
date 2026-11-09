#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
"""Unit tests for the discovery benchmark's generator and scoring, which need neither a compiler nor rev.ng"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchmark  # noqa: E402


class PlanTest(unittest.TestCase):
    def test_returning_functions_do_not_call_throwers(self):
        functions = benchmark.plan(200, seed=4)
        for function in functions:
            for callee in function["callees"]:
                self.assertNotEqual(functions[callee]["kind"], "always_throws")
                self.assertFalse(functions[callee]["indirect"])
            if function["kind"] == "calls_thrower":
                self.assertEqual(functions[function["thrower"]]["kind"], "always_throws")

    def test_indirect_functions_are_called_only_through_the_table(self):
        functions = benchmark.plan(40, seed=5)
        text = benchmark.source(functions)
        run = text.split('void run() {')[1]
        for index, function in enumerate(functions):
            self.assertEqual(f" f{index}(input)" in run, not function["indirect"])

    def test_switch_helpers_are_called_only_from_their_case(self):
        functions = benchmark.plan(60, seed=1)
        text = benchmark.source(functions)
        switches = [index for index, function in enumerate(functions) if function["kind"] == "switches"]
        self.assertTrue(switches)
        for index in switches:
            for case in range(benchmark.CASES):
                self.assertEqual(text.count(f"g{index}_{case}(x)"), 1)
                self.assertIn((f"g{index}_{case}", "case_only", False), benchmark.scored(functions))


class IdentityTest(unittest.TestCase):
    def test_the_path_of_the_binary_does_not_count(self):
        model = "Binaries:\n  - CanonicalPath: '/a/project/functions'\nFunctions:\n  - Entry: \"0x1000:Code_x86_64\"\n"
        other = model.replace("/a/project", "/b/project-1")
        self.assertEqual(benchmark.identity(model), benchmark.identity(other))
        self.assertNotEqual(benchmark.identity(model), benchmark.identity(model.replace("0x1000", "0x1010")))


class ScoreTest(unittest.TestCase):
    functions = [{"kind": "plain", "indirect": False}, {"kind": "always_throws", "indirect": True},
                 {"kind": "cleanup", "indirect": False}]
    symbols = {"_Z2f0i": (0x1000, 0x20), "_Z2f1i": (0x1020, 0x20), "_Z2f2i": (0x1040, 0x40)}

    def verdicts(self, entries):
        model = {"Functions": [{"Entry": f"0x{a:x}:Code_x86_64", **({"Attributes": ["NoReturn"]} if n else {})}
                               for a, n in entries]}
        return [e["verdicts"] for e in benchmark.score(self.functions, self.symbols, model)]

    def test_all_found(self):
        self.assertEqual(self.verdicts([(0x1000, False), (0x1020, True), (0x1040, False)]),
                         [["found"], ["found"], ["found"]])

    def test_missing_split_and_noreturn(self):
        self.assertEqual(self.verdicts([(0x1000, True), (0x1040, False), (0x1060, False)]),
                         [["false noreturn"], ["missing"], ["split"]])

    def test_missed_noreturn(self):
        self.assertEqual(self.verdicts([(0x1000, False), (0x1020, False), (0x1040, False)])[1], ["missed noreturn"])


if __name__ == "__main__":
    unittest.main()
