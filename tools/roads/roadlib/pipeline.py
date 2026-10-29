"""From classified tiles to a map's road graph."""
import math

import numpy as np

from .adt import SIZE, pixel_to_world, world_to_pixel
from .graph import LINK, RoadGraphBuilder, link_components
from .raster import ROAD, TRAIL, pad_tile
from .skeleton import thin, trace

PAD = 32
SPUR_YARDS, BRIDGE_YARDS, BRIDGE_DEGREES = 15.0, 12.0, 35.0
SIMPLIFY_YARDS, EDGE_YARDS, COMPONENT_YARDS = 2.0, 40.0, 100.0
JOIN_YARDS, LINK_YARDS = 30.0, 180.0


def tile_polylines(padded, tile_x, tile_y, pad=PAD):
    """World polylines of one tile, each with its majority class; pixels of the padding are cut off."""
    out = []
    for line in trace(thin(padded > 0)):
        run = []
        for pixel in line + [None]:
            if pixel is not None and pad <= pixel[0] < pad + SIZE and pad <= pixel[1] < pad + SIZE:
                run.append(pixel)
                continue
            if len(run) >= 2:
                classes = [padded[a, b] for a, b in run]
                kind = TRAIL if classes.count(TRAIL) > classes.count(ROAD) else ROAD
                out.append(([pixel_to_world(tile_x, tile_y, a - pad, b - pad) for a, b in run], kind))
            run = []
    return out


def build_map(tiles, heights):
    lines = []
    for tile_x, tile_y in sorted(tiles):
        padded = pad_tile(lambda tx, ty: tiles.get((tx, ty)), tile_x, tile_y, PAD)
        lines += tile_polylines(padded, tile_x, tile_y, PAD)
    return assemble(lines, heights, len(tiles))


def assemble(lines, heights, tile_count):
    """Clean the polylines of a whole map into nodes (x, y, z), edges (a, b, kind) and summary stats."""
    builder = RoadGraphBuilder()
    for points, kind in lines:
        builder.add_polyline(points, kind)
    builder.prune_spurs(SPUR_YARDS)
    builder.bridge_gaps(BRIDGE_YARDS, BRIDGE_DEGREES)
    builder.simplify(SIMPLIFY_YARDS)
    builder.split_long(EDGE_YARDS)
    builder.drop_small_components(COMPONENT_YARDS)
    flat, edges = builder.finish()
    edges += link_components(flat, edges, JOIN_YARDS, LINK_YARDS)

    samplers = {}
    nodes = []
    for x, y in flat:
        tile_x, tile_y, a, b = world_to_pixel(x, y)
        key = (tile_x, tile_y)
        if key not in samplers:
            samplers[key] = heights(tile_x, tile_y)
        sampler = samplers[key]
        nodes.append((x, y, sampler.z(a, b) if sampler is not None else 0.0))

    yards = {ROAD: 0.0, TRAIL: 0.0, LINK: 0.0}
    for a, b, kind in edges:
        yards[kind] = yards.get(kind, 0.0) + math.dist(flat[a], flat[b])
    stats = {'tiles': tile_count, 'nodes': len(nodes), 'edges': len(edges), 'road_yards': yards[ROAD],
             'trail_yards': yards[TRAIL], 'link_yards': yards[LINK]}
    return nodes, edges, stats


def pack_tile(classes):
    return np.packbits(classes == ROAD), np.packbits(classes == TRAIL)


def unpack_tile(packed):
    road, trail = packed
    out = np.zeros(SIZE * SIZE, np.uint8)
    out[np.unpackbits(road, count=SIZE * SIZE).astype(bool)] = ROAD
    out[np.unpackbits(trail, count=SIZE * SIZE).astype(bool)] = TRAIL
    return out.reshape(SIZE, SIZE)
