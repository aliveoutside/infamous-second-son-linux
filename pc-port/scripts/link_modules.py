# SPDX-License-Identifier: GPL-2.0-or-later
"""Link the user's inFAMOUS eboot/libc/Fios2 by exact, versioned PS4 identity.

Format ISSPRB02 extends the entry probe with module/TLS metadata and native bindings.
All changes affect only the generated workspace image, never original executables.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
import os
from pathlib import Path
import struct

from prepare import ROOT, DEFAULT_GAME, EXPECTED_EBOOT, module, identity_key, names_database
from ps4_format import span

def align(value, alignment=65536):
    return (value + alignment - 1) & -alignment

def build_linked(modules):
    image = bytearray()
    segments, metadata, exports, relocs, imports = [], [], {}, [], {}
    hints, _ = names_database()
    rows, bases = [], []
    import_kinds = {}
    for index, m in enumerate(modules):
        base = align(len(image))
        bases.append(base)
        loads = [p for p in m['ph'] if p['type'] in (1, 0x61000010)]
        size = max(p['vaddr'] + p['memsz'] for p in loads)
        if base + size > 512 * 1024 * 1024:
            raise ValueError('linked image exceeds 512 MiB')
        image.extend(bytes(base + size - len(image)))

        def mapped(address, length=8):
            return any(p['vaddr'] <= address and address + length <= p['vaddr'] + p['memsz'] for p in loads)

        for p in loads:
            if p['filesz'] > p['memsz'] or p['vaddr'] % 4096 or p['flags'] > 7:
                raise ValueError('invalid load segment')
            image[base + p['vaddr']:base + p['vaddr'] + p['filesz']] = span(m['elf'], p['offset'], p['filesz'])
            segments.append((base + p['vaddr'], p['memsz'], p['flags']))
        tls = next((p for p in m['ph'] if p['type'] == 7), None)
        tls_values = (base + tls['vaddr'], tls['filesz'], tls['memsz'], tls['align']) if tls else (0, 0, 0, 1)
        if tls and (tls['filesz'] > tls['memsz'] or tls['memsz'] > 1024 * 1024 or
                    not mapped(tls['vaddr'], tls['memsz']) or tls['align'] < 1 or
                    tls['align'] > 4096 or tls['align'] & (tls['align'] - 1)):
            raise ValueError('invalid TLS layout')
        init = base + m['tags'][12] if 12 in m['tags'] else 0xffffffffffffffff
        metadata.append((base, size, init, *tls_values, index + 1))
        for symbol in m['symbols']:
            key = identity_key(symbol)
            if key and symbol['section'] and symbol['binding'] in (1, 2) and symbol['type'] in (1, 2):
                if not mapped(symbol['value'], max(1, symbol['size'])) or key in exports:
                    raise ValueError('invalid or ambiguous native export')
                exports[key] = (base + symbol['value'], 2 if symbol['type'] == 1 else 1, index)
        counts = Counter()
        for target, kind, symid, addend in m['relocs']:
            if not mapped(target):
                raise ValueError('relocation outside module')
            target += base
            counts[kind] += 1
            symbol = m['symbols'][symid]
            if kind == 8:
                if not 0 <= addend < size:
                    raise ValueError('relative relocation outside module')
                relocs.append((target, 0, base + addend, 0))
            elif kind == 16:
                if symbol['name'] or symbol['type'] != 3 or addend or not tls:
                    raise ValueError('unsupported external TLS module relocation')
                struct.pack_into('<Q', image, target, index + 1)
            elif kind in (1, 6, 7):
                if symbol['section']:
                    value = symbol['value'] + addend
                    if symbol['section'] == 0xfff1:
                        struct.pack_into('<Q', image, target, value)
                    else:
                        if not 0 <= value < size:
                            raise ValueError('defined relocation outside module')
                        relocs.append((target, 0, base + value, 0))
                elif symbol['binding'] == 2 and not symbol['identity']:
                    struct.pack_into('<Q', image, target, 0)
                else:
                    key = identity_key(symbol)
                    if not key or symbol['type'] not in (1, 2):
                        raise ValueError('unsupported undefined symbol')
                    if key not in imports:
                        imports[key] = len(imports)
                    rk = 2 if symbol['type'] == 1 else 1
                    if key in import_kinds and import_kinds[key] != rk:
                        raise ValueError('conflicting import types for the same identity')
                    import_kinds[key] = rk
                    if (rk == 1 and addend) or not 0 <= addend < 4096:
                        raise ValueError('unsupported import addend')
                    relocs.append((target, rk, imports[key], addend))
                    rows.append(dict(index=imports[key], owner=Path(m['path']).name,
                                     original_symbol=symbol['name'], symbol='#'.join(map(str, key)),
                                     nid=key[0], library=key[1], library_version=key[2],
                                     module=key[3], module_version=key[4], symbol_type=symbol['type'],
                                     api_hint=hints.get(key[0], ''), status='unresolved'))
            else:
                raise ValueError(f'unsupported relocation {kind}')
    bindings = []
    compatibility_aliases = []
    for key, i in imports.items():
        export = exports.get(key)
        alias = False
        # Explicit SysV C vprintf compatibility: same NID and signature, using
        # the initialized native libc. Do not generalize by NID to other APIs.
        if not export and key[1:] == ('libSceLibcInternal',1,'libSceLibcInternal',257) and hints.get(key[0]) in ('vprintf','fflush','_Stdout'):
            export = exports.get((key[0],'libc',1,'libc',257))
            alias = export is not None
            if alias: compatibility_aliases.append('#'.join(map(str,key)))
        if export:
            if export[1] != import_kinds[key]:
                raise ValueError('native export/import type mismatch')
            bindings.append((i, export[0], export[1]))
            for row in rows:
                if row['index'] == i:
                    row['status'] = ('native_compatibility_alias:' if alias else 'native:') + Path(modules[export[2]]['path']).name
    # No guessed libSceLibcInternal aliases: mismatched identities remain traps.
    main = modules[0]
    procparam = next(p for p in main['ph'] if p['type'] == 0x61000001)['vaddr']
    names = ['#'.join(map(str, key)) for key in imports]
    raw = bytearray(struct.pack('<8s5Q', b'ISSPRB02', len(image), main['header'][4], len(segments), len(relocs), len(names)))
    raw.extend(struct.pack('<3Q', len(metadata), len(bindings), procparam))
    for row in metadata:
        raw.extend(struct.pack('<8Q', *row))
    for row in bindings:
        raw.extend(struct.pack('<3Q', *row))
    for row in segments:
        raw.extend(struct.pack('<3Q', *row))
    for name in names:
        b = name.encode('ascii')
        if len(b) >= 128:
            raise ValueError('canonical import identity too long')
        raw.extend(b.ljust(128, b'\0'))
    for row in relocs:
        raw.extend(struct.pack('<4Q', *row))
    raw.extend(image)
    return raw, dict(image_size=len(image), entry=hex(main['header'][4]), segments=len(segments), compatibility_aliases=compatibility_aliases,
                     relocations=len(relocs), imports=len(names), bindings=len(bindings),
                     module_layout=[dict(file=Path(m['path']).name, base=hex(row[0]), size=row[1],
                                         init=hex(row[2]), tls_address=hex(row[3]), tls_filesz=row[4],
                                         tls_memsz=row[5], tls_align=row[6], tls_id=row[7])
                                    for m, row in zip(modules, metadata)]), rows

def has_sse4a():
    try:
        return any(l.startswith('flags') and ' sse4a' in l for l in open('/proc/cpuinfo'))
    except OSError:
        return True

def link(game, out):
    out = out.resolve()
    out.relative_to(ROOT.resolve())
    if out == ROOT.resolve() or out == game.resolve() or game.resolve() in out.parents:
        raise ValueError('output must be isolated inside pc-port')
    modules = [module(game / 'eboot.bin')] + [module(game / 'sce_module' / name)
                                            for name in ('libc.prx', 'libSceFios2.prx')]
    if modules[0]['sha256'] != EXPECTED_EBOOT:
        raise ValueError('unsupported eboot identity')
    raw, report, rows = build_linked(modules)
    if not has_sse4a():
        from sse4a_fix import patch_linked_image
        raw, report['sse4a_extrq_rewritten'] = patch_linked_image(raw)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'boot-linked.bin').write_bytes(raw)
    report.update(sha256=hashlib.sha256(raw).hexdigest(),
                  sources=[{'path': m['path'], 'sha256': m['sha256']} for m in modules],
                  status='linked_prepared_only', weak_undefined_policy='zero only unencoded weak symbols',
                  identity_policy='Exact identities plus explicit native libc stdio compatibility aliases, recorded in compatibility_aliases')
    with (out / 'linked-imports.csv').open('w', newline='', encoding='utf-8-sig') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (out / 'link.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=DEFAULT_GAME)
    parser.add_argument('--out', type=Path, default=ROOT / 'out')
    args = parser.parse_args()
    try:
        link(args.game, args.out)
    except (ValueError, OSError, KeyError, IndexError, StopIteration, struct.error) as e:
        parser.exit(1, f'Linking blocked: {e}\n')
