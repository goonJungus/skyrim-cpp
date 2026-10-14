"""Build a synthetic NIF (version 20.2.0.7) whose geometry we know by hand.

This is NOT a game asset. It is a hand-assembled file that follows the layout
the runtime parser expects, so tests/nif_test.cpp can check the parser against
ground truth rather than against itself.

The fixture is one NiTriShape with a single triangle:

  vertices  (0,0,0) (1,0,0) (0,1,0)
  uvs       (0,0)   (1,0)   (0,1)
  triangle  0,1,2
  texture   textures\\test.dds

Block order: NiTriShape, NiTriShapeData, BSLightingShaderProperty,
BSShaderTextureSet.
"""
import os
import struct
import sys

VERSION = 0x14020007
BANNER = b"Gamebryo File Format, Version 20.2.0.7\n"  # 39 bytes

TYPES = ["NiTriShape", "NiTriShapeData", "BSLightingShaderProperty",
         "BSShaderTextureSet", "NiAlphaProperty"]
TYPE_IDX = [0, 1, 2, 3]
STRINGS = ["TestShape", "textures\\test.dds"]

VERTICES = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
UVS = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0)]
TRIANGLES = [(0, 1, 2)]


def build_shape_block():
    b = bytearray(96)
    struct.pack_into('<I', b, 0, 0)          # name string index -> "TestShape"
    struct.pack_into('<I', b, 66, 0)         # property count
    struct.pack_into('<I', b, 72, 1)         # geometry data ref -> block 1
    struct.pack_into('<I', b, 89, 2)         # shader ref -> block 2
    return bytes(b)


def build_data_block():
    nv = len(VERTICES)
    b = bytearray()
    b += struct.pack('<I', 0)                # groupID
    b += struct.pack('<H', nv)               # num vertices
    b += bytes([0, 0, 1])                    # keep, compress, hasVertices
    for (x, y, z) in VERTICES:
        b += struct.pack('<fff', x, y, z)
    b += struct.pack('<H', 1)                # data flags: 1 UV set
    b += struct.pack('<I', 0)                # material CRC
    b += bytes([0])                          # no normals
    b += struct.pack('<ffff', 0.0, 0.0, 0.0, 1.0)  # centre + radius
    b += bytes([0])                          # no vertex colours
    for (u, v) in UVS:
        b += struct.pack('<ff', u, v)
    # triangle section
    b += struct.pack('<H', 0)                # consistency flags
    b += struct.pack('<HH', 0xFFFF, 0xFFFF)  # padding
    b += struct.pack('<H', len(TRIANGLES))   # num triangles
    b += struct.pack('<I', 0)                # (num triangles u32)
    b += bytes([1])                          # has triangles
    for (a, c, d) in TRIANGLES:
        b += struct.pack('<HHH', a, c, d)
    return bytes(b)


def build_shader_block():
    b = bytearray(44)
    struct.pack_into('<I', b, 40, 3)         # texture set ref -> block 3
    return bytes(b)


def build_texset_block():
    tex = STRINGS[1].encode('ascii')
    return struct.pack('<I', 1) + struct.pack('<I', len(tex)) + tex


def build_header(block_sizes):
    b = bytearray()
    b += BANNER                              # 0..38
    b += struct.pack('<I', VERSION)          # 39..42 numeric version
    b += bytes([1])                          # 43 endianness
    b += struct.pack('<I', 12)               # 44..47 user version
    b += struct.pack('<I', len(block_sizes))  # 48 num blocks (fixed offset 0x30)
    b += b'\x00' * 34                        # 52..85 header padding
    assert len(b) == 86
    # type list begins at offset 86, as in real Skyrim meshes
    b += struct.pack('<H', len(TYPES))
    for t in TYPES:
        e = t.encode('ascii')
        b += struct.pack('<I', len(e)) + e
    for i in TYPE_IDX:
        b += struct.pack('<H', i)
    for s in block_sizes:
        b += struct.pack('<I', s)
    b += struct.pack('<I', len(STRINGS))     # string count
    b += struct.pack('<I', 0)                # unknown
    for s in STRINGS:
        e = s.encode('ascii')
        b += struct.pack('<I', len(e)) + e
    b += struct.pack('<I', 0)                # group count
    return bytes(b)


def build_nif():
    blocks = [build_shape_block(), build_data_block(),
              build_shader_block(), build_texset_block()]
    sizes = [len(x) for x in blocks]
    return build_header(sizes) + b''.join(blocks)


def build_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'triangle.nif'), 'wb') as f:
        f.write(build_nif())
    with open(os.path.join(out_dir, 'bad_magic.nif'), 'wb') as f:
        f.write(b'XXXX' + b'\x00' * 400)
    with open(os.path.join(out_dir, 'truncated.nif'), 'wb') as f:
        f.write(build_nif()[:80])
    print('wrote triangle.nif, bad_magic.nif, truncated.nif')


if __name__ == '__main__':
    build_all(sys.argv[1] if len(sys.argv) > 1 else 'data/nif_fixture')
