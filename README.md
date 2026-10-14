# Skyrim Engine Module

A from-scratch reimplementation of the TES V: Skyrim (2011) game logic and data
layer, packaged as a **C++20 static library** for integration into
"The Elder Scrolls: San Andreas".

This module is **not** a game. It has no `main()`, no renderer, no window and no
audio device. It reads the player's own game files and exposes the game's
simulation and record layer through interfaces.

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
| `skyrim_standalone` | executable | the "Skyrim only" host shell (not part of the library) |
| `inflate_test` | test | DEFLATE decoder verification |
| `bsa_test` | test | BSA archive reader verification |
| `strings_test` | test | string-table reader verification |
| `cell_test` | test | cell system verification |
| `bsa_probe` | tool | list/decompress entries from a real `.bsa` |
| `cell_demo` | tool | load real ESM masters and report cells and references |

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
./cell_demo        "C:\Games\Skyrim\Data" Whiterun
./bsa_probe        "C:\Games\Skyrim\Data\Skyrim - Meshes.bsa"
```

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
- `probe_esm.py` — an independent Python walker used to cross-check the C++
  reader against ground truth (top-level groups, record counts, compressed
  cells, duplicate coordinates, editor-id search).
- `trace_esm.py` — walks a generated ESM the same way the reader does; used to
  localise walker bugs.
- `inflate_test.cpp`, `bsa_test.cpp`, `strings_test.cpp`, `cell_test.cpp` —
  the CTest targets.

Run with `ctest --test-dir build -C Release`.

---

## Conventions shared with the GTA module

See `../../COORDINATION.md`. In short: `namespace skyrim::` (their `gta::`),
public headers in `include/skyrim/`, private in `src/`, snake_case files,
PascalCase types, `m_` member prefix, no root `CMakeLists.txt`, no
`third_party/` at module level.
