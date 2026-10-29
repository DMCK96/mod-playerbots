"""Client MPQ archives in the order the core map_extractor loads them (src/tools/map_extractor/System.cpp)."""
import os
import struct
import sys

from . import mpq

BASE = ('common.MPQ', 'common-2.MPQ', 'expansion.MPQ', 'lichking.MPQ')
LOCALE_BASE = ('locale-', 'expansion-locale-', 'lichking-locale-')


def _join(*parts):
    return '/'.join(parts)


def _lettered_patches(data_dir, listdir):
    found = []
    for name in listdir(data_dir):
        upper = name.upper()
        if len(upper) < 11 or not upper.startswith('PATCH-') or not upper.endswith('.MPQ'):
            continue
        suffix = upper[6:-4]
        if suffix.isdigit():
            continue
        found.append((suffix, name))
    return [_join(data_dir, name) for _, name in sorted(found)]


def client_archive_chain(client_dir, locale, listdir=os.listdir, exists=os.path.exists):
    """Existing archives, lowest priority first; the last one wins a name collision."""
    data_dir = _join(client_dir.rstrip('/\\'), 'Data')
    locale_dir = _join(data_dir, locale)
    chain = [_join(data_dir, name) for name in BASE]
    chain += [_join(locale_dir, prefix + locale + '.MPQ') for prefix in LOCALE_BASE]
    for i in range(1, 100):
        number = '-%d' % i if i > 1 else ''
        chain.append(_join(data_dir, 'patch%s.MPQ' % number))
        chain.append(_join(locale_dir, 'patch-%s%s.MPQ' % (locale, number)))
    chain += _lettered_patches(data_dir, listdir)
    return [path for path in chain if exists(path)]


def detect_locale(client_dir, listdir=os.listdir):
    data_dir = _join(client_dir.rstrip('/\\'), 'Data')
    for name in sorted(listdir(data_dir)):
        try:
            if 'locale-%s.MPQ' % name in listdir(_join(data_dir, name)):
                return name
        except (KeyError, OSError):
            continue
    raise FileNotFoundError('no locale folder with locale-<name>.MPQ under ' + data_dir)


class ArchiveSet:
    def __init__(self, archives):
        self.archives = list(archives)

    def read(self, name):
        for archive in reversed(self.archives):
            try:
                data = archive.read(name)
            except (KeyError, ValueError, NotImplementedError, OSError):
                continue
            if data:
                return data
        return None


def open_client(client_dir):
    archives = []
    for path in client_archive_chain(client_dir, detect_locale(client_dir)):
        try:
            archives.append(mpq.Mpq(path))
        except (OSError, ValueError, struct.error) as error:
            print('warning: cannot open %s: %s' % (path, error), file=sys.stderr)
    return ArchiveSet(archives)
