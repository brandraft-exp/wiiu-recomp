"""Decode a generated red frame through the guest H264 ABI and callback."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

@unittest.skipUnless(shutil.which('clang') and shutil.which('ffmpeg'), 'clang and ffmpeg required')
class VideoTests(unittest.TestCase):
    def test_nv12_callback(self):
        root=Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-video-') as work:
            work=Path(work)
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=red:s=32x32',
                '-frames:v','1','-c:v','libx264','-bf','2','-f','h264',str(work/'red.h264')],check=True,capture_output=True)
            (work/'shim.c').write_text('#include "standalone/rt.h"\n#include <stdlib.h>\n'
                'uint8_t *g_mem; uint32_t callback_arg, written_address, written_size;\n'
                'void hle_gpu_memory_written(uint32_t a,uint32_t s){written_address=a;written_size=s;if(!g_mem[a])abort();}\n'
                'void rt_fatal(ppc_ctx*c,const char*f,...){abort();}\n'
                'uint32_t hle_sys_alloc(uint32_t size){return 0x8000;}\n'
                'void ppc_call_indirect(ppc_ctx*c,uint32_t a){if(a!=0x1234)abort();callback_arg=c->r[3];}\n')
            subprocess.run(['clang','-shared','-fPIC','-I'+str(root/'runtime'),
                str(root/'runtime/standalone/h264.c'),str(work/'shim.c'),
                '-lavcodec','-lavutil','-lswscale','-o',str(work/'video.so')],check=True,capture_output=True)
            lib=ctypes.CDLL(str(work/'video.so'));lib.hle_h264_lookup.restype=ctypes.c_void_p
            mem=ctypes.create_string_buffer(0x100000);ctypes.c_void_p.in_dll(lib,'g_mem').value=ctypes.addressof(mem)
            ctx=ctypes.create_string_buffer(1024);regs=(ctypes.c_uint32*32).from_buffer(ctx)
            floats=(ctypes.c_double*32).from_buffer(ctx,128)
            def call(name,*args):
                for i,a in enumerate(args):regs[3+i]=a
                ctypes.CFUNCTYPE(None,ctypes.c_void_p)(lib.hle_h264_lookup(name.encode()))(ctx)
                return regs[3]
            def word(a):return struct.unpack_from('>I',mem,a)[0]
            self.assertEqual(call('H264DECMemoryRequirement',100,10,32,32,256),0)
            self.assertEqual(word(256),(0x18c<<10)+0x447)
            self.assertEqual(call('H264DECInitParam',1024,0x1000),0)
            self.assertEqual(call('H264DECOpen',0x1000),0)
            self.assertEqual(call('H264DECSetParam_FPTR_OUTPUT',0x1000,0x1234),0)
            self.assertEqual(call('H264DECSetParam_OUTPUT_PER_FRAME',0x1000,1),0)
            struct.pack_into('>I',mem,256,0xabcdef)
            call('H264DECSetParam_USER_MEMORY',0x1000,256)
            call('H264DECBegin',0x1000)
            data=(work/'red.h264').read_bytes();ctypes.memmove(ctypes.addressof(mem)+0x10000,data,len(data));floats[1]=0.25
            call('H264DECSetBitstream',0x1000,0x10000,len(data))
            self.assertEqual(call('H264DECExecute',0x1000,0x20000),0xe4)
            a=ctypes.c_uint32.in_dll(lib,'callback_arg').value;self.assertTrue(a)
            self.assertEqual((word(a),word(a+8)),(1,0xabcdef))
            result=word(word(a+4));self.assertEqual(word(result),100)
            self.assertEqual(struct.unpack_from('>d',mem,result+8)[0],0.25)
            self.assertEqual((word(result+16),word(result+20),word(result+24)),(32,32,256))
            self.assertEqual(word(result+0x44),0x20000)
            self.assertEqual(ctypes.c_uint32.in_dll(lib,'written_address').value,0x20000)
            self.assertEqual(ctypes.c_uint32.in_dll(lib,'written_size').value,256*32*3//2)
            self.assertAlmostEqual(mem.raw[0x20000],81,delta=2)
            self.assertAlmostEqual(mem.raw[0x22000],90,delta=2)
            self.assertAlmostEqual(mem.raw[0x22001],240,delta=2)
            self.assertEqual(call('H264DECEnd',0x1000),0)
            self.assertEqual(call('H264DECClose',0x1000),0)
