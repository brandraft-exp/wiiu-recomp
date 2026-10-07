"""Exercise native GL depth, uniform banks, lookup textures and surface copies."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class BackendTests(unittest.TestCase):
    def test_render_state(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-render-test-') as work:
            binary = str(Path(work) / 'render')
            subprocess.run(['clang++', '-std=c++20', '-DENABLE_OPENGL', '-I' + str(root / 'runtime'),
                str(root / 'tests/render_backend.cpp'),
                str(root / 'runtime/gx2/decompiler/FetchShader.cpp'),
                *[str(root / 'runtime/gx2' / name) for name in ('addr.cpp', 'coord.cpp', 'surface.cpp')],
                '-lfmt', '-lEGL', '-lGL', '-o', binary], check=True, capture_output=True, text=True)
            subprocess.run([binary, work], check=True, capture_output=True, text=True)
