#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run host profile persistence and authenticated-password tests."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('source', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parent
source = a.source.resolve() / 'client/AROS'
with tempfile.TemporaryDirectory(prefix='rdp-profile-tests-') as temp:
    for name, files, libs in [('profile', ['profile.c'], ['-Wl,--wrap=rename']),
                              ('vault', ['vault.c', 'profile.c'], ['-lcrypto'])]:
        binary = Path(temp) / name
        subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror',
                        '-I' + str(source), str(root / (name + '-test.c')),
                        *[str(source / f) for f in files], '-o', str(binary), *libs], check=True)
        subprocess.run([str(binary)], check=True)
