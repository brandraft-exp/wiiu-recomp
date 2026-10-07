from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

@unittest.skipUnless(shutil.which('clang'), 'clang required')
class AlarmTests(unittest.TestCase):
    def test_deadlines_periodic_and_cancel(self):
        root=Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-alarms-') as work:
            exe=Path(work)/'alarms'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(root/'runtime'),str(root/'tests/alarms.c'),
                str(root/'runtime/standalone/alarms.c'),'-o',str(exe)],check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,timeout=5)
