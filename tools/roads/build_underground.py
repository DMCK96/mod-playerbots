"""Underground waypoint graph for one map, from its navmesh: where a bot stuck below the terrain walks to leave.

Usage: python -I tools/roads/build_underground.py <DataDir> <map id> <gx0,gx1> <gy0,gy1> <exits file> <out.under>
                                                  [--candidates]
<gx0,gx1> and <gy0,gy1> are the mmtile file numbers (`<map><gx><gy>.mmtile`) of the rectangle to cover; take every tile
the underground area touches. <exits file> lists the exit points, one `x y z` a line (`#` comments); the graph is written to
<out.under> in the road file format. With --candidates the places where an exit could be listed are printed instead.
The server reads <DataDir>/roads/<map id>.under and the same exits file as <map id>.exits.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from roadlib import navmesh, roadfile, underground


def main(argv):
    if len(argv) < 7:
        sys.exit(__doc__)
    data_dir, map_id = argv[1], int(argv[2])
    gx = tuple(int(v) for v in argv[3].split(','))
    gy = tuple(int(v) for v in argv[4].split(','))
    exits_path, out = argv[5], argv[6]
    polys, adjacency = navmesh.load(data_dir, map_id, gx, gy)
    terrain = navmesh.Terrain(data_dir, map_id)
    print('%d polygons in %d x %d tiles' % (len(polys), gx[1] - gx[0] + 1, gy[1] - gy[0] + 1))
    if '--candidates' in argv:
        found = underground.regions(polys, adjacency, terrain)
        print('%d deep underground regions' % len(found))
        for region in found:
            xs = [polys[i]['c'][0] for i in region['polys']]
            ys = [polys[i]['c'][1] for i in region['polys']]
            zs = [polys[i]['c'][2] for i in region['polys']]
            print('  region of %d polygons (%d deep): x %.0f..%.0f y %.0f..%.0f floor z %.0f..%.0f' % (
                len(region['polys']), region['deep'], min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
        print('surface polygons beside them, clustered (x y z, polygons):')
        for x, y, z, count in sorted(underground.exit_candidates(polys, adjacency, terrain, found), key=lambda c: -c[3]):
            print('%.1f %.1f %.1f  # %d' % (x, y, z, count))
        return
    exits = underground.parse_exits(open(exits_path, encoding='utf-8').read())
    nodes, edges, exit_nodes, notes = underground.build(polys, adjacency, terrain, exits)
    reached, stranded = underground.reachable_from_exits(len(nodes), edges, exit_nodes)
    with open(out, 'wb') as f:
        f.write(roadfile.encode(nodes, edges))
    print('%d nodes, %d edges, %d exits placed of %d' % (len(nodes), len(edges), sum(n is not None for n in exit_nodes),
                                                            len(exits)))
    print('%d nodes reach an exit; groups with no exit (sizes): %s' % (len(reached), stranded[:10]))
    for note in notes:
        print('note:', note)


if __name__ == '__main__':
    main(sys.argv)
