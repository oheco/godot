#!/usr/bin/env python3
"""Inspect an unencrypted standalone export PCK and reject duplicate paths."""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import posixpath
import struct


@dataclass(frozen=True)
class PackEntry:
    path: str
    offset: int
    size: int
    md5: str


class PackAuditError(ValueError):
    pass


def pack_index(path):
    """Return every index entry; duplicate resource paths are an export failure."""
    path = Path(path)
    file_size = path.stat().st_size
    with path.open('rb') as stream:
        def read(size):
            data = stream.read(size)
            if len(data) != size:
                raise PackAuditError('Truncated PCK header or directory')
            return data

        magic, version, _major, _minor, _patch, flags, file_base = struct.unpack('<6IQ', read(32))
        if magic != 0x43504447 or version not in (2, 3, 4):
            raise PackAuditError('Expected a standalone Godot PCK version 2, 3 or 4')
        if flags & ~2:
            raise PackAuditError('Encrypted, sparse or unknown PCK flags cannot be audited')
        if version >= 3:
            directory_offset = struct.unpack('<Q', read(8))[0]
            if directory_offset < 40 or directory_offset > file_size - 4:
                raise PackAuditError('PCK directory offset is outside the file')
            stream.seek(directory_offset)
        else:
            read(64)
        if file_base > file_size:
            raise PackAuditError('PCK file base is outside the file')
        count = struct.unpack('<I', read(4))[0]
        if count > (file_size - stream.tell()) // 40:
            raise PackAuditError('PCK entry count exceeds the available directory')
        entries = []
        seen = set()
        duplicates = set()
        for _ in range(count):
            name_size = struct.unpack('<I', read(4))[0]
            if name_size == 0 or name_size > file_size - stream.tell() - 36:
                raise PackAuditError('Invalid PCK resource path length')
            try:
                name = read(name_size).rstrip(b'\0').decode('utf-8')
            except UnicodeDecodeError as error:
                raise PackAuditError('PCK resource path is not valid UTF-8') from error
            name = name.removeprefix('res://')
            canonical = posixpath.normpath(name)
            if not name or '\0' in name or '\\' in name or ':' in name or canonical.startswith('/') or canonical == '..' or canonical.startswith('../') or canonical == '.':
                raise PackAuditError('Unsafe PCK resource path')
            offset, size, digest, entry_flags = struct.unpack('<QQ16sI', read(36))
            if entry_flags:
                raise PackAuditError('Encrypted, removed or delta PCK entries cannot be audited')
            absolute_offset = file_base + offset
            if absolute_offset > file_size or size > file_size - absolute_offset:
                raise PackAuditError(f'PCK payload is outside the file: {canonical}')
            if canonical in seen:
                duplicates.add(canonical)
            seen.add(canonical)
            entries.append(PackEntry(canonical, absolute_offset, size, digest.hex()))
        if duplicates:
            sample = ', '.join(sorted(duplicates)[:3])
            raise PackAuditError(f'Duplicate PCK resource paths ({len(duplicates)}): {sample}')
        return tuple(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pck', type=Path)
    args = parser.parse_args()
    try:
        entries = pack_index(args.pck)
    except (OSError, PackAuditError) as error:
        parser.exit(1, f'PCK audit failed: {error}\n')
    managed = [entry for entry in entries if entry.path.startswith('.godot/mono/publish/')]
    print(json.dumps({'entries': len(entries), 'size': args.pck.stat().st_size,
                      'managed_entries': len(managed), 'managed_payload_bytes': sum(entry.size for entry in managed)}, indent=2))


if __name__ == '__main__':
    main()
