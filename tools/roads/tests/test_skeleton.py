import unittest

import numpy as np

from roadlib.skeleton import thin, trace


def neighbours(skel, a, b):
    return int(skel[max(a - 1, 0):a + 2, max(b - 1, 0):b + 2].sum()) - 1


def polyline_length(points):
    return sum(np.hypot(p[0] - q[0], p[1] - q[1]) for p, q in zip(points, points[1:]))


class ThinTest(unittest.TestCase):
    def test_band_thins_to_its_centreline(self):
        mask = np.zeros((64, 260), bool)
        mask[29:36, 20:240] = True
        skel = thin(mask)
        rows = np.nonzero(skel)[0]
        self.assertTrue(((rows >= 31) & (rows <= 33)).all())
        for a, b in zip(*np.nonzero(skel)):
            self.assertLessEqual(neighbours(skel, a, b), 2)


class TraceTest(unittest.TestCase):
    def test_band_gives_one_polyline(self):
        mask = np.zeros((64, 260), bool)
        mask[29:36, 20:240] = True
        lines = trace(thin(mask))
        self.assertEqual(len(lines), 1)
        self.assertGreater(polyline_length(lines[0]), 190)

    def test_t_junction_gives_three_polylines_meeting_at_one_pixel(self):
        mask = np.zeros((200, 200), bool)
        mask[97:104, 10:190] = True
        mask[100:190, 97:104] = True
        lines = trace(thin(mask))
        self.assertEqual(len(lines), 3)
        ends = [tuple(line[0]) for line in lines] + [tuple(line[-1]) for line in lines]
        shared = {end for end in ends if ends.count(end) == 3}
        self.assertEqual(len(shared), 1)

    def test_closed_ring_gives_two_polylines(self):
        mask = np.zeros((120, 120), bool)
        yy, xx = np.mgrid[:120, :120]
        radius = np.hypot(yy - 60, xx - 60)
        mask[(radius > 37) & (radius < 43)] = True
        lines = trace(thin(mask))
        self.assertEqual(len(lines), 2)
        self.assertEqual({tuple(lines[0][0]), tuple(lines[0][-1])}, {tuple(lines[1][0]), tuple(lines[1][-1])})


if __name__ == '__main__':
    unittest.main()
