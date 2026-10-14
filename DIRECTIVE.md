# PACKAGING & DELIVERY DIRECTIVE: SKYRIM ENGINE MODULE

> Source of truth for the Skyrim-side work. Restated verbatim from the owner.
> Session-scoped instructions that accompany this directive:
> - Scope is **Skyrim only**. No launcher, no engine, no game combination.
> - **Game assets do not ship.** The software is code that lets people play the
>   games they already own, in 64-bit, on Windows / macOS / Linux.
> - Cross-module integration is explicitly **late-stage**: only once Skyrim and
>   GTA are ~99% complete does the owner hand the whole A1 workspace to a
>   separate integration AI.
> - Therefore this module must be developed as a **library that stands alone**,
>   never as a monolith that assumes the host engine already exists.

---

Your rewrite of TES V: Skyrim is going to be integrated into a unified C++20
engine ("The Elder Scrolls: San Andreas") alongside GTA: San Andreas and a
Max Payne 2 combat module.

Do NOT build this as a standalone monolithic executable. You must package your
completed codebase as a modular C++ library target.

## Required Delivery Layout

Package your files inside `modules/skyrim/` with this exact structure:

```
modules/skyrim/
├── CMakeLists.txt                         # Defines a clean library target (e.g. skyrim_module)
├── README.md                              # API documentation and system overview
├── include/                               # PUBLIC API (Headers for the Integrator AI)
│   └── skyrim/
│       ├── skyrim_module.hpp              # Primary master header & initialization API
│       ├── cell_system.hpp                # Exterior/interior cell loading and NavMesh interfaces
│       ├── actors_radiant.hpp             # Radiant AI, schedules, factions, crimes/bounties
│       ├── rpg_stats.hpp                  # Health, Magicka, Stamina, Perk Trees, Shouts
│       ├── inventory.hpp                  # Skyrim encumbrance weight inventory & item DB
│       ├── magic_system.hpp               # Spellcasting, enchantments, and active effects
│       └── papyrus_vm.hpp                 # Papyrus VM execution & quest state machines
└── src/                                   # PRIVATE IMPLEMENTATION
    ├── world/
    ├── actors/
    ├── rpg/
    ├── inventory/
    ├── magic/
    └── quests/
```

## Architectural Rules for Integration

1. **No Hardcoded `main()`:** Expose an engine-facing lifecycle API:
   - `ISkyrimWorld::init(const SkyrimConfig& config)`
   - `ISkyrimWorld::update(float deltaTime)`
   - `ISkyrimWorld::shutdown()`
2. **Hookable Magicka & Actor Interface:** Expose an interface where external
   systems (such as Bullet Time) can consume the player's Magicka bar and
   apply active status effects.
3. **Pluggable Perk Tree:** Allow registering custom perk trees (e.g. Firearms
   trees: Handguns, Shotguns, Rifles) into the Skyrim constellation/leveling
   system.
4. **Decoupled Inventory & Quick-Select:** Keep the underlying inventory
   database (items, weight, enchantment stats) pure and accessible so the host
   engine can wire it to an external radial wheel.
5. **No Assets in Source Tree:** Use parsers that read the user's raw `.bsa`,
   `.esm`, `.esp`, and voice archives rather than embedding binary game assets
   into the repository.
