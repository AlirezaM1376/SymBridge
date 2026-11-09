"""Tests for coverage_all.py: the matrix counts agree with an independent recount from the map CSVs."""
import collections
import csv
import json
import os
import sys
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import coverage_all as CA          # noqa: E402
import import_coverage as IC       # noqa: E402


class CoverageAll(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        reg, _ = IC.load_shim_registry(os.path.join(ROOT, "src"))
        base = IC.load_importmap_h(os.path.join(ROOT, "src"))
        cls.games = CA.load_games(ROOT)
        for g in cls.games:
            CA.classify_game(g, reg, base)
        cls.matrix = CA.build_matrix(cls.games)

    def test_user_counts_match_recount(self):
        users = collections.defaultdict(set)
        for g in self.games:
            folder = os.path.join(ROOT, "games", g["id"])
            with open(os.path.join(folder, "game.json"), encoding="utf-8") as f:
                main = next(v for v in json.load(f)["versions"] if v["id"] == "main")
            with open(os.path.join(folder, main["map"]), newline="", encoding="utf-8-sig") as f:
                for r in csv.DictReader(f):
                    users[(r["dll"].lower(), int(r["v2_ordinal"]))].add(g["id"])
        self.assertEqual(len(self.matrix), len(users))
        for st, ids, dll, o, nm in self.matrix:
            self.assertEqual(set(ids), users[(dll, o)])

    def test_traps_come_first_and_most_used_first(self):
        states = [r[0] for r in self.matrix]
        self.assertEqual(states, sorted(states, key=["trap", "shim", "direct"].index))
        traps = [len(r[1]) for r in self.matrix if r[0] == "trap"]
        self.assertEqual(traps, sorted(traps, reverse=True))

    def test_both_a_single_game_trap_and_a_multi_game_trap_exist(self):
        counts = {len(r[1]) for r in self.matrix if r[0] == "trap"}
        self.assertIn(1, counts)
        self.assertGreaterEqual(max(counts), 5)

    def test_per_game_counts_add_up(self):
        # (Whether the prediction equals the last device run is what check_all's coverage check reports; it goes
        # stale on purpose whenever a shim is added, so it is not asserted here.)
        for g in self.games:
            s = g["summary"]
            if not s:
                continue          # pending game
            self.assertEqual(g["count"]["direct"] + g["count"]["shim"] + g["count"]["trap"], int(s["imports"]), g["id"])


if __name__ == "__main__":
    unittest.main()
