"""Build synthetic BSA archives to test the reader against known content.

The generator mirrors the real TES4 layout exactly:

  header            36 bytes, "BSA\\0" magic, no extension
  folder records    16 bytes (v103/v104) or 24 (v105): {u64 hash, u32 count,
                    u32 firstFileIndex} + 4 pad on v105
  body              per folder, in order:
                      u8 nameLen (includes NUL) + folderName
                      then that folder's file records, 16 bytes each
  leaf-name table   NUL-terminated leaf names, in file-record order
  payload data      absolute byte offsets, sector-aligned

Header fields for the name tables hold LENGTHS (excluding the u8 prefixes),
not offsets -- that is what the real format does and what the reader expects.

We write our own generator so the test is checked against ground truth rather
than against the reader itself.
"""
import os
import struct
import sys
import zlib

SECTOR = 2048
HEADER_SIZE = 36

# archiveFlags bits
F_DIRECTORY_STRINGS = 0x001
F_FILE_STRINGS = 0x002
F_COMPRESSED = 0x004
F_EMBEDDED_FILE_NAMES = 0x100

# size-field bits
SIZE_COMPRESSION_BIT = 0x40000000


def bethesda_crc32(data):
    """The archive's own CRC: crc = c + crc * 0x1003F, seeded at 0."""
    crc = 0
    for c in data:
        crc = (c + crc * 0x1003F) & 0xFFFFFFFF
    return crc


def hash_directory(path):
    s = path.lower().replace('/', '\\').encode('ascii', 'replace')
    last = s[-1] if len(s) >= 1 else 0
    last2 = s[-2] if len(s) >= 2 else 0
    first = s[0] if len(s) >= 1 else 0
    crc = bethesda_crc32(s[1:len(s) - 2]) if len(s) > 3 else 0
    return struct.pack('<BBBB I', last, last2, len(s) & 0xFF, first, crc)


class BsaWriter:
    """Collects entries, then serialises them in the real on-disk order."""

    def __init__(self, version=104):
        self.version = version
        self.entries = []

    def add(self, folder, name, data, compressed=False):
        self.entries.append(dict(folder=folder.lower(),
                                 name=name.split('\\')[-1].split('/')[-1].lower(),
                                 data=data, compressed=compressed))

    def build(self, embedded_names=False, raw_deflate=False):
        # Preserve insertion order but group by folder, matching how the real
        # writer emits folders consecutively.
        folders = []
        for e in self.entries:
            if e['folder'] not in folders:
                folders.append(e['folder'])

        ordered = []
        for f in folders:
            for e in self.entries:
                if e['folder'] == f:
                    ordered.append(e)
        self.entries = ordered

        # ---- payloads ----
        # Stored payloads are written verbatim then padded to a sector.
        # Compressed payloads are a u32 decompressed size followed by a single
        # u16-framed DEFLATE block. With embedded_names each payload is first
        # prefixed by its own full virtual path as a bstring.
        payloads = []
        archive_compressed = any(e['compressed'] for e in self.entries)
        for e in self.entries:
            d = e['data']
            prefix = b''
            if embedded_names:
                full = (e['folder'] + '\\' + e['name']).encode('ascii') + b'\0'
                prefix = struct.pack('<B', len(full)) + full
            if not e['compressed']:
                payloads.append(prefix + bytes(d))
            else:
                # Stock Skyrim archives store a single zlib stream (RFC 1950:
                # 2-byte header + DEFLATE + Adler-32) after the u32 size, with
                # no per-block framing. Confirmed against Animations.bsa and
                # Meshes.bsa.
                c = zlib.compress(d, 9)
                payloads.append(prefix + struct.pack('<I', len(d)) + c)

        folder_stride = 24 if self.version >= 105 else 16
        header_size = HEADER_SIZE

        # ---- compute the body length so data can be placed after it ----
        def body_length():
            n = folder_stride * len(folders)
            for f in folders:
                n += 1 + len(f) + 1  # u8 prefix (incl NUL) + name + NUL
                n += 16 * sum(1 for e in self.entries if e['folder'] == f)
            return n

        name_blob = b''.join(e['name'].encode('ascii') + b'\0'
                             for e in self.entries)
        body_len = body_length()
        data_off = header_size + body_len + len(name_blob)
        data_off = ((data_off + SECTOR - 1) // SECTOR) * SECTOR

        # ---- assign payload offsets (absolute bytes) ----
        cur = data_off
        offsets = []
        for p in payloads:
            offsets.append(cur)
            cur += len(p)
        total_size = ((cur + SECTOR - 1) // SECTOR) * SECTOR
        out = bytearray(total_size)

        # ---- header ----
        # name lengths EXCLUDE the per-name u8 prefix
        folder_names_len = sum(len(f) for f in folders)
        flags = F_DIRECTORY_STRINGS | F_FILE_STRINGS
        if archive_compressed:
            flags |= F_COMPRESSED
        if embedded_names:
            flags |= F_EMBEDDED_FILE_NAMES
        out[0:HEADER_SIZE] = struct.pack(
            '<4sIIIIIII H H',
            b'BSA\x00', self.version, header_size, flags,
            len(folders), len(self.entries),
            folder_names_len, len(name_blob),
            0, 0)

        # ---- folder records ----
        p = header_size
        for f in folders:
            cnt = sum(1 for e in self.entries if e['folder'] == f)
            first = next(i for i, e in enumerate(self.entries) if e['folder'] == f)
            rec = hash_directory(f) + struct.pack('<II', cnt, first)
            if self.version >= 105:
                rec += struct.pack('<I', 0)  # x64 padding
            out[p:p + len(rec)] = rec
            p += folder_stride

        # ---- body: per folder, name then that folder's file records ----
        file_index = 0
        for f in folders:
            nb = f.encode('ascii') + b'\0'
            out[p] = len(nb)
            out[p + 1:p + 1 + len(nb)] = nb
            p += 1 + len(nb)

            for e in self.entries:
                if e['folder'] != f:
                    continue
                idx = file_index
                file_index += 1
                size_field = len(payloads[idx])
                # The compression bit means "differs from the archive default".
                # In a compressed archive a stored file must set it; in an
                # uncompressed archive a compressed file must set it.
                if e['compressed'] != archive_compressed:
                    size_field |= SIZE_COMPRESSION_BIT
                h = struct.pack('<8s', b'\0' * 8)
                out[p:p + 16] = h + struct.pack('<II', size_field, offsets[idx])
                p += 16

        # ---- leaf-name table ----
        name_off = p
        assert name_off == header_size + body_len, (name_off, header_size + body_len)
        out[name_off:name_off + len(name_blob)] = name_blob

        # ---- payload data ----
        for pl, off in zip(payloads, offsets):
            out[off:off + len(pl)] = pl

        return bytes(out)


def build_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    text = b'MESH' + b'\x00' * 60
    binary = bytes(range(256)) * 8
    rnd = bytes(((i * 37 + (i >> 3) * 11 + (i >> 6)) & 0xFF) for i in range(5000))

    # Archive A: v104, mixed stored and compressed, several folders,
    # including edge cases (0-byte file, large file, nested folder paths).
    w = BsaWriter(104)
    w.add('meshes', 'a\\plain.nif', text, compressed=False)
    w.add('meshes', 'a\\zipped.nif', binary, compressed=True)
    w.add('textures', 'a\\d.dds', rnd, compressed=True)
    w.add('textures', 'a\\big.dds', b'X' * 40000, compressed=True)
    w.add('strings', 'strings0.strings', b'S' * 9000, compressed=False)
    w.add('meshes', 'empty.nif', b'', compressed=False)
    w.add('meshes\\architecture\\whiterun', 'deep.nif', b'D' * 1234, compressed=True)
    data104 = w.build()

    # Archive B: v105 (x64 folder records, 24-byte stride).
    w1 = BsaWriter(105)
    w1.add('meshes', 's\\plain.nif', text, compressed=False)
    w1.add('meshes', 's\\zipped.nif', binary, compressed=True)
    w1.add('textures', 's\\big.dds', b'Y' * 30000, compressed=False)
    data105 = w1.build()

    # Archive C: v103, the 2011 Steam layout.
    w2 = BsaWriter(103)
    w2.add('meshes', 'v103.nif', binary, compressed=True)
    w2.add('textures', 't.dds', rnd, compressed=False)
    data103 = w2.build()

    # Archive D: v104 with embedded file names (archiveFlags 0x100), which is
    # how Skyrim SE writes its texture archives.
    w3 = BsaWriter(104)
    w3.add('textures', 'actors\\dog\\dog.dds', rnd, compressed=True)
    w3.add('textures', 'clutter\\food\\bread.dds', b'B' * 7000, compressed=True)
    w3.add('meshes', 'a\\plain.nif', text, compressed=False)
    data104e = w3.build(embedded_names=True)

    with open(os.path.join(out_dir, 'fixture104.bsa'), 'wb') as f:
        f.write(data104)
    with open(os.path.join(out_dir, 'fixture105.bsa'), 'wb') as f:
        f.write(data105)
    with open(os.path.join(out_dir, 'fixture103.bsa'), 'wb') as f:
        f.write(data103)
    with open(os.path.join(out_dir, 'fixture104_embed.bsa'), 'wb') as f:
        f.write(data104e)

    import json
    expected = {}
    for key, wr in (('104', w), ('105', w1), ('103', w2), ('104_embed', w3)):
        expected[key] = {
            (e['folder'] + '\\' + e['name']) if e['folder'] else e['name']: len(e['data'])
            for e in wr.entries
        }
    with open(os.path.join(out_dir, 'expected.json'), 'w') as f:
        json.dump(expected, f, indent=1)

    print(f'wrote fixture104.bsa ({len(data104)}), '
          f'fixture105.bsa ({len(data105)}), '
          f'fixture103.bsa ({len(data103)}), '
          f'fixture104_embed.bsa ({len(data104e)}), expected.json')


if __name__ == '__main__':
    build_all(sys.argv[1] if len(sys.argv) > 1 else 'data/bsa_fixture')
