#!/usr/bin/env python3
"""Exercise export index auditing, including identical and conflicting duplicates."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

from pck_audit import PackAuditError, pack_index


def fixture(entries, version=4):
    directory = bytearray(struct.pack('<I', len(entries)))
    payload = bytearray()
    for name, content in entries:
        encoded = name.encode('utf-8')
        encoded += bytes((-len(encoded)) % 4)
        directory.extend(struct.pack('<I', len(encoded)))
        directory.extend(encoded)
        directory.extend(struct.pack('<QQ16sI', len(payload), len(content), hashlib.md5(content).digest(), 0))
        payload.extend(content)
    if version == 2:
        base = 96 + len(directory)
        header = struct.pack('<6IQ', 0x43504447, version, 4, 7, 2, 2, base) + bytes(64)
        return header + directory + payload
    header = struct.pack('<6IQQ', 0x43504447, version, 4, 7, 2, 2, 112, 112 + len(payload)) + bytes(72)
    return header + payload + directory


class PackAuditTests(unittest.TestCase):
    def audit(self, data):
        with tempfile.TemporaryDirectory(prefix='godot-pck-audit-') as directory:
            path = Path(directory) / 'template.pck'
            path.write_bytes(data)
            return pack_index(path)

    def test_unique_export_versions(self):
        for version in (2, 3, 4):
            with self.subTest(version=version):
                entries = self.audit(fixture([('Main.tscn', b'scene'), ('.godot/mono/publish/arm64/Game.dll', b'IL')], version))
                self.assertEqual([entry.path for entry in entries], ['Main.tscn', '.godot/mono/publish/arm64/Game.dll'])
                self.assertEqual([entry.size for entry in entries], [5, 2])

    def test_reject_identical_publish_payload_twice(self):
        path = '.godot/mono/publish/arm64/System.Private.CoreLib.dll'
        with self.assertRaisesRegex(PackAuditError, r'Duplicate PCK resource paths \(1\)'):
            self.audit(fixture([(path, b'runtime'), (path, b'runtime')]))

    def test_reject_conflicting_manifest_twice(self):
        path = '.godot/mono/publish/arm64/.dotnet-publish-manifest'
        with self.assertRaisesRegex(PackAuditError, 'Duplicate PCK resource paths'):
            self.audit(fixture([(path, b'first enumeration'), (path, b'second enumeration')]))

    def test_reject_alias_duplicate(self):
        with self.assertRaisesRegex(PackAuditError, 'Duplicate PCK resource paths'):
            self.audit(fixture([('Main.tscn', b'scene'), ('res://./Main.tscn', b'scene')]))

    def test_reject_truncated_directory(self):
        with self.assertRaises(PackAuditError):
            self.audit(fixture([('Main.tscn', b'scene')])[:-3])

    def test_reject_payload_outside_pack(self):
        data = bytearray(fixture([('Main.tscn', b'scene')]))
        directory = struct.unpack_from('<Q', data, 32)[0]
        name_length = struct.unpack_from('<I', data, directory + 4)[0]
        struct.pack_into('<Q', data, directory + 8 + name_length, len(data) + 1)
        with self.assertRaisesRegex(PackAuditError, 'payload is outside'):
            self.audit(data)


if __name__ == '__main__':
    unittest.main()
