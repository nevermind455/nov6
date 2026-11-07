"""Offline tests for the development-only journal comparison tool."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


class CompareTests(unittest.TestCase):
    def compare(self, left, right, *options):
        with tempfile.TemporaryDirectory() as folder:
            paths = [pathlib.Path(folder) / name for name in ("a", "b")]
            for path, rows in zip(paths, (left, right)):
                path.write_text("".join(json.dumps(row) + "\n" for row in rows))
            return subprocess.run([sys.executable, "-I", str(pathlib.Path(__file__).with_name("compare.py")),
                                   *(str(path) for path in paths), *options],
                                  text=True, capture_output=True)

    def test_identical_decisions_pass(self):
        rows = [{"result": "filled", "side": "UP", "shares": 5}]
        self.assertEqual(self.compare(rows, rows).returncode, 0)

    def test_empty_and_nondecision_journals_do_not_prove_parity(self):
        self.assertNotEqual(self.compare([], []).returncode, 0)
        rows = [{"result": "summary"}]
        self.assertNotEqual(self.compare(rows, rows).returncode, 0)

    def test_nondiagnostic_field_changes_fail_strict_comparison(self):
        left = [{"result": "filled", "side": "UP", "shares": 5, "fee": 0.1}]
        right = [{**left[0], "fee": 0.2}]
        self.assertNotEqual(self.compare(left, right).returncode, 0)
        self.assertEqual(self.compare(left, right, "--decision-only").returncode, 0)

    def test_missing_is_not_a_literal_string_or_boolean_number(self):
        left = [{"result": "skipped", "side": "UP", "extra": "<absent>"}]
        right = [{"result": "skipped", "side": "UP"}]
        self.assertNotEqual(self.compare(left, right).returncode, 0)
        self.assertNotEqual(self.compare([{**left[0], "extra": True}],
                                         [{**left[0], "extra": 1}]).returncode, 0)

    def test_alignment_differences_cannot_produce_negative_parity(self):
        result = self.compare([{"result": "filled"}, {"result": "summary"}],
                              [{"result": "summary"}, {"result": "filled"}])
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("parity: -", result.stdout)


if __name__ == "__main__":
    unittest.main()
