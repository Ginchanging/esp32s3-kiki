"""USB migration/update safety without opening a serial port."""
import binascii
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from flash_layout import COMMON, OLD_LAYOUT, NEW_LAYOUT, parse_table, plan_flash, check_writes


def table(layout):
    data = b"".join(struct.pack("<2sBBII16sI", b"\xaa\x50", *p[:4], name.encode(), p[4])
                    for name, p in sorted(layout.items(), key=lambda x: x[1][2]))
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(0xC00, b"\xff")


def ota(seq=1, state=2, index=0):
    data = bytearray(b"\xff" * 0x2000)
    word = struct.pack("<I", seq)
    data[index * 4096:index * 4096 + 32] = struct.pack("<I20sII", seq, b"\xff" * 20, state,
                                                                   binascii.crc32(word, 0xFFFFFFFF) & 0xFFFFFFFF)
    return bytes(data)


class FlashSafety(unittest.TestCase):
    def setUp(self):
        self.images = {0: b"B" * 21168, 0x8000: table(NEW_LAYOUT),
                       0x216000: b"\xff" * 0x2000, 0x220000: b"A" * 1024}

    def test_migration_commits_table_last_without_touching_credentials(self):
        writes, migration, slot = plan_flash(self.images, table(OLD_LAYOUT), 0x1000000)
        self.assertTrue(migration)
        self.assertEqual(slot, 0)
        self.assertEqual([x[0] for x in writes], [0x220000, 0x216000, 0, 0x8000])
        self.assertEqual({k: parse_table(writes[-1][1])[k] for k in COMMON}, COMMON)

    def test_usb_update_alternates_and_preserves_other_boot_record(self):
        for seq, expected in ((1, 1), (2, 0), (99, 1)):
            writes, migration, slot = plan_flash(self.images, table(NEW_LAYOUT), 0x1000000, ota(seq))
            self.assertFalse(migration)
            self.assertEqual(slot, expected)
            self.assertEqual(writes[0][0], 0x220000 + expected * 0x600000)
            self.assertEqual(writes[-1][0], 0x217000)
            self.assertEqual(len(writes[-1][1]), 4096)
            self.assertEqual(struct.unpack_from("<I", writes[-1][1], 24)[0], 0)
            self.assertNotIn(0x8000, [w[0] for w in writes])

    def test_unknown_layout_rejected(self):
        wrong = {**OLD_LAYOUT, "extra": (1, 6, 0x300000, 4096, 0)}
        with self.assertRaisesRegex(ValueError, "未知"):
            plan_flash(self.images, table(wrong), 0x1000000)

    def test_small_chip_and_oversized_image_rejected(self):
        with self.assertRaises(ValueError):
            plan_flash(self.images, table(OLD_LAYOUT), 0x400000)
        self.images[0x220000] = b"A" * (0x600000 + 1)
        with self.assertRaises(ValueError):
            plan_flash(self.images, table(OLD_LAYOUT), 0x1000000)

    def test_pending_verification_not_overwritten(self):
        for state in (0, 1):
            for boot in (ota(1, state), ota(2, state, 1)):
                with self.assertRaisesRegex(ValueError, "启动确认"):
                    plan_flash(self.images, table(NEW_LAYOUT), 0x1000000, boot)

    def test_invalid_boot_data_requires_explicit_recovery(self):
        with self.assertRaises(ValueError):
            plan_flash(self.images, table(NEW_LAYOUT), 0x1000000, b"\xff" * 8192)
        writes, migration, slot = plan_flash(self.images, table(NEW_LAYOUT), 0x1000000,
                                            b"\xff" * 8192, recover=True)
        self.assertEqual(slot, 0)
        self.assertFalse(migration)
        self.assertEqual(writes[1][0], 0x216000)

    def test_no_write_can_erase_any_protected_sector(self):
        for offset, data in ((0x9000, b"x"), (0xF000, b"x"), (0x200000, b"x" * 65537),
                             (0x215000, b"x"), (0xFFF000, b"x" * 4097)):
            with self.assertRaises(ValueError):
                check_writes([(offset, data)], 0x1000000)

    def test_corrupt_table_and_unexpected_images_rejected(self):
        bad = bytearray(table(OLD_LAYOUT))
        bad[10] ^= 1
        with self.assertRaises(ValueError):
            parse_table(bad)
        self.images[0x210000] = b"NEVER"
        with self.assertRaises(ValueError):
            plan_flash(self.images, table(OLD_LAYOUT), 0x1000000)


if __name__ == "__main__":
    unittest.main()
