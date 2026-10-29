import struct
import unittest

from roadlib.dbc import MapRow, parse_map_dbc


def build_map_dbc(rows, fields=66):
    strings = b'\0'
    records = b''
    for map_id, directory, instance_type in rows:
        offset = len(strings)
        strings += directory.encode() + b'\0'
        records += struct.pack('<%dI' % fields, map_id, offset, instance_type, *([0] * (fields - 3)))
    return struct.pack('<4s4I', b'WDBC', len(rows), fields, fields * 4, len(strings)) + records + strings


class MapDbcTest(unittest.TestCase):
    def test_reads_id_directory_and_instance_type(self):
        data = build_map_dbc([(0, 'Azeroth', 0), (36, 'DeadminesInstance', 1), (30, 'PVPZone01', 3)])
        self.assertEqual(parse_map_dbc(data), [MapRow(0, 'Azeroth', 0), MapRow(36, 'DeadminesInstance', 1),
                                               MapRow(30, 'PVPZone01', 3)])

    def test_rejects_other_files(self):
        with self.assertRaises(ValueError):
            parse_map_dbc(b'WDB2' + bytes(16))


if __name__ == '__main__':
    unittest.main()
