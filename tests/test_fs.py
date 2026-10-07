"""Exercise the FS ABI using temporary synthetic content and save roots."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang'), 'clang is required')
class FilesystemTests(unittest.TestCase):
    def test_overlay_io_saves_and_path_confinement(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-fs-test-') as work:
            work = Path(work)
            (work / 'memory.c').write_text('#include <stdint.h>\nuint8_t *g_mem;\n')
            library = work / 'fs.so'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            '-I' + str(root / 'runtime'), str(root / 'runtime/standalone/fs.c'),
                            str(work / 'memory.c'), '-o', str(library)],
                           check=True, capture_output=True, text=True)
            lib = ctypes.CDLL(str(library))
            memory = ctypes.create_string_buffer(0x100000)
            ctypes.c_void_p.in_dll(lib, 'g_mem').value = ctypes.addressof(memory)
            regs = (ctypes.c_uint32 * 32)()
            lib.hle_fs_lookup.argtypes = [ctypes.c_char_p]
            lib.hle_fs_lookup.restype = ctypes.c_void_p
            lib.hle_fs_configure.argtypes = [ctypes.c_char_p] * 3
            for name in ('base', 'update', 'save'):
                (work / name).mkdir()
            (work / 'base/shared').write_bytes(b'old')
            (work / 'base/base-only').write_bytes(b'base')
            (work / 'update/shared').write_bytes(b'updated')
            (work / 'update/update-only').write_bytes(b'new')
            (work / 'outside').write_bytes(b'private')
            (work / 'base/escape').symlink_to(work / 'outside')
            lib.hle_fs_configure(*(str(work / n).encode() for n in ('base', 'update', 'save')))

            def call(name, *args):
                for i, value in enumerate(args):
                    regs[3 + i] = value
                lookup = lib.hle_save_lookup if name.startswith('SAVE') else lib.hle_fs_lookup
                lookup.restype = ctypes.c_void_p
                addr = lookup(name.encode())
                self.assertTrue(addr, name)
                ctypes.CFUNCTYPE(None, ctypes.c_void_p)(addr)(regs)
                return ctypes.c_int32(regs[3]).value

            def put(addr, value):
                ctypes.memmove(ctypes.addressof(memory) + addr, value + b'\0', len(value) + 1)

            def word(addr):
                return struct.unpack('>I', memory[addr:addr + 4])[0]

            def open_file(path, mode=b'r'):
                put(0x1000, path); put(0x2000, mode)
                return call('FSOpenFile', 0, 0, 0x1000, 0x2000, 0x3000, 0)

            call('FSInitCmdBlock', 0x4003)
            call('FSSetUserData', 0x4003, 0x12345678)
            self.assertEqual(call('FSGetUserData', 0x4003), 0x12345678)
            self.assertEqual(open_file(b'/vol/content/shared'), 0)
            handle = word(0x3000)
            self.assertEqual(call('FSReadFile', 0, 0, 0x6000, 1, 64, handle, 0, 0), 7)
            self.assertEqual(memory[0x6000:0x6007], b'updated')
            self.assertEqual(call('FSReadFile', 0, 0, 0x6000, 1, 64, handle, 0, 0), 0)
            self.assertEqual(call('FSReadFileWithPos', 0, 0, 0x6000, 1, 3, 2, handle, 0), 3)
            self.assertEqual(memory[0x6000:0x6003], b'dat')
            self.assertEqual(call('FSGetPosFile', 0, 0, handle, 0x7000), 0)
            self.assertEqual(word(0x7000), 5)
            self.assertEqual(call('FSGetStatFile', 0, 0, handle, 0x7000), 0)
            self.assertEqual(word(0x7010), 7)
            self.assertEqual(call('FSCloseFile', 0, 0, handle), 0)
            self.assertEqual(open_file(b'/vol/content/base-only'), 0)
            self.assertEqual(call('FSCloseFile', 0, 0, word(0x3000)), 0)
            self.assertEqual(open_file(b'/vol/content/missing'), -6)
            self.assertEqual(open_file(b'/vol/content/shared', b'w'), -10)
            self.assertEqual(open_file(b'/vol/content/../outside'), -10)
            self.assertEqual(open_file(b'/vol/content/escape'), -10)
            put(0x1000, b'/vol/content')
            self.assertEqual(call('FSOpenDir', 0, 0, 0x1000, 0x3000), 0)
            directory = word(0x3000); names = []
            while True:
                result = call('FSReadDir', 0, 0, directory, 0x7000)
                if result == -2:
                    break
                self.assertEqual(result, 0)
                names.append(memory[0x7064:0x70e4].split(b'\0')[0])
            self.assertEqual(names, [b'base-only', b'shared', b'update-only'])
            call('FSCloseDir', 0, 0, directory)
            self.assertEqual(open_file(b'/vol/save/state.bin', b'w+'), 0)
            handle = word(0x3000); put(0x6000, b'save-data')
            self.assertEqual(call('FSWriteFile', 0, 0, 0x6000, 1, 9, handle, 0, 0), 9)
            self.assertEqual(call('FSFlushFile', 0, 0, handle), 0)
            self.assertEqual(call('FSSetPosFile', 0, 0, handle, 0), 0)
            self.assertEqual(call('FSReadFile', 0, 0, 0x7000, 1, 9, handle, 0, 0), 9)
            self.assertEqual(memory[0x7000:0x7009], b'save-data')
            call('FSCloseFile', 0, 0, handle)
            self.assertEqual((work / 'save/state.bin').read_bytes(), b'save-data')
            put(0x1000, b'/vol/save/subdir')
            self.assertEqual(call('FSMakeDir', 0, 0, 0x1000), 0)
            self.assertEqual(call('FSRemove', 0, 0, 0x1000), 0)
            put(0x1000, b'/vol/save/state.bin')
            self.assertEqual(call('FSRemove', 0, 0, 0x1000), 0)
            self.assertFalse((work / 'save/state.bin').exists())

            self.assertEqual(call('SAVEInitSaveDir',1),0)
            self.assertEqual(call('SAVEInitSaveDir',255),0)
            self.assertEqual(call('SAVEInitSaveDir',0),-6)
            put(0x1000,b'system');put(0x2000,b'w+')
            self.assertEqual(call('SAVEGetStat',0,0,1,0x1000,0x7000,0),-6)
            self.assertEqual(call('SAVEOpenFile',0,0,1,0x1000,0x2000,0x3000,0),0)
            handle=word(0x3000)
            put(0x6000,b'account-save')
            self.assertEqual(call('FSWriteFile',0,0,0x6000,1,12,handle,0,0),12)
            self.assertEqual(call('SAVEFlushQuota',0,0,1,0),0)
            self.assertEqual(call('FSCloseFile',0,0,handle),0)
            self.assertEqual((work/'save/80000001/system').read_bytes(),b'account-save')
            self.assertEqual(call('SAVEGetStat',0,0,255,0x1000,0x7000,0),-6)
            put(0x1000,b'../common/escape')
            self.assertEqual(call('SAVEOpenFile',0,0,1,0x1000,0x2000,0x3000,0),-10)
            self.assertEqual(call('SAVEGetFreeSpaceSize',0,0,1,0x7000,0),0)
            self.assertGreater(struct.unpack_from('>Q',memory,0x7000)[0],0)
