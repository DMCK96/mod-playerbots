"""Build bot road graphs from a WoW 3.3.5a client's terrain textures.

Usage: python -I build_roads.py <client dir> <out dir> [--maps 0,1,530] [--jobs 4] [--no-trails]
                                [--preview DIR --tiles 0_32_48,1_36_32]
Writes <out dir>/<mapId:03d>.roads for every map with ADT tiles and at least one road edge. See README.md.
"""
import argparse
import multiprocessing
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

from roadlib import adt as adtlib  # noqa: E402
from roadlib.archives import open_client  # noqa: E402
from roadlib.dbc import parse_map_dbc  # noqa: E402
from roadlib.pipeline import PAD, assemble, pack_tile, tile_polylines, unpack_tile  # noqa: E402
from roadlib.raster import DIRT_NAME, ROAD_NAME, classify, pad_tile  # noqa: E402
from roadlib.roadfile import write_roads  # noqa: E402

_archives = None
BATCH = 32  # padded tiles handed to the workers at a time, about 1.2 MB each


_trails = False


def _open(client, trails):
    global _archives, _trails
    _archives = open_client(client)
    _trails = trails


def _classify(task):
    directory, tile_x, tile_y = task
    name = 'World\\Maps\\%s\\%s_%d_%d.adt' % (directory, directory, tile_x, tile_y)
    try:
        data = _archives.read(name)
        if not data:
            return (tile_x, tile_y), None, None, 'missing'
        road = adtlib.layer_weights(data, lambda n: bool(ROAD_NAME.search(n)))
        dirt = adtlib.layer_weights(data, lambda n: bool(DIRT_NAME.search(n)) and not ROAD_NAME.search(n))
        return (tile_x, tile_y), pack_tile(classify(road, dirt, _trails)), adtlib.HeightSampler(data), None
    except Exception as error:  # a broken tile must not stop the map
        return (tile_x, tile_y), None, None, '%s: %s' % (type(error).__name__, error)


def _polylines(task):
    padded, tile_x, tile_y = task
    return tile_polylines(padded, tile_x, tile_y, PAD)


def _preview(path, classes, tile_x, tile_y, nodes, edges):
    from PIL import Image, ImageDraw
    palette = np.array([[0, 0, 0], [226, 69, 58], [78, 200, 120]], np.uint8)
    image = Image.fromarray(palette[classes].transpose(1, 0, 2))
    draw = ImageDraw.Draw(image)
    for a, b, _ in edges:
        ends = []
        for node in (nodes[a], nodes[b]):
            tx, ty, pa, pb = adtlib.world_to_pixel(node[0], node[1])
            ends.append((pa + (tx - tile_x) * adtlib.SIZE, pb + (ty - tile_y) * adtlib.SIZE))
        if all(-50 <= p <= adtlib.SIZE + 50 for end in ends for p in end):
            draw.line(ends, fill=(255, 255, 255), width=1)
    image.save(path)


def build(args):
    started = time.time()
    archives = open_client(args.client)
    dbc = archives.read('DBFilesClient\\Map.dbc')
    if not dbc:
        print('error: DBFilesClient\\Map.dbc not found in the client archives', file=sys.stderr)
        return 2
    wanted = {int(m) for m in args.maps.split(',')} if args.maps else None
    previews = {}
    for spec in (args.tiles.split(',') if args.tiles else []):
        map_id, tile_x, tile_y = (int(v) for v in spec.split('_'))
        previews.setdefault(map_id, set()).add((tile_x, tile_y))

    totals = {'maps': 0, 'skipped': 0, 'nodes': 0, 'edges': 0, 'road_yards': 0.0, 'trail_yards': 0.0,
              'link_yards': 0.0}
    with multiprocessing.Pool(args.jobs, initializer=_open, initargs=(args.client, args.trails)) as pool:
        for row in sorted(parse_map_dbc(dbc), key=lambda r: r.id):
            if wanted is not None and row.id not in wanted:
                continue
            wdt = archives.read('World\\Maps\\%s\\%s.wdt' % (row.directory, row.directory))
            tiles = adtlib.wdt_tiles(wdt) if wdt else []
            if not tiles:
                totals['skipped'] += 1
                print('map %d %s: skipped (%s)' % (row.id, row.directory, 'wmo-only' if wdt else 'no-wdt'),
                      flush=True)
                continue
            map_started = time.time()
            classes, heights = {}, {}
            for key, packed, sampler, error in pool.imap_unordered(
                    _classify, [(row.directory, x, y) for x, y in tiles], chunksize=4):
                if error:
                    print('  tile %d_%d_%d: %s' % (row.id, key[0], key[1], error), flush=True)
                    continue
                classes[key], heights[key] = packed, sampler

            def get(tx, ty):
                packed = classes.get((tx, ty))
                return None if packed is None else unpack_tile(packed)

            lines = []
            keys = sorted(classes)
            for start in range(0, len(keys), BATCH):
                batch = [(pad_tile(get, x, y, PAD), x, y) for x, y in keys[start:start + BATCH]]
                for result in pool.imap(_polylines, batch, chunksize=2):
                    lines += result
            nodes, edges, stats = assemble(lines, lambda tx, ty: heights.get((tx, ty)), len(classes))
            if edges:
                write_roads(os.path.join(args.out, '%03d.roads' % row.id), nodes, edges)
            for tile_x, tile_y in sorted(previews.get(row.id, ())):
                tile = get(tile_x, tile_y)
                if tile is not None:
                    os.makedirs(args.preview, exist_ok=True)
                    _preview(os.path.join(args.preview, '%d_%d_%d.png' % (row.id, tile_x, tile_y)), tile,
                             tile_x, tile_y, nodes, edges)
            totals['maps'] += 1
            for key in ('nodes', 'edges', 'road_yards', 'trail_yards', 'link_yards'):
                totals[key] += stats[key]
            print('map %d %s: tiles %d, nodes %d, edges %d, road %.0f yd, trail %.0f yd, links %.0f yd, %.0fs%s'
                  % (row.id, row.directory, len(classes), stats['nodes'], stats['edges'], stats['road_yards'],
                     stats['trail_yards'], stats['link_yards'], time.time() - map_started,
                     '' if edges else ' (no file)'), flush=True)
    print('total: %d maps built, %d skipped, nodes %d, edges %d, road %.0f yd, trail %.0f yd, links %.0f yd, %.0fs'
          % (totals['maps'], totals['skipped'], totals['nodes'], totals['edges'], totals['road_yards'],
             totals['trail_yards'], totals['link_yards'], time.time() - started))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('client', help='client folder holding Data/')
    parser.add_argument('out', help='output folder, normally <DataDir>/roads')
    parser.add_argument('--maps', help='comma list of map ids (default: every map with terrain tiles)')
    parser.add_argument('--jobs', type=int, default=4, help='worker processes (default 4)')
    parser.add_argument('--preview', help='folder for preview PNGs of the tiles named by --tiles')
    parser.add_argument('--tiles', help='comma list of map_x_y tiles to preview')
    parser.add_argument('--no-trails', dest='trails', action='store_false',
                        help='leave out narrow dirt trails and keep only painted roads')
    args = parser.parse_args(argv)
    if args.tiles and not args.preview:
        parser.error('--tiles needs --preview')
    return build(args)


if __name__ == '__main__':
    sys.exit(main())
