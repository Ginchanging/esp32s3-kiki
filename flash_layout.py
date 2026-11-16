"""Pure flash planning. Every USB write is checked against protected NVS sectors."""
import binascii
import hashlib
import struct

SECTOR = 0x1000
COMMON = {"nvs": (1, 2, 0x9000, 0x6000, 0),
          "phy_init": (1, 1, 0xF000, 0x1000, 0),
          "campus_nvs": (1, 2, 0x210000, 0x6000, 0)}
OLD_LAYOUT = {**COMMON, "factory": (0, 0, 0x10000, 0x200000, 0)}
NEW_LAYOUT = {**COMMON, "otadata": (1, 0, 0x216000, 0x2000, 0),
              "ota_0": (0, 0x10, 0x220000, 0x600000, 0),
              "ota_1": (0, 0x11, 0x820000, 0x600000, 0)}


def parse_table(data):
    entries, digest_seen = {}, False
    for offset in range(0, min(len(data), 0xC00), 32):
        entry = data[offset:offset + 32]
        if len(entry) != 32:
            raise ValueError("truncated partition table")
        if entry == b"\xff" * 32:
            break
        if entry[:2] == b"\xeb\xeb":
            if entry[16:] != hashlib.md5(data[:offset]).digest():
                raise ValueError("partition table MD5 mismatch")
            digest_seen = True
            continue
        if digest_seen or entry[:2] != b"\xaa\x50":
            raise ValueError("invalid partition table")
        _, kind, subtype, start, size, name, flags = struct.unpack("<2sBBII16sI", entry)
        name = name.split(b"\0", 1)[0].decode("ascii")
        if name in entries or not size or start % SECTOR or size % SECTOR:
            raise ValueError("invalid partition entry")
        entries[name] = kind, subtype, start, size, flags
    if not entries or not digest_seen:
        raise ValueError("missing partition table checksum")
    previous = 0x9000
    for p in sorted(entries.values(), key=lambda p: p[2]):
        if p[2] < previous:
            raise ValueError("overlapping partition entries")
        previous = p[2] + p[3]
    return entries


def ota_record(data, index):
    entry = data[index * SECTOR:index * SECTOR + 32]
    if len(entry) != 32:
        return None
    seq, _, state, crc = struct.unpack("<I20sII", entry)
    if seq in (0, 0xFFFFFFFF) or state not in (2, 0xFFFFFFFF):
        return None
    if binascii.crc32(entry[:4], 0xFFFFFFFF) & 0xFFFFFFFF != crc:
        return None
    return seq, (seq - 1) % 2, index


def next_ota_record(data):
    for i in range(2):
        e = data[i * SECTOR:i * SECTOR + 32]
        if len(e) == 32:
            seq, _, state, crc = struct.unpack("<I20sII", e)
            if seq not in (0, 0xFFFFFFFF) and state in (0, 1) and \
                    binascii.crc32(e[:4], 0xFFFFFFFF) & 0xFFFFFFFF == crc:
                raise ValueError("新固件尚未完成启动确认；请等待，或使用 --recover。")
    records = [r for i in range(2) if (r := ota_record(data, i))]
    if not records:
        raise ValueError("无法确认有效启动槽，请使用 ./esp.sh flash --recover 恢复。")
    seq, current, copy = max(records)
    target = 1 - current
    seq += 1
    if (seq - 1) % 2 != target:
        seq += 1
    if seq >= 0xFFFFFFFF:
        raise ValueError("OTA sequence exhausted; use --recover")
    word = struct.pack("<I", seq)
    # NEW, not VALID: USB updates receive the same application health check.
    record = struct.pack("<I20sII", seq, b"\xff" * 20, 0,
                         binascii.crc32(word, 0xFFFFFFFF) & 0xFFFFFFFF)
    return target, 0x216000 + (1 - copy) * SECTOR, record + b"\xff" * (SECTOR - 32)


def check_writes(writes, physical_size):
    regions = []
    for offset, data in writes:
        start, end = offset // SECTOR * SECTOR, (offset + len(data) + SECTOR - 1) // SECTOR * SECTOR
        if not data or offset % SECTOR or end > physical_size:
            raise ValueError("flash write outside chip/alignment")
        for name in ("nvs", "phy_init", "campus_nvs"):
            _, _, p, size, _ = COMMON[name]
            if start < p + size and end > p:
                raise ValueError(f"refusing write/erase overlapping {name}")
        if any(start < e and end > s for s, e in regions):
            raise ValueError("overlapping flash writes")
        regions.append((start, end))


def plan_flash(images, previous_table, physical_size, otadata=None, recover=False):
    if physical_size < 0x1000000:
        raise ValueError("此固件需要至少 16 MiB Flash，未写入任何数据。")
    if parse_table(images.get(0x8000, b"")) != NEW_LAYOUT:
        raise ValueError("编译产物分区布局不匹配 dual6-v1。")
    previous = parse_table(previous_table)
    if previous not in (OLD_LAYOUT, NEW_LAYOUT):
        raise ValueError("未知分区布局，拒绝修改。")
    if set(images) != {0, 0x8000, 0x216000, 0x220000}:
        raise ValueError("unexpected flash images")
    app = images[0x220000]
    if not 320 <= len(app) <= 0x600000:
        raise ValueError("application exceeds OTA slot")
    if images[0x216000] != b"\xff" * 0x2000:
        raise ValueError("invalid initial otadata image")
    migration = previous == OLD_LAYOUT
    if migration or recover:
        writes = [(0x220000, app), (0x216000, images[0x216000]), (0, images[0])]
        if migration:
            writes.append((0x8000, images[0x8000]))  # layout becomes active only after app verification
        target = 0
    else:
        target, record_offset, record = next_ota_record(otadata or b"")
        writes = [(0x220000 + target * 0x600000, app), (0, images[0]), (record_offset, record)]
    check_writes(writes, physical_size)
    return writes, migration, target
