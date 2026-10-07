from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

@unittest.skipUnless(shutil.which('clang++'), 'clang++ is required')
class CommandTests(unittest.TestCase):
    def test_register_packets_and_indirection(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-pm4-test-') as work:
            exe = Path(work) / 'commands'
            subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root / 'runtime'), str(root / 'tests/gpu_commands.cpp'),
                str(root / 'runtime/gx2/commands.cpp'), '-o', str(exe)],
                check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=5)
