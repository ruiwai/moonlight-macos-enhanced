"""Compile and execute the portable production SDL2/Qt macOS wheel policy."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='wheel-coherence-') as tmp:
    binary = str(Path(tmp) / 'wheel')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all',
                    '-I' + str(root / 'Limelight/Input'),
                    str(root / 'tests/input-coherence/wheel.c'), '-lm', '-o', binary], check=True)
    subprocess.run([binary], check=True, timeout=30)
