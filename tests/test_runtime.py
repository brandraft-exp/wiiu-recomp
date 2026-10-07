"""Build and exercise the runtime without any RPX or generated game code."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('clang'), 'clang is required')
class RuntimeTests(unittest.TestCase):
    def test_thread_synchronization(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-runtime-test-') as work:
            exe = Path(work) / 'threads'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(root / 'runtime'), str(root / 'tests/runtime_threads.c'),
                            str(root / 'runtime/standalone/threads.c'), '-pthread', '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('tests passed', result.stdout)
