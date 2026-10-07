"""GX2 ABI and layout regression checks; synthetic surfaces only."""
import ctypes
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang++'), 'clang++ is required')
class SurfaceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='wiiu-gx2-test-')
        root = Path(__file__).resolve().parent.parent
        work = Path(cls.work.name)
        (work / 'shim.c').write_text('#include <stdint.h>\n#include <stdlib.h>\n'
                                   'uint8_t *g_mem;\nvoid rt_fatal(void *c, const char *s, ...) { abort(); }\n')
        for source, name in [(root / 'runtime/standalone/gx2.c', 'gx2'), (work / 'shim.c', 'shim')]:
            subprocess.run(['clang', '-fPIC', '-I' + str(root / 'runtime'), '-c', str(source),
                            '-o', str(work / (name + '.o'))], check=True, capture_output=True)
        subprocess.run(['clang++', '-std=c++20', '-O2', '-fPIC', '-shared',
                        str(root / 'runtime/gx2/addr.cpp'), str(root / 'runtime/gx2/surface.cpp'),
                        str(root / 'runtime/gx2/coord.cpp'),
                        str(work / 'gx2.o'), str(work / 'shim.o'), '-o', str(work / 'gx2.so')],
                       check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(work / 'gx2.so'))
        cls.lib.gx2_surface_layout.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.hle_gx2_lookup.argtypes = [ctypes.c_char_p]
        cls.lib.hle_gx2_lookup.restype = ctypes.c_void_p

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def surface(self, width, height, fmt=26, tile=16, mips=1, depth=1, dim=1, aa=0):
        words = (ctypes.c_uint32 * 29)(dim, width, height, depth, mips, fmt, aa, 3)
        words[12] = tile
        self.assertEqual(self.lib.gx2_surface_layout(words), 1)
        return list(words)

    def test_linear_and_bc1_mips(self):
        s = self.surface(13, 7)
        self.assertEqual((s[8], s[14], s[15]), (13 * 7 * 4, 1, 13))
        s = self.surface(13, 7, fmt=49, mips=4)
        # BC1 is 8 bytes per 4x4 block: 4x2 blocks in base, then 2,1,1 blocks.
        self.assertEqual((s[8], s[10], s[15]), (64, 32, 4))
        self.assertEqual(s[16:19], [64, 16, 24])

    def test_scan_buffer_tiles(self):
        s = self.surface(1920, 1080, tile=0)
        self.assertEqual((s[8], s[12], s[14], s[15]), (1920 * 1088 * 4, 4, 2048, 1920))
        s = self.surface(854, 480, tile=0)
        self.assertEqual((s[8], s[15]), (864 * 480 * 4, 864))

    def test_layout_invariants(self):
        rng = random.Random(19)
        for _ in range(200):
            dim = rng.choice([1, 2, 3, 5, 6, 7])
            depth = 6 if dim == 3 else rng.choice([1, 2, 4, 8]) if dim in (2, 5, 7) else 1
            aa = rng.randrange(4) if dim in (6, 7) else 0
            fmt = rng.choice([1, 7, 26, 0x81c, 49, 50, 51, 52, 53]) if not aa else 26
            s = self.surface(rng.randint(1, 512), rng.randint(1, 512), fmt=fmt,
                             tile=rng.choice([0, 1, 2, 4, 8] if dim >= 6 else [0, 1, 2, 4, 8, 16]),
                             mips=1 if aa else rng.randint(1, 7), depth=depth, dim=dim, aa=aa)
            self.assertGreater(s[8], 0)
            self.assertGreater(s[14], 0)
            self.assertEqual(s[14] & (s[14] - 1), 0)
            self.assertGreater(s[15], 0)
            if s[4] > 1:
                self.assertGreaterEqual(s[16], s[8])
                self.assertGreater(s[10], 0)

    def test_unsupported_linear_special_msaa(self):
        for dim in (6, 7):
            words = (ctypes.c_uint32 * 29)(dim, 64, 64, 1, 1, 26, 1, 3)
            words[12] = 16
            self.assertEqual(self.lib.gx2_surface_layout(words), 0)

    def test_aperture_roundtrip_and_slice_isolation(self):
        rng = random.Random(42)
        copy = self.lib.gx2_aperture_copy
        copy.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, ctypes.c_uint32,
                         ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int]
        for fmt in (1, 7, 26, 49, 50):
            for tile in (1, 2, 4):
                with self.subTest(fmt=fmt, tile=tile):
                    s = self.surface(32, 32, fmt=fmt, tile=tile, depth=2, dim=5)
                    words = (ctypes.c_uint32 * 29)(*s)
                    original = bytes(rng.randrange(256) for _ in range(s[8]))
                    tiled = ctypes.create_string_buffer(original)
                    linear = ctypes.create_string_buffer(s[8])
                    result = ctypes.create_string_buffer(original)
                    self.assertEqual(copy(words, 0, 1, linear, s[8], tiled, s[8], 0), 1)
                    self.assertEqual(copy(words, 0, 1, linear, s[8], result, s[8], 1), 1)
                    self.assertEqual(result.raw[:s[8]], original)
                    # Changing slice 1 must preserve slice 0 as seen by a reader.
                    ctypes.memset(linear, 0x5a, s[8])
                    self.assertEqual(copy(words, 0, 1, linear, s[8], result, s[8], 1), 1)
                    before = ctypes.create_string_buffer(s[8]); after = ctypes.create_string_buffer(s[8])
                    self.assertEqual(copy(words, 0, 0, before, s[8], tiled, s[8], 0), 1)
                    self.assertEqual(copy(words, 0, 0, after, s[8], result, s[8], 0), 1)
                    self.assertEqual(before.raw, after.raw)

    def test_linear_gpu_read_pitch_slice_and_bounds(self):
        read = self.lib.gx2_surface_read
        read.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32, ctypes.c_uint32,
                         ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32]
        for fmt, bytes_per_block, block in ((1, 1, 1), (7, 2, 1), (26, 4, 1), (49, 8, 4)):
            s = self.surface(13, 7, fmt=fmt, tile=1, depth=2, dim=5)
            words = (ctypes.c_uint32 * 29)(*s)
            info = (ctypes.c_uint32 * 8)()
            self.assertEqual(self.lib.gx2_mip_layout(words, 0, info), 1)
            stride = info[0] * bytes_per_block
            slice_size = stride * info[1]
            source = bytes((i * 31 + i // 256) % 256 for i in range(s[8]))
            tiled = ctypes.create_string_buffer(source)
            output = ctypes.create_string_buffer(slice_size)
            self.assertEqual(read(words, 0, 1, output, slice_size, tiled, s[8]), 1)
            row = ((13 + block - 1) // block) * bytes_per_block
            rows = (7 + block - 1) // block
            for y in range(rows):
                self.assertEqual(output.raw[y*stride:y*stride+row],
                                 source[slice_size+y*stride:slice_size+y*stride+row])
                self.assertEqual(output.raw[y*stride+row:(y+1)*stride], bytes(stride-row))
            self.assertEqual(read(words, 0, 1, output, slice_size, tiled, slice_size), 0)
            self.assertEqual(read(words, 0, 1, output, row-1, tiled, s[8]), 0)

    def test_big_endian_guest_bridge_and_texture_regs(self):
        memory = ctypes.create_string_buffer(0x10000)
        ctypes.c_void_p.in_dll(self.lib, 'g_mem').value = ctypes.addressof(memory)
        regs = (ctypes.c_uint32 * 32)()
        initial = [1, 64, 32, 1, 1, 26, 0, 3] + [0] * 21
        initial[12] = 16
        ctypes.memmove(ctypes.addressof(memory) + 0x1000, struct.pack('>29I', *initial), 0x74)
        def call(name, *args):
            for i, a in enumerate(args): regs[i + 3] = a
            fn = self.lib.hle_gx2_lookup(name.encode())
            self.assertTrue(fn)
            ctypes.CFUNCTYPE(None, ctypes.c_void_p)(fn)(regs)
        call('GX2CalcSurfaceSizeAndAlignment', 0x1000)
        self.assertEqual(list(struct.unpack('>29I', memory[0x1000:0x1074])), self.surface(64, 32))
        ctypes.memmove(ctypes.addressof(memory) + 0x1084, struct.pack('>I', 0x00010203), 4)
        call('GX2InitTextureRegs', 0x1000)
        word = lambda off: struct.unpack('>I', memory[off:off + 4])[0]
        self.assertEqual(word(0x1088), 1 | (7 << 8) | (63 << 19))
        self.assertEqual(word(0x108c), 31 | (26 << 26))
        self.assertEqual(word(0x1090), (2 << 14) | (1 << 19) | (2 << 22) | (3 << 25))
        call('GX2CalcTVSize', 5, 26, 3, 0x2000, 0x2004)
        self.assertEqual(word(0x2000), 1920 * 1088 * 4 * 3)
