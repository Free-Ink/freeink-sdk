#!/usr/bin/env python3
"""Validate an unencrypted ESP image and map its segments into an ELF for Ghidra.

Example: esp_image_to_elf.py full.bin app.elf --offset 0x10000
This never runs or flashes the firmware. The ELF has load segments, not symbols.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def convert(source, output, offset):
    data = source.read_bytes()
    if offset < 0 or offset + 24 > len(data) or data[offset] != 0xE9:
        raise ValueError("No ESP image header at the specified offset")
    count = data[offset + 1]
    if not 1 <= count <= 16:
        raise ValueError("Invalid ESP segment count")
    chip = struct.unpack_from("<H", data, offset + 12)[0]
    if chip not in (0, 2, 9):
        raise ValueError(f"This tool currently supports Xtensa ESP32/S2/S3 only (chip={chip})")
    entry = struct.unpack_from("<I", data, offset + 4)[0]
    cursor = offset + 24
    checksum = 0xEF
    segments = []
    for index in range(count):
        if cursor + 8 > len(data):
            raise ValueError("Truncated segment header")
        address, size = struct.unpack_from("<II", data, cursor)
        cursor += 8
        if cursor + size > len(data) or address + size > 2**32:
            raise ValueError("Truncated or overflowing segment")
        payload = data[cursor:cursor + size]
        for byte in payload:
            checksum ^= byte
        segments.append(dict(index=index, address=address, file_offset=cursor, size=size))
        cursor += size
    image_end = offset + ((cursor - offset + 16) & ~15)
    if image_end > len(data) or data[image_end - 1] != checksum:
        raise ValueError("ESP segment checksum mismatch")
    image_hash = hashlib.sha256(data[offset:image_end]).digest()
    hash_appended = data[offset + 23] == 1
    if hash_appended and data[image_end:image_end + 32] != image_hash:
        raise ValueError("Appended ESP SHA-256 mismatch")

    # ELF32 little-endian, EM_XTENSA (94), one PT_LOAD per ESP segment.
    elf = bytearray(b"\x7fELF" + bytes([1, 1, 1]) + bytes(9))
    elf += struct.pack("<HHIIIIIHHHHHH", 2, 94, 1, entry, 52, 0, 0, 52, 32, count, 40, 0, 0)
    elf_offset = 52 + 32 * count
    for segment in segments:
        address, size = segment["address"], segment["size"]
        executable = 0x40000000 <= address < 0x44000000
        flags = 5 if executable else 4 if 0x3C000000 <= address < 0x3E000000 else 6
        elf += struct.pack("<IIIIIIII", 1, elf_offset, address, address, size, size, flags, 4)
        elf_offset += size
    for segment in segments:
        start, size = segment["file_offset"], segment["size"]
        elf += data[start:start + size]
    output.write_bytes(elf)
    manifest = dict(source=str(source), sha256=hashlib.sha256(data).hexdigest(),
                    image_offset=offset, chip_id=chip, entry=entry,
                    checksum_verified=True, appended_sha256_verified=hash_appended,
                    image_sha256=image_hash.hex(), segments=segments)
    output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--offset", type=lambda value: int(value, 0), default=0)
    args = parser.parse_args()
    try:
        convert(args.source, args.output, args.offset)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
