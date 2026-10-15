# Roadmap

The single source of truth for the README progress bar. `[x]` done, `[~]` partly
done (counts half), `[ ]` not started. The number in `(w N)` is a rough effort
weight, so the world streamer does not count the same as the Papyrus VM. After
changing anything here, run `python scripts/progress.py` to refresh the README.

## Data and archives

- [x] TES4 ESM/ESP record walker: nested groups, compressed records, subrecords (w 2)
- [x] BSA archive reader (v103 / v104 / v105), seek-based (w 2)
- [x] String tables (`.STRINGS`) (w 1)
- [x] Runtime NIF mesh loader for Skyrim LE (`NiTriShape` / `NiTriStrips`) (w 3)
- [x] DDS texture decoder (BC1 / BC2 / BC3 and uncompressed) (w 2)

## World

- [x] Cell hierarchy and spatial index (interiors, exteriors, persistent cells) (w 2)
- [x] Cell to placed models to meshes pipeline (`MODL` → archive → mesh) (w 2)
- [~] Cell streaming around the player (configurable grid; no unloading or LOD objects yet) (w 2)
- [x] Terrain: `LAND` heightmaps (`VHGT`) and walkable ground (w 2)
- [~] Land layer textures: per-quadrant `ATXT`/`BTXT` selection, no per-vertex blending yet (w 1)
- [x] Distant terrain: coarse `LAND` ring out to the horizon, one draw call, no textures (w 2)
- [ ] Interiors: doors, portals and interior lighting (w 2)
- [ ] Water (w 1)
- [ ] Weather, sky and time of day (w 2)

## Playing

- [x] Direct3D 9 renderer: lit, textured, alpha cutout (w 2)
- [x] Daylight: sun, sky fill, post-lighting exposure and fog out to the far terrain (w 2)
- [x] First-person controller: gravity, collision, walk and fly (w 2)
- [ ] Collision from real geometry (triangles, not bounding boxes) (w 3)
- [ ] Third-person camera (w 1)
- [ ] HUD, menus and settings (w 2)

## Launcher

- [x] Locate the Skyrim install automatically and remember it (w 1)
- [ ] Extract/import the player's data so the original install can be deleted (w 4)
- [ ] Mod load order (`plugins.txt`, master resolution) (w 2)

## Gameplay

- [ ] Stats: health, magicka, stamina, skills and perks (w 3)
- [ ] Inventory and item database (w 2)
- [ ] Combat: melee, archery and magic (w 4)
- [ ] Shouts and dragon souls (w 2)
- [ ] NPCs and radiant AI (w 4)
- [ ] Quests and dialogue (w 4)
- [ ] Papyrus script VM (w 4)

## Audio

- [ ] Sound effects (w 2)
- [ ] Music (w 1)
- [ ] Voice (w 1)
