"""Real accelerated SDL/EGL integration on SDL's offscreen video driver."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

class WindowBridgeTests(unittest.TestCase):
    def test_accelerated_context_handoff(self):
        root=Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-window-bridge-') as work:
            obj=str(Path(work)/'input.o');binary=str(Path(work)/'bridge')
            subprocess.run(['clang','-std=c11','-I'+str(root/'runtime'),'-c',
                str(root/'runtime/standalone/input.c'),'-o',obj],check=True,capture_output=True,text=True)
            subprocess.run(['clang++','-std=c++20','-DENABLE_OPENGL','-I'+str(root/'runtime'),
                str(root/'tests/window_bridge.cpp'),obj,
                *[str(root/'runtime/gx2'/name) for name in ('addr.cpp','coord.cpp','surface.cpp')],
                '-lfmt','-lEGL','-lGL','-lSDL2_ttf','-lSDL2','-o',binary],check=True,capture_output=True,text=True)
            subprocess.run([binary,work],env={**os.environ,'SDL_VIDEODRIVER':'offscreen','SDL_RENDER_DRIVER':'opengl'},
                check=True,capture_output=True,text=True,timeout=15)
