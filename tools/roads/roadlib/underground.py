"""Underground waypoint graph from the navmesh: where a bot stuck below the terrain walks to leave.

The navmesh polygons whose floor lies COVERED_YARDS or more under the terrain are grouped into small cells, one node per
connected group of polygons in a cell, and two nodes are joined where their polygons touch. A listed exit point is a node
too, joined to the polygons next to it. The graph is written in the road file format (`roadfile`), z being the floor.
"""
import math
from collections import defaultdict, deque

COVERED_YARDS = 4.0
CELL_YARDS = 16.0
CELL_HEIGHT_YARDS = 6.0
EXIT_SNAP_YARDS = 12.0
ROAD = 1


def parse_exits(text):
    """Exit points from a text list: one `x y z` per line, `#` starts a comment."""
    exits = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3:
            raise ValueError('exit list line %d: expected x y z' % number)
        exits.append(tuple(float(p) for p in parts))
    return exits


def covered(poly, terrain):
    height = terrain(poly['c'][0], poly['c'][1])
    return height is not None and height - poly['c'][2] >= COVERED_YARDS


def _components(ids, adjacency):
    seen, groups = set(), []
    members = set(ids)
    for start in ids:
        if start in seen:
            continue
        seen.add(start)
        group, queue = [], deque([start])
        while queue:
            current = queue.popleft()
            group.append(current)
            for other in adjacency[current]:
                if other in members and other not in seen:
                    seen.add(other)
                    queue.append(other)
        groups.append(group)
    return groups


def build(polys, adjacency, terrain, exits=()):
    """Nodes and edges of the underground graph.

    polys: list of {'c': (x, y, z)}; adjacency: polygon index -> list of polygon indices; terrain: (x, y) -> height or None;
    exits: points. Returns (nodes, edges, exit_nodes, notes): nodes (x, y, z), edges (a, b, kind), the node of each exit (None
    when no polygon lies within EXIT_SNAP_YARDS of it), and the exits that could not be placed.
    """
    inside = [i for i, p in enumerate(polys) if covered(p, terrain)]
    cells = defaultdict(list)
    for i in inside:
        x, y, z = polys[i]['c']
        cells[(math.floor(x / CELL_YARDS), math.floor(y / CELL_YARDS), math.floor(z / CELL_HEIGHT_YARDS))].append(i)
    nodes, node_of = [], {}
    for ids in cells.values():
        for group in _components(ids, adjacency):
            mean = [sum(polys[i]['c'][k] for i in group) / len(group) for k in range(3)]
            pick = min(group, key=lambda i: math.dist(polys[i]['c'], mean))
            for i in group:
                node_of[i] = len(nodes)
            nodes.append(polys[pick]['c'])
    edges = set()
    for i in inside:
        for j in adjacency[i]:
            if j in node_of and node_of[j] != node_of[i]:
                edges.add((min(node_of[i], node_of[j]), max(node_of[i], node_of[j])))

    exit_nodes, notes = [], []
    for point in exits:
        nearest = min(range(len(polys)), key=lambda i: math.dist(polys[i]['c'], point), default=None)
        if nearest is None or math.dist(polys[nearest]['c'], point) > EXIT_SNAP_YARDS:
            exit_nodes.append(None)
            notes.append('exit %s is not within %.0f yards of the navmesh' % (point, EXIT_SNAP_YARDS))
            continue
        node = len(nodes)
        nodes.append(polys[nearest]['c'])
        exit_nodes.append(node)
        linked = False
        for other in adjacency[nearest]:
            if other in node_of:
                edges.add((node_of[other], node))
                linked = True
        if nearest in node_of:
            edges.add((node_of[nearest], node))
            linked = True
        if not linked:
            notes.append('exit %s touches no underground polygon' % (point,))
    return nodes, sorted((a, b, ROAD) for a, b in edges), exit_nodes, notes


def reachable_from_exits(node_count, edges, exit_nodes):
    """Nodes joined to some exit, and the size of every group of nodes that no exit reaches."""
    neighbours = defaultdict(list)
    for a, b, _ in edges:
        neighbours[a].append(b)
        neighbours[b].append(a)
    reached, queue = set(), deque(n for n in exit_nodes if n is not None)
    reached.update(queue)
    while queue:
        current = queue.popleft()
        for other in neighbours[current]:
            if other not in reached:
                reached.add(other)
                queue.append(other)
    stranded, seen = [], set(reached)
    for start in range(node_count):
        if start in seen:
            continue
        size, queue = 0, deque([start])
        seen.add(start)
        while queue:
            current = queue.popleft()
            size += 1
            for other in neighbours[current]:
                if other not in seen:
                    seen.add(other)
                    queue.append(other)
        stranded.append(size)
    return reached, sorted(stranded, reverse=True)


def regions(polys, adjacency, terrain, deep_yards=15.0, min_deep=50):
    """Groups of touching covered polygons that hold at least `min_deep` polygons `deep_yards` or more under the terrain."""
    ids = [i for i, p in enumerate(polys) if covered(p, terrain)]
    found = []
    for group in _components(ids, adjacency):
        deep = sum(1 for i in group if terrain(polys[i]['c'][0], polys[i]['c'][1]) - polys[i]['c'][2] >= deep_yards)
        if deep >= min_deep:
            found.append({'polys': group, 'deep': deep})
    return found


def exit_candidates(polys, adjacency, terrain, found, reach=COVERED_YARDS):
    """Surface polygons next to a deep underground region, clustered by 24 yards: places an exit could be listed."""
    inside = {i for region in found for i in region['polys']}
    clusters = defaultdict(list)
    for i, p in enumerate(polys):
        height = terrain(p['c'][0], p['c'][1])
        if height is None or abs(height - p['c'][2]) >= reach:
            continue
        if any(j in inside for j in adjacency[i]):
            clusters[(round(p['c'][0] / 24), round(p['c'][1] / 24))].append(p['c'])
    return [tuple(round(sum(c[k] for c in cs) / len(cs), 1) for k in range(3)) + (len(cs),) for cs in clusters.values()]
