"""Road and trail classification of one tile's texture weights, and padding a tile with its neighbours.

Road     layers whose texture name matches ROAD_NAME, blended weight > 0.5, plus road shoulders.
Shoulder dirt blobs at most MAX_WIDTH yd wide that lie within NEAR yd of a road.
Trail    the stretches of the dirt skeleton that stay narrow, where they chain into a line of TRAIL_LENGTH yd.
Width is twice the distance to the dirt's edge, as a running median along the branch. Judging the stretch, not the
blob, keeps a path that runs into a field or a rocky patch while the field's own stretches are dropped.
Trails are off by default: the same dirt texture paints trails, fields and rocky ground.
Adapted from coa-research scripts/client/adt_road_classes.py.
"""
import re

import numpy as np
from scipy import ndimage as ndi

from .adt import SIZE, YD
from .skeleton import thin, trace

ROAD_NAME = re.compile(r'road|path|trail|cobble|brick|pave|street|stone(?!wall)|dirtpath', re.I)
DIRT_NAME = re.compile('dirt|trail|mud|gravel', re.I)

NONE, ROAD, TRAIL = 0, 1, 2
MAX_WIDTH, MIN_LENGTH, NEAR, MIN_PIXELS = 9.0, 60.0, 3.0, 150
TRAIL_LENGTH, TRAIL_WIDTH, TRAIL_WINDOW, TRAIL_WIDEN, TRAIL_MESH = 40.0, 8.0, 15, 6, 2.0
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


def trail_mask(dirt):
    """Dirt along the narrow stretches of the skeleton, kept where they chain into a long, thin line."""
    out = np.zeros(dirt.shape, bool)
    if not dirt.any():
        return out
    smooth = ndi.binary_opening(ndi.binary_closing(dirt, iterations=2), iterations=1)
    edt = ndi.distance_transform_edt(smooth)
    for line in trace(thin(smooth)):
        rows = np.fromiter((p[0] for p in line), int, len(line))
        cols = np.fromiter((p[1] for p in line), int, len(line))
        width = ndi.median_filter(2.0 * edt[rows, cols] * YD, size=TRAIL_WINDOW, mode='nearest')
        narrow = width <= TRAIL_WIDTH
        out[rows[narrow], cols[narrow]] = True
    labels, count = ndi.label(out, structure=np.ones((3, 3)))
    keep = np.zeros(dirt.shape, bool)
    for index, region in enumerate(ndi.find_objects(labels), start=1):
        if region is None:
            continue
        piece = labels[region] == index
        rows, cols = np.nonzero(piece)
        extent = float(np.hypot(np.ptp(rows), np.ptp(cols))) * YD
        if extent >= TRAIL_LENGTH and piece.sum() * YD <= TRAIL_MESH * extent:
            keep[region] |= piece
    return ndi.binary_dilation(keep, iterations=TRAIL_WIDEN) & smooth if keep.any() else keep


def classify(road_w, dirt_w, trails=False):
    """Road (painted road textures and the dirt shoulders beside them) and, with `trails`, narrow dirt trails."""
    road = road_w > 0.5
    dirt = (dirt_w > 0.5) & ~road
    dirt = ndi.binary_opening(dirt, iterations=1)
    near_road = ndi.binary_dilation(road, iterations=max(1, int(NEAR / YD))) if road.any() else road
    if dirt.any():
        labels, _ = ndi.label(dirt)
        edt = ndi.distance_transform_edt(dirt)
        for index, region in enumerate(ndi.find_objects(labels), start=1):
            if region is None:
                continue
            blob = labels[region] == index
            if int(blob.sum()) < MIN_PIXELS or not (blob & near_road[region]).any():
                continue
            depth = edt[region][blob]
            width = 2 * float(np.median(depth[depth >= np.percentile(depth, 70)])) * YD
            if width <= MAX_WIDTH:
                road[region] |= blob
    trail = trail_mask(dirt & ~road) if trails else np.zeros(road.shape, bool)
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
