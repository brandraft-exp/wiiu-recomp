"""DMA endian modes and sampler fixed-point register semantics."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang'), 'clang is required')
class GPUDataTests(unittest.TestCase):
    def test_dma_and_sampler(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-gpu-data-') as work:
            work = Path(work)
            (work / 'shim.c').write_text('#include "standalone/rt.h"\n#include <stdlib.h>\n'
                'uint8_t *g_mem;\nuint64_t ppc_timebase(ppc_ctx *c) { (void)c; return 100; }\n'
                'void rt_fatal(ppc_ctx *c,const char *fmt,...) { (void)c; (void)fmt; abort(); }\n')
            subprocess.run(['clang', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'runtime'), str(root / 'runtime/standalone/dmae.c'),
                str(root / 'runtime/standalone/gx2_sampler.c'), str(work / 'shim.c'),
                '-lm', '-o', str(work / 'gpu.so')], check=True, capture_output=True)
            lib = ctypes.CDLL(str(work / 'gpu.so'))
            memory = ctypes.create_string_buffer(0x10000)
            ctypes.c_void_p.in_dll(lib, 'g_mem').value = ctypes.addressof(memory)
            # PPC register layout starts with 32 u32 GPRs then 32 u64 FPRs.
            ctx = ctypes.create_string_buffer(1024)
            regs = (ctypes.c_uint32 * 32).from_buffer(ctx)
            floats = (ctypes.c_double * 32).from_buffer(ctx, 128)
            for lookup in (lib.hle_dmae_lookup, lib.hle_gx2_sampler_lookup):
                lookup.argtypes = [ctypes.c_char_p]; lookup.restype = ctypes.c_void_p
            def call(name, *args):
                for i, arg in enumerate(args): regs[3 + i] = arg
                lookup = lib.hle_dmae_lookup if name.startswith('DMA') else lib.hle_gx2_sampler_lookup
                ptr = lookup(name.encode()); self.assertTrue(ptr)
                ctypes.CFUNCTYPE(None, ctypes.c_void_p)(ptr)(ctx)
                return regs[3]
            src = bytes(range(16))
            ctypes.memmove(ctypes.addressof(memory) + 0x1000, src, 16)
            previous = 0
            for mode, unit in ((0, 1), (1, 2), (2, 4)):
                call('DMAECopyMem', 0x2000, 0x1000, 4, mode)
                self.assertEqual(memory[0x2000:0x2010], b''.join(src[i:i+unit][::-1] for i in range(0,16,unit)))
                stamp = regs[3] << 32 | regs[4]; self.assertGreater(stamp, previous); previous = stamp
                call('DMAEGetRetiredTimeStamp'); self.assertEqual(regs[3] << 32 | regs[4], stamp)
                self.assertEqual(call('DMAEWaitDone', stamp >> 32, stamp & 0xffffffff), 1)
            call('DMAEFillMem', 0x2000, 0x12345678, 4)
            self.assertEqual(memory[0x2000:0x2010], bytes.fromhex('12345678') * 4)
            word = lambda a: struct.unpack('>I', memory[a:a + 4])[0]
            call('GX2InitSampler', 0x3000, 2, 1)
            initial = 2 | 2 << 3 | 2 << 6 | 1 << 9 | 1 << 12 | 1 << 15 | 1 << 17 | 1 << 25
            self.assertEqual(word(0x3000), initial)
            self.assertEqual(word(0x3004), 1023 << 10)
            self.assertEqual(word(0x3008), 1 << 31)
            call('GX2InitSamplerXYFilter', 0x3000, 0, 1, 3)
            self.assertEqual((word(0x3000) >> 9) & 7, 4)
            self.assertEqual((word(0x3000) >> 12) & 7, 5)
            self.assertEqual((word(0x3000) >> 19) & 7, 3)
            floats[1], floats[2], floats[3] = -1, 40, -1.25
            call('GX2InitSamplerLOD', 0x3000)
            self.assertEqual(word(0x3004), 1023 << 10 | ((-80 & 4095) << 20))
