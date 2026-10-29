import unittest

import numpy as np

from roadlib.raster import classify, pad_tile
from roadlib.skeleton import thin, trace

N = 1024


def empty():
    return np.zeros((N, N), np.float32)


class ClassifyTest(unittest.TestCase):
    def test_road_texture_is_road(self):
        road = empty()
        road[100:106, 100:600] = 1.0
        self.assertGreater((classify(road, empty())[100:106, 100:600] == 1).mean(), 0.95)

    def test_long_narrow_dirt_is_trail(self):
        dirt = empty()
        dirt[300:310, 100:500] = 1.0
        cls = classify(empty(), dirt, trails=True)
        self.assertGreater((cls[300:310, 100:500] == 2).mean(), 0.9)

    def test_trails_are_off_by_default(self):
        dirt = empty()
        dirt[300:310, 100:500] = 1.0
        self.assertEqual(int((classify(empty(), dirt) > 0).sum()), 0)

    def test_speckled_road_thins_to_one_centreline(self):
        rng = np.random.default_rng(7)
        road = empty()
        road[400:420, 100:700] = 1.0
        road[(rng.random((N, N)) < 0.08) & (road > 0)] = 0.0
        road[(rng.random((N, N)) < 0.002)] = 1.0
        self.assertEqual(len(trace(thin(classify(road, empty()) > 0))), 1)

    def test_wide_dirt_is_dropped(self):
        dirt = empty()
        dirt[500:700, 500:700] = 1.0
        self.assertEqual(int((classify(empty(), dirt, trails=True) > 0).sum()), 0)

    def test_short_narrow_dirt_is_dropped(self):
        dirt = empty()
        dirt[800:810, 100:160] = 1.0
        self.assertEqual(int((classify(empty(), dirt, trails=True) > 0).sum()), 0)

    def test_dirt_beside_road_is_road(self):
        road = empty()
        road[100:106, 100:600] = 1.0
        dirt = empty()
        dirt[106:114, 100:200] = 1.0
        cls = classify(road, dirt)
        self.assertGreater((cls[106:114, 100:200] == 1).mean(), 0.9)


class PadTest(unittest.TestCase):
    def test_neighbours_fill_the_border(self):
        tiles = {(10, 10): np.full((N, N), 1, np.uint8), (11, 10): np.full((N, N), 2, np.uint8)}
        padded = pad_tile(lambda tx, ty: tiles.get((tx, ty)), 10, 10, 32)
        self.assertEqual(padded.shape, (N + 64, N + 64))
        self.assertTrue((padded[32:32 + N, 32:32 + N] == 1).all())
        self.assertTrue((padded[32 + N:, 32:32 + N] == 2).all())
        self.assertTrue((padded[:32, 32:32 + N] == 0).all())


if __name__ == '__main__':
    unittest.main()
