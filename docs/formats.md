# Format notes

Everything here was established against real Skyrim data and is reproduced in
the readers under `src/world/`. It is kept out of the README so the README can
stay short; the code is the authority, but these notes record the traps.

## TES4 container (ESM / ESP)

A plugin is a flat sequence of records and groups. Two details are easy to get
wrong, and both were caught by the synthetic fixture rather than assumed:

1. **A `GRUP` header *is* the 24-byte record header, reinterpreted.** It is not
   an extra header stacked on top. The fields at `+8`/`+12` are the group label
   and group type rather than flags/formID. Consequently `groupSize` covers the
   *whole* group **including** its header.
2. **A plain record's `dataSize` is the payload only.** The record spans
   `24 + dataSize`. The two cases advance the cursor differently; treating them
   the same desynchronises the walker 24 bytes at a time, which surfaces as
   garbage record types and pointer-sized sizes.

A record whose `formID` is `0` is an unused slot and is skipped.

### Compressed records: a `u32` size, then a zlib stream

A record flagged `0x00040000` stores its payload zlib-compressed, but the body
is **framed**: a little-endian `u32` decompressed size, then a single zlib
stream (RFC 1950). Feeding the whole body to a bare zlib decoder fails — the
size prefix is read as the zlib header. This matters in practice: **7,506 of the
17,568 `CELL` records in `Skyrim.esm` are compressed**, and before the framing
was handled they inflated to nothing and were dropped, so exterior cells never
received grid coordinates.

The reader is a self-contained RFC 1950/1951 decoder (`src/world/inflate.cpp`),
verified against zlib-produced streams covering stored, fixed and dynamic Huffman
blocks, plus malformed-input and bomb-limit rejection.

## FormIDs

A `FormID` is a 32-bit value whose **high byte is the source plugin's index in
load order** and whose low 24 bits are the form's index *within that file*:

```
0xSSIIII  ──┬──┘ └──┬──┘
            │       └ form index within its own source file
            └ source file index (0 = Skyrim.esm, then each plugin)
```

This is Bethesda's layout and is reproduced exactly, because override resolution
depends on it: a plugin at index 3 that defines form `0x03000010` is referenced
as `0x03000010` and does **not** overwrite `0x00000010` from the master.
References *inside* a record are local indices relative to the referencing
record's own source; use `FormDatabase::findIn()` for those.

## BSA archives

`bsa_archive` reads v103 / v104 / v105 and is verified against a real install:
**every entry in every stock archive decompresses correctly** — 133,034 files
across ten archives, including `Skyrim - Meshes.bsa` (23,424 files) and
`Skyrim - Voices.bsa` (61,211 files). It is seek-based: only the requested file is
read and decompressed, so a 1.4 GB archive is never loaded into memory.

- The header is 36 bytes: `BSA\0` magic, version, header size, archive flags,
  folder count, file count, **folder name length**, **file name length**,
  archive type, padding. There are **no table offsets** — every position is
  derived sequentially.
- Those two lengths are *lengths*, not offsets, and they **exclude** each name's
  `u8` length prefix, so the folder-name region spans `folderNamesLen +
  folderCount` bytes.
- The body is **interleaved**: each folder's name is immediately followed by that
  folder's own file records. Treating folder records, file records and name
  tables as three separate sequential tables desynchronises the parse.
- Folder records are 16 bytes (v103/v104) or 24 (v105); file records are always
  16 bytes: a `u64` name hash, a `u32` size (bits 30/31 are flags) and a `u32`
  absolute byte offset (bit 31 marks a secondary archive).
- A compressed payload is a `u32` decompressed size followed by a single zlib
  stream (RFC 1950). No per-block framing on this path.
- `archiveFlags` `0x100` means each payload is prefixed with its own virtual path
  as a bstring, which is how Skyrim SE writes its texture archives.

Two traps cost real time: the archive's CRC is Bethesda's own rolling CRC
(`crc = c + crc * 0x1003F`), not zlib's; and the compression bit means "differs
from the archive default", so a stored file in a compressed archive must set it.

## Localized strings (`FULL` / `DESC`)

`FULL` and `DESC` are **not** NUL-terminated strings on Skyrim SE (1.5.976+).
They are a 4-byte reference into a string table. Verified against a real
`Skyrim.esm`: **all 21,606 `FULL` subrecords were exactly 4 bytes**. Reading them
as plain strings yields garbage.

- `displayNameRaw()` / `descriptionRaw()` return a `LocalizedString` that is
  either inline text or a string-table id.
- `displayName()` returns the text only, and is therefore **empty for most SE
  records**. Prefer `displayNameRaw()`.
- The distinction is by subrecord length alone (4 = reference, otherwise inline),
  which keeps older non-SE records working.

## String tables (`.STRINGS`)

```
u32 count
u32 contentLength
(u32 index, u32 offset) * count    <- interleaved pairs, NOT separate arrays
char data[]                        <- NUL-terminated strings
```

Offsets are cumulative encoded-byte positions relative to the start of `data[]`:
each offset is the running total of all previous strings plus their NUL
terminators. The data section starts at `8 + 8*count`. Verified against the real
`Data/Strings/Skyrim_English.STRINGS`: **30,301 entries, 100% resolved**.

Two variants exist: `Normal` (`.strings`, NUL-terminated — handled) and
`LengthPrepended` (`.dlstrings` / `.ilstrings`, a `u32` length before each string
— not yet handled).

## Cells and worldspaces

`cell_system` rebuilds the CELL → REFR/ACHR hierarchy the flat form database
cannot express, by re-walking the plugin with parent tracking.

**Tree shape.** Interior cells live under the top-level `CELL` group (`GRUP
gtype 2 → gtype 3 → CELL`). Exterior cells live under the top-level `WRLD` group
as a sibling `GRUP gtype 1` for each worldspace, whose **label is the `WRLD`
FormID** (`→ gtype 4 → gtype 5 → CELL`). Each `CELL` is immediately followed by
its own child group holding its `REFR`/`ACHR` records. The naive mistake is to
recurse into a `WRLD` *record's payload* looking for cells; the cells are in the
sibling group.

**Grid coordinates are worldspace-scoped.** `XCLC` gives a cell's `int32` grid
coordinate, but that pair is only unique *within a worldspace*: Tamriel and
WhiterunWorld both define a cell at `(0,0)`. In `Skyrim.esm` there are 16,978
exterior `XCLC` records over only 11,236 distinct coordinate pairs, so keying the
spatial index on `(x, y)` alone collapses thousands of cells together. The index
is keyed on `(worldspace FormID, gridX, gridY)`.

**The persistent cell.** Each worldspace's first `CELL` is a pseudo-cell holding
every always-loaded reference for the map, stored at `(0,0)` while its references
sit anywhere. It is a **direct child of the worldspace's children group**, is
flagged `Cell::isPersistent`, and is **excluded from the spatial index** so it
cannot shadow a real cell that also occupies `(0,0)` — which is common.

Not yet handled: NavMesh (`NAVM`), cell `DATA` flags beyond
exterior/interior, `REFR` `XESP` enable-parent state, and cross-plugin cell
overrides (which need the file's `MAST` list to remap).

## NIF meshes

The mesh loader is a faithful port of the community converter `convert_mesh.py`
rather than a strict spec reader, because the real files are not strict.

- Real NIFs open with a 39-byte `Gamebryo File Format, Version 20.2.0.7\n`
  banner; the numeric version sits at offset 39, and `num_blocks` at `0x30`. The
  type list is discovered by scanning, not by trusting an offset.
- Shape geometry comes from `NiTriShape`, `NiTriStrips` and `BSLODTriShape`:
  positions, normals, UVs, and strip de-triangulation. The shape's data
  reference is preferred at block offset `+72` and recovered by scanning if that
  fails; the shader reference is read at `+89` (so the shape block must be at
  least 93 bytes); alpha properties sit at `+66`/`+70`.
- Diffuse textures resolve through `BSLightingShaderProperty` →
  `BSShaderTextureSet`; `NiAlphaProperty` marks cutout geometry.
- LOD duplicates are suppressed by name **and** bounding-box containment.

Validated against `Skyrim - Meshes.bsa`: **1,477 of 1,500** sampled meshes parse,
and every miss matches the reference converter (empty skeletons and corrupt strip
counts are legitimately blank). One mesh, `WRHouseBlackSmith01.nif`, matches the
reference exactly: **13,329 verts / 9,524 tris / 12 texture groups**.

Only the Skyrim LE generation (`20.2.0.7`, `NiTriShape`) is handled. Skyrim SE's
`BSTriShape` is not read.

## DDS textures

`texture.{hpp,cpp}` decodes the top mip of BC1/BC2/BC3 block-compressed images
plus uncompressed BGRA8/BGRX8, and recognises the DX10 extended header. The
decoder returns tightly packed, top-left-origin RGBA8 that the host uploads
directly as a managed `D3DFMT_A8B8G8R8` texture.

## Verified against real data

| Quantity | Value |
|---|---|
| forms in `Skyrim.esm` + masters | 870,856 |
| cells | 17,612 |
| exterior cells (grid-indexed) | 16,958 |
| worldspaces | 40 |
| references (REFR + ACHR) | 703,934 |
| `Skyrim - Meshes.bsa` files | 23,424 (17,216 `.nif`) |
| files across ten stock archives | 133,034 |
| `Skyrim_English.STRINGS` entries | 30,301 |
| Helgen Exterior (`Tamriel` `0x3C`, cell `4,-20`) | 1,080 instances over 9 cells |
