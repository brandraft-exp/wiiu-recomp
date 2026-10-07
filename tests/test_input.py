"""GamePad ABI, neutral samples, and unavailable-channel behavior."""
import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang'), 'clang required')
class InputTests(unittest.TestCase):
    def test_window_event_delivery(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-input-events-') as work:
            binary = str(Path(work) / 'events')
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'runtime'), str(root / 'tests/input_events.c'),
                str(root / 'runtime/standalone/input.c'), '-lSDL2_ttf','-lSDL2', '-pthread', '-o', binary],
                check=True, capture_output=True, text=True)
            subprocess.run([binary], env={**os.environ, 'SDL_VIDEODRIVER': 'dummy'},
                check=True, capture_output=True, text=True, timeout=10)

    def test_samples(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-input-') as work:
            work = Path(work)
            (work / 'shim.c').write_text('#include "standalone/rt.h"\n#include <stdlib.h>\n'
                'uint8_t *g_mem;\nvoid rt_fatal(ppc_ctx *c,const char *fmt,...) {abort();}\n')
            subprocess.run(['clang', '-shared', '-fPIC', '-I'+str(root/'runtime'),
                str(root/'runtime/standalone/input.c'), str(work/'shim.c'),
                '-lSDL2_ttf','-lSDL2', '-o', str(work/'input.so')], check=True, capture_output=True)
            lib = ctypes.CDLL(str(work/'input.so'))
            mem = ctypes.create_string_buffer(4096)
            ctypes.c_void_p.in_dll(lib, 'g_mem').value = ctypes.addressof(mem)
            ctx = ctypes.create_string_buffer(1024)
            regs = (ctypes.c_uint32*32).from_buffer(ctx)
            lib.hle_input_lookup.restype = ctypes.c_void_p
            read = ctypes.CFUNCTYPE(None, ctypes.c_void_p)(lib.hle_input_lookup(b'VPADRead'))
            regs[3:7] = (0, 256, 1, 512)
            read(ctx)
            self.assertEqual(regs[3], 1)
            self.assertEqual(mem.raw[256:268], bytes(12))
            self.assertEqual(struct.unpack_from('>I',mem,512)[0],0)
            self.assertEqual(mem.raw[256+0xa1],0xc0)
            self.assertEqual(struct.unpack_from('>H',mem,256+0x58)[0],3)
            for off in (0x6c,0x7c,0x8c):
                self.assertEqual(struct.unpack_from('>f',mem,256+off)[0],1.)
            before=mem.raw[256:256+0xac]
            # No Wii Remote/Pro controllers are connected. Both probe and
            # sample APIs must report that, including initialized error slots.
            def invoke(name, *args):
                for i, arg in enumerate(args): regs[3+i] = arg
                fn = lib.hle_input_lookup(name.encode())
                self.assertTrue(fn)
                ctypes.CFUNCTYPE(None, ctypes.c_void_p)(fn)(ctx)
            # Calibration must overwrite stale stack data, especially touch
            # and validity: a phantom touch can capture a game's menu input.
            mem[600:608] = b'\xff' * 8
            invoke('VPADGetTPCalibratedPoint', 0, 600, 256+0x52)
            self.assertEqual(struct.unpack_from('>HH', mem, 604), (0, 3))
            # Raw coordinates at the display bounds; support in-place output.
            for raw, expected in (((92,3841,1,0),(0,0,1,0)),
                                  ((3975,147,1,2),(1280,720,1,2))):
                struct.pack_into('>HHHH', mem, 600, *raw)
                invoke('VPADGetTPCalibratedPoint', 0, 600, 600)
                self.assertEqual(struct.unpack_from('>HHHH', mem, 600), expected)
            for channel in range(4):
                invoke('WPADProbe', channel, 512)
                self.assertEqual(regs[3], 0xffffffff)
                self.assertEqual(struct.unpack_from('>I', mem, 512)[0], 253)
                invoke('KPADReadEx', channel, 256, 16, 512)
                self.assertEqual(regs[3], 0)
                self.assertEqual(struct.unpack_from('>i', mem, 512)[0], -5)
            invoke('KPADInitEx', 0, 0)
            for channel in range(4):
                invoke('KPADReadEx', channel, 256, 16, 512)
                self.assertEqual(regs[3], 0)
                self.assertEqual(struct.unpack_from('>i', mem, 512)[0], -2)
                self.assertEqual(mem.raw[256:256+0xac], before)
            invoke('KPADShutdown')
            invoke('KPADReadEx', 0, 256, 16, 512)
            self.assertEqual(struct.unpack_from('>i', mem, 512)[0], -5)
            regs[3:7]=(1,256,1,512)
            read(ctx)
            self.assertEqual(regs[3],0)
            self.assertEqual(struct.unpack_from('>i',mem,512)[0],-2)
            self.assertEqual(mem.raw[256:256+0xac],before)
            regs[3:7]=(0,256,0,512)
            read(ctx)
            self.assertEqual(regs[3],0)
            self.assertEqual(mem.raw[256:256+0xac],before)

            replay=work/'buttons.txt'
            replay.write_text('0 8000\n1 8000 0.5 -1\n2 0\n')
            lib.hle_input_replay(str(replay).encode())
            for expected, axes in zip(((0x8000,0x8000,0),(0x8000,0,0),(0,0,0x8000)),
                                      ((0.,0.),(0.5,-1.),(0.,0.))):
                regs[3:7]=(0,256,1,512)
                read(ctx)
                self.assertEqual(struct.unpack_from('>III',mem,256),expected)
                self.assertEqual(struct.unpack_from('>ff',mem,256+12),axes)
