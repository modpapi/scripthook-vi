#!/usr/bin/env python3
"""Convert a fake-signed, unencrypted, uncompressed PS5 SELF into a plain ELF.

Usage: python3 tools/self2elf.py <in.self> <out.elf>
"""
import struct
import sys

SELF_MAGIC = 0x1D3D154F
ENTRY_BASE = 0x20
ENTRY_SIZE = 32
PROP_ENCRYPTED = 0x2
PROP_COMPRESSED = 0x8
PROP_BLOCK = 0x800


class SelfError(Exception):
    pass


def convert(self_bytes: bytes) -> bytes:
    data = bytes(self_bytes)
    if len(data) < ENTRY_BASE:
        raise SelfError("SELF truncated: header incomplete")
    if struct.unpack_from("<I", data, 0)[0] != SELF_MAGIC:
        raise SelfError("not a SELF: bad magic")
    n_entries = struct.unpack_from("<H", data, 0x18)[0]
    elf_off = ENTRY_BASE + n_entries * ENTRY_SIZE
    if len(data) < elf_off + 64:
        raise SelfError("SELF truncated: ELF header incomplete")
    if data[elf_off:elf_off + 4] != b"\x7fELF":
        raise SelfError("no embedded ELF header at 0x%x" % elf_off)
    phoff, = struct.unpack_from("<Q", data, elf_off + 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, elf_off + 0x36)
    ph_end = elf_off + phoff + phnum * phentsize
    if len(data) < ph_end:
        raise SelfError("SELF truncated: program headers incomplete")

    phdrs = []
    for i in range(phnum):
        p_type, _flags, p_offset, _va, _pa, p_filesz, _memsz, _al = struct.unpack_from(
            "<IIQQQQQQ", data, elf_off + phoff + i * phentsize)
        phdrs.append((p_offset, p_filesz))
    out_size = max([phoff + phnum * phentsize] + [o + s for o, s in phdrs])
    out = bytearray(out_size)
    out[0:phoff + phnum * phentsize] = data[elf_off:ph_end]

    for i in range(n_entries):
        props, offset, filesz, _memsz = struct.unpack_from(
            "<QQQQ", data, ENTRY_BASE + i * ENTRY_SIZE)
        if not props & PROP_BLOCK:
            continue  # signature / digest block
        if props & PROP_ENCRYPTED:
            raise SelfError("entry %d is encrypted; decrypt the SELF first" % i)
        if props & PROP_COMPRESSED:
            raise SelfError("entry %d is compressed; decompress the SELF first" % i)
        if offset + filesz > len(data):
            raise SelfError("SELF truncated: entry %d ends at 0x%x, file is 0x%x bytes"
                            % (i, offset + filesz, len(data)))
        seg = (props >> 20) & 0xFFF
        if seg >= phnum:
            raise SelfError("entry %d refers to missing segment %d" % (i, seg))
        dst, seg_filesz = phdrs[seg]
        if filesz > seg_filesz:
            raise SelfError("entry %d (0x%x bytes) exceeds segment %d file size 0x%x"
                            % (i, filesz, seg, seg_filesz))
        out[dst:dst + filesz] = data[offset:offset + filesz]
    return bytes(out)


def main(argv):
    if len(argv) != 3:
        print("usage: self2elf.py <in.self> <out.elf>", file=sys.stderr)
        return 2
    try:
        with open(argv[1], "rb") as f:
            result = convert(f.read())
    except (SelfError, OSError) as e:
        print("self2elf: %s" % e, file=sys.stderr)
        return 1
    with open(argv[2], "wb") as f:
        f.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
