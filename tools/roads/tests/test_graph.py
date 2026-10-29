import math
import random
import unittest

from roadlib.graph import RoadGraphBuilder


def line(x0, y0, x1, y1, steps=2):
    return [(x0 + (x1 - x0) * i / (steps - 1), y0 + (y1 - y0) * i / (steps - 1)) for i in range(steps)]


def has_node_near(nodes, x, y, tol=0.5):
    return any(math.hypot(nx - x, ny - y) <= tol for nx, ny in nodes)


class MergeTest(unittest.TestCase):
    def test_close_endpoints_share_a_node(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 50, 0), 1)
        g.add_polyline(line(50.6, 0, 100, 0), 1)
        nodes, edges = g.finish()
        self.assertEqual((len(nodes), len(edges)), (3, 2))
        self.assertEqual(g.components(), 1)


class SpurTest(unittest.TestCase):
    def test_short_dead_end_off_a_junction_is_removed(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 100, 0), 1)
        g.add_polyline(line(100, 0, 200, 0), 1)
        g.add_polyline(line(100, 0, 100, 10), 1)
        g.add_polyline(line(100, 0, 100, -20), 1)
        g.prune_spurs(15.0)
        nodes, _ = g.finish()
        self.assertFalse(has_node_near(nodes, 100, 10))
        self.assertTrue(has_node_near(nodes, 100, -20))
        self.assertTrue(has_node_near(nodes, 0, 0))


class BridgeTest(unittest.TestCase):
    def test_aligned_gap_is_bridged(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 100, 0), 1)
        g.add_polyline(line(108, 1, 200, 1), 1)
        g.bridge_gaps(12.0, 35.0)
        self.assertEqual(g.components(), 1)

    def test_sideways_or_long_gap_is_not_bridged(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 100, 0), 1)
        g.add_polyline(line(105, 10, 200, 10), 1)
        g.add_polyline(line(120, 0, 220, 0), 1)
        g.bridge_gaps(12.0, 35.0)
        self.assertEqual(g.components(), 3)


class SimplifyTest(unittest.TestCase):
    def test_noisy_straight_line_keeps_its_ends(self):
        rng = random.Random(1)
        points = [(i * 1.0, rng.uniform(-0.5, 0.5)) for i in range(101)]
        g = RoadGraphBuilder()
        g.add_polyline(points, 1)
        g.simplify(2.0)
        nodes, edges = g.finish()
        self.assertEqual(len(edges), 1)

    def test_corner_survives(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 50, 0, 51) + line(50, 1, 50, 50, 50), 1)
        g.simplify(2.0)
        nodes, edges = g.finish()
        self.assertEqual(len(edges), 2)
        self.assertTrue(has_node_near(nodes, 50, 0, 1.5))


class SplitTest(unittest.TestCase):
    def test_long_edges_are_split_evenly(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 100, 0), 1)
        g.split_long(40.0)
        nodes, edges = g.finish()
        self.assertEqual(len(edges), 3)
        for a, b, _ in edges:
            self.assertLessEqual(math.dist(nodes[a], nodes[b]), 40.0)


class ComponentTest(unittest.TestCase):
    def test_short_components_are_dropped(self):
        g = RoadGraphBuilder()
        g.add_polyline(line(0, 0, 80, 0), 1)
        g.add_polyline(line(0, 500, 120, 500), 2)
        g.drop_small_components(100.0)
        nodes, edges = g.finish()
        self.assertEqual(len(edges), 1)
        self.assertEqual(edges[0][2], 2)
        self.assertTrue(has_node_near(nodes, 120, 500))


if __name__ == '__main__':
    unittest.main()
