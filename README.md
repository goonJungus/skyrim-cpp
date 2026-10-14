# Skyrim C++

A from-scratch **port of The Elder Scrolls V: Skyrim (2011)** — a native engine
built up record by record and system by system against the real game data, not
copied from it. The goal is a playable Skyrim, on 64-bit Windows / macOS /
Linux, from the player's own installation.

**Port first, module second.** The project is split into a portable C++20
**library** (`skyrim_module`) that holds the engine, and a thin **host**
(`skyrim_standalone`) that drives it — no `main()` in the library. That split
is what makes the port reusable: the same library drops into a larger engine as
a module (see `DIRECTIVE.md` for the integration contract). You do not build a
"module"; you build the port, and the port serves as a module.

It is early but **playable**. Today it reads the player's own game files and
rebuilds the world data (records, cells, references, archives, strings); turns
cells into placed models through the record layer; loads and decodes the real
meshes and textures from the game's archives; and renders a streamed,
first-person view through a Direct3D 9 host — a walkable vertical slice of the
world. Combat, AI, quests and the RPG systems are still ahead.

Nothing derived from Bethesda's binaries or assets is committed here. The only
binary files in the tree are **synthetic test fixtures we generate ourselves**
(see `tests/`).

---

## Design rules

| Rule | Consequence |
|---|---|
| No hardcoded `main()` | Library target; lifecycle via `ISkyrimWorld` |
| No game assets in the source tree | Everything is read at runtime from the user's install via `SkyrimConfig::dataDirectory` |
| Hookable Magicka & actors | `rpg_stats.hpp` (planned) exposes the bar and active effects for external systems such as Bullet Time |
| Pluggable perk trees | `rpg_stats.hpp` (planned) accepts custom trees (the Firearms trees) at runtime |
| Decoupled inventory | `inventory.hpp` (planned) keeps the item DB pure so a host can drive an external radial wheel |
| Cross-platform | Windows / macOS / Linux, 64-bit, C++20, standard library only, no compiler extensions, no third-party dependencies |

No source file, test or doc hardcodes a user's install path; paths arrive at
runtime through `SkyrimConfig`.

---

## Building

The module is standalone-configurable — it builds with no parent project:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Targets produced:

| Target | Kind | Purpose |
|---|---|---|
| `skyrim_module` | static library | the module itself |
| `skyrim_standalone` | executable | the "Skyrim only" host shell (not part of the library); `--play` runs the world |
| `inflate_test` | test | DEFLATE decoder verification |
| `bsa_test` | test | BSA archive reader verification |
| `strings_test` | test | string-table reader verification |
| `cell_test` | test | cell system verification |
| `texture_test` | test | DDS texture decoder verification |
| `nif_test` | test | NIF mesh-loader verification |
| `world_render_test` | test | cell → placed-model → mesh pipeline verification |
| `bsa_probe` | tool | list/decompress entries from a real `.bsa` |
| `cell_demo` | tool | load real ESM masters and report cells and references |
| `mesh_probe` | tool | parse meshes from a real `.bsa` (single or `--batch`) |
| `world_probe` | tool | stream a real cell end to end: refs → models → meshes |

At integration time a root `CMakeLists.txt` will `add_subdirectory()` this
module. **Do not create one from here** — the integration AI owns it.

---

## Quick start

```cpp
#include "skyrim/skyrim_module.hpp"
#include "skyrim/form_database.hpp"
#include "skyrim/cell_system.hpp"

skyrim::SkyrimConfig cfg;
cfg.dataDirectory = "/path/to/Skyrim/Data";   // the folder with *.esm
cfg.plugins       = {"SomeMod.esp"};          // load order after the masters

// Record layer.
skyrim::ISkyrimWorld* world = skyrim::createSkyrimWorld();
if (world->init(cfg) == skyrim::Status::Ok) {
    const skyrim::FormDatabase* db = world->database();
    const skyrim::TESForm* cell = db->find(skyrim::makeFormID(0, 0x0001ABCD));
    world->update(1.0f / 60.0f);
}
world->shutdown();
skyrim::destroySkyrimWorld(world);

// World cells (standalone subsystem).
skyrim::CellSystem cells;
if (cells.load(cfg) == skyrim::Status::Ok) {
    const auto* c = cells.findCell(0x0000003C, /*gridX=*/4, /*gridY=*/-20);
    if (c) {
        for (const auto& obj : c->objects) {
            std::printf("%s base=0x%08x at (%.0f, %.0f, %.0f)\n",
                        obj.isActor ? "ACHR" : "REFR",
                        obj.baseObject, obj.x, obj.y, obj.z);
        }
    }
}
```

Or via the bundled host / tools:

```sh
./skyrim_standalone --data "C:\Games\Skyrim\Data" --plugin SomeMod.esp --types
./skyrim_standalone --data "C:\Games\Skyrim\Data" --play --ws 3C --grid 4 -20
./cell_demo        "C:\Games\Skyrim\Data" Whiterun
./bsa_probe        "C:\Games\Skyrim\Data\Skyrim - Meshes.bsa"
./world_probe      "C:\Games\Skyrim\Data" --ws 3C --grid 4 -20
```

---

## Playable world

On Windows the host can run the world for real. Point it at your own install; it
opens the archives, streams the exterior cells around the player, and renders
through Direct3D 9:

```sh
./skyrim_standalone --data "C:\Games\Skyrim\Data" --play --ws 3C --grid 4 -20 --radius 1
```

| Input | Action |
|---|---|
| `W` `A` `S` `D` | move |
| mouse | look |
| `Shift` | run |
| `Space` | jump (walk) / rise (fly) |
| `Ctrl` | descend (fly) |
| `F` | toggle walk / fly |

`--ws` is the worldspace FormID (`3C` is Tamriel), `--grid X Y` the starting
exterior cell, and `--radius N` how many cells stream around the player (a
`(2N+1)²` grid). With no `--meshes`, every `*.bsa` in the data folder is searched
for meshes and textures; pass `--meshes` to restrict it.

Windowed mode is Windows-only because the renderer is Direct3D 9. Elsewhere the
CLI still builds and explains that; the library it drives — parsing, cells,
meshes, textures — compiles everywhere.

A bounded run renders a few frames and exits, which is how the whole pipeline is
smoke tested against a real install:

```sh
./skyrim_standalone --data "..." --play --ws 3C --grid 4 -20 --frames 3
```

That path exercises the real thing end to end: ~870k forms loaded, archives
opened, ~1,000 instances streamed across nine cells, a real device created,
meshes uploaded and textures decoded on the GPU, then a clean exit.

---

## FormIDs

A `FormID` is a 32-bit value whose **high byte is the source plugin's index in
load order** and whose low 24 bits are the form's index *within that file*:

```
0xSSIIII  ──┬──┘ └──┬──┘
            │       └ form index within its own source file
            └ source file index (0 = Skyrim.esm, then each plugin)
```

This is Bethesda's layout and is reproduced exactly, because override
resolution depends on it: a plugin at index 3 that defines form `0x03000010` is
referenced as `0x03000010` and does **not** overwrite `0x00000010` from the
master. References *inside* a record are local indices relative to the
referencing record's own source — use `FormDatabase::findIn()` for those.

This deliberately does **not** share an ID abstraction with the GTA module; the
two handle models are unrelated and the integrator should write an adapter.

---

## System overview

### Implemented

**Asset / record layer** (`src/world/`)
- `inflate.{hpp,cpp}` — RFC 1950/1951 DEFLATE decoder. Needed because Skyrim
  record payloads and BSA v105 blocks are zlib-deflated. Self-contained to avoid
  a zlib dependency on three platforms. Verified against zlib-produced streams
  covering stored, fixed and dynamic Huffman blocks, plus malformed-input and
  bomb-limit rejection.
- `bsa_archive.{hpp,cpp}` — TES4-family BSA reader (v103 / v104 / v105),
  seek-based (see below).
- `string_table.{hpp,cpp}` — loose `.STRINGS` reader (see below).
- `form_database.{hpp,cpp}` — TES4 `ESM`/`ESP` reader. Walks the record/group
  tree (groups nest arbitrarily deep), inflates compressed records, and indexes
  every form by `FormID`. Provides record-type queries and a histogram.
- `cell_system.{hpp,cpp}` — cell hierarchy and spatial index (see below).
- `skyrim_world.cpp` — the concrete `ISkyrimWorld` and module factory.

**Geometry / rendering layer** (`src/world/`, `include/skyrim/mesh.hpp`, `texture.hpp`)
- `mesh.hpp` — the renderer-facing `Mesh`: interleaved vertex stream
  (position, normal, UV), a shared 32-bit index buffer, and submeshes (index
  range, diffuse texture path, alpha-test flag, fallback colour).
- `nif.{hpp,cpp}` — the runtime **NIF loader**. Parses the Skyrim LE
  generation (`20.2.0.7`) directly: the file banner + type list, `NiTriShape` /
  `NiTriStrips` / `BSLODTriShape` geometry (positions, normals, UVs, strip
  de-triangulation), diffuse-texture resolution through
  `BSLightingShaderProperty` → `BSShaderTextureSet`, `NiAlphaProperty` cutout
  detection, and LOD duplicate suppression by name **and** bounding-box
  containment. Validated against `Skyrim - Meshes.bsa`: **1,477 of 1,500**
  sampled meshes parse, and every miss matches the reference converter (empty
  skeletons and corrupt strip counts are legitimately blank). One mesh,
  `WRHouseBlackSmith01.nif`, matches the reference byte-for-byte — **13,329
  verts / 9,524 tris / 12 texture groups**.
- `texture.{hpp,cpp}` — DDS decoder: BC1/BC2/BC3 block compression plus
  uncompressed BGRA8/BGRX8, with the DX10 extended header recognised. Decodes
  the top mip to tightly packed RGBA8.
- `world_render.{hpp,cpp}` — the bridge from world structure to drawables:
  `ModelResolver` (base `FormID` → `MODL` model path), `instancesForCell`
  (the placed models for a cell, with position/rotation/scale), and `MeshCache`
  (loads and caches meshes out of the BSA archives).

**Playable host** (`host/`, renderer is Windows-only)
- `host/engine/` — the Win32 window and the Direct3D 9 renderer (fixed-function
  lighting, per-submesh textures, alpha-cutout, GPU mesh and texture caches), a
  first-person player with gravity and axis-aligned collision, and cell
  streaming around the player.
- `host/main.cpp` — the thin CLI (`--play`, `--ws`, `--grid`, `--radius`,
  `--meshes`, `--frames`).

Verified end to end on a real install: Helgen Exterior (`Tamriel` `0x3C`, cell
`4,-20`) streams **1,080 instances across nine cells**, all uploaded and
rendered on a real device.

### Not yet implemented

Each will land as a header in `include/skyrim/` plus implementation in `src/`:

| Header | Subsystem | `src/` dir |
|---|---|---|
| `actors_radiant.hpp` | radiant AI, schedules, factions, crimes/bounties | `actors/` |
| `rpg_stats.hpp` | Health/Magicka/Stamina, perk trees, shouts | `rpg/` |
| `inventory.hpp` | encumbrance inventory, item database | `inventory/` |
| `magic_system.hpp` | spellcasting, enchantments, active effects | `magic/` |
| `papyrus_vm.hpp` | script VM execution, quest state machines | `quests/` |

`CellSystem` is a standalone subsystem; wiring it into `ISkyrimWorld` is
pending.

---

## TES4 container quirks

Two details in the TES4 container are easy to get wrong, and both were caught by
the synthetic fixture rather than assumed:

1. **A `GRUP` header *is* the 24-byte record header, reinterpreted.** It is not
   an extra header stacked on top. The fields at `+8`/`+12` are the group label
   and group type rather than flags/formID. Consequently `groupSize` covers the
   *whole* group **including** its header.
2. **A plain record's `dataSize` is the payload only.** The record spans
   `24 + dataSize`. So the two cases advance the cursor differently — treating
   them the same silently desynchronises the walker 24 bytes at a time, which
   surfaces as garbage record types and bogus pointer-sized sizes.

A record whose `formID` is `0` is an unused slot and is skipped (the form does
not exist; keeping it would break reference resolution).

### Compressed records: `u32` size, then a zlib stream

A record whose flags carry `0x00040000` stores its payload zlib-compressed, but
the body is **framed**: a little-endian `u32` decompressed size, then a single
zlib stream (RFC 1950). Feeding the whole body to a bare zlib decoder fails —
the size prefix is read as the zlib header. `zip::zlibInflateRecord()` handles
the framing and is what `form_database` and `cell_system` use.

This matters in practice: **7,506 of the 17,568 `CELL` records in `Skyrim.esm`
are compressed.** Before the framing was handled they inflated to nothing and
were silently dropped, so exterior cells never received grid coordinates. This
is exactly the failure the synthetic fixture now covers.

### BSA archives: working, verified against real Skyrim archives

`bsa_archive` reads v103 / v104 / v105 and is verified against a real install:
**every entry in every stock-game archive decompresses correctly** — 133,034
files across ten archives, including `Skyrim - Meshes.bsa` (23,424 files) and
`Skyrim - Voices.bsa` (61,211 files). It is a seek-based reader: only the
requested file is read and decompressed, so a 1.4 GB archive is never loaded
into memory.

The layout, established from a reference implementation and then confirmed
byte-for-byte against real data:

- The header is 36 bytes: `BSA\0` magic, version, header size, archive flags,
  folder count, file count, **folder name length**, **file name length**,
  archive type, padding. There are **no table offsets** — every position is
  derived sequentially.
- Those two lengths are *lengths*, not offsets, and they **exclude** each
  name's `u8` length prefix. The folder-name region therefore spans
  `folderNamesLen + folderCount` bytes.
- The body is **interleaved**: each folder's name is immediately followed by
  that folder's own file records. Treating the folder records, file records
  and name tables as three separate sequential tables desynchronises the parse.
- Folder records are 16 bytes (v103/v104) or 24 (v105); file records are always
  16 bytes: a `u64` name hash, a `u32` size (bits 30/31 are flags) and a `u32`
  absolute byte offset (bit 31 marks a secondary archive).
- A compressed payload is a `u32` decompressed size followed by a **single
  zlib stream** (RFC 1950). There is no per-block framing on this path.
- `archiveFlags` 0x100 means each payload is prefixed with its own virtual path
  as a bstring, which is how Skyrim SE writes its texture archives.

Two traps worth recording, because both cost real time: the archive's CRC is
Bethesda's own rolling CRC (`crc = c + crc * 0x1003F`), not zlib's; and the
compression bit means "differs from the archive default", so a stored file in
a compressed archive must set it.

### FULL/DESC are localized-string references on Skyrim SE

`FULL` and `DESC` are **not** NUL-terminated strings on Skyrim SE (1.5.976+).
They are a 4-byte reference into a string table. Verified against a real
`Skyrim.esm`: **all 21,606 `FULL` subrecords were exactly 4 bytes** — no inline
form at all. Reading them as plain strings yields garbage (a worldspace would
render as a single stray character).

- `TESForm::displayNameRaw()` / `descriptionRaw()` return a `LocalizedString`
  that is either inline text or a string-table id.
- `displayName()` returns the text only, and is therefore **empty for most SE
  records**. Prefer `displayNameRaw()`.
- Turning an id into text needs a string table (see below).
- The distinction is by subrecord length alone (4 = reference, otherwise
  inline), which is what keeps older non-SE records working.

### String tables: working, verified against a real file

`string_table` reads the loose `.STRINGS` format. The layout was confirmed
against `Mutagen.Bethesda.Core/Strings/StringsWriter.cs`:

```
u32 count
u32 contentLength
(u32 index, u32 offset) * count    <- interleaved pairs, NOT separate arrays
char data[]                        <- NUL-terminated strings
```

Offsets are cumulative encoded-byte positions relative to the start of `data[]`.
The writer does `size += strLen + 1` per entry, so each offset is the running
total of all previous strings plus their NUL terminators. The data section
starts at `8 + 8*count` (after `count`, `contentLength`, and the directory).

Verified against the real `Data/Strings/Skyrim_English.STRINGS`: **30,301
entries, 100% resolved**, all probe strings correct ("The Ratway Vaults",
"Windhelm Stable Services", "Razelan", "Why are you looking for a Dragonborn?").

Two format variants exist (`StringsFileFormat`):
- `Normal` — `.strings`, NUL-terminated strings (handled here)
- `LengthPrepended` — `.dlstrings` / `.ilstrings`, a `u32` length prefix
  precedes each string (not yet handled)

A stock 2011 install has no loose `.STRINGS` at all — base strings live in
`Skyrim - Strings.bsa` — so BSA-backed tables are the remaining path for a
clean install.

---

## Cells and worldspaces

`cell_system` rebuilds the CELL → REFR/ACHR hierarchy that the flat form
database cannot express, by re-walking the ESM with parent tracking.

**Tree shape.** Interior cells live under the top-level `CELL` group
(`GRUP gtype 2 → gtype 3 → CELL`). Exterior cells live under the top-level
`WRLD` group as a sibling `GRUP gtype 1` for each worldspace, whose **label is
the `WRLD` FormID** (`→ gtype 4 → gtype 5 → CELL`). Each `CELL` is immediately
followed by its own child group holding its `REFR`/`ACHR` records. The naive
mistake is to recurse into a `WRLD` *record's payload* looking for cells; the
cells are in the sibling group, not the payload.

**Grid coordinates are worldspace-scoped.** `XCLC` gives a cell's `int32` grid
coordinate, but that pair is only unique *within a worldspace*: Tamriel and
WhiterunWorld both define a cell at `(0,0)`, and every worldspace has a
persistent pseudo-cell at `(0,0)` too. In `Skyrim.esm` there are 16,978
exterior `XCLC` records over only 11,236 distinct coordinate pairs — keying the
spatial index on `(x, y)` alone collapses **thousands** of cells from different
worldspaces into one. The index is therefore keyed on
`(worldspace FormID, gridX, gridY)`.

**The persistent cell.** Each worldspace's first `CELL` is a pseudo-cell that
holds every always-loaded reference for the map, stored at `(0,0)` while its
references sit anywhere. It is a **direct child of the worldspace's children
group** (real cells are nested under `gtype 4/5`). It is flagged
`Cell::isPersistent` and **excluded from the spatial index**, so it cannot
shadow a real cell that also occupies `(0,0)` — which is common (Tamriel alone
has `BleakwindBasinExterior01` at `(0,0)`).

**API.** `findCell(worldspace, gridX, gridY)`, `findCellById(formId)`,
`queryBounds(worldspace, minX, minY, maxX, maxY)`, and
`worldspaceHistogram()`. Exterior lookups return pointers kept stable by a
`std::deque` backing store.

**Verified against a real install** (`Skyrim.esm` + `Update.esm`; the DLC
masters were absent from the tested install and are skipped gracefully):

| Quantity | Value |
|---|---|
| total cells | 17,612 |
| exterior cells (grid-indexed) | 16,958 |
| persistent cells | 41 |
| worldspaces | 40 |
| references (REFR + ACHR) | 703,934 |

Independent Python cross-checks (`tests/probe_esm.py`) agree on the per-file
counts: `Skyrim.esm` has 17,568 `CELL` (16,978 exterior, 590 interior, 7,506
compressed), 693,339 `REFR`, 10,504 `ACHR`, 37 `WRLD`.

Not yet handled: NavMesh (`NAVM`), cell `DATA` flags beyond exterior/interior,
`REFR` `XESP` enable-parent state, and cross-plugin cell overrides (a plugin's
override record carries the original master's FormID index, which would need
the file's TES4 `MAST` list to remap).

---

## Tests

`tests/` contains only data we generated ourselves; no game content.

- `make_fixtures.py` — synthetic zlib streams (text-like, long runs,
  incompressible, empty, binary-ish) covering all three DEFLATE block types.
- `make_esm_fixture.py` — a synthetic `Skyrim.esm` + plugin exercising the TES4
  container: header record, top-level groups, groups nested 4 deep, a `formID 0`
  slot, a compressed record (with the real `u32`+zlib framing), and
  `EDID`/`FULL` subrecords.
- `make_bsa_fixture.py` — a synthetic BSA archive (stored and compressed
  entries) covering the v105 layout.
- `make_strings_fixture.py` — a synthetic `.STRINGS` table.
- `make_cell_fixture.py` — a synthetic ESM for the cell system: a compressed
  cell, two worldspaces sharing grid `(0,0)`, a persistent cell colliding with a
  real cell, interior cells, and `REFR`/`ACHR` references.
- `make_dds_fixture.py` — synthetic DDS images: DXT1/DXT3/DXT5, uncompressed
  BGRA8, a DX10-header image, and a truncated/bad-magic pair.
- `make_nif_fixture.py` — a synthetic NIF written with the **realistic 39-byte
  banner** (so the loader is exercised against the shape real files have, not
  the naive header the spec implies), plus bad-magic and truncated cases.
- `make_worldrender_fixture.py` — a synthetic ESM for the pipeline: a placed
  `REFR` → `MODL` path → mesh, resolved end to end.
- `probe_esm.py` — an independent Python walker used to cross-check the C++
  reader against ground truth (top-level groups, record counts, compressed
  cells, duplicate coordinates, editor-id search).
- `trace_esm.py` — walks a generated ESM the same way the reader does; used to
  localise walker bugs.
- `inflate_test.cpp`, `bsa_test.cpp`, `strings_test.cpp`, `cell_test.cpp`,
  `texture_test.cpp`, `nif_test.cpp`, `world_render_test.cpp` — the CTest
  targets.

Run with `ctest --test-dir build -C Release`.

---

## Conventions shared with the GTA module

See `../../COORDINATION.md`. In short: `namespace skyrim::` (their `gta::`),
public headers in `include/skyrim/`, private in `src/`, snake_case files,
PascalCase types, `m_` member prefix, no root `CMakeLists.txt`, no
`third_party/` at module level.
