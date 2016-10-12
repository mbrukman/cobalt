#!/usr/bin/env python
# Copyright 2016 The Fuchsia Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Runs cpp lint on all of Cobalt's C++ files."""

import os
import shutil
import subprocess
import sys

THIS_DIR = os.path.dirname(__file__)
SRC_ROOT_DIR = os.path.abspath(os.path.join(THIS_DIR, os.pardir))
CPP_LINT = os.path.join(SRC_ROOT_DIR, 'third_party', 'cpplint', 'cpplint.py')

# A list of directories which should be skipped while walking the directory
# tree looking for C++ files to be linted. We also skip directories starting
# with a "." such as ".git"
SKIP_LINT_DIRS = [
    os.path.join(SRC_ROOT_DIR, 'docker'),
    os.path.join(SRC_ROOT_DIR, 'out'),
    os.path.join(SRC_ROOT_DIR, 'prototype'),
    os.path.join(SRC_ROOT_DIR, 'shuffler'),
    os.path.join(SRC_ROOT_DIR, 'third_party'),
    os.path.join(SRC_ROOT_DIR, 'tools'),
]

def main():
  print "\nLinting C++ files...\n"
  for root, dirs, files in os.walk(SRC_ROOT_DIR):
    for f in files:
      if f.endswith('.h') or f.endswith('.cc'):
        full_path = os.path.join(root, f)
        subprocess.call([CPP_LINT,  full_path])
        print

    # Before recursing into directories remove the ones we want to skip.
    dirs_to_skip = [dir for dir in dirs if dir.startswith(".") or
        os.path.join(root, dir) in SKIP_LINT_DIRS]
    for d in dirs_to_skip:
      dirs.remove(d)

if __name__ == '__main__':
  main()