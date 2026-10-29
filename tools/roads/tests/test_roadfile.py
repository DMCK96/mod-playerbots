import os
import struct
import tempfile
import unittest

from roadlib.roadfile import decode, encode, read_roads, write_roads

NODES = [(1.5, -2.25, 30.0), (100.0, 4.0, 31.5), (-8533.5, 64.25, 56.0)]
EDGES = [(0, 1, 1), (1, 2, 2)]


class RoadFileTest(unittest.TestCase):
    def test_layout(self):
        data = encode(NODES, EDGES)
        self.assertEqual(data[:4], b'CRDS')
        self.assertEqual(struct.unpack_from('<3I', data, 4), (1, 3, 2))
        self.assertEqual(len(data), 16 + 12 * 3 + 12 * 2)
        self.assertEqual(struct.unpack_from('<IIB3x', data, 16 + 36), (0, 1, 1))

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, '000.roads')
            write_roads(path, NODES, EDGES)
            nodes, edges = read_roads(path)
        self.assertEqual(edges, EDGES)
        for got, want in zip(nodes, NODES):
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, places=3)

    def test_rejects_bad_magic_and_truncation(self):
        with self.assertRaises(ValueError):
            decode(b'XXXX' + encode(NODES, EDGES)[4:])
        with self.assertRaises(ValueError):
            decode(encode(NODES, EDGES)[:-1])


if __name__ == '__main__':
    unittest.main()
