import math
import unittest

import numpy as np

from roadlib.adt import SIZE, pixel_to_world
from roadlib.graph import RoadGraphBuilder
from roadlib.pipeline import build_map, tile_polylines
from roadlib.raster import pad_tile


def band_across_border():
    left = np.zeros((SIZE, SIZE), np.uint8)
    right = np.zeros((SIZE, SIZE), np.uint8)
    left[SIZE - 200:, 500:507] = 1
    right[:200, 500:507] = 1
    return {(20, 30): left, (21, 30): right}


class TilePolylineTest(unittest.TestCase):
    def test_polylines_stay_inside_their_tile(self):
        tiles = band_across_border()
        padded = pad_tile(lambda tx, ty: tiles.get((tx, ty)), 20, 30, 32)
        x_max, y_max = pixel_to_world(20, 30, -0.5, -0.5)
        x_min, y_min = pixel_to_world(20, 30, SIZE - 0.5, SIZE - 0.5)
        for points, kind in tile_polylines(padded, 20, 30, 32):
            self.assertEqual(kind, 1)
            for x, y in points:
                self.assertTrue(x_min - 1 <= x <= x_max + 1 and y_min - 1 <= y <= y_max + 1)

    def test_band_crossing_a_border_becomes_one_component(self):
        tiles = band_across_border()
        builder = RoadGraphBuilder()
        for tx, ty in tiles:
            padded = pad_tile(lambda x, y: tiles.get((x, y)), tx, ty, 32)
            for points, kind in tile_polylines(padded, tx, ty, 32):
                builder.add_polyline(points, kind)
        self.assertEqual(builder.components(), 1)


class BuildMapTest(unittest.TestCase):
    def test_builds_nodes_with_heights_and_drops_specks(self):
        tiles = band_across_border()
        tiles[(20, 30)][100:104, 100:110] = 2

        class Flat:
            def z(self, a, b):
                return 42.0

        nodes, edges, stats = build_map(tiles, lambda tx, ty: Flat())
        self.assertGreater(len(edges), 3)
        self.assertTrue(all(abs(z - 42.0) < 1e-3 for _, _, z in nodes))
        for a, b, kind in edges:
            self.assertLessEqual(math.dist(nodes[a][:2], nodes[b][:2]), 40.0 + 1e-3)
        self.assertEqual(stats['tiles'], 2)
        self.assertGreater(stats['road_yards'], 150)


if __name__ == '__main__':
    unittest.main()
