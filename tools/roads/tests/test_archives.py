import unittest

from roadlib.archives import ArchiveSet, client_archive_chain, detect_locale

CLIENT = 'client'
FILES = {
    'client/Data': ['common.MPQ', 'common-2.MPQ', 'expansion.MPQ', 'lichking.MPQ', 'patch.MPQ', 'patch-2.MPQ',
                    'patch-3.MPQ', 'patch-CA.MPQ', 'patch-A.MPQ', 'patch-B.MPQ', 'enUS', 'Content', 'notes.txt'],
    'client/Data/enUS': ['locale-enUS.MPQ', 'expansion-locale-enUS.MPQ', 'lichking-locale-enUS.MPQ',
                         'patch-enUS.MPQ', 'patch-enUS-2.MPQ'],
    'client/Data/Content': [],
}


def listdir(path):
    return FILES[path.replace('\\', '/')]


def exists(path):
    path = path.replace('\\', '/')
    folder, _, name = path.rpartition('/')
    return name in FILES.get(folder, [])


class ArchiveChainTest(unittest.TestCase):
    def test_order_matches_map_extractor(self):
        chain = [p.replace('\\', '/') for p in client_archive_chain(CLIENT, 'enUS', listdir, exists)]
        expected = ['common.MPQ', 'common-2.MPQ', 'expansion.MPQ', 'lichking.MPQ',
                    'enUS/locale-enUS.MPQ', 'enUS/expansion-locale-enUS.MPQ', 'enUS/lichking-locale-enUS.MPQ',
                    'patch.MPQ', 'enUS/patch-enUS.MPQ', 'patch-2.MPQ', 'enUS/patch-enUS-2.MPQ', 'patch-3.MPQ',
                    'patch-A.MPQ', 'patch-B.MPQ', 'patch-CA.MPQ']
        self.assertEqual(chain, ['client/Data/' + name for name in expected])

    def test_locale_is_the_folder_holding_its_locale_archive(self):
        self.assertEqual(detect_locale(CLIENT, listdir), 'enUS')


class FakeArchive:
    def __init__(self, files):
        self.files = files

    def read(self, name):
        if name not in self.files:
            raise KeyError(name)
        return self.files[name]


class ArchiveSetTest(unittest.TestCase):
    def test_last_archive_wins(self):
        archives = ArchiveSet([FakeArchive({'a': b'old', 'b': b'base'}), FakeArchive({'a': b'new'})])
        self.assertEqual(archives.read('a'), b'new')
        self.assertEqual(archives.read('b'), b'base')
        self.assertIsNone(archives.read('missing'))


if __name__ == '__main__':
    unittest.main()
