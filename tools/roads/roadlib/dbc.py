"""Map.dbc rows: id, directory and instance type."""
import struct
from typing import NamedTuple


class MapRow(NamedTuple):
    id: int
    directory: str
    instance_type: int


def parse_map_dbc(data):
    if len(data) < 20 or data[:4] != b'WDBC':
        raise ValueError('not a WDBC file')
    _, rows, fields, record_size, string_size = struct.unpack_from('<4s4I', data)
    if fields < 3 or len(data) < 20 + rows * record_size + string_size:
        raise ValueError('truncated WDBC file')
    strings = data[20 + rows * record_size:20 + rows * record_size + string_size]
    result = []
    for row in range(rows):
        map_id, offset, instance_type = struct.unpack_from('<3I', data, 20 + row * record_size)
        end = strings.index(b'\0', offset)
        result.append(MapRow(map_id, strings[offset:end].decode('latin1'), instance_type))
    return result
