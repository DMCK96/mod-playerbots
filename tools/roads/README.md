# Road graphs for bots

`build_roads.py` reads a WoW 3.3.5a client's terrain, finds the roads and dirt trails painted on it, and writes
one road graph per map. With the graphs in place, random bots on long trips usually follow roads and trails
instead of the shortest walkable line (`AiPlayerbot.CoaRoadShare`, `AiPlayerbot.CoaRoadMinTrip` in
`playerbots.conf`). Without them, bots move as before.

The graphs are derived from the client, like `maps/` and `mmaps/`. Each server builds them from its own client;
nothing is sent to players.

## Requirements

Python 3.10 or later with `numpy`, `scipy` and `Pillow`:

```
python -m pip install numpy scipy pillow
```

## Usage

```
python -I tools/roads/build_roads.py <client folder> <DataDir>/roads
```

- `<client folder>` holds `Data/` with the MPQ archives. They are read in the same order as `map_extractor`,
  so patched terrain wins.
- `<DataDir>` is the worldserver's `DataDir`, the folder holding `maps/` and `mmaps/`. The server loads
  `<DataDir>/roads/<mapId>.roads` the first time a bot travels on that map.
- `--maps 0,1,530,571` builds only those maps. The default is every map in `Map.dbc` with terrain tiles,
  instances and battlegrounds included.
- `--jobs N` sets the worker processes (default 4). A full client takes a while; each map prints a summary line.
- `--preview DIR --tiles 0_32_48,1_36_32` writes a PNG per named tile (`<map>_<x>_<y>`), north up. It shows road
  in red, trail in green and graph edges in white, so a map can be checked by eye.

Restart the worldserver after rebuilding; graphs are cached per map once loaded.

## How it works

1. **Layers:** each terrain tile's texture layers are blended. Textures named like roads (road, path, cobble,
   brick, stone...) above half weight are road.
2. **Dirt:** dirt blobs at most 9 yd wide that touch a road are road shoulders. Elsewhere the dirt is thinned to
   a skeleton and the stretches where its width (a 15-pixel running median) stays at 8 yd or less are kept; kept
   stretches that chain into a line at least 40 yd long and not a mesh are trails. Fields, river beds and rocky
   patches are wide or tangled and are ignored; a trail that runs into one is kept. `--no-trails` turns trails off.
3. **Centrelines:** the mask is thinned to one-pixel centrelines (about half a yard per pixel), traced into
   polylines and joined across tile borders.
4. **Clean-up:**
   - Spurs under 15 yd are pruned.
   - Gaps up to 12 yd that keep the trail's direction are bridged.
   - Polylines are simplified and split into edges of at most 40 yd.
   - Pieces under 100 yd in total are dropped.
5. **Links:** each dead end proposes the nearest node of another piece within 180 yd; shortest first, a proposal
   is kept while the two pieces are still apart. Gaps up to 30 yd (paint that fades out) become road. Longer
   ones become links (kind 3): bridges, town gates and city walls are buildings, not painted terrain, so the
   textures stop at them. Links are not checked against the navmesh here; the server plans over them at a 25 %
   higher cost, walks each leg on the navmesh and drops a road route whose leg the navmesh cannot walk or only
   reaches by a long detour, blocking those edges for later trips.

## File format (version 1, little-endian)

| Part | Layout |
| --- | --- |
| Header | `char[4] "CRDS"`, `uint32 version = 1`, `uint32 nodeCount`, `uint32 edgeCount` |
| Node | `float x, y, z` (world coordinates; z is the terrain height, a hint only) |
| Edge | `uint32 a, uint32 b, uint8 kind` (1 road, 2 trail, 3 link), 3 zero bytes |

## Limits

- **Building-only maps:** maps made only of buildings (WMO-only, most dungeon interiors) have no terrain
  textures and get no graph. The summary lists them as skipped.
- **Water:** liquid is not read. Bots skip a waypoint that lands in water.
- **Heights:** on bridges and in towns the terrain height is not the walkable floor. Bots path to each waypoint
  with the navmesh, and give up the road for that trip when a leg cannot be walked.
- **Thresholds:** they were tuned on a few Elwynn Forest and Barrens tiles. Use the previews to check other zones.

## Tests

```
cd tools/roads
python -B -m unittest discover -s tests -t .
```
