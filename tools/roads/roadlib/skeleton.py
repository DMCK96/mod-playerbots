"""Thinning a road mask to one-pixel centrelines and tracing them into polylines."""
import numpy as np
from scipy import ndimage as ndi

OFFSETS = ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1))
EIGHT = np.ones((3, 3), bool)


def _zhang_suen(img):
    while True:
        changed = False
        for step in (0, 1):
            c = img[1:-1, 1:-1]
            p2, p3, p4 = img[:-2, 1:-1], img[:-2, 2:], img[1:-1, 2:]
            p5, p6, p7 = img[2:, 2:], img[2:, 1:-1], img[2:, :-2]
            p8, p9 = img[1:-1, :-2], img[:-2, :-2]
            ring = (p2, p3, p4, p5, p6, p7, p8, p9, p2)
            count = sum(p.astype(np.int16) for p in ring[:-1])
            transitions = sum(((ring[i] == 0) & (ring[i + 1] == 1)).astype(np.int16) for i in range(8))
            if step == 0:
                side = (p2 * p4 * p6 == 0) & (p4 * p6 * p8 == 0)
            else:
                side = (p2 * p4 * p8 == 0) & (p2 * p6 * p8 == 0)
            remove = (c == 1) & (count >= 2) & (count <= 6) & (transitions == 1) & side
            if remove.any():
                c[remove] = 0
                changed = True
        if not changed:
            return img


def _connectivity(img, r, c):
    """Yokoi 8-connectivity number: 1 means the pixel can go without splitting its neighbours."""
    e, ne, n, nw = img[r, c + 1], img[r - 1, c + 1], img[r - 1, c], img[r - 1, c - 1]
    w, sw, s, se = img[r, c - 1], img[r + 1, c - 1], img[r + 1, c], img[r + 1, c + 1]
    x = [1 - v for v in (e, ne, n, nw, w, sw, s, se, e)]
    return sum(x[k] - x[k] * x[k + 1] * x[k + 2] for k in (0, 2, 4, 6))


def _remove_staircases(img):
    changed = True
    while changed:
        changed = False
        rows, cols = np.nonzero(img[1:-1, 1:-1])
        for r, c in zip(rows + 1, cols + 1):
            if not img[r, c]:
                continue
            if int(img[r - 1:r + 2, c - 1:c + 2].sum()) - 1 >= 2 and _connectivity(img, r, c) == 1:
                img[r, c] = 0
                changed = True
    return img


def thin(mask):
    out = np.zeros(mask.shape, bool)
    if not mask.any():
        return out
    rows, cols = np.nonzero(mask)
    r0, r1 = rows.min(), rows.max() + 1
    c0, c1 = cols.min(), cols.max() + 1
    img = np.pad(mask[r0:r1, c0:c1].astype(np.uint8), 1)
    img = _remove_staircases(_zhang_suen(img))
    out[r0:r1, c0:c1] = img[1:-1, 1:-1].astype(bool)
    return out


def trace(skel):
    """Polylines of (a, b) pixels between nodes (pixels whose neighbour count is not 2).

    Touching node pixels form one node, represented by its lowest pixel. A loop without nodes is split into two
    polylines between its first pixel and the pixel half way round.
    """
    skel = np.asarray(skel, bool)
    degree = ndi.convolve(skel.astype(np.uint8), EIGHT.astype(np.uint8), mode='constant').astype(np.int16) - 1
    node_mask = skel & (degree != 2)
    labels, _ = ndi.label(node_mask, structure=EIGHT)
    rows, cols = skel.shape
    rep = {}
    for r, c in zip(*np.nonzero(node_mask)):
        rep.setdefault(int(labels[r, c]), (int(r), int(c)))

    def neighbours(p):
        for dr, dc in OFFSETS:
            r, c = p[0] + dr, p[1] + dc
            if 0 <= r < rows and 0 <= c < cols and skel[r, c]:
                yield (r, c)

    visited = set()
    lines = []
    for r, c in zip(*np.nonzero(node_mask)):
        start = (int(r), int(c))
        label = int(labels[start])
        for first in neighbours(start):
            if labels[first] or first in visited:
                continue
            path = [rep[label]]
            prev, cur, end = start, first, None
            while True:
                visited.add(cur)
                path.append(cur)
                ahead = [q for q in neighbours(cur) if q != prev]
                ends = [q for q in ahead if labels[q] and (labels[q] != label or len(path) > 2)]
                plain = [q for q in ahead if not labels[q] and q not in visited]
                if ends:
                    end = rep[int(labels[ends[0]])]
                    break
                if not plain:
                    break
                prev, cur = cur, plain[0]
            if end is not None:
                path.append(end)
            if len(path) >= 2:
                lines.append(path)

    for r, c in zip(*np.nonzero(skel & ~node_mask)):
        p = (int(r), int(c))
        if p in visited:
            continue
        ring, prev, cur = [p], None, p
        visited.add(p)
        while True:
            ahead = [q for q in neighbours(cur) if q != prev and q not in visited]
            if not ahead:
                break
            prev, cur = cur, ahead[0]
            visited.add(cur)
            ring.append(cur)
        if len(ring) < 4:
            continue
        half = len(ring) // 2
        lines.append(ring[:half + 1])
        lines.append(ring[half:] + [ring[0]])
    return lines
