#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
"""Unit tests for the layouts the prototype benchmark compares, which need neither a compiler nor rev.ng"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchmark  # noqa: E402


class PlacementTest(unittest.TestCase):
    def test_microsoft_positions(self):
        laid = benchmark.placement(["float", "int", "int", "float", "int"], "float", microsoft=True)
        self.assertEqual(laid["arguments"], sorted([("zmm0", "float"), ("rdx", "int"), ("r8", "int"),
                                                    ("zmm3", "float")]))
        self.assertEqual(laid["slots"], 1)

    def test_sysv_classes_fill_separately(self):
        laid = benchmark.placement(["float", "int"], "int", microsoft=False)
        self.assertEqual(laid, benchmark.placement(["int", "float"], "int", microsoft=False))
        self.assertEqual(laid["arguments"], [("rdi", "int"), ("zmm0", "float")])

    def test_sysv_seventh_integer_on_the_stack(self):
        laid = benchmark.placement(["int"] * 7 + ["float"], "void", microsoft=False)
        self.assertEqual(laid["slots"], 1)
        self.assertEqual(len(laid["arguments"]), 7)

    def test_class_in_a_register_matters(self):
        self.assertNotEqual(benchmark.placement(["int"], "void", microsoft=True),
                            benchmark.placement(["float"], "void", microsoft=True))

    def test_stack_arguments_have_no_class(self):
        self.assertEqual(benchmark.placement(["int"] * 4 + ["float"], "void", microsoft=True),
                         benchmark.placement(["int"] * 4 + ["other"], "void", microsoft=True))


class ExpectedLocationsTest(unittest.TestCase):
    def test_microsoft_double_and_int(self):
        self.assertEqual(benchmark.expected_locations("i64", ["f64", "i32"], microsoft=True),
                         {"arguments": ["rdx", "zmm0"], "returns": ["rax"], "slots": 0})

    def test_sysv_float_return(self):
        self.assertEqual(benchmark.expected_locations("f32", ["ptr"], microsoft=False),
                         {"arguments": ["rdi"], "returns": ["zmm0"], "slots": 0})


class SourceTest(unittest.TestCase):
    def test_every_function_is_called(self):
        signatures = benchmark.signatures(20, seed=3)
        text = benchmark.source(signatures, microsoft=False)
        for index in range(len(signatures)):
            self.assertIn(f"f{index}(", text.split("void run(void)")[1])

    def test_microsoft_defines_fltused(self):
        self.assertIn("int _fltused;", benchmark.source([("f64", ["f64"])], microsoft=True))


if __name__ == "__main__":
    unittest.main()
