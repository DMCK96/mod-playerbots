"""Road graph file, version 1, little-endian.

Header: b'CRDS', uint32 version, uint32 node count, uint32 edge count.
Node:   float x, y, z.
Edge:   uint32 a, uint32 b, uint8 kind (1 road, 2 trail), 3 zero bytes.
"""
import os
import struct

MAGIC = b'CRDS'
VERSION = 1
HEADER = struct.Struct('<4s3I')
NODE = struct.Struct('<3f')
EDGE = struct.Struct('<IIB3x')


def encode(nodes, edges):
    out = bytearray(HEADER.pack(MAGIC, VERSION, len(nodes), len(edges)))
    for x, y, z in nodes:
        out += NODE.pack(x, y, z)
    for a, b, kind in edges:
        out += EDGE.pack(a, b, kind)
    return bytes(out)


def decode(data):
    if len(data) < HEADER.size:
        raise ValueError('truncated road file')
    magic, version, node_count, edge_count = HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError('not a road file')
    if version != VERSION:
        raise ValueError('unsupported road file version %d' % version)
    if len(data) != HEADER.size + NODE.size * node_count + EDGE.size * edge_count:
        raise ValueError('road file size does not match its counts')
    nodes = [NODE.unpack_from(data, HEADER.size + NODE.size * i) for i in range(node_count)]
    base = HEADER.size + NODE.size * node_count
    edges = [EDGE.unpack_from(data, base + EDGE.size * i) for i in range(edge_count)]
    for a, b, _ in edges:
        if a >= node_count or b >= node_count:
            raise ValueError('edge refers to a missing node')
    return nodes, edges


def write_roads(path, nodes, edges):
    folder = os.path.dirname(path)
    if folder:
        os.makedirs(folder, exist_ok=True)
    with open(path, 'wb') as handle:
        handle.write(encode(nodes, edges))


def read_roads(path):
    with open(path, 'rb') as handle:
        return decode(handle.read())
