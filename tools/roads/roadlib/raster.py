"""Road and trail classification of one tile's texture weights, and padding a tile with its neighbours.

Road     layers whose texture name matches ROAD_NAME, blended weight > 0.5, plus road shoulders.
Shoulder dirt blobs at most MAX_WIDTH yd wide that lie within NEAR yd of a road.
Trail    dirt blobs at most MAX_WIDTH yd wide whose estimated centreline is at least MIN_LENGTH yd.
Width is twice the median distance-to-edge over the blob's central ridge; length is area / width.
Trails are off by default: the same dirt texture paints trails, fields and rocky ground, and on the tiles checked
(Elwynn 32_48, Barrens 36_32, against the client minimap) most dirt classed as trail was not a path.
Adapted from coa-research scripts/client/adt_road_classes.py.
"""
import re

import numpy as np
from scipy import ndimage as ndi

from .adt import SIZE, YD

ROAD_NAME = re.compile(r'road|path|trail|cobble|brick|pave|street|stone(?!wall)|dirtpath', re.I)
DIRT_NAME = re.compile('dirt|trail|mud|gravel', re.I)

NONE, ROAD, TRAIL = 0, 1, 2
MAX_WIDTH, MIN_LENGTH, NEAR, MIN_PIXELS = 9.0, 60.0, 3.0, 150
CLOSE_STEPS, OPEN_STEPS, MAX_HOLE_PIXELS = 3, 1, 2000


def clean_mask(mask):
    """Close speckle and small holes so a painted road thins to one centreline instead of a mesh."""
    if not mask.any():
        return mask
    closed = ndi.binary_closing(mask, iterations=CLOSE_STEPS)
    holes = ndi.binary_fill_holes(closed) & ~closed
    if holes.any():
        labels, count = ndi.label(holes)
        sizes = ndi.sum(holes, labels, range(1, count + 1))
        closed |= np.isin(labels, np.nonzero(sizes < MAX_HOLE_PIXELS)[0] + 1)
    opened = ndi.binary_opening(closed, iterations=OPEN_STEPS)
    labels, count = ndi.label(opened)
    if not count:
        return opened
    sizes = ndi.sum(opened, labels, range(1, count + 1))
    return np.isin(labels, np.nonzero(sizes >= MIN_PIXELS)[0] + 1)


def classify(road_w, dirt_w, trails=False):
    """Road (painted road textures and the dirt shoulders beside them) and, with `trails`, narrow dirt trails."""
    road = road_w > 0.5
    dirt = (dirt_w > 0.5) & ~road
    dirt = ndi.binary_opening(dirt, iterations=1)
    near_road = ndi.binary_dilation(road, iterations=max(1, int(NEAR / YD))) if road.any() else road
    trail = np.zeros(road.shape, bool)
    if dirt.any():
        labels, _ = ndi.label(dirt)
        edt = ndi.distance_transform_edt(dirt)
        for index, region in enumerate(ndi.find_objects(labels), start=1):
            if region is None:
                continue
            blob = labels[region] == index
            area = int(blob.sum())
            if area < MIN_PIXELS:
                continue
            depth = edt[region][blob]
            width = 2 * float(np.median(depth[depth >= np.percentile(depth, 70)])) * YD
            length = area * YD * YD / max(width, 0.1)
            if width > MAX_WIDTH:
                continue
            if (blob & near_road[region]).any():
                road[region] |= blob
            elif trails and length >= MIN_LENGTH:
                trail[region] |= blob
    out = np.zeros(road.shape, np.uint8)
    out[clean_mask(trail)] = TRAIL
    out[clean_mask(road)] = ROAD
    return out


def pad_tile(get, tile_x, tile_y, pad=32):
    out = np.zeros((SIZE + 2 * pad, SIZE + 2 * pad), np.uint8)
    centre = get(tile_x, tile_y)
    if centre is None:
        return out
    spans = {-1: (slice(0, pad), slice(SIZE - pad, SIZE)), 0: (slice(pad, pad + SIZE), slice(0, SIZE)),
             1: (slice(pad + SIZE, SIZE + 2 * pad), slice(0, pad))}
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            tile = centre if dx == 0 and dy == 0 else get(tile_x + dx, tile_y + dy)
            if tile is None:
                continue
            (out_a, src_a), (out_b, src_b) = spans[dx], spans[dy]
            out[out_a, out_b] = tile[src_a, src_b]
    return out
