#!/usr/bin/env python3
"""Build and run host protocol/signature checks. Does not contact a board."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(command):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'), help='C++ compiler executable')
    args = parser.parse_args()
    if not args.skip_build:
        run([sys.executable, '-m', 'platformio', 'run', '-e', 'minimal_agent', '-j', '1'])
    includes = ROOT / '.pio/libdeps/minimal_agent/ArduinoJson/src'
    if not includes.is_dir():
        parser.error('Build first to install the pinned ArduinoJson dependency')
    if not shutil.which(args.cxx):
        parser.error('Install g++ or pass --cxx /path/to/compiler')
    if not shutil.which('openssl'):
        parser.error('Install OpenSSL CLI for the official framework signer comparison')
    build = ROOT / 'tests/build'
    build.mkdir(parents=True, exist_ok=True)
    executable = build / ('minimal_protocol_test.exe' if os.name == 'nt' else 'minimal_protocol_test')
    flags = ['-std=c++11', '-Wall', '-Wextra', '-Werror']
    if os.name == 'nt':
        flags += ['-static', '-static-libgcc', '-static-libstdc++']
    run([args.cxx, *flags, '-I' + str(includes), '-I' + str(ROOT / 'src'),
         ROOT / 'tests/minimal_protocol_test.cpp', '-o', executable])
    run([executable])
    run([sys.executable, ROOT / 'tests/minimal_package_test.py'])


if __name__ == '__main__':
    main()
