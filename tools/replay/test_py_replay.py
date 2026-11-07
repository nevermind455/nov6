"""Reference-driver resource ownership; no external reference needed."""
import asyncio
import pathlib
import unittest
from unittest.mock import patch

import py_replay


class ReplayResources(unittest.TestCase):
    def check_cleanup(self, fail):
        observed = []

        async def fake(*args):
            directory = pathlib.Path(args[-1])
            observed.append(directory)
            (directory / "j.jsonl").write_text('{"result":"test"}\n')
            if fail:
                raise RuntimeError("reference failed")
            return 1

        with patch.object(py_replay, "_replay", fake):
            if fail:
                with self.assertRaisesRegex(RuntimeError, "reference failed"):
                    asyncio.run(py_replay.replay("", "", "", {}))
            else:
                self.assertEqual(asyncio.run(py_replay.replay("", "", "", {})), 1)
        self.assertEqual(len(observed), 1)
        self.assertFalse(observed[0].exists())

    def test_scratch_journal_is_removed_after_success(self):
        self.check_cleanup(False)

    def test_scratch_journal_is_removed_after_failure(self):
        self.check_cleanup(True)


if __name__ == "__main__":
    unittest.main()
