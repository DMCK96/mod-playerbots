import struct
import unittest

import numpy as np

from roadlib.adt import YD, HeightSampler, layer_weights, pixel_to_world, wdt_tiles


def chunk(tag, payload):
    return tag[::-1].encode() + struct.pack('<I', len(payload)) + payload


def mcnk(index_x, index_y, layers, alphas, base_z=0.0, outer=None):
    """layers: [(texture_id, flags)], alphas: one 64x64 uint8 array (0-15) per layer that has flag 0x100."""
    mcal = b''
    offsets = []
    pending = list(alphas)
    for _, flags in layers:
        if flags & 0x100:
            offsets.append(len(mcal))
            nib = pending.pop(0).astype(np.uint8).reshape(-1)
            mcal += bytes((int(nib[i]) | (int(nib[i + 1]) << 4)) for i in range(0, 4096, 2))
        else:
            offsets.append(0)
    mcly = b''.join(struct.pack('<IIII', tex, flags, off, 0) for (tex, flags), off in zip(layers, offsets))
    heights = [0.0] * 145
    if outer is not None:
        for row in range(9):
            for col in range(9):
                heights[row * 17 + col] = outer[row][col]
    mcvt = struct.pack('<145f', *heights)
    header_size = 128
    subs = [('MCVT', mcvt), ('MCLY', mcly), ('MCAL', mcal)]
    body = b''
    offs = {}
    for tag, payload in subs:
        offs[tag] = 8 + header_size + len(body)
        body += chunk(tag, payload)
    header = bytearray(header_size)
    struct.pack_into('<IIIII', header, 0, 0, index_x, index_y, len(layers), 0)
    struct.pack_into('<I', header, 0x14, offs['MCVT'])
    struct.pack_into('<I', header, 0x1C, offs['MCLY'])
    struct.pack_into('<II', header, 0x24, offs['MCAL'], len(mcal) + 8)
    struct.pack_into('<3f', header, 0x68, 0.0, 0.0, base_z)
    return chunk('MCNK', bytes(header) + body)


def adt(textures, mcnks):
    mtex = b''.join(t.encode() + b'\0' for t in textures)
    return chunk('MVER', struct.pack('<I', 18)) + chunk('MTEX', mtex) + b''.join(mcnks)


class WdtTest(unittest.TestCase):
    def test_lists_flagged_tiles(self):
        main = bytearray(4096 * 8)
        struct.pack_into('<I', main, (5 * 64 + 3) * 8, 1)
        struct.pack_into('<I', main, (2 * 64 + 10) * 8, 1)
        wdt = chunk('MVER', struct.pack('<I', 18)) + chunk('MPHD', bytes(32)) + chunk('MAIN', bytes(main))
        self.assertEqual(wdt_tiles(wdt), [(3, 5), (10, 2)])


class CoordinatesTest(unittest.TestCase):
    def test_pixel_centre_to_world(self):
        x, y = pixel_to_world(32, 48, 0, 0)
        self.assertAlmostEqual(x, 17066.6667 - (48 * 533.3333 + 0.5 * YD), places=2)
        self.assertAlmostEqual(y, 17066.6667 - (32 * 533.3333 + 0.5 * YD), places=2)

    def test_a_moves_world_y_and_b_moves_world_x(self):
        x0, y0 = pixel_to_world(30, 40, 100, 100)
        x1, y1 = pixel_to_world(30, 40, 101, 100)
        x2, y2 = pixel_to_world(30, 40, 100, 101)
        self.assertAlmostEqual(x1, x0, places=4)
        self.assertAlmostEqual(y1, y0 - YD, places=3)
        self.assertAlmostEqual(x2, x0 - YD, places=3)


class LayerWeightTest(unittest.TestCase):
    def test_alpha_row_maps_to_b_and_column_to_a(self):
        alpha = np.zeros((64, 64), np.uint8)
        alpha[10, :] = 15
        data = adt(['tileset\\grass.blp', 'tileset\\road.blp'], [mcnk(2, 5, [(0, 0), (1, 0x100)], [alpha])])
        w = layer_weights(data, lambda name: 'road' in name)
        self.assertEqual(w.shape, (1024, 1024))
        self.assertTrue(np.allclose(w[128:192, 5 * 64 + 10], 1.0))
        self.assertTrue(np.allclose(w[128:192, 5 * 64 + 11], 0.0))
        self.assertTrue(np.allclose(w[:128, :], 0.0))

    def test_upper_layer_covers_lower(self):
        full = np.full((64, 64), 15, np.uint8)
        data = adt(['grass', 'road', 'rock'], [mcnk(0, 0, [(0, 0), (1, 0x100), (2, 0x100)], [full, full])])
        self.assertTrue(np.allclose(layer_weights(data, lambda n: n == 'road')[:64, :64], 0.0))
        self.assertTrue(np.allclose(layer_weights(data, lambda n: n == 'rock')[:64, :64], 1.0))

    def test_base_layer_counts_where_nothing_covers_it(self):
        data = adt(['road'], [mcnk(1, 1, [(0, 0)], [])])
        self.assertTrue(np.allclose(layer_weights(data, lambda n: n == 'road')[64:128, 64:128], 1.0))


class HeightTest(unittest.TestCase):
    def test_base_plus_outer_vertices(self):
        outer = [[float(col) for col in range(9)] for _ in range(9)]
        data = adt(['grass'], [mcnk(0, 0, [(0, 0)], [], base_z=100.0, outer=outer)])
        sampler = HeightSampler(data)
        self.assertAlmostEqual(sampler.z(0.0, 0.0), 100.0, places=3)
        self.assertAlmostEqual(sampler.z(64.0 * 4 / 8, 10.0), 104.0, places=3)
        self.assertGreater(sampler.z(60.0, 30.0), sampler.z(10.0, 30.0))


if __name__ == '__main__':
    unittest.main()
