"""Compile and execute the portable production relative-motion policy."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='motion-coherence-') as tmp:
    binary = str(Path(tmp) / 'motion')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined,float-cast-overflow',
                    '-I' + str(root / 'Limelight/Input'),
                    str(root / 'tests/input-coherence/motion.c'), '-lm', '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=30)
