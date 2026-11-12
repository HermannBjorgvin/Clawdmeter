"""simgrid's action parsing, and (when the sims are built) a run end to end.

    python3 -m unittest discover -s tools/simgrid
The end-to-end tests need `simgrid.py build --boards 154,round` first.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import engine as eng  # noqa: E402
import simgrid  # noqa: E402
from profiles import PROFILES, select  # noqa: E402


class ParseTest(unittest.TestCase):
    def test_positional_and_keyword_words(self):
        self.assertEqual(simgrid.parse_action(["tap", "154", "120", "y=80", "hold_ms=900"]),
                         ("tap", ["154", "120"], {"y": "80", "hold_ms": "900"}))

    def test_json_word_stays_positional(self):
        self.assertEqual(simgrid.parse_action(["serial", '{"a=1"}']), ("serial", ['{"a=1"}'], {}))

    def test_coerce_follows_the_default(self):
        self.assertIs(eng.coerce("false", True), False)
        self.assertEqual(eng.coerce("1500", 120), 1500)
        self.assertEqual(eng.coerce("0.5", 1.0), 0.5)
        self.assertEqual(eng.coerce("42.5", None), 42.5)
        self.assertEqual(eng.coerce("Aug 15", None), "Aug 15")

    def test_every_action_is_described(self):
        d = eng.describe()
        self.assertEqual(set(d), set(eng.ACTIONS))
        for name, (_, doc) in d.items():
            self.assertTrue(doc, f"{name} has no docstring")


class ProfilesTest(unittest.TestCase):
    def test_ids_are_unique(self):
        ids = [p.id for p in PROFILES]
        self.assertEqual(len(ids), len(set(ids)))

    def test_select(self):
        self.assertEqual([p.id for p in select("154,round")], ["154", "round"])
        by_size = select("240x240")           # every cell of that size
        self.assertIn("154", [p.id for p in by_size])
        self.assertTrue(all(p.size == "240x240" for p in by_size))
        self.assertTrue(all(p.on_main for p in select("main")))
        with self.assertRaises(ValueError):
            select("nope")

    def test_round_cells_are_square(self):
        for p in PROFILES:
            if p.round:
                self.assertEqual(p.w, p.h)


BUILT = all(os.path.exists(eng.program_path(s)) for s in ("240x240", "466x466"))


@unittest.skipUnless(BUILT, "sims not built (simgrid.py build --boards 154,round)")
class RunTest(unittest.TestCase):
    SCRIPT = """
        step 3000
        beat attention
        step 2500
        shot {out}/a.png
        tap 154 120 120 hold_ms=200
        link false
        step 500
        shot {out}/b.png board=round
    """

    def run_once(self, out):
        e = eng.Engine("154,round", log_dir=out)
        try:
            simgrid.run_script(e, self.SCRIPT.format(out=out).splitlines(), log=lambda *_: None)
            return e.now
        finally:
            e.close()

    def test_same_script_same_pixels(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self.assertEqual(self.run_once(a), 6200)
            self.run_once(b)
            for name in ("a.png", "b.png"):
                with open(os.path.join(a, name), "rb") as fa, open(os.path.join(b, name), "rb") as fb:
                    self.assertEqual(fa.read(), fb.read(), name)


if __name__ == "__main__":
    unittest.main()
