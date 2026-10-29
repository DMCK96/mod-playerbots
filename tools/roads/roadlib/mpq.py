"""Minimal MPQ (v1/v2) reader: zlib and bzip2 sectors, single-unit files and encrypted tables.

Adapted from coa-research scripts/client/mpq.py. PKWARE implode, Huffman and ADPCM sectors are reported as errors.
"""
from array import array
import bz2
import re
import struct
import sys
import zlib

CRYPT = {}


def _init():
    seed = 0x00100001
    for i in range(256):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            t1 = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            t2 = seed & 0xFFFF
            CRYPT[idx] = (t1 | t2) & 0xFFFFFFFF
            idx += 256


_init()


def hash_string(name, kind):
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in name.upper().replace('/', '\\'):
        c = ord(ch)
        seed1 = CRYPT[(kind << 8) + c] ^ ((seed1 + seed2) & 0xFFFFFFFF)
        seed2 = (c + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def decrypt(data, key):
    out = []
    seed = 0xEEEEEEEE
    for (v,) in struct.iter_unpack('<I', data):
        seed = (seed + CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        v ^= (key + seed) & 0xFFFFFFFF
        key = ((((~key) << 21) + 0x11111111) | (key >> 11)) & 0xFFFFFFFF
        seed = (v + seed + (seed << 5) + 3) & 0xFFFFFFFF
        out.append(v)
    return out


class Mpq:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.base = 0
        head = self.f.read(4)
        while head != b'MPQ\x1a':
            self.base += 512
            self.f.seek(self.base)
            head = self.f.read(4)
            if not head:
                raise ValueError('no MPQ header')
        self.f.seek(self.base)
        raw = self.f.read(32)
        _, hsize, _, ver, shift, hpos, bpos, hcount, bcount = struct.unpack('<4sIIHHIIII', raw)
        self.sector = 512 << shift
        self.f.seek(self.base + hpos)
        hv = decrypt(self.f.read(hcount * 16), hash_string('(hash table)', 3))
        self.hashes = array('I', hv)
        self.f.seek(self.base + bpos)
        bv = decrypt(self.f.read(bcount * 16), hash_string('(block table)', 3))
        self.blocks = array('I', bv)
        self.hcount = hcount

    def find(self, name):
        n1, n2 = hash_string(name, 1), hash_string(name, 2)
        i = hash_string(name, 0) & (self.hcount - 1)
        for _ in range(self.hcount):
            h0, h1, _, block = self.hashes[4 * i:4 * i + 4]
            if block == 0xFFFFFFFF:
                return None
            if h0 == n1 and h1 == n2 and block != 0xFFFFFFFE:
                return tuple(self.blocks[4 * block:4 * block + 4]) if 4 * block < len(self.blocks) else None
            i = (i + 1) & (self.hcount - 1)
        return None

    def read(self, name):
        blk = self.find(name)
        if not blk:
            return None
        pos, csize, fsize, flags = blk
        if not flags & 0x80000000 or flags & 0x02000000:
            return None
        key = 0
        if flags & 0x10000:
            key = hash_string(name.replace('/', chr(92)).split(chr(92))[-1], 3)
            if flags & 0x20000:
                key = ((key + pos) ^ fsize) & 0xFFFFFFFF
        self.f.seek(self.base + pos)
        data = self.f.read(csize)
        if not flags & 0x200 and not flags & 0x100:
            if key:
                data = self._decrypt_bytes(data, key)
            return data[:fsize]
        if flags & 0x01000000:
            if key:
                data = self._decrypt_bytes(data, key)
            return self._unpack(data, fsize)
        count = (fsize + self.sector - 1) // self.sector
        extra = 1 if flags & 0x04000000 else 0
        table = 4 * (count + 1 + extra)
        head = data[:table]
        if key:
            head = self._decrypt_bytes(head, (key - 1) & 0xFFFFFFFF)
        offs = struct.unpack('<%dI' % (count + 1 + extra), head)
        out = bytearray()
        for i in range(count):
            chunk = data[offs[i]:offs[i + 1]]
            if key:
                chunk = self._decrypt_bytes(chunk, (key + i) & 0xFFFFFFFF)
            want = min(self.sector, fsize - i * self.sector)
            out += chunk if len(chunk) == want else self._unpack(chunk, want)
        return bytes(out)

    @staticmethod
    def _decrypt_bytes(data, key):
        whole = len(data) // 4 * 4
        words = decrypt(data[:whole], key)
        return struct.pack('<%dI' % len(words), *words) + data[whole:]

    def _unpack(self, chunk, want):
        if len(chunk) == want:
            return chunk
        mask, body = chunk[0], chunk[1:]
        if mask & 0x20:
            raise ValueError('sparse compression')
        if mask & 0x02:
            body = zlib.decompress(body)
            mask &= ~0x02
        elif mask & 0x10:
            body = bz2.decompress(body)
            mask &= ~0x10
        if mask:
            raise ValueError('unsupported compression mask 0x%02x' % mask)
        return body

    def names(self):
        data = self.read('(listfile)')
        if data is None:
            return []
        return [n for n in re.split(r'[\r\n;]+', data.decode('latin1')) if n]
