# Copyright 2014 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""A wrapper for the gyp_main that ensures the appropriate include directories
are brought in.
"""

import os
import sys


def apply_gyp_environment_from_file(file_path):
  """Reads in a *.gyp_env file and applies the valid keys to os.environ."""
  if not os.path.exists(file_path):
    return
  with open(file_path, 'rU') as f:
    file_contents = f.read()
  try:
    file_data = eval(file_contents, {'__builtins__': None}, None)
  except SyntaxError, e:
    e.filename = os.path.abspath(file_path)
    raise
  supported_vars = (
      'GYP_DEFINES',
      'GYP_GENERATOR_FLAGS',
      'GYP_GENERATORS',
      'GYP_MSVS_VERSION',
  )
  for var in supported_vars:
    file_val = file_data.get(var)
    if file_val:
      if var in os.environ:
        print 'INFO: Environment value for "%s" overrides value in %s.' % (
            var, os.path.abspath(file_path)
        )
      else:
        os.environ[var] = file_val


def apply_syzygy_gyp_env(syzygy_src_path):
  if 'SKIP_SYZYGY_GYP_ENV' not in os.environ:
    # Update the environment based on syzygy.gyp_env
    path = os.path.join(syzygy_src_path, 'syzygy.gyp_env')
    apply_gyp_environment_from_file(path)


if __name__ == '__main__':
  # Get the path of the root 'src' directory.
  self_dir = os.path.abspath(os.path.dirname(__file__))
  src_dir = os.path.abspath(os.path.join(self_dir, '..', '..'))

  apply_syzygy_gyp_env(src_dir)

  # Get the path to src/build. This contains a bunch of gyp
  # 'plugins' that get called by common.gypi and base.gyp.
  build_dir = os.path.join(src_dir, 'build')

  # Get the path to the downloaded version of gyp.
  gyp_dir = os.path.join(src_dir, 'tools', 'gyp')

  # Get the path to the gyp module directoy, and the gyp_main
  # that we'll defer to.
  gyp_pylib = os.path.join(gyp_dir, 'pylib')
  gyp_main = os.path.join(gyp_dir, 'gyp_main.py')

  # Ensure the gyp plugin and module directories are in the module path
  # before passing execution to gyp_main.
  sys.path.append(gyp_pylib)
  sys.path.append(build_dir)
  execfile(gyp_main)
