"""Software-keyboard guest ABI and receiver callbacks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang'), 'clang required')
class KeyboardTests(unittest.TestCase):
    def test_guest_dialog(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-swkbd-') as work:
            binary = str(Path(work) / 'keyboard')
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'runtime'), str(root / 'tests/swkbd.c'),
                str(root / 'runtime/standalone/swkbd.c'), '-o', binary],
                check=True, capture_output=True, text=True)
            subprocess.run([binary], check=True, capture_output=True, text=True, timeout=10)
