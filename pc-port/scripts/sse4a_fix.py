#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rewrites the 96 register-form SSE4A EXTRQ instructions of the 1.07 eboot for CPUs without SSE4A.

EXTRQ is an AMD instruction (the PS4's Jaguar has it); Intel CPUs raise #UD on it. Each site
becomes a jump to a stub in the zero padding at the end of the text segment. The stub steps over
the SysV red zone, calls a helper that does the EXTRQ with BMI2 (no flags touched) and jumps back.
A site shorter than the 5-byte jump also moves the instruction after it into its stub.

The sites are listed for the one supported eboot (found with a disassembler, none is a branch
target); each is checked against the image bytes first, so any other image is refused.
link_modules.py calls patch_linked_image when the CPU has no SSE4A (or ISS_SSE4A_FIX=1).
Idea and first version: Castlesea/infamous-second-son-pc-port#38.
"""
import struct

IMAGE_SIZE = 0x823854D
TEXT_END = 0xCC55B8
CAVE, CAVE_END = 0xCBF000, 0xCC0600

# (eboot offset, EXTRQ length, xmm dst, xmm src, the EXTRQ bytes + the moved instruction's bytes)
SITES = (
    (0x890a77, 4, 1, 7, '660f79cfc4c1796ff8'),
    (0x890a84, 4, 5, 4, '660f79ecc55162c1'),
    (0x890a9f, 4, 1, 4, '660f79ccc5f96fee'),
    (0x890aa7, 4, 5, 3, '660f79ebc5d162e9'),
    (0x890c8e, 5, 5, 15, '66410f79ef'),
    (0x890c9c, 4, 7, 0, '660f79f8c5c162c5'),
    (0x890cbb, 4, 5, 0, '660f79e8c4c1796ff9'),
    (0x890ccd, 4, 7, 0, '660f79f8c5c162ed'),
    (0x890ce3, 4, 7, 0, '660f79f8c5f828842490020000'),
    (0x890cf0, 4, 0, 3, '660f79c3c57962ef'),
    (0x890d20, 5, 3, 10, '66410f79da'),
    (0x890d2e, 4, 0, 4, '660f79c4c57962cb'),
    (0x890df9, 5, 0, 15, '66410f79c7'),
    (0x890e15, 4, 1, 3, '660f79cbc5f162c0'),
    (0x890e2b, 5, 1, 10, '66410f79ca'),
    (0x890e3d, 4, 3, 4, '660f79dcc5e162c9'),
    (0x8910b3, 4, 0, 3, '660f79c3c5f96fde'),
    (0x8910bb, 4, 3, 7, '660f79dfc5e162d8'),
    (0x8910d6, 4, 0, 7, '660f79c7c5f96ffc'),
    (0x8910de, 4, 4, 5, '660f79e5c5d962e0'),
    (0x89137d, 4, 0, 1, '660f79c1c5f8288c24d0020000'),
    (0x89138a, 4, 1, 3, '660f79cbc5f162c8'),
    (0x8913aa, 4, 0, 3, '660f79c3c5f8289c2490020000'),
    (0x8913b7, 5, 3, 10, '66410f79da'),
    (0x8913ce, 4, 0, 3, '660f79c3c5f96fda'),
    (0x8913df, 4, 3, 4, '660f79dcc5e162d8'),
    (0x8913f5, 4, 0, 4, '660f79c4c4c1796fe5'),
    (0x89140c, 4, 4, 6, '660f79e6c5d962c0'),
    (0x8914d7, 4, 0, 1, '660f79c1c5796fa424f0000000'),
    (0x8914f2, 4, 1, 3, '660f79cbc5f162c0'),
    (0x891511, 4, 1, 3, '660f79cbc441796fce'),
    (0x89151f, 5, 3, 10, '66410f79da'),
    (0x8917ea, 4, 0, 3, '660f79c3c4c1796fd2'),
    (0x8917f3, 5, 2, 9, '66410f79d1'),
    (0x891807, 5, 0, 15, '66410f79c7'),
    (0x891811, 5, 2, 14, '66410f79d6'),
    (0x891827, 4, 0, 3, '660f79c3c5f8289424c0020000'),
    (0x891834, 5, 2, 9, '66410f79d1'),
    (0x891846, 5, 2, 15, '66410f79d7'),
    (0x891854, 5, 3, 14, '66410f79de'),
    (0x891958, 4, 6, 0, '660f79f0c5f828e5'),
    (0x891960, 4, 4, 0, '660f79e0c4e3210fc304'),
    (0x89196a, 5, 10, 0, '66440f79d0'),
    (0x891978, 4, 7, 0, '660f79f8c5b0c6942460020000cc'),
    (0x89199e, 4, 1, 6, '660f79cec5f828ac2490020000'),
    (0x8919ab, 4, 5, 3, '660f79ebc5d162c9'),
    (0x891a10, 4, 7, 6, '660f79fec5f828b424f0020000'),
    (0x891a1d, 4, 6, 3, '660f79f3c5c962df'),
    (0x8938f1, 4, 4, 7, '660f79e7c4c1796ff3'),
    (0x8938fa, 4, 6, 0, '660f79f0c5c962e4'),
    (0x89391f, 4, 6, 0, '660f79f0c5796f842410030000'),
    (0x893931, 5, 7, 14, '66410f79fe'),
    (0x893b2d, 4, 3, 6, '660f79dec5f828b42450020000'),
    (0x893b3a, 4, 6, 0, '660f79f0c5c962c3'),
    (0x893b56, 4, 3, 0, '660f79d8c4c1796ff2'),
    (0x893b68, 4, 6, 0, '660f79f0c54962db'),
    (0x893b7e, 4, 3, 0, '660f79d8c578288c2410020000'),
    (0x893b90, 5, 6, 14, '66410f79f6'),
    (0x893bbb, 5, 4, 14, '66410f79e6'),
    (0x893bc9, 4, 0, 5, '660f79c5c57962d4'),
    (0x893c8b, 4, 0, 1, '660f79c1c5f8288c2430020000'),
    (0x893ca1, 4, 1, 3, '660f79cbc5f162c8'),
    (0x893cb2, 5, 0, 14, '66410f79c6'),
    (0x893cbc, 4, 3, 5, '660f79ddc5e162c0'),
    (0x893e60, 4, 0, 5, '660f79c5c5f96fe7'),
    (0x893e68, 4, 4, 1, '660f79e1c5d962e0'),
    (0x893e83, 4, 0, 1, '660f79c1c5f96ffa'),
    (0x893e8b, 4, 7, 6, '660f79fec5c162c0'),
    (0x893fa7, 4, 3, 0, '660f79d8c5f828842400030000'),
    (0x893fbd, 4, 0, 4, '660f79c4c5f962db'),
    (0x893fd7, 4, 0, 4, '660f79c4c5f828a424e0020000'),
    (0x893fe4, 4, 4, 6, '660f79e6c5d962c0'),
    (0x89404b, 4, 6, 3, '660f79f3c4e3110fd104'),
    (0x89405e, 4, 0, 2, '660f79c2c5f962f6'),
    (0x894075, 5, 7, 10, '66410f79fa'),
    (0x894083, 4, 0, 1, '660f79c1c5f962c7'),
    (0x8940f0, 4, 5, 3, '660f79ebc57828842400030000'),
    (0x8940fd, 5, 8, 2, '66440f79c2'),
    (0x89412c, 5, 0, 10, '66410f79c2'),
    (0x89413a, 4, 5, 1, '660f79e9c5d162c0'),
    (0x89436a, 4, 0, 1, '660f79c1c5f96fcc'),
    (0x894372, 4, 1, 3, '660f79cbc5f162d8'),
    (0x8943d9, 4, 5, 1, '660f79e9c5f96ff2'),
    (0x8943e5, 4, 6, 7, '660f79f7c5c962ed'),
    (0x8944a9, 4, 3, 5, '660f79ddc5f96fec'),
    (0x8944b1, 4, 5, 1, '660f79e9c5d162db'),
    (0x89450f, 4, 5, 1, '660f79e9c5f96ff8'),
    (0x894517, 5, 7, 14, '66410f79fe'),
    (0x894618, 4, 0, 5, '660f79c5c5f96fec'),
    (0x894629, 4, 5, 7, '660f79efc5d162e8'),
    (0x89463f, 4, 0, 7, '660f79c7c4c1796ffc'),
    (0x894651, 4, 7, 6, '660f79fec5c162c0'),
    (0x8946bb, 4, 1, 0, '660f79c8c5f8288424e0010000'),
    (0x8946c8, 4, 4, 0, '660f79e0c5e1ebac2440020000'),
    (0x894757, 5, 11, 0, '66440f79d8'),
    (0x89475c, 5, 12, 14, '66450f79e6'),
)


def rex(w, r):
    return bytes([0x40 | (8 if w else 0) | (4 if r else 0)])


def extrq(dst, src):
    """EXTRQ xmmD,xmmS: 66 [REX] 0F 79 /r."""
    prefix = bytes([0x40 | (4 if dst >= 8 else 0) | (1 if src >= 8 else 0)]) if max(dst, src) >= 8 else b''
    return b'\x66' + prefix + b'\x0f\x79' + bytes([0xC0 | (dst & 7) << 3 | (src & 7)])


def helper(dst, src):
    """What EXTRQ does: dst[63:0]=(dst>>index)&mask(length); dst[127:64]=0."""
    return (
        b'\x50\x51\x52'  # push rax/rcx/rdx
        + b'\x66' + rex(1, dst >= 8) + b'\x0f\x7e' + bytes([0xC0 | (dst & 7) << 3])  # movq rax,xmmD
        + b'\x66' + rex(1, src >= 8) + b'\x0f\x7e' + bytes([0xC1 | (src & 7) << 3])  # movq rcx,xmmS
        + b'\x0f\xb6\xd5'                    # movzx edx,ch (index = control[13:8])
        + b'\xc4\xe2\xeb\xf7\xc0'            # shrx rax,rax,rdx
        + b'\xf7\xd1'                        # not ecx
        + b'\x8d\x49\x01'                    # lea ecx,[rcx+1] (-length mod 64)
        + b'\xc4\xe2\xf1\xf7\xc0'            # shlx rax,rax,rcx
        + b'\xc4\xe2\xf3\xf7\xc0'            # shrx rax,rax,rcx
        + b'\x66' + rex(1, dst >= 8) + b'\x0f\x6e' + bytes([0xC0 | (dst & 7) << 3])  # movq xmmD,rax
        + b'\x5a\x59\x58\xc3'                # pop rdx/rcx/rax; ret
    )


def patch_image(img):
    if any(img[0xCBEAD0:TEXT_END]):
        raise ValueError('the code cave of the text segment is not empty')
    for ip, length, dst, src, hexcode in SITES:
        want = bytes.fromhex(hexcode)
        if img[ip:ip + len(want)] != want or want[:length] != extrq(dst, src):
            raise ValueError(f'eboot+0x{ip:x} is not the expected EXTRQ')

    pos, helpers = CAVE, {}
    for pair in sorted({(s[2], s[3]) for s in SITES}):
        code = helper(*pair)
        helpers[pair] = pos
        img[pos:pos + len(code)] = code
        pos = (pos + len(code) + 15) & ~15

    for ip, length, dst, src, hexcode in SITES:
        moved = bytes.fromhex(hexcode)[length:]
        stub, back = pos, ip + length + len(moved)
        code = b'\x48\x8d\x64\x24\x80'  # lea rsp,[rsp-0x80]: the call must not hit the red zone
        code += b'\xe8' + struct.pack('<i', helpers[(dst, src)] - (stub + len(code) + 5))
        code += b'\x48\x8d\xa4\x24\x80\x00\x00\x00'  # lea rsp,[rsp+0x80]
        code += moved
        code += b'\xe9' + struct.pack('<i', back - (stub + len(code) + 5))
        img[stub:stub + len(code)] = code
        pos = (pos + len(code) + 15) & ~15
        img[ip:back] = b'\xe9' + struct.pack('<i', stub - (ip + 5)) + b'\xcc' * (back - ip - 5)
    if pos > CAVE_END:
        raise ValueError('the SSE4A stubs do not fit their code cave')


def patch_linked_image(source):
    """The ISSPRB02 image of link_modules.py with every EXTRQ rewritten, and the number of sites."""
    data = bytearray(source)
    if data[:8] != b'ISSPRB02' or len(data) < 16:
        raise ValueError('not an ISSPRB02 linked image')
    size = struct.unpack_from('<Q', data, 8)[0]
    if size != IMAGE_SIZE or len(data) < IMAGE_SIZE:
        raise ValueError(f'unexpected linked image size 0x{size:x}')
    base = len(data) - IMAGE_SIZE
    img = bytearray(data[base:])
    patch_image(img)
    data[base:] = img
    return bytes(data), len(SITES)
