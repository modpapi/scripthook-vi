import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import self2elf  # noqa: E402

SEG_OFFSET = 0x1000
SEG_DATA = bytes(range(0x10, 0x30))
DATA_OFFSET = 0x200


def build_self(block_props=0x800 | 0x4, entry_offset=DATA_OFFSET, pad=True):
    """Synthetic SELF: 2 entries (signature block + segment block), one PT_LOAD."""
    n_entries = 2
    elf_off = 0x20 + n_entries * 32
    ehdr = bytearray(64)
    ehdr[0:16] = b"\x7fELF\x02\x01\x01\x09" + bytes(8)
    struct.pack_into("<HHIQQQIHHHHHH", ehdr, 16, 2, 0x3E, 1, 0x401000, 64, 0,
                     0, 64, 56, 1, 64, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, SEG_OFFSET, 0x401000, 0x401000,
                       len(SEG_DATA), len(SEG_DATA), 0x4000)
    hdr = bytearray(0x20)
    struct.pack_into("<I", hdr, 0, 0x1D3D154F)
    struct.pack_into("<H", hdr, 0x18, n_entries)
    entries = struct.pack("<QQQQ", 0x1, 0x400, 0x20, 0x20)  # non-block (signature)
    entries += struct.pack("<QQQQ", block_props, entry_offset, len(SEG_DATA), len(SEG_DATA))
    blob = bytearray(hdr) + entries + ehdr + phdr
    if pad:
        blob += bytes(DATA_OFFSET - len(blob))
        blob += SEG_DATA
    return bytes(blob), bytes(ehdr) + phdr


class Self2ElfTests(unittest.TestCase):
    def test_plain_segment_copied(self):
        blob, elf_start = build_self()
        out = self2elf.convert(blob)
        self.assertEqual(out[:len(elf_start)], elf_start)
        self.assertEqual(out[SEG_OFFSET:SEG_OFFSET + len(SEG_DATA)], SEG_DATA)
        self.assertEqual(len(out), SEG_OFFSET + len(SEG_DATA))

    def test_encrypted_entry_rejected(self):
        blob, _ = build_self(block_props=0x800 | 0x2)
        with self.assertRaisesRegex(self2elf.SelfError, "encrypted"):
            self2elf.convert(blob)

    def test_compressed_entry_rejected(self):
        blob, _ = build_self(block_props=0x800 | 0x8)
        with self.assertRaisesRegex(self2elf.SelfError, "compressed"):
            self2elf.convert(blob)

    def test_truncated_self_rejected(self):
        blob, _ = build_self(entry_offset=len(build_self()[0]) - 4)
        with self.assertRaisesRegex(self2elf.SelfError, "truncated"):
            self2elf.convert(blob)


if __name__ == "__main__":
    unittest.main()
