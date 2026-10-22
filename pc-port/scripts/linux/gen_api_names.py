# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes api_names.h, PS4 import NIDs -> API names sorted by NID, from shadPS4's aerolib.inl
(GPL-2.0+). Run by CMake at build time; it needs no game files.
   gen_api_names.py <aerolib.inl> <api_names.h>"""
import json
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding='utf-8')
names = dict(re.findall(r'STUB\(\s*"([^"]+)"\s*,\s*([^\s,)]+)\s*\)', text))
body = ''.join('{' + json.dumps(k) + ',' + json.dumps(v) + '},\n' for k, v in sorted(names.items()))
out = Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('// Generated from shadPS4 src/core/aerolib/aerolib.inl (GPL-2.0+).\n'
               'static const struct { const char *nid, *api; } api_names[] = {\n' + body + '};\n',
               encoding='utf-8')
