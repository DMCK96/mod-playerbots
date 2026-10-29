"""Assembling traced road polylines (world yards) into a graph and cleaning it up."""
import math
from collections import defaultdict

LINK = 3
_LINK_CELL = 32.0


def _length(points):
    return sum(math.dist(p, q) for p, q in zip(points, points[1:]))


def _douglas_peucker(points, tolerance):
    if len(points) < 3:
        return list(points)
    (x0, y0), (x1, y1) = points[0], points[-1]
    dx, dy = x1 - x0, y1 - y0
    norm = math.hypot(dx, dy)
    best, index = -1.0, 0
    for i in range(1, len(points) - 1):
        px, py = points[i]
        if norm == 0.0:
            d = math.hypot(px - x0, py - y0)
        else:
            d = abs(dy * (px - x0) - dx * (py - y0)) / norm
        if d > best:
            best, index = d, i
    if best <= tolerance:
        return [points[0], points[-1]]
    left = _douglas_peucker(points[:index + 1], tolerance)
    return left[:-1] + _douglas_peucker(points[index:], tolerance)


class _Line:
    __slots__ = ('start', 'end', 'interior', 'kind', 'alive')

    def __init__(self, start, end, interior, kind):
        self.start, self.end, self.interior, self.kind, self.alive = start, end, interior, kind, True


class RoadGraphBuilder:
    def __init__(self, merge_tolerance=1.5):
        self.tolerance = merge_tolerance
        self.nodes = []
        self.grid = defaultdict(list)
        self.lines = []

    def _cell(self, x, y):
        return int(math.floor(x / self.tolerance)), int(math.floor(y / self.tolerance))

    def _near(self, x, y, radius):
        cx, cy = self._cell(x, y)
        reach = int(math.ceil(radius / self.tolerance))
        for gx in range(cx - reach, cx + reach + 1):
            for gy in range(cy - reach, cy + reach + 1):
                for node in self.grid.get((gx, gy), ()):
                    d = math.dist(self.nodes[node], (x, y))
                    if d <= radius:
                        yield d, node

    def _node(self, point):
        x, y = float(point[0]), float(point[1])
        found = min(self._near(x, y, self.tolerance), default=None)
        if found is not None:
            return found[1]
        self.nodes.append((x, y))
        self.grid[self._cell(x, y)].append(len(self.nodes) - 1)
        return len(self.nodes) - 1

    def _points(self, line):
        return [self.nodes[line.start]] + line.interior + [self.nodes[line.end]]

    def _alive(self):
        return [line for line in self.lines if line.alive]

    def _degrees(self):
        degree = defaultdict(int)
        for line in self._alive():
            degree[line.start] += 1
            degree[line.end] += 1
        return degree

    def add_polyline(self, points, kind):
        if len(points) < 2:
            return
        start, end = self._node(points[0]), self._node(points[-1])
        interior = [(float(x), float(y)) for x, y in points[1:-1]]
        if start == end and not interior:
            return
        self.lines.append(_Line(start, end, interior, kind))

    def prune_spurs(self, min_length):
        degree = self._degrees()
        changed = True
        while changed:
            changed = False
            for line in self._alive():
                for dead, other in ((line.start, line.end), (line.end, line.start)):
                    if degree[dead] == 1 and degree[other] >= 3 and _length(self._points(line)) < min_length:
                        line.alive = False
                        degree[line.start] -= 1
                        degree[line.end] -= 1
                        changed = True
                        break

    def bridge_gaps(self, max_gap, max_turn_deg):
        degree = self._degrees()
        owner = {}
        for line in self._alive():
            for node in (line.start, line.end):
                if degree[node] == 1:
                    owner[node] = line
        bridged = set()
        limit = math.cos(math.radians(max_turn_deg))
        for dead in sorted(owner):
            if dead in bridged:
                continue
            line = owner[dead]
            points = self._points(line)
            behind = points[-2] if dead == line.end else points[1]
            ox, oy = self.nodes[dead][0] - behind[0], self.nodes[dead][1] - behind[1]
            outward = math.hypot(ox, oy)
            if outward == 0.0:
                continue
            best = None
            for d, node in sorted(self._near(*self.nodes[dead], max_gap)):
                if node in (line.start, line.end) or degree[node] == 0 or node in bridged:
                    continue
                bx, by = self.nodes[node][0] - self.nodes[dead][0], self.nodes[node][1] - self.nodes[dead][1]
                if d > 0.0 and (ox * bx + oy * by) / (outward * d) >= limit:
                    best = node
                    break
            if best is None:
                continue
            self.lines.append(_Line(dead, best, [], line.kind))
            bridged.add(dead)
            if degree[best] == 1:
                bridged.add(best)

    def simplify(self, tolerance):
        for line in self._alive():
            line.interior = _douglas_peucker(self._points(line), tolerance)[1:-1]

    def split_long(self, max_length):
        for line in self._alive():
            points = self._points(line)
            out = [points[0]]
            for p, q in zip(points, points[1:]):
                parts = max(1, math.ceil(math.dist(p, q) / max_length))
                for k in range(1, parts):
                    out.append((p[0] + (q[0] - p[0]) * k / parts, p[1] + (q[1] - p[1]) * k / parts))
                out.append(q)
            line.interior = out[1:-1]

    def _component_roots(self):
        parent = {}

        def find(node):
            parent.setdefault(node, node)
            while parent[node] != node:
                parent[node] = parent[parent[node]]
                node = parent[node]
            return node

        for line in self._alive():
            ra, rb = find(line.start), find(line.end)
            if ra != rb:
                parent[ra] = rb
        return find

    def drop_small_components(self, min_total):
        find = self._component_roots()
        total = defaultdict(float)
        for line in self._alive():
            total[find(line.start)] += _length(self._points(line))
        for line in self._alive():
            if total[find(line.start)] < min_total:
                line.alive = False

    def components(self):
        find = self._component_roots()
        return len({find(line.start) for line in self._alive()})

    def finish(self):
        ids = {}
        nodes = []
        edges = []
        seen = set()

        def node_id(key, point):
            if key not in ids:
                ids[key] = len(nodes)
                nodes.append((float(point[0]), float(point[1])))
            return ids[key]

        for index, line in enumerate(self.lines):
            if not line.alive:
                continue
            chain = [node_id(('n', line.start), self.nodes[line.start])]
            for k, point in enumerate(line.interior):
                chain.append(node_id(('p', index, k), point))
            chain.append(node_id(('n', line.end), self.nodes[line.end]))
            for a, b in zip(chain, chain[1:]):
                pair = (min(a, b), max(a, b))
                if a == b or pair in seen or math.dist(nodes[a], nodes[b]) == 0.0:
                    continue
                seen.add(pair)
                edges.append((pair[0], pair[1], line.kind))
        return nodes, edges


def link_components(nodes, edges, join_yards, link_yards):
    """Edges that join separate components where the textures stop: each dead end (degree <= 1) proposes the nearest
    node of another component within `link_yards`. Shortest first, a proposal is kept only while its two components
    are still apart. Gaps up to `join_yards` become road; longer ones become LINK edges (bridges, gates, faded
    paint) that the server walks on the navmesh and drops when they fail."""
    parent = list(range(len(nodes)))

    def find(node):
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    degree = defaultdict(int)
    for a, b, _ in edges:
        degree[a] += 1
        degree[b] += 1
        parent[find(a)] = find(b)
    component = [find(node) for node in range(len(nodes))]

    grid = defaultdict(list)
    for node, (x, y) in enumerate(p[:2] for p in nodes):
        if degree[node]:
            grid[(math.floor(x / _LINK_CELL), math.floor(y / _LINK_CELL))].append(node)
    reach = int(math.ceil(link_yards / _LINK_CELL))

    proposals = []
    for dead in range(len(nodes)):
        if degree[dead] != 1:
            continue
        x, y = nodes[dead][:2]
        cx, cy = math.floor(x / _LINK_CELL), math.floor(y / _LINK_CELL)
        best = None
        for gx in range(cx - reach, cx + reach + 1):
            for gy in range(cy - reach, cy + reach + 1):
                for node in grid.get((gx, gy), ()):
                    if component[node] == component[dead]:
                        continue
                    d = math.dist(nodes[node][:2], (x, y))
                    if d <= link_yards and (best is None or d < best[0]):
                        best = (d, node)
        if best is not None:
            proposals.append((best[0], dead, best[1]))

    parent = list(range(len(nodes)))
    for a, b, _ in edges:
        parent[find(a)] = find(b)
    links = []
    for d, a, b in sorted(proposals):
        if find(a) == find(b):
            continue
        parent[find(a)] = find(b)
        links.append((min(a, b), max(a, b), 1 if d <= join_yards else LINK))
    return links
