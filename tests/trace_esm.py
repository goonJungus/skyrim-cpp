"""Walk a generated ESM exactly the way the C++ reader does, to localise failures."""
import struct
import sys
import zlib

path = sys.argv[1]
buf = open(path, 'rb').read()
print(f"file size = {len(buf)}")

p = 0


def rdtype(b):
    return b[0:4].decode('ascii', 'replace')


def walk(p, end, depth):
    indent = "  " * depth
    while end - p >= 24:
        rtype = rdtype(buf[p:p + 4])
        dsize, flags, fid = struct.unpack_from('<III', buf, p + 4)
        if rtype == 'GRUP':
            label = rdtype(buf[p + 8:p + 12])
            gtype = struct.unpack_from('<i', buf, p + 12)[0]
            print(f"{indent}GRUP label={label} gtype={gtype} groupSize={dsize} "
                  f"children={dsize - 24}")
            if dsize < 24:
                print(f"{indent}!! groupSize < 24")
                return False
            if p + dsize > end:
                print(f"{indent}!! GRUP overruns parent ({p + dsize} > {end})")
                return False
            if not walk(p + 24, p + dsize, depth + 1):
                return False
        else:
            body = p + 24
            if end - body < dsize:
                print(f"{indent}!! TRUNCATED {rtype}: need {dsize}, have {end - body}")
                return False
            comp = " COMPRESSED" if flags & 0x00040000 else ""
            note = " (formID 0 -> skipped)" if fid == 0 else ""
            print(f"{indent}{rtype} id=0x{fid:06X} size={dsize}{comp}{note}")
            p = body + dsize
            continue
        p += dsize
    if p != end:
        print(f"{'  ' * depth}!! TRAILING/GAP: ended at {p}, expected {end} "
              f"(gap {end - p})")
        return False
    return True


# Skip the TES4 header the same way the reader does
rtype = rdtype(buf[0:4])
dsize = struct.unpack_from('<I', buf, 4)[0]
print(f"TES4 header: type={rtype} dataSize={dsize} flags={struct.unpack_from('<I', buf, 8)[0]}")
p = 24 + dsize
print(f"after TES4 header, p={p}")
print("--- walk ---")
ok = walk(p, len(buf), 0)
print("RESULT:", "clean" if ok else "FAILED")
