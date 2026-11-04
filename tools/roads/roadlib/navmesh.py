"""Navmesh polygons of a rectangle of mmtiles, with the joins between them, and the terrain height from the .map files."""
import struct
from collections import defaultdict

TILE_YARDS = 533.33333333
CLIMB_YARDS = 1.7


class Terrain:
    def __init__(self, data_dir, map_id):
        self.data_dir, self.map_id, self.tiles = data_dir, map_id, {}

    def __call__(self, x, y):
        gx, gy = int(32 - x / TILE_YARDS), int(32 - y / TILE_YARDS)
        if (gx, gy) not in self.tiles:
            self.tiles[(gx, gy)] = self._load(gx, gy)
        values = self.tiles[(gx, gy)]
        if values is None:
            return None
        fx, fy = (32 - x / TILE_YARDS - gx) * 128, (32 - y / TILE_YARDS - gy) * 128
        return values[min(int(fx), 127) * 129 + min(int(fy), 127)]

    def _load(self, gx, gy):
        try:
            raw = open('%s/maps/%03d%02d%02d.map' % (self.data_dir, self.map_id, gx, gy), 'rb').read()
        except OSError:
            return None
        offset, = struct.unpack_from('<I', raw, 20)
        _, flags, low, high = struct.unpack_from('<4sIff', raw, offset)
        if flags & 1:
            return None
        if flags & 2:
            return [low + v * (high - low) / 65535 for v in struct.unpack_from('<%dH' % 16641, raw, offset + 16)]
        if flags & 4:
            return [low + v * (high - low) / 255 for v in struct.unpack_from('<%dB' % 16641, raw, offset + 16)]
        return list(struct.unpack_from('<%df' % 16641, raw, offset + 16))


def load(data_dir, map_id, gx_range, gy_range):
    """(polys, adjacency): polys are {'c': (x, y, z), 'tile': (gx, gy)}, adjacency maps an index to its neighbours' indices."""
    polys, borders, adjacency = [], defaultdict(list), defaultdict(list)
    for gx in range(gx_range[0], gx_range[1] + 1):
        for gy in range(gy_range[0], gy_range[1] + 1):
            try:
                data = open('%s/mmaps/%03d%02d%02d.mmtile' % (data_dir, map_id, gx, gy), 'rb').read()
            except OSError:
                continue
            body = data[data.find(b'VAND'):]
            head = struct.unpack_from('<15i', body, 0)
            poly_count, vert_count = head[6], head[7]
            offset = 100
            verts = [struct.unpack_from('<3f', body, offset + 12 * i) for i in range(vert_count)]
            offset += 12 * vert_count
            base = len(polys)
            for _ in range(poly_count):
                indices = struct.unpack_from('<6H', body, offset + 4)
                links = struct.unpack_from('<6H', body, offset + 16)
                count = struct.unpack_from('<B', body, offset + 30)[0]
                shape = [verts[k] for k in indices[:count]]
                index = len(polys)
                polys.append({'c': (sum(v[2] for v in shape) / count, sum(v[0] for v in shape) / count,
                                    sum(v[1] for v in shape) / count), 'tile': (gx, gy)})
                for e, link in enumerate(links[:count]):
                    a, b = shape[e], shape[(e + 1) % count]
                    if link & 0x8000:
                        for axis in (0, 2):
                            if abs(a[axis] - b[axis]) < 0.01:
                                other = 2 if axis == 0 else 0
                                borders[(axis, round(a[axis] * 20) / 20)].append(
                                    (index, min(a[other], b[other]), max(a[other], b[other]), a, b, other))
                    elif link:
                        adjacency[index].append(base + link - 1)
                offset += 32
    for group in borders.values():
        for i in range(len(group)):
            for j in range(i + 1, len(group)):
                first, low1, high1, a1, b1, other = group[i]
                second, low2, high2, a2, b2, _ = group[j]
                if polys[first]['tile'] == polys[second]['tile'] or min(high1, high2) - max(low1, low2) < 0.05:
                    continue
                mid = (max(low1, low2) + min(high1, high2)) / 2

                def height(a, b):
                    span = b[other] - a[other]
                    return a[1] if abs(span) < 1e-6 else a[1] + (b[1] - a[1]) * (mid - a[other]) / span
                if abs(height(a1, b1) - height(a2, b2)) <= CLIMB_YARDS:
                    adjacency[first].append(second)
                    adjacency[second].append(first)
    for i in range(len(polys)):
        adjacency[i]
    return polys, adjacency
