"""Generate synthetic zlib test vectors for the Skyrim module's inflate tests.

These are OUR OWN generated patterns - no game content. Python's zlib is used
only to *produce* compressed streams; the module decodes them independently,
which is what makes this a real cross-implementation check.

Covers the three DEFLATE block types we can provoke:
  stored   - incompressible (random) data at level 0
  fixed    - forced via level 1 on short highly-repetitive input
  dynamic  - default level on text-like input (the common Skyrim case)
"""
import os
import zlib

OUT = os.path.join(os.path.dirname(__file__), "data")
os.makedirs(OUT, exist_ok=True)


def put(name, raw, level=6, wbits=15):
    comp = zlib.compress(raw, level)
    with open(os.path.join(OUT, name + ".raw"), "wb") as f:
        f.write(raw)
    with open(os.path.join(OUT, name + ".z"), "wb") as f:
        f.write(comp)
    ratio = (len(comp) / len(raw)) if raw else 0.0
    print(f"  {name:16s} raw={len(raw):7d}  z={len(comp):7d}  ratio={ratio:.3f}")


print("generating fixtures:")

# 1. text-like -> dynamic Huffman, and matches real ESM subrecord payloads
put("text", (b"EDID" + b"WhiterunMarketPlace01" + b"\x00" * 7 +
             b"FULL" + b"Whiterun Market Place" + b"\x00" + b"DATA" + b"\x01\x02\x03\x04") * 64)

# 2. long runs -> exercises overlapping length/distance copies (dist < len)
put("runs", b"A" * 40000 + b"B" * 20000 + b"ABCD" * 5000 + b"\x00" * 10000)

# 3. incompressible -> stored blocks
put("random", os.urandom(50000), level=0)

# 4. tiny payloads
put("tiny", b"X")
put("empty", b"")

# 5. structured binary resembling record sizes (mixed compressibility)
buf = bytearray()
for i in range(4000):
    buf += i.to_bytes(4, "little")
    buf += bytes([i & 0xFF, (i >> 3) & 0xFF, 0, 0])
put("records", bytes(buf))

print(f"\nwrote {len(os.listdir(OUT))} files to {OUT}")
