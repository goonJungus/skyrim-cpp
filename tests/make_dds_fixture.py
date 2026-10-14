"""Build synthetic DDS textures whose decoded pixels we know by hand.

These are NOT game assets. They are tiny hand-assembled headers plus a single
block, built so tests/texture_test.cpp can check the decoder against ground
truth (the exact RGBA the S3TC algorithm must produce) rather than against
itself.

Fixtures written:
  rgba32.dds     2x2 uncompressed BGRA8 (legacy RGB masks)
  dxt1.dds       4x4 BC1, 4-colour mode
  dxt3.dds       4x4 BC2, explicit 4-bit alpha
  dxt5.dds       4x4 BC3, interpolated alpha
  dx10_bc1.dds   4x4 BC1 carrying a DX10 extended header (DXGI_FORMAT_BC1)
  bad_magic.dds  wrong magic
  truncated.dds  DXT1 header but a short pixel body

The DXT1 colour block used throughout:
  c0 = 0xF800 (pure red), c1 = 0x07E0 (pure green), c0 > c1 -> 4-colour mode
  indices = 0,1,2,3 across the top row, 0 elsewhere
so the palette is red / green / (2/3 red + 1/3 green) / (1/3 red + 2/3 green).
"""
import os
import struct
import sys

DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PITCH = 0x8
DDSD_PIXELFORMAT = 0x1000
DDSD_LINEARSIZE = 0x80000

DDPF_ALPHAPIXELS = 0x1
DDPF_FOURCC = 0x4
DDPF_RGB = 0x40

DDSCAPS_TEXTURE = 0x1000


def header(w, h, flags, pitch_or_linear, pf_flags, fourcc=0, bitcount=0,
           rmask=0, gmask=0, bmask=0, amask=0, mipcount=1):
    out = struct.pack('<4s', b'DDS ')
    out += struct.pack('<IIIIIII', 124, flags, h, w, pitch_or_linear, 0, mipcount)
    out += b'\x00' * 44  # dwReserved1[11]
    out += struct.pack('<IIIIIIII', 32, pf_flags, fourcc, bitcount,
                       rmask, gmask, bmask, amask)
    out += struct.pack('<IIIII', DDSCAPS_TEXTURE, 0, 0, 0, 0)
    assert len(out) == 128, len(out)
    return out


def rgba32_fixture():
    # 2x2, BGRA byte order.
    px = [
        bytes([30, 20, 10, 255]),    # (10,20,30,255)
        bytes([50, 100, 200, 128]),  # (200,100,50,128)
        bytes([0, 255, 0, 255]),     # (0,255,0,255)
        bytes([255, 255, 255, 64]),  # (255,255,255,64)
    ]
    h = header(2, 2,
               DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT,
               2 * 4, DDPF_RGB | DDPF_ALPHAPIXELS, bitcount=32,
               rmask=0x00FF0000, gmask=0x0000FF00, bmask=0x000000FF, amask=0xFF000000)
    return h + b''.join(px)


def dxt1_block():
    c0, c1 = 0xF800, 0x07E0
    indices = 0 | (1 << 2) | (2 << 4) | (3 << 6)  # row 0 = 0,1,2,3
    return struct.pack('<HHI', c0, c1, indices)


def dxt1_fixture():
    h = header(4, 4,
               DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_LINEARSIZE | DDSD_PIXELFORMAT,
               8, DDPF_FOURCC, fourcc=struct.unpack('<I', b'DXT1')[0])
    return h + dxt1_block()


def dxt3_fixture():
    h = header(4, 4,
               DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_LINEARSIZE | DDSD_PIXELFORMAT,
               16, DDPF_FOURCC, fourcc=struct.unpack('<I', b'DXT3')[0])
    alpha = b'\xff' * 8  # all nibbles 0xF -> alpha 255
    return h + alpha + dxt1_block()


def dxt5_fixture():
    h = header(4, 4,
               DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_LINEARSIZE | DDSD_PIXELFORMAT,
               16, DDPF_FOURCC, fourcc=struct.unpack('<I', b'DXT5')[0])
    # a0=200 > a1=100 -> 8-value mode; all 3-bit indices 0 -> alpha = a0 = 200
    alpha = struct.pack('<BB', 200, 100) + b'\x00' * 6
    return h + alpha + dxt1_block()


def dx10_bc1_fixture():
    h = header(4, 4,
               DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_LINEARSIZE | DDSD_PIXELFORMAT,
               8, DDPF_FOURCC, fourcc=struct.unpack('<I', b'DX10')[0])
    dx10 = struct.pack('<IIIII', 71, 3, 0, 1, 0)  # BC1_UNORM, TEXTURE2D
    return h + dx10 + dxt1_block()


def build_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    files = {
        'rgba32.dds': rgba32_fixture(),
        'dxt1.dds': dxt1_fixture(),
        'dxt3.dds': dxt3_fixture(),
        'dxt5.dds': dxt5_fixture(),
        'dx10_bc1.dds': dx10_bc1_fixture(),
        'bad_magic.dds': b'XXXX' + b'\x00' * 200,
        'truncated.dds': dxt1_fixture()[:128 + 3],  # body cut short
    }
    for name, data in files.items():
        with open(os.path.join(out_dir, name), 'wb') as f:
            f.write(data)
    print('wrote ' + ', '.join(f'{n} ({len(d)})' for n, d in files.items()))


if __name__ == '__main__':
    build_all(sys.argv[1] if len(sys.argv) > 1 else 'data/dds_fixture')
