#!/usr/bin/env python3
"""Compatibility entry point for the shared minimal FFmpeg builder."""
from pathlib import Path
import subprocess
import sys

if __name__ == "__main__":
    # --qt-root belonged to the old headers-only builder. Preserve old callers
    # while never selecting Qt's full FFmpeg runtime.
    arguments = sys.argv[1:]
    if "--qt-root" in arguments:
        index = arguments.index("--qt-root")
        del arguments[index:index + 2]
    subprocess.run([sys.executable, str(Path(__file__).with_name("prepare-video.py")),
                    "--platform", "windows", *arguments], check=True)
