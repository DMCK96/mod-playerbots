import unittest

from roadlib import underground


def mesh(points, links):
    polys = [{'c': p} for p in points]
    adjacency = {i: [] for i in range(len(points))}
    for a, b in links:
        adjacency[a].append(b)
        adjacency[b].append(a)
    return polys, adjacency


def flat(height):
    return lambda x, y: height


class ExitListTest(unittest.TestCase):
    def test_comments_and_blank_lines_are_skipped(self):
        text = '# Undercity\n\n1654 725 80.6  # north ramp\n 10 20 30\n'
        self.assertEqual(underground.parse_exits(text), [(1654.0, 725.0, 80.6), (10.0, 20.0, 30.0)])

    def test_a_line_that_is_not_three_numbers_is_an_error(self):
        with self.assertRaises(ValueError):
            underground.parse_exits('1 2\n')


class BuildTest(unittest.TestCase):
    def test_polygons_under_the_terrain_become_nodes_and_surface_ones_do_not(self):
        polys, adjacency = mesh([(0, 0, -50), (5, 0, -50), (200, 0, 100)], [(0, 1)])
        terrain = lambda x, y: 100.0
        nodes, edges, exit_nodes, notes = underground.build(polys, adjacency, terrain)
        self.assertEqual(len(nodes), 1)
        self.assertEqual(edges, [])

    def test_touching_polygons_in_other_cells_are_joined_by_an_edge(self):
        polys, adjacency = mesh([(0, 0, -50), (20, 0, -50), (40, 0, -50)], [(0, 1), (1, 2)])
        nodes, edges, _, _ = underground.build(polys, adjacency, flat(0.0))
        self.assertEqual(len(nodes), 3)
        self.assertEqual(len(edges), 2)

    def test_polygons_in_one_cell_that_do_not_touch_stay_apart(self):
        polys, adjacency = mesh([(0, 0, -50), (10, 0, -50)], [])
        nodes, edges, _, _ = underground.build(polys, adjacency, flat(0.0))
        self.assertEqual((len(nodes), len(edges)), (2, 0))

    def test_an_exit_joins_the_underground_polygons_beside_it(self):
        polys, adjacency = mesh([(0, 0, -50), (20, 0, -50), (40, 0, -3)], [(0, 1), (1, 2)])
        terrain = lambda x, y: 0.0 if x >= 40 else 100.0
        nodes, edges, exit_nodes, notes = underground.build(polys, adjacency, terrain, [(40.0, 0.0, -3.0)])
        self.assertEqual(notes, [])
        self.assertEqual(len(exit_nodes), 1)
        reached, stranded = underground.reachable_from_exits(len(nodes), edges, exit_nodes)
        self.assertEqual(len(reached), len(nodes))
        self.assertEqual(stranded, [])

    def test_an_exit_off_the_mesh_is_reported(self):
        polys, adjacency = mesh([(0, 0, -50)], [])
        _, _, exit_nodes, notes = underground.build(polys, adjacency, flat(0.0), [(500.0, 500.0, 0.0)])
        self.assertEqual(exit_nodes, [None])
        self.assertEqual(len(notes), 1)


class ReachTest(unittest.TestCase):
    def test_a_group_with_no_exit_is_listed_by_size(self):
        edges = [(0, 1, 1), (2, 3, 1), (3, 4, 1)]
        reached, stranded = underground.reachable_from_exits(5, edges, [0])
        self.assertEqual(sorted(reached), [0, 1])
        self.assertEqual(stranded, [3])


class CandidateTest(unittest.TestCase):
    def test_a_surface_polygon_beside_a_deep_region_is_a_candidate(self):
        polys, adjacency = mesh([(0, 0, -50), (10, 0, -50), (30, 0, 0), (200, 0, 0)], [(0, 1), (1, 2)])
        terrain = lambda x, y: 0.0 if x >= 30 else 100.0
        found = underground.regions(polys, adjacency, terrain, min_deep=2)
        self.assertEqual(len(found), 1)
        candidates = underground.exit_candidates(polys, adjacency, terrain, found)
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0][:3], (30.0, 0.0, 0.0))

    def test_a_shallow_overhang_is_not_a_region(self):
        polys, adjacency = mesh([(0, 0, -5), (10, 0, -5)], [(0, 1)])
        self.assertEqual(underground.regions(polys, adjacency, flat(0.0), min_deep=1), [])


if __name__ == '__main__':
    unittest.main()
