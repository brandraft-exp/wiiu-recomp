from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

@unittest.skipUnless(shutil.which('clang'), 'clang required')
class AudioTests(unittest.TestCase):
    def test_pcm_adpcm_loop_and_completion(self):
        root=Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory(prefix='wiiu-audio-') as work:
            exe=Path(work)/'audio'
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(root/'runtime'),str(root/'tests/audio.c'),
                str(root/'runtime/standalone/audio.c'),'-lSDL2','-lm','-o',str(exe)],check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,timeout=5)
