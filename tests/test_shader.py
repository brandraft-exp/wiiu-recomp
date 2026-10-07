"""Compile synthetic Latte shaders to GLSL and validate with the real frontend."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang++') and shutil.which('glslangValidator'),
                     'clang++ and glslangValidator are required')
class ShaderTests(unittest.TestCase):
    def test_constant_shaders(self):
        root = Path(__file__).resolve().parent.parent
        source = root / 'runtime/gx2/decompiler'
        with tempfile.TemporaryDirectory(prefix='wiiu-shader-test-') as work:
            work = Path(work)
            subprocess.run(['clang++', '-std=c++20', '-DENABLE_OPENGL', '-I' + str(source),
                *map(str, sorted(source.glob('*.cpp'))), str(root / 'tests/shader_translate.cpp'),
                str(root / 'tests/shader_render.cpp'), '-lfmt', '-lEGL', '-lGL', '-o', str(work / 'translate')], check=True, capture_output=True, text=True)
            subprocess.run([str(work / 'translate'), str(work)], check=True, capture_output=True, text=True)
            for name in ('constant.vert', 'constant.frag'):
                subprocess.run(['glslangValidator', str(work / name)], check=True, capture_output=True, text=True)
            self.assertIn('passPixelColor0', (work / 'constant.frag').read_text())
            self.assertIn('gl_Position', (work / 'constant.vert').read_text())
