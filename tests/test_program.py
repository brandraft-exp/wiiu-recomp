import collections
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from recomp.program import Program, FuncEmitter


class ProgramTests(unittest.TestCase):
    def test_branch_table_slots_are_callable_across_partitions(self):
        p = Program.__new__(Program)
        # Dispatch bctr is separate from a three-slot branch table.
        words = [0x4e800420, 0x4800000c, 0x48000008, 0x48000004, 0x4e800020]
        p.lo, p.hi = 0x1000, 0x1014
        p.word = lambda a: words[(a - p.lo) // 4]
        p.funcs = [(0x1000, 4), (0x1004, 16)]
        p.func_starts = [0x1000, 0x1004]; p.func_set = set(p.func_starts)
        p.rel24 = {}; p.discovered_entries = {0x1004}
        p._entries()
        self.assertTrue(p.callable(0x1008))
        self.assertTrue(p.callable(0x100c))

    def test_data_import_alias_with_signed_low_half_and_addend(self):
        p = Program.__new__(Program)
        # lis r3,0x31a; lwz r3,-0x7cf0(r3): pre-relocated address 0x3198310.
        # Symbol+16 was referenced, so the symbol itself resides at 0x3198300.
        p.text = SimpleNamespace(type=1, addr=0x1000, data=struct.pack('>II', 0x3c60031a, 0x80638310))
        symtab = SimpleNamespace(type=2, data=struct.pack('>IIIBBH', 0, 0xc0001000, 4, 1, 0, 0))
        relocs = SimpleNamespace(type=4, info=0, data=struct.pack('>IIiIIi', 0x1002, 6, 16, 0x1006, 4, 16))
        p.rpx = SimpleNamespace(sections=[p.text, symtab, relocs])
        p.lo, p.hi = 0x1000, 0x1008
        p.imports = {0xc0001000: ('coreinit', 'test_data', 'data')}
        p._relocs()
        self.assertEqual(p.data_import_aliases, {0x3198300: 0xc0001000})

    @unittest.skipUnless(shutil.which('clang'), 'clang is required')
    def test_emitted_calls_and_conditional_loop_execute(self):
        p = Program.__new__(Program)
        # main: li r3,0; bl worker; blr. worker: addi r3,r3,1;
        # cmpwi r3,5; blt worker; blr. A relocation is used for the local loop
        # too, to verify that relocation-resolved branch labels are emitted.
        words = [0x38600000, 0x48000009, 0x4e800020,
                 0x38630001, 0x2c030005, 0x4180fff8, 0x4e800020]
        p.lo, p.hi = 0x1000, 0x101c
        p.word = lambda a: words[(a - p.lo) // 4]
        p.funcs = [(0x1000, 12), (0x100c, 16)]
        p.func_starts = [a for a, _ in p.funcs]; p.func_set = set(p.func_starts)
        p.imports = {}; p.rel24 = {0x1014: 0x100c}; p.sorted_code_refs = []
        p.discovered_entries = {0x1010}
        p._entries(); p.validate()
        report = collections.defaultdict(list)
        code = '#include "ppc.h"\nvoid fn_00001000(ppc_ctx *); void fn_0000100C(ppc_ctx *);\n'
        code += '\n'.join('\n'.join(FuncEmitter(p, a, n, report).emit()) for a, n in p.funcs)
        self.assertFalse(report)
        code += '\nint main(void) { ppc_ctx c = {0}; fn_00001000(&c); return c.r[3] != 5; }\n'
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-emitter-test-') as work:
            source, exe = Path(work) / 'test.c', Path(work) / 'test'
            source.write_text(code)
            subprocess.run(['clang', '-O1', '-I' + str(root / 'runtime'), str(source), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, timeout=5)
