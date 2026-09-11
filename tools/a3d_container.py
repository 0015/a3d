# SPDX-FileCopyrightText: 2026 Eric Nam
# SPDX-License-Identifier: Apache-2.0

"""
a3d asset container writer.

Emits exactly the layout defined in `a3d/src/a3d/a3d_format.h`. That header
is the normative definition; this module must follow it, never the reverse.

The one rule that shapes everything: **no pointers, only uint32 byte offsets
from the start of the file image**. That is what lets the container be used
zero-copy straight from flash. Offsets are therefore resolved in a second pass,
once each chunk's position in the file is known.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

# --- constants mirrored from a3d_format.h -----------------------------------

def fourcc(s: str) -> int:
    b = s.encode("ascii")
    assert len(b) == 4, f"fourcc must be 4 chars: {s!r}"
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)

MAGIC = fourcc("A3DA")
BYTE_ORDER_OK = 0x04030201
VERSION_MAJOR = 0
VERSION_MINOR = 1

NONE32 = 0xFFFFFFFF
NONE16 = 0xFFFF
NONE8 = 0xFF

CHUNK_STRT = fourcc("STRT")
CHUNK_NODE = fourcc("NODE")
CHUNK_SKEL = fourcc("SKEL")
CHUNK_MESH = fourcc("MESH")
CHUNK_MSHC = fourcc("MSHC")
CHUNK_SKIN = fourcc("SKIN")
CHUNK_MATL = fourcc("MATL")
CHUNK_TEXR = fourcc("TEXR")
CHUNK_ANIM = fourcc("ANIM")
CHUNK_MRPH = fourcc("MRPH")

# A container may carry a mesh pre-built for one renderer; the id says which.
BACKEND_COOKED_TEST = fourcc("CKD0")

MAX_BONES = 255
MAX_BONES_PER_VERTEX = 2

# --- struct formats. Sizes are asserted below against the header's values. ---

F_FILE_HEADER = "<IHHIIIIII"       # 32
F_CHUNK_ENTRY = "<IIII"            # 16
F_NODE = "<IHHHH3f4f3f"            # 52
F_BONE = "<IHBB16f"                # 72
F_MESH = "<IIIIIIIHH3f3f"          # 56
F_COOKED = "<IIII"                 # 16
F_SKINVTX = "<BBBB"                # 4
F_SKIN = "<IIII"                   # 16
F_MATERIAL = "<I3ffffHH"           # 32
F_TEXTURE = "<IHHHHII"             # 20
F_CHANNEL = "<HBBHHII3f3fII"       # 48
F_CLIP = "<IIIIHHI"                # 24

_EXPECTED_SIZES = {
    F_FILE_HEADER: 32, F_CHUNK_ENTRY: 16, F_NODE: 52, F_BONE: 72,
    F_MESH: 56, F_COOKED: 16, F_SKINVTX: 4, F_SKIN: 16,
    F_MATERIAL: 32, F_TEXTURE: 20, F_CHANNEL: 48, F_CLIP: 24,
}
for _fmt, _sz in _EXPECTED_SIZES.items():
    assert struct.calcsize(_fmt) == _sz, (
        f"struct format {_fmt} is {struct.calcsize(_fmt)} bytes, header says {_sz}")


def _align4(n: int) -> int:
    return (n + 3) & ~3


class ChunkBuilder:
    """
    Accumulates one chunk's payload, recording every place that needs a
    file-absolute offset patched in later.
    """

    def __init__(self, chunk_type: int):
        self.type = chunk_type
        self.buf = bytearray()
        # (write_position_in_buf, target_position_in_buf)
        self._relocs: list[tuple[int, int]] = []

    def __len__(self) -> int:
        return len(self.buf)

    def raw(self, data: bytes) -> int:
        at = len(self.buf)
        self.buf += data
        return at

    def pack(self, fmt: str, *values) -> int:
        return self.raw(struct.pack(fmt, *values))

    def reserve_ref(self) -> int:
        """Write a placeholder uint32 offset. Returns its position."""
        at = len(self.buf)
        self.buf += b"\0\0\0\0"
        return at

    def set_ref(self, ref_pos: int, target_in_chunk: int) -> None:
        """Point a placeholder at a position inside this chunk."""
        self._relocs.append((ref_pos, target_in_chunk))

    def set_none(self, ref_pos: int) -> None:
        struct.pack_into("<I", self.buf, ref_pos, NONE32)

    def blob(self, data: bytes, align: int = 4) -> int:
        """Append aligned data, returning its offset within this chunk."""
        pad = (-len(self.buf)) % align
        self.buf += b"\0" * pad
        at = len(self.buf)
        self.buf += data
        return at

    def pad_to_align(self, align: int = 4) -> None:
        self.buf += b"\0" * ((-len(self.buf)) % align)

    def resolve(self, chunk_base: int) -> bytes:
        out = bytearray(self.buf)
        for ref_pos, target in self._relocs:
            struct.pack_into("<I", out, ref_pos, chunk_base + target)
        return bytes(out)


class StringTable:
    """Deduplicating UTF-8 string table. Offset NONE32 means 'no name'."""

    def __init__(self):
        self.buf = bytearray(b"\0")     # offset 0 is the empty string
        self._seen: dict[str, int] = {"": 0}

    def add(self, s: str | None) -> int:
        if s is None:
            return NONE32
        if s in self._seen:
            return self._seen[s]
        at = len(self.buf)
        self.buf += s.encode("utf-8") + b"\0"
        self._seen[s] = at
        return at


class ContainerWriter:
    """Lays chunks out in the file and resolves every offset."""

    def __init__(self):
        self.chunks: list[ChunkBuilder] = []

    def add(self, chunk: ChunkBuilder) -> None:
        chunk.pad_to_align(4)
        self.chunks.append(chunk)

    def build(self) -> bytes:
        header_size = struct.calcsize(F_FILE_HEADER)
        table_off = header_size
        table_size = struct.calcsize(F_CHUNK_ENTRY) * len(self.chunks)

        # First pass: assign each chunk a 4-aligned position.
        cursor = _align4(table_off + table_size)
        placements: list[tuple[ChunkBuilder, int]] = []
        for c in self.chunks:
            cursor = _align4(cursor)
            placements.append((c, cursor))
            cursor += len(c)
        file_size = _align4(cursor)

        out = bytearray(file_size)
        struct.pack_into(
            F_FILE_HEADER, out, 0,
            MAGIC, VERSION_MAJOR, VERSION_MINOR, BYTE_ORDER_OK,
            file_size, len(self.chunks), table_off, 0, 0)

        for i, (c, base) in enumerate(placements):
            struct.pack_into(F_CHUNK_ENTRY, out, table_off + i * 16,
                             c.type, base, len(c), 0)
            # Second pass: now that `base` is known, every internal reference
            # can be turned into a file-absolute offset.
            body = c.resolve(base)
            out[base:base + len(body)] = body

        return bytes(out)


def read_header(data: bytes) -> dict:
    """Minimal reader used by the self-check in the exporter."""
    (magic, vmaj, vmin, order, size, nchunks, table_off, flags, _res) = \
        struct.unpack_from(F_FILE_HEADER, data, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08X}")
    if order != BYTE_ORDER_OK:
        raise ValueError(f"bad byte-order sentinel 0x{order:08X}")
    if size != len(data):
        raise ValueError(f"header file_size {size} != actual {len(data)}")
    chunks = []
    for i in range(nchunks):
        t, off, sz, fl = struct.unpack_from(F_CHUNK_ENTRY, data, table_off + i * 16)
        if off % 4 != 0:
            raise ValueError(f"chunk {i} offset {off} is not 4-aligned")
        if off + sz > len(data):
            raise ValueError(f"chunk {i} runs past end of file")
        chunks.append({"type": t, "offset": off, "size": sz, "flags": fl})
    return {"version": (vmaj, vmin), "file_size": size, "chunks": chunks}


def fourcc_str(v: int) -> str:
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF]).decode("ascii")
