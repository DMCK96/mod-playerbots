"""ADT and WDT decoding: tile lists, blended texture-layer weights and terrain heights.

Raster convention for one tile (1024 x 1024 pixels, 64 x 64 per MCNK):
    a = indexX * 64 + alpha column (along the file name's first number, tile_x)
    b = indexY * 64 + alpha row    (along tile_y)
World position of a pixel centre:
    x = 17066.6667 - (tile_y * TILE + (b + 0.5) * YD)
    y = 17066.6667 - (tile_x * TILE + (a + 0.5) * YD)
"""
import struct

import numpy as np

TILE = 533.3333
SIZE = 1024
YD = TILE / SIZE
ORIGIN = 32 * TILE


def chunks(data, start=0, end=None):
    pos, end = start, len(data) if end is None else end
    while pos + 8 <= end:
        tag = data[pos:pos + 4][::-1].decode('latin1')
        size = struct.unpack_from('<I', data, pos + 4)[0]
        yield tag, pos, size
        pos += 8 + size


def rle(src, out_len=4096):
    out = bytearray()
    i = 0
    while len(out) < out_len and i < len(src):
        b = src[i]
        i += 1
        n = b & 0x7F
        if b & 0x80:
            out += bytes([src[i]]) * n
            i += 1
        else:
            out += src[i:i + n]
            i += n
    return bytes(out[:out_len]).ljust(out_len, b'\0')


def wdt_tiles(wdt):
    for tag, pos, size in chunks(wdt):
        if tag == 'MAIN':
            count = min(4096, size // 8)
            flags = struct.unpack_from('<%dI' % (2 * count), wdt, pos + 8)[0::2]
            return sorted((i % 64, i // 64) for i in range(count) if flags[i] & 1)
    return []


def pixel_to_world(tile_x, tile_y, a, b):
    return (ORIGIN - (tile_y * TILE + (b + 0.5) * YD), ORIGIN - (tile_x * TILE + (a + 0.5) * YD))


def world_to_pixel(x, y):
    """Inverse of pixel_to_world: (tile_x, tile_y, a, b) with a, b as floats inside the tile."""
    ua = (ORIGIN - y) / YD - 0.5
    ub = (ORIGIN - x) / YD - 0.5
    tile_x = int((ua + 0.5) // SIZE)
    tile_y = int((ub + 0.5) // SIZE)
    return tile_x, tile_y, ua - tile_x * SIZE, ub - tile_y * SIZE


def _mtex(adt):
    for tag, pos, size in chunks(adt):
        if tag == 'MTEX':
            return [s.decode('latin1') for s in adt[pos + 8:pos + 8 + size].split(b'\0') if s]
    return []


def _mcnks(adt):
    return [(pos, size) for tag, pos, size in chunks(adt) if tag == 'MCNK']


def _header(adt, pos):
    h = struct.unpack_from('<11I', adt, pos + 8)
    return {'index_x': h[1], 'index_y': h[2], 'layers': h[3], 'ofs_height': h[5], 'ofs_layer': h[7],
            'ofs_alpha': h[9], 'size_alpha': h[10]}


def _layer_alphas(adt, pos, header):
    count = header['layers']
    layers = [struct.unpack_from('<IIII', adt, pos + header['ofs_layer'] + 8 + 16 * k) for k in range(count)]
    size_alpha = header['size_alpha'] - 8 if header['size_alpha'] >= 8 else 0
    start = pos + header['ofs_alpha'] + 8
    mcal = adt[start:start + size_alpha]
    alphas = []
    for k, (_, flags, offset, _) in enumerate(layers):
        if k == 0:
            alphas.append(np.ones((64, 64), np.float32))
            continue
        if not flags & 0x100 or not mcal:
            alphas.append(np.zeros((64, 64), np.float32))
            continue
        following = layers[k + 1][2] if k + 1 < count and layers[k + 1][1] & 0x100 else len(mcal)
        raw = mcal[offset:following]
        if flags & 0x200:
            alpha = np.frombuffer(rle(raw), np.uint8).astype(np.float32) / 255.0
        elif len(raw) >= 4096:
            alpha = np.frombuffer(raw[:4096], np.uint8).astype(np.float32) / 255.0
        else:
            packed = np.frombuffer(raw[:2048].ljust(2048, b'\0'), np.uint8)
            alpha = np.empty(4096, np.float32)
            alpha[0::2] = (packed & 15) / 15.0
            alpha[1::2] = (packed >> 4) / 15.0
        alphas.append(alpha.reshape(64, 64))
    return layers, alphas


def layer_weights(adt, predicate):
    """Blended weight of the layers whose texture name satisfies `predicate`, as a (SIZE, SIZE) raster."""
    names = _mtex(adt)
    out = np.zeros((SIZE, SIZE), np.float32)
    for pos, _ in _mcnks(adt):
        header = _header(adt, pos)
        layers, alphas = _layer_alphas(adt, pos, header)
        above = np.ones((64, 64), np.float32)
        total = np.zeros((64, 64), np.float32)
        for k in range(len(layers) - 1, -1, -1):
            weight = alphas[k] * above
            above = above * (1.0 - alphas[k])
            texture = layers[k][0]
            if texture < len(names) and predicate(names[texture]):
                total += weight
        ix, iy = header['index_x'], header['index_y']
        if ix < 16 and iy < 16:
            out[ix * 64:(ix + 1) * 64, iy * 64:(iy + 1) * 64] = total.T
    return out


class HeightSampler:
    """Terrain height at raster coordinates: chunk base plus the bilinear outer MCVT grid."""

    def __init__(self, adt):
        self.chunks = {}
        for pos, _ in _mcnks(adt):
            header = _header(adt, pos)
            base = struct.unpack_from('<f', adt, pos + 8 + 0x68 + 8)[0]
            values = struct.unpack_from('<145f', adt, pos + header['ofs_height'] + 8)
            outer = np.array([[values[row * 17 + col] for col in range(9)] for row in range(9)], np.float32)
            self.chunks[(header['index_x'], header['index_y'])] = (base, outer)

    def z(self, a, b):
        a = min(max(a, 0.0), SIZE - 1e-3)
        b = min(max(b, 0.0), SIZE - 1e-3)
        key = (int(a) // 64, int(b) // 64)
        if key not in self.chunks:
            return 0.0
        base, outer = self.chunks[key]
        u = (a % 64) / 64 * 8
        v = (b % 64) / 64 * 8
        c0, r0 = min(int(u), 7), min(int(v), 7)
        fu, fv = u - c0, v - r0
        top = outer[r0, c0] * (1 - fu) + outer[r0, c0 + 1] * fu
        bottom = outer[r0 + 1, c0] * (1 - fu) + outer[r0 + 1, c0 + 1] * fu
        return float(base + top * (1 - fv) + bottom * fv)
