"""Generate a synthetic .STRINGS file matching the real format.

Format (confirmed against Mutagen.Bethesda.Core/Strings/StringsWriter.cs):
    u32 count
    u32 contentLength
    (u32 index, u32 offset) * count
    char data[]                        NUL-terminated strings

Offsets are cumulative encoded-byte positions relative to the start of data[].
"""
import os
import struct
import sys

# (index, string) pairs -- deliberately out of id order to prove the reader
# uses the directory, not positional assumptions.
ENTRIES = [
    (1,  "The Ratway Vaults"),
    (2,  "Windhelm Stable Services"),
    (3,  "Razelan"),
    (6,  "Why are you looking for a Dragonborn?"),
    (7,  "Imperial Boots of Dwindling Flames"),
    (8,  "Hide Bracers of Major Alchemy"),
    (9,  "Stand"),
    (12, "Brawling Victor"),
]


def build():
    count = len(ENTRIES)
    # Directory: (u32 index, u32 offset) * count, offsets cumulative.
    directory = bytearray()
    data = bytearray()
    size = 0
    for index, text in ENTRIES:
        directory += struct.pack('<II', index, size)
        data += text.encode('ascii') + b'\x00'
        size += len(text) + 1

    header = struct.pack('<II', count, size)
    return bytes(header + bytes(directory) + bytes(data))


if __name__ == '__main__':
    out_dir = sys.argv[1] if len(sys.argv) > 1 else 'tests/data/strings'
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, 'Skyrim_English.STRINGS')
    with open(path, 'wb') as f:
        f.write(build())
    print('wrote', path, os.path.getsize(path), 'bytes')
