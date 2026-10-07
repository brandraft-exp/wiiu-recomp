"""Synthetic control-flow fixtures; no game data required."""
import unittest
from types import SimpleNamespace
from recomp.discovery import discover


def program(words, refs=(), rel24=None):
    return SimpleNamespace(lo=0x1000, hi=0x1000 + 4 * len(words),
                           rpx=SimpleNamespace(entry=0x1000),
                           data_code_refs=set(refs), code_addr_refs=set(),
                           rel24=rel24 or {}, word=lambda a: words[(a - 0x1000) // 4])


class DiscoveryTests(unittest.TestCase):
    def test_direct_call(self):
        p = program([0x48000009, 0x4e800020, 0x4e800020])
        self.assertEqual(discover(p)[0], [(0x1000, 8), (0x1008, 4)])

    def test_conditional_crossing_keeps_entry(self):
        p = program([0x41820008, 0x60000000, 0x4e800020], [0x1004])
        funcs, entries = discover(p)
        self.assertEqual(funcs, [(0x1000, 12)])
        self.assertIn(0x1004, entries)

    def test_backward_branch_crossing(self):
        p = program([0x60000000, 0x60000000, 0x4182fff8], [0x1004])
        self.assertEqual(discover(p)[0], [(0x1000, 12)])

    def test_relocated_call_uses_resolved_target(self):
        p = program([0x48000001, 0x4e800020, 0x4e800020], rel24={0x1000: 0x1008})
        self.assertEqual(discover(p)[0], [(0x1000, 8), (0x1008, 4)])

    def test_unconditional_tail_call(self):
        p = program([0x48000008, 0x60000000, 0x4e800020], [0x1008])
        self.assertEqual(discover(p)[0], [(0x1000, 8), (0x1008, 4)])

    def test_jump_into_distant_helper_does_not_merge_intervening_code(self):
        p = program([0x48000010, 0x4e800020, 0x4e800020, 0x60000000, 0x4e800020], [0x1008, 0x100c])
        funcs, entries = discover(p)
        self.assertEqual(funcs, [(0x1000, 8), (0x1008, 4), (0x100c, 4), (0x1010, 4)])
        self.assertIn(0x1010, entries)


if __name__ == '__main__':
    unittest.main()
