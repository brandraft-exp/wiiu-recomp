"""IPC pool exhaustion, validation, FIFO reuse and guest ABI checks."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang'), 'clang is required')
class IPCPoolTests(unittest.TestCase):
    def test_pool_lifecycle(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-ipc-test-') as work:
            work = Path(work)
            (work / 'shim.c').write_text('#include "standalone/rt.h"\nuint8_t *g_mem;\n'
                'static void init(ppc_ctx *c) { st32(c->r[3], 0x6d557458); }\n'
                'hle_fn hle_thread_lookup(const char *n) { (void)n; return init; }\n')
            subprocess.run(['clang', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'runtime'), str(root / 'runtime/standalone/ipcbuf.c'),
                str(work / 'shim.c'), '-o', str(work / 'ipc.so')], check=True, capture_output=True)
            lib = ctypes.CDLL(str(work / 'ipc.so'))
            memory = ctypes.create_string_buffer(0x10000)
            ctypes.c_void_p.in_dll(lib, 'g_mem').value = ctypes.addressof(memory)
            lib.hle_ipcbuf_lookup.argtypes = [ctypes.c_char_p]
            lib.hle_ipcbuf_lookup.restype = ctypes.c_void_p
            regs = (ctypes.c_uint32 * 32)()
            def call(name, *args):
                for i, arg in enumerate(args): regs[i + 3] = arg
                fn = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(lib.hle_ipcbuf_lookup(name.encode()))
                fn(regs)
                return regs[3]
            word = lambda a: struct.unpack('>I', memory[a:a + 4])[0]
            self.assertEqual(call('IPCBufPoolCreate', 0x1000, 0x40, 0x100, 0x100, 0), 0)
            self.assertEqual(word(0x100), 0)
            pool = call('IPCBufPoolCreate', 0x1001, 0x2000, 0x101, 0x100, 0)
            self.assertEqual(pool, 0x1004)
            self.assertEqual(word(pool), 0x0badf00d)
            self.assertEqual(word(pool + 0x3c), 0x6d557458)
            self.assertEqual(word(pool + 0x14), 0x140)
            count = word(0x100)
            entries = [call('IPCBufPoolAllocate', pool, 0x101) for _ in range(count)]
            self.assertEqual(len(set(entries)), count)
            self.assertTrue(all(e >= 0x1001 and e + 0x140 <= 0x3001 for e in entries))
            self.assertEqual(call('IPCBufPoolAllocate', pool, 1), 0)
            self.assertEqual(call('IPCBufPoolFree', pool, entries[0] + 1), 0xfffffffc)
            for e in reversed(entries): self.assertEqual(call('IPCBufPoolFree', pool, e), 0)
            self.assertEqual(call('IPCBufPoolFree', pool, entries[0]), 0xfffffff8)
            self.assertEqual(call('IPCBufPoolAllocate', pool, 0x141), 0)
            self.assertEqual([call('IPCBufPoolAllocate', pool, 1) for _ in entries], entries[::-1])
