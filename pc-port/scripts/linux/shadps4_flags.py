# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the compile flags and link libraries of a configured shadPS4 build (Ninja) into a CMake file.
SHADPS4_LIBRARY_FILES lists the static libraries the build has to make for libiss-gpu."""
import re
import shlex
import sys
from pathlib import Path

build = Path(sys.argv[1]).resolve()
text = (build / 'build.ninja').read_text()
block = re.search(r'^build [^\n]*vk_rasterizer\.cpp\.o:.*?\n((?:  .*\n)+)', text, re.M).group(1)
variables = dict(re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M))
link = re.search(r'^build shadps4: .*?\n((?:  .*\n)+)', text, re.M).group(1)
libraries = shlex.split(re.search(r'^  LINK_LIBRARIES = (.*)$', link, re.M).group(1))

def cmake_list(tokens):
    return ' '.join('"' + t.replace('\\', '\\\\').replace('"', '\\"') + '"' for t in tokens)

def grouped(tokens):
    result = []
    it = iter(tokens)
    for t in it:
        result.append('SHELL:' + t + ' ' + next(it) if t == '-isystem' else t)
    return result

resolved = []
for lib in libraries:
    # The bundled ffmpeg is static and not PIC; a shared object links the system one.
    if '/ffmpeg-' in lib:
        continue
    if lib.startswith('-') or lib.startswith('/'):
        resolved.append(lib)
    else:
        resolved.append(str(build / lib))
resolved += ['-lavcodec', '-lswscale', '-lavutil']
files = sorted({lib for lib in resolved if lib.startswith(str(build) + '/') and lib.endswith('.a')})
out = [
    'set(SHADPS4_LIBRARY_FILES %s)' % cmake_list(files),
    'set(SHADPS4_DEFINES %s)' % cmake_list(shlex.split(variables['DEFINES'])),
    'set(SHADPS4_FLAGS %s)' % cmake_list(shlex.split(variables['FLAGS'])),
    'set(SHADPS4_INCLUDES %s)' % cmake_list(grouped(shlex.split(variables['INCLUDES']))),
    'set(SHADPS4_LIBRARIES %s)' % cmake_list(resolved),
]
Path(sys.argv[2]).write_text('\n'.join(out) + '\n')
