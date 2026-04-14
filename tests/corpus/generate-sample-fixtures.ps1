param()

$samplesDir = Join-Path $PSScriptRoot "samples"
New-Item -ItemType Directory -Force -Path $samplesDir | Out-Null

$python = @'
from pathlib import Path
import struct
import sys

samples_dir = Path(sys.argv[1])

APFS_BLOCK_SIZE = 4096
BLOCK_COUNT = 15
BTREE_INFO_SIZE = 40

CONTAINER_OMAP_OBJECT_BLOCK = 2
CONTAINER_OMAP_ROOT_BLOCK = 3
CONTAINER_OMAP_LEAF_BLOCK = 4
LEGACY_VOLUME_BLOCK = 5
CURRENT_VOLUME_BLOCK = 6
VOLUME_OMAP_BLOCK = 7
VOLUME_OMAP_ROOT_BLOCK = 8
FS_TREE_BLOCK = 9
ALPHA_DATA_BLOCK1 = 10
ALPHA_DATA_BLOCK2 = 11
NOTE_DATA_BLOCK = 12
SPARSE_DATA_BLOCK1 = 13
SPARSE_DATA_BLOCK2 = 14

VOLUME_OBJECT_ID = 77
VOLUME_OMAP_OBJECT_ID = 88
FS_TREE_OBJECT_ID = 200
CURRENT_CHECKPOINT_XID = 42

ROOT_INODE_ID = 2
ALPHA_INODE_ID = 20
DOCS_INODE_ID = 30
NOTE_INODE_ID = 31
SPARSE_INODE_ID = 40
COMPRESSED_INODE_ID = 50
EMPTY_INODE_ID = 60

VOL_INCOMPAT_CASE_INSENSITIVE = 0x1
VOL_INCOMPAT_DATALLESS_SNAPS = 0x2
VOL_INCOMPAT_SEALED = 0x20
VOL_ROLE_SYSTEM = 0x0001
VOL_ROLE_DATA = 0x0040

OBJ_TYPE_NXSB = 0x01
OBJ_TYPE_BTREE_NODE = 0x03
OBJ_TYPE_OMAP = 0x0B
OBJ_TYPE_FS = 0x0D

BTN_ROOT = 0x0001
BTN_LEAF = 0x0002
BTN_FIXED_KV = 0x0004
BTN_OFF_INVALID = 0xFFFF

OMAP_VALUE_DELETED = 0x00000001
FS_TYPE_INODE = 3
FS_TYPE_XATTR = 4
FS_TYPE_FILE_EXTENT = 8
FS_TYPE_DIR_REC = 9
SPARSE_FLAG = 0x00000200

ALPHA_EXTENT1 = b"Hello "
ALPHA_EXTENT2 = b"Orchard\n"
NOTE_TEXT = b"Nested note\n"
SPARSE_EXTENT1 = b"ABCD"
SPARSE_EXTENT2 = b"WXYZ"
COMPRESSED_TEXT = b"Compressed orchard\n"

def w16(buf, off, value): struct.pack_into("<H", buf, off, value)
def w32(buf, off, value): struct.pack_into("<I", buf, off, value)
def w64(buf, off, value): struct.pack_into("<Q", buf, off, value)
def wa(buf, off, text): buf[off:off+len(text)] = text.encode("ascii") if isinstance(text, str) else text
def wutf16(buf, off, text): buf[off:off+len(text.encode("utf-16-le"))] = text.encode("utf-16-le")
def wbytes(buf, off, raw): buf[off:off+len(raw)] = raw

def object_header(buf, base, oid, xid, typ, subtype):
    w64(buf, base + 0x00, 0)
    w64(buf, base + 0x08, oid)
    w64(buf, base + 0x10, xid)
    w32(buf, base + 0x18, typ)
    w32(buf, base + 0x1C, subtype)

def nxsb(buf, base, xid, block_count=BLOCK_COUNT):
    object_header(buf, base, base // APFS_BLOCK_SIZE, xid, OBJ_TYPE_NXSB, 0)
    wa(buf, base + 0x20, "NXSB")
    w32(buf, base + 0x24, APFS_BLOCK_SIZE)
    w64(buf, base + 0x28, block_count)
    wbytes(buf, base + 0x48, bytes([0x10,0x11,0x12,0x13,0x20,0x21,0x22,0x23,0x30,0x31,0x32,0x33,0x40,0x41,0x42,0x43]))
    w64(buf, base + 0x60, xid + 1)
    w32(buf, base + 0x68, 1)
    w64(buf, base + 0x70, 1)
    w64(buf, base + 0x98, 5)
    w64(buf, base + 0xA0, CONTAINER_OMAP_OBJECT_BLOCK)
    w64(buf, base + 0xA8, 7)
    w32(buf, base + 0xB4, 100)
    w64(buf, base + 0xB8, VOLUME_OBJECT_ID)

def omap_sb(buf, base, oid, xid, tree_oid):
    object_header(buf, base, oid, xid, OBJ_TYPE_OMAP, OBJ_TYPE_OMAP)
    w32(buf, base + 0x28, OBJ_TYPE_OMAP)
    w64(buf, base + 0x30, tree_oid)

def btree_footer(buf, base, key_size, value_size, key_count, node_count):
    footer = base + APFS_BLOCK_SIZE - BTREE_INFO_SIZE
    w32(buf, footer + 0x04, APFS_BLOCK_SIZE)
    w32(buf, footer + 0x08, key_size)
    w32(buf, footer + 0x0C, value_size)
    w32(buf, footer + 0x10, key_size)
    w32(buf, footer + 0x14, value_size)
    w64(buf, footer + 0x18, key_count)
    w64(buf, footer + 0x20, node_count)

def omap_key(buf, off, oid, xid):
    w64(buf, off + 0x00, oid)
    w64(buf, off + 0x08, xid)

def omap_value(buf, off, flags, physical_block):
    w32(buf, off + 0x00, flags)
    w32(buf, off + 0x04, APFS_BLOCK_SIZE)
    w64(buf, off + 0x08, physical_block)

def omap_root(buf, base, oid, xid, min_oid, min_xid, child_block):
    table_len = 4
    key_start = base + 0x38 + table_len
    value_end = base + APFS_BLOCK_SIZE - BTREE_INFO_SIZE
    value_off = value_end - 8
    object_header(buf, base, oid, xid, OBJ_TYPE_BTREE_NODE, OBJ_TYPE_OMAP)
    w16(buf, base + 0x20, BTN_ROOT | BTN_FIXED_KV)
    w16(buf, base + 0x22, 1)
    w32(buf, base + 0x24, 1)
    w16(buf, base + 0x2A, table_len)
    w16(buf, base + 0x30, BTN_OFF_INVALID)
    w16(buf, base + 0x34, BTN_OFF_INVALID)
    w16(buf, base + 0x38, 0)
    w16(buf, base + 0x3A, value_end - value_off)
    omap_key(buf, key_start, min_oid, min_xid)
    w64(buf, value_off, child_block)
    btree_footer(buf, base, 16, 8, 3, 2)

def omap_leaf(buf, base, oid, xid, records, root=False):
    table_len = len(records) * 4
    key_start = base + 0x38 + table_len
    value_end = base + APFS_BLOCK_SIZE - (BTREE_INFO_SIZE if root else 0)
    object_header(buf, base, oid, xid, OBJ_TYPE_BTREE_NODE, OBJ_TYPE_OMAP)
    w16(buf, base + 0x20, (BTN_ROOT if root else 0) | BTN_LEAF | BTN_FIXED_KV)
    w32(buf, base + 0x24, len(records))
    w16(buf, base + 0x2A, table_len)
    w16(buf, base + 0x30, BTN_OFF_INVALID)
    w16(buf, base + 0x34, BTN_OFF_INVALID)
    for i, (roid, rxid, rflags, rblock) in enumerate(records):
        toc = base + 0x38 + i * 4
        key_off = i * 16
        value_off = (i + 1) * 16
        w16(buf, toc + 0x00, key_off)
        w16(buf, toc + 0x02, value_off)
        omap_key(buf, key_start + key_off, roid, rxid)
        omap_value(buf, value_end - value_off, rflags, rblock)
    if root:
        btree_footer(buf, base, 16, 16, len(records), 1)

def fs_header_value(object_id, record_type): return object_id | (record_type << 60)
def inode_key(object_id): return struct.pack("<Q", fs_header_value(object_id, FS_TYPE_INODE))
def named_key(object_id, record_type, name): return struct.pack("<QH", fs_header_value(object_id, record_type), len(name)) + name.encode("ascii")
def extent_key(object_id, logical): return struct.pack("<QQ", fs_header_value(object_id, FS_TYPE_FILE_EXTENT), logical)
DEFAULT_TIMESTAMP_NS = 1704067200000000000
def inode_value(parent_id, logical_size, allocated_size, flags, child_count, mode, link_count=1,
                creation_time_ns=DEFAULT_TIMESTAMP_NS, access_time_ns=None,
                write_time_ns=None, change_time_ns=None):
    if access_time_ns is None: access_time_ns = creation_time_ns
    if write_time_ns is None: write_time_ns = creation_time_ns
    if change_time_ns is None: change_time_ns = write_time_ns
    data = bytearray(0x60)
    w64(data, 0x00, parent_id); w64(data, 0x10, allocated_size); w64(data, 0x18, flags)
    w32(data, 0x20, child_count); w16(data, 0x24, mode)
    w64(data, 0x28, creation_time_ns); w64(data, 0x30, access_time_ns)
    w64(data, 0x38, write_time_ns); w64(data, 0x40, change_time_ns)
    w32(data, 0x48, link_count); w64(data, 0x58, logical_size)
    return bytes(data)
def dir_value(file_id): return struct.pack("<QH", file_id, 0)
def extent_value(length, physical_block): return struct.pack("<QQQ", length, physical_block, 0)
def compression_payload(text): return b"cmpf" + struct.pack("<IQ", 9, len(text)) + text
def xattr_value(data): return struct.pack("<HHI", 0, 0, len(data)) + data

def variable_root_leaf(buf, base, oid, xid, subtype, records):
    table_len = len(records) * 8
    key_start = base + 0x38 + table_len
    value_end = base + APFS_BLOCK_SIZE - BTREE_INFO_SIZE
    object_header(buf, base, oid, xid, OBJ_TYPE_BTREE_NODE, subtype)
    w16(buf, base + 0x20, BTN_ROOT | BTN_LEAF)
    w32(buf, base + 0x24, len(records))
    w16(buf, base + 0x2A, table_len)
    w16(buf, base + 0x30, BTN_OFF_INVALID)
    w16(buf, base + 0x34, BTN_OFF_INVALID)
    key_off = 0
    value_off = 0
    longest_key = 0
    longest_value = 0
    for i, (key, value) in enumerate(records):
        toc = base + 0x38 + i * 8
        value_off += len(value)
        w16(buf, toc + 0x00, key_off)
        w16(buf, toc + 0x02, len(key))
        w16(buf, toc + 0x04, value_off)
        w16(buf, toc + 0x06, len(value))
        wbytes(buf, key_start + key_off, key)
        wbytes(buf, value_end - value_off, value)
        key_off += len(key)
        longest_key = max(longest_key, len(key))
        longest_value = max(longest_value, len(value))
    footer = base + APFS_BLOCK_SIZE - BTREE_INFO_SIZE
    w32(buf, footer + 0x04, APFS_BLOCK_SIZE)
    w32(buf, footer + 0x10, longest_key)
    w32(buf, footer + 0x14, longest_value)
    w64(buf, footer + 0x18, len(records))
    w64(buf, footer + 0x20, 1)

def variable_leaf(buf, base, oid, xid, subtype, records):
    table_len = len(records) * 8
    key_start = base + 0x38 + table_len
    value_end = base + APFS_BLOCK_SIZE
    object_header(buf, base, oid, xid, OBJ_TYPE_BTREE_NODE, subtype)
    w16(buf, base + 0x20, BTN_LEAF)
    w32(buf, base + 0x24, len(records))
    w16(buf, base + 0x2A, table_len)
    w16(buf, base + 0x30, BTN_OFF_INVALID)
    w16(buf, base + 0x34, BTN_OFF_INVALID)
    key_off = 0
    value_off = 0
    for i, (key, value) in enumerate(records):
        toc = base + 0x38 + i * 8
        value_off += len(value)
        w16(buf, toc + 0x00, key_off)
        w16(buf, toc + 0x02, len(key))
        w16(buf, toc + 0x04, value_off)
        w16(buf, toc + 0x06, len(value))
        wbytes(buf, key_start + key_off, key)
        wbytes(buf, value_end - value_off, value)
        key_off += len(key)

def variable_root_internal(buf, base, oid, xid, subtype, children, key_count, node_count):
    table_len = len(children) * 8
    key_start = base + 0x38 + table_len
    value_end = base + APFS_BLOCK_SIZE - BTREE_INFO_SIZE
    object_header(buf, base, oid, xid, OBJ_TYPE_BTREE_NODE, subtype)
    w16(buf, base + 0x20, BTN_ROOT)
    w16(buf, base + 0x22, 1)
    w32(buf, base + 0x24, len(children))
    w16(buf, base + 0x2A, table_len)
    w16(buf, base + 0x30, BTN_OFF_INVALID)
    w16(buf, base + 0x34, BTN_OFF_INVALID)
    key_off = 0
    value_off = 0
    longest_key = 0
    for i, (key, child_block) in enumerate(children):
        value = struct.pack("<Q", child_block)
        toc = base + 0x38 + i * 8
        value_off += len(value)
        w16(buf, toc + 0x00, key_off)
        w16(buf, toc + 0x02, len(key))
        w16(buf, toc + 0x04, value_off)
        w16(buf, toc + 0x06, len(value))
        wbytes(buf, key_start + key_off, key)
        wbytes(buf, value_end - value_off, value)
        key_off += len(key)
        longest_key = max(longest_key, len(key))
    footer = base + APFS_BLOCK_SIZE - BTREE_INFO_SIZE
    w32(buf, footer + 0x04, APFS_BLOCK_SIZE)
    w32(buf, footer + 0x10, longest_key)
    w32(buf, footer + 0x14, 8)
    w64(buf, footer + 0x18, key_count)
    w64(buf, footer + 0x20, node_count)

def variable_leaf_fits(records):
    table_len = len(records) * 8
    key_bytes = sum(len(key) for key, _ in records)
    value_bytes = sum(len(value) for _, value in records)
    return 0x38 + table_len + key_bytes + value_bytes <= APFS_BLOCK_SIZE

def split_variable_records(records):
    chunks = []
    current = []
    for record in records:
        trial = current + [record]
        if current and not variable_leaf_fits(trial):
            chunks.append(current)
            current = [record]
            continue
        current = trial
    if current:
        chunks.append(current)
    return chunks

def volume_superblock(buf, base, xid, incompat, role, name):
    object_header(buf, base, VOLUME_OBJECT_ID, xid, OBJ_TYPE_FS, 0)
    wa(buf, base + 0x20, "APSB")
    w64(buf, base + 0x38, incompat)
    w32(buf, base + 0x74, OBJ_TYPE_FS)
    w64(buf, base + 0x80, VOLUME_OMAP_OBJECT_ID)
    w64(buf, base + 0x88, FS_TREE_OBJECT_ID)
    wbytes(buf, base + 0xF0, bytes([0x50,0x51,0x52,0x53,0x60,0x61,0x62,0x63,0x70,0x71,0x72,0x73,0x80,0x81,0x82,0x83]))
    wa(buf, base + 0x2C0, name)
    w16(buf, base + 0x3C4, role)

def build_records():
    return [
        (inode_key(ROOT_INODE_ID), inode_value(ROOT_INODE_ID, 0, 0, 0, 4, 0x4000)),
        (inode_key(ALPHA_INODE_ID), inode_value(ROOT_INODE_ID, len(ALPHA_EXTENT1)+len(ALPHA_EXTENT2), len(ALPHA_EXTENT1)+len(ALPHA_EXTENT2), 0, 0, 0x8000)),
        (inode_key(DOCS_INODE_ID), inode_value(ROOT_INODE_ID, 0, 0, 0, 2, 0x4000)),
        (inode_key(NOTE_INODE_ID), inode_value(DOCS_INODE_ID, len(NOTE_TEXT), len(NOTE_TEXT), 0, 0, 0x8000)),
        (inode_key(SPARSE_INODE_ID), inode_value(ROOT_INODE_ID, 12, 8, SPARSE_FLAG, 0, 0x8000)),
        (inode_key(COMPRESSED_INODE_ID), inode_value(ROOT_INODE_ID, len(COMPRESSED_TEXT), 0, 0, 0, 0x8000)),
        (inode_key(EMPTY_INODE_ID), inode_value(DOCS_INODE_ID, 0, 0, 0, 0, 0x8000)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "alpha.txt"), dir_value(ALPHA_INODE_ID)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "compressed.txt"), dir_value(COMPRESSED_INODE_ID)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "docs"), dir_value(DOCS_INODE_ID)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "holes.bin"), dir_value(SPARSE_INODE_ID)),
        (named_key(DOCS_INODE_ID, FS_TYPE_DIR_REC, "empty.txt"), dir_value(EMPTY_INODE_ID)),
        (named_key(DOCS_INODE_ID, FS_TYPE_DIR_REC, "note.txt"), dir_value(NOTE_INODE_ID)),
        (extent_key(ALPHA_INODE_ID, 0), extent_value(len(ALPHA_EXTENT1), ALPHA_DATA_BLOCK1)),
        (extent_key(ALPHA_INODE_ID, len(ALPHA_EXTENT1)), extent_value(len(ALPHA_EXTENT2), ALPHA_DATA_BLOCK2)),
        (extent_key(NOTE_INODE_ID, 0), extent_value(len(NOTE_TEXT), NOTE_DATA_BLOCK)),
        (extent_key(SPARSE_INODE_ID, 0), extent_value(len(SPARSE_EXTENT1), SPARSE_DATA_BLOCK1)),
        (extent_key(SPARSE_INODE_ID, 8), extent_value(len(SPARSE_EXTENT2), SPARSE_DATA_BLOCK2)),
        (named_key(COMPRESSED_INODE_ID, FS_TYPE_XATTR, "com.apple.decmpfs"), xattr_value(compression_payload(COMPRESSED_TEXT))),
    ]

def build_direct(volume_name="Orchard Data", incompat=VOL_INCOMPAT_CASE_INSENSITIVE, role=VOL_ROLE_DATA, delete_latest=False):
    buf = bytearray(APFS_BLOCK_SIZE * BLOCK_COUNT)
    nxsb(buf, 0, 1)
    nxsb(buf, APFS_BLOCK_SIZE, CURRENT_CHECKPOINT_XID)
    omap_sb(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_OBJECT_BLOCK, CONTAINER_OMAP_OBJECT_BLOCK, CURRENT_CHECKPOINT_XID, CONTAINER_OMAP_ROOT_BLOCK)
    omap_root(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_ROOT_BLOCK, CONTAINER_OMAP_ROOT_BLOCK, CURRENT_CHECKPOINT_XID, VOLUME_OBJECT_ID, 20, CONTAINER_OMAP_LEAF_BLOCK)
    omap_leaf(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_LEAF_BLOCK, CONTAINER_OMAP_LEAF_BLOCK, CURRENT_CHECKPOINT_XID, [
        (VOLUME_OBJECT_ID, 20, 0, LEGACY_VOLUME_BLOCK),
        (VOLUME_OBJECT_ID, CURRENT_CHECKPOINT_XID, OMAP_VALUE_DELETED if delete_latest else 0, CURRENT_VOLUME_BLOCK),
        (VOLUME_OMAP_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, VOLUME_OMAP_BLOCK),
    ])
    volume_superblock(buf, APFS_BLOCK_SIZE * LEGACY_VOLUME_BLOCK, 20, VOL_INCOMPAT_CASE_INSENSITIVE, VOL_ROLE_DATA, "Legacy Data")
    volume_superblock(buf, APFS_BLOCK_SIZE * CURRENT_VOLUME_BLOCK, CURRENT_CHECKPOINT_XID, incompat, role, volume_name)
    omap_sb(buf, APFS_BLOCK_SIZE * VOLUME_OMAP_BLOCK, VOLUME_OMAP_OBJECT_ID, CURRENT_CHECKPOINT_XID, VOLUME_OMAP_ROOT_BLOCK)
    omap_leaf(buf, APFS_BLOCK_SIZE * VOLUME_OMAP_ROOT_BLOCK, VOLUME_OMAP_ROOT_BLOCK, CURRENT_CHECKPOINT_XID, [(FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, FS_TREE_BLOCK)], root=True)
    variable_root_leaf(buf, APFS_BLOCK_SIZE * FS_TREE_BLOCK, FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, OBJ_TYPE_FS, build_records())
    wa(buf, APFS_BLOCK_SIZE * ALPHA_DATA_BLOCK1, ALPHA_EXTENT1)
    wa(buf, APFS_BLOCK_SIZE * ALPHA_DATA_BLOCK2, ALPHA_EXTENT2)
    wa(buf, APFS_BLOCK_SIZE * NOTE_DATA_BLOCK, NOTE_TEXT)
    wa(buf, APFS_BLOCK_SIZE * SPARSE_DATA_BLOCK1, SPARSE_EXTENT1)
    wa(buf, APFS_BLOCK_SIZE * SPARSE_DATA_BLOCK2, SPARSE_EXTENT2)
    return bytes(buf)

def build_gpt():
    logical = 512
    first_lba = 40
    last_lba = 159
    buf = bytearray(logical * 256)
    wa(buf, logical, "EFI PART")
    w32(buf, logical + 8, 0x00010000)
    w32(buf, logical + 12, 92)
    w64(buf, logical + 24, 1)
    w64(buf, logical + 32, 255)
    w64(buf, logical + 40, 34)
    w64(buf, logical + 48, 200)
    w64(buf, logical + 72, 2)
    w32(buf, logical + 80, 1)
    w32(buf, logical + 84, 128)
    part = logical * 2
    wbytes(buf, part, bytes([0xEF,0x57,0x34,0x7C,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC]))
    wbytes(buf, part + 16, bytes([0x91,0x92,0x93,0x94,0xA0,0xA1,0xA2,0xA3,0xB0,0xB1,0xB2,0xB3,0xC0,0xC1,0xC2,0xC3]))
    w64(buf, part + 32, first_lba)
    w64(buf, part + 40, last_lba)
    wutf16(buf, part + 56, "Orchard GPT")
    container = build_direct(volume_name="GPT Data")
    buf[first_lba * logical:first_lba * logical + len(container)] = container
    return bytes(buf)

def make_copy_chunk(index):
    header = f"ORCHARD-COPY-BLOCK-{index:02d}\n".encode("ascii")
    repeated = (header * ((APFS_BLOCK_SIZE // len(header)) + 1))[:APFS_BLOCK_SIZE]
    return repeated

def build_explorer_stress():
    block_count = 64
    fs_tree_root_block = 9
    fs_tree_leaf_start_block = 10
    copy_data_blocks = [32, 33, 34, 35]
    preview_data_block = 36
    deep_note_data_block = 37
    alpha_data_blocks = [38, 39]

    bulk_dir_inode_id = 70
    nested_dir_inode_id = 71
    preview_inode_id = 72
    copy_inode_id = 73
    deep_note_inode_id = 74
    bulk_file_inode_start = 1000
    bulk_file_count = 180

    preview_text = b"Explorer preview\n"
    deep_note_text = b"Explorer deep note\n"
    copy_chunks = [make_copy_chunk(index) for index in range(len(copy_data_blocks))]

    def bulk_name(index):
        variants = (
            f"entry {index:03d}.txt",
            f"MixCase {index:03d}.TXT",
            f"space name {index:03d}.log",
            f"Zeta{index:03d}.dat",
        )
        return variants[index % len(variants)]

    records = [
        (inode_key(ROOT_INODE_ID), inode_value(ROOT_INODE_ID, 0, 0, 0, 4, 0x4000)),
        (inode_key(ALPHA_INODE_ID), inode_value(ROOT_INODE_ID, len(ALPHA_EXTENT1)+len(ALPHA_EXTENT2), len(ALPHA_EXTENT1)+len(ALPHA_EXTENT2), 0, 0, 0x8000)),
        (inode_key(bulk_dir_inode_id), inode_value(ROOT_INODE_ID, 0, 0, 0, bulk_file_count + 1, 0x4000)),
        (inode_key(nested_dir_inode_id), inode_value(bulk_dir_inode_id, 0, 0, 0, 1, 0x4000)),
        (inode_key(preview_inode_id), inode_value(ROOT_INODE_ID, len(preview_text), len(preview_text), 0, 0, 0x8000)),
        (inode_key(copy_inode_id), inode_value(ROOT_INODE_ID, len(copy_chunks) * APFS_BLOCK_SIZE, len(copy_chunks) * APFS_BLOCK_SIZE, 0, 0, 0x8000)),
        (inode_key(deep_note_inode_id), inode_value(nested_dir_inode_id, len(deep_note_text), len(deep_note_text), 0, 0, 0x8000)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "alpha.txt"), dir_value(ALPHA_INODE_ID)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "bulk items"), dir_value(bulk_dir_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "copy-source.bin"), dir_value(copy_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "preview.txt"), dir_value(preview_inode_id)),
        (named_key(bulk_dir_inode_id, FS_TYPE_DIR_REC, "Nested Folder"), dir_value(nested_dir_inode_id)),
        (named_key(nested_dir_inode_id, FS_TYPE_DIR_REC, "deep-note.txt"), dir_value(deep_note_inode_id)),
        (extent_key(ALPHA_INODE_ID, 0), extent_value(len(ALPHA_EXTENT1), alpha_data_blocks[0])),
        (extent_key(ALPHA_INODE_ID, len(ALPHA_EXTENT1)), extent_value(len(ALPHA_EXTENT2), alpha_data_blocks[1])),
        (extent_key(preview_inode_id, 0), extent_value(len(preview_text), preview_data_block)),
        (extent_key(deep_note_inode_id, 0), extent_value(len(deep_note_text), deep_note_data_block)),
    ]

    for index, data_block in enumerate(copy_data_blocks):
        records.append((extent_key(copy_inode_id, index * APFS_BLOCK_SIZE),
                        extent_value(APFS_BLOCK_SIZE, data_block)))

    for index in range(bulk_file_count):
        inode_id = bulk_file_inode_start + index
        records.append((inode_key(inode_id), inode_value(bulk_dir_inode_id, 0, 0, 0, 0, 0x8000)))
        records.append((named_key(bulk_dir_inode_id, FS_TYPE_DIR_REC, bulk_name(index)),
                        dir_value(inode_id)))

    sorted_records = sorted(records, key=lambda record: record[0])
    leaf_chunks = split_variable_records(sorted_records)
    if len(leaf_chunks) > (copy_data_blocks[0] - fs_tree_leaf_start_block):
        raise RuntimeError("Explorer stress fixture exceeded the reserved metadata leaf range.")

    buf = bytearray(APFS_BLOCK_SIZE * block_count)
    nxsb(buf, 0, 1, block_count)
    nxsb(buf, APFS_BLOCK_SIZE, CURRENT_CHECKPOINT_XID, block_count)
    omap_sb(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_OBJECT_BLOCK, CONTAINER_OMAP_OBJECT_BLOCK, CURRENT_CHECKPOINT_XID, CONTAINER_OMAP_ROOT_BLOCK)
    omap_root(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_ROOT_BLOCK, CONTAINER_OMAP_ROOT_BLOCK, CURRENT_CHECKPOINT_XID, VOLUME_OBJECT_ID, 20, CONTAINER_OMAP_LEAF_BLOCK)
    omap_leaf(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_LEAF_BLOCK, CONTAINER_OMAP_LEAF_BLOCK, CURRENT_CHECKPOINT_XID, [
        (VOLUME_OBJECT_ID, 20, 0, LEGACY_VOLUME_BLOCK),
        (VOLUME_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, CURRENT_VOLUME_BLOCK),
        (VOLUME_OMAP_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, VOLUME_OMAP_BLOCK),
    ])
    volume_superblock(buf, APFS_BLOCK_SIZE * LEGACY_VOLUME_BLOCK, 20, VOL_INCOMPAT_CASE_INSENSITIVE, VOL_ROLE_DATA, "Legacy Data")
    volume_superblock(buf, APFS_BLOCK_SIZE * CURRENT_VOLUME_BLOCK, CURRENT_CHECKPOINT_XID, VOL_INCOMPAT_CASE_INSENSITIVE, VOL_ROLE_DATA, "Explorer Stress")
    omap_sb(buf, APFS_BLOCK_SIZE * VOLUME_OMAP_BLOCK, VOLUME_OMAP_OBJECT_ID, CURRENT_CHECKPOINT_XID, VOLUME_OMAP_ROOT_BLOCK)
    omap_leaf(buf, APFS_BLOCK_SIZE * VOLUME_OMAP_ROOT_BLOCK, VOLUME_OMAP_ROOT_BLOCK, CURRENT_CHECKPOINT_XID, [(FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, fs_tree_root_block)], root=True)

    child_records = []
    for index, chunk in enumerate(leaf_chunks):
        leaf_block = fs_tree_leaf_start_block + index
        leaf_oid = FS_TREE_OBJECT_ID + index + 1
        variable_leaf(buf, APFS_BLOCK_SIZE * leaf_block, leaf_oid, CURRENT_CHECKPOINT_XID, OBJ_TYPE_FS, chunk)
        child_records.append((chunk[0][0], leaf_block))

    if len(child_records) == 1:
        variable_root_leaf(buf, APFS_BLOCK_SIZE * fs_tree_root_block, FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, OBJ_TYPE_FS, leaf_chunks[0])
    else:
        variable_root_internal(buf, APFS_BLOCK_SIZE * fs_tree_root_block, FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, OBJ_TYPE_FS, child_records, len(sorted_records), 1 + len(child_records))

    wa(buf, APFS_BLOCK_SIZE * alpha_data_blocks[0], ALPHA_EXTENT1)
    wa(buf, APFS_BLOCK_SIZE * alpha_data_blocks[1], ALPHA_EXTENT2)
    wa(buf, APFS_BLOCK_SIZE * preview_data_block, preview_text)
    wa(buf, APFS_BLOCK_SIZE * deep_note_data_block, deep_note_text)
    for block, chunk in zip(copy_data_blocks, copy_chunks):
        wbytes(buf, APFS_BLOCK_SIZE * block, chunk)

    return bytes(buf)

def build_link_behavior():
    block_count = 24
    root_tree_block = 9
    alpha_data_blocks = [10, 11]
    note_data_block = 12
    relative_link_data_block = 13
    absolute_link_data_block = 14
    broken_link_data_block = 15
    hard_link_data_block = 16

    relative_link_inode_id = 70
    absolute_link_inode_id = 71
    broken_link_inode_id = 72
    hard_link_inode_id = 73

    relative_link_target = b"docs/note.txt"
    absolute_link_target = b"/alpha.txt"
    broken_link_target = b"/missing/ghost.txt"
    hard_link_text = b"Shared hard-link payload\n"

    records = [
        (inode_key(ROOT_INODE_ID), inode_value(ROOT_INODE_ID, 0, 0, 0, 8, 0x4000)),
        (inode_key(ALPHA_INODE_ID), inode_value(ROOT_INODE_ID, len(ALPHA_EXTENT1)+len(ALPHA_EXTENT2), len(ALPHA_EXTENT1)+len(ALPHA_EXTENT2), 0, 0, 0x8000)),
        (inode_key(DOCS_INODE_ID), inode_value(ROOT_INODE_ID, 0, 0, 0, 1, 0x4000)),
        (inode_key(NOTE_INODE_ID), inode_value(DOCS_INODE_ID, len(NOTE_TEXT), len(NOTE_TEXT), 0, 0, 0x8000, link_count=2)),
        (inode_key(relative_link_inode_id), inode_value(ROOT_INODE_ID, len(relative_link_target), len(relative_link_target), 0, 0, 0xA000)),
        (inode_key(absolute_link_inode_id), inode_value(ROOT_INODE_ID, len(absolute_link_target), len(absolute_link_target), 0, 0, 0xA000)),
        (inode_key(broken_link_inode_id), inode_value(ROOT_INODE_ID, len(broken_link_target), len(broken_link_target), 0, 0, 0xA000)),
        (inode_key(hard_link_inode_id), inode_value(ROOT_INODE_ID, len(hard_link_text), len(hard_link_text), 0, 0, 0x8000, link_count=2)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "a-relative-note-link.txt"), dir_value(relative_link_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "absolute-alpha-link.txt"), dir_value(absolute_link_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "alpha.txt"), dir_value(ALPHA_INODE_ID)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "broken-link.txt"), dir_value(broken_link_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "docs"), dir_value(DOCS_INODE_ID)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "hard-a.txt"), dir_value(hard_link_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "hard-b.txt"), dir_value(hard_link_inode_id)),
        (named_key(ROOT_INODE_ID, FS_TYPE_DIR_REC, "note-link.txt"), dir_value(NOTE_INODE_ID)),
        (named_key(DOCS_INODE_ID, FS_TYPE_DIR_REC, "note.txt"), dir_value(NOTE_INODE_ID)),
        (extent_key(ALPHA_INODE_ID, 0), extent_value(len(ALPHA_EXTENT1), alpha_data_blocks[0])),
        (extent_key(ALPHA_INODE_ID, len(ALPHA_EXTENT1)), extent_value(len(ALPHA_EXTENT2), alpha_data_blocks[1])),
        (extent_key(NOTE_INODE_ID, 0), extent_value(len(NOTE_TEXT), note_data_block)),
        (extent_key(relative_link_inode_id, 0), extent_value(len(relative_link_target), relative_link_data_block)),
        (extent_key(absolute_link_inode_id, 0), extent_value(len(absolute_link_target), absolute_link_data_block)),
        (extent_key(broken_link_inode_id, 0), extent_value(len(broken_link_target), broken_link_data_block)),
        (extent_key(hard_link_inode_id, 0), extent_value(len(hard_link_text), hard_link_data_block)),
    ]

    sorted_records = sorted(records, key=lambda record: record[0])
    buf = bytearray(APFS_BLOCK_SIZE * block_count)
    nxsb(buf, 0, 1, block_count)
    nxsb(buf, APFS_BLOCK_SIZE, CURRENT_CHECKPOINT_XID, block_count)
    omap_sb(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_OBJECT_BLOCK, CONTAINER_OMAP_OBJECT_BLOCK, CURRENT_CHECKPOINT_XID, CONTAINER_OMAP_ROOT_BLOCK)
    omap_root(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_ROOT_BLOCK, CONTAINER_OMAP_ROOT_BLOCK, CURRENT_CHECKPOINT_XID, VOLUME_OBJECT_ID, 20, CONTAINER_OMAP_LEAF_BLOCK)
    omap_leaf(buf, APFS_BLOCK_SIZE * CONTAINER_OMAP_LEAF_BLOCK, CONTAINER_OMAP_LEAF_BLOCK, CURRENT_CHECKPOINT_XID, [
        (VOLUME_OBJECT_ID, 20, 0, LEGACY_VOLUME_BLOCK),
        (VOLUME_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, CURRENT_VOLUME_BLOCK),
        (VOLUME_OMAP_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, VOLUME_OMAP_BLOCK),
    ])
    volume_superblock(buf, APFS_BLOCK_SIZE * LEGACY_VOLUME_BLOCK, 20, VOL_INCOMPAT_CASE_INSENSITIVE, VOL_ROLE_DATA, "Legacy Data")
    volume_superblock(buf, APFS_BLOCK_SIZE * CURRENT_VOLUME_BLOCK, CURRENT_CHECKPOINT_XID, VOL_INCOMPAT_CASE_INSENSITIVE, VOL_ROLE_DATA, "Link Behavior")
    omap_sb(buf, APFS_BLOCK_SIZE * VOLUME_OMAP_BLOCK, VOLUME_OMAP_OBJECT_ID, CURRENT_CHECKPOINT_XID, VOLUME_OMAP_ROOT_BLOCK)
    omap_leaf(buf, APFS_BLOCK_SIZE * VOLUME_OMAP_ROOT_BLOCK, VOLUME_OMAP_ROOT_BLOCK, CURRENT_CHECKPOINT_XID, [(FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, 0, root_tree_block)], root=True)
    variable_root_leaf(buf, APFS_BLOCK_SIZE * root_tree_block, FS_TREE_OBJECT_ID, CURRENT_CHECKPOINT_XID, OBJ_TYPE_FS, sorted_records)
    wa(buf, APFS_BLOCK_SIZE * alpha_data_blocks[0], ALPHA_EXTENT1)
    wa(buf, APFS_BLOCK_SIZE * alpha_data_blocks[1], ALPHA_EXTENT2)
    wa(buf, APFS_BLOCK_SIZE * note_data_block, NOTE_TEXT)
    wbytes(buf, APFS_BLOCK_SIZE * relative_link_data_block, relative_link_target)
    wbytes(buf, APFS_BLOCK_SIZE * absolute_link_data_block, absolute_link_target)
    wbytes(buf, APFS_BLOCK_SIZE * broken_link_data_block, broken_link_target)
    wbytes(buf, APFS_BLOCK_SIZE * hard_link_data_block, hard_link_text)
    return bytes(buf)

(samples_dir / "plain-user-data.img").write_bytes(build_direct())
(samples_dir / "gpt-user-data.img").write_bytes(build_gpt())
(samples_dir / "snapshot-volume.img").write_bytes(build_direct(volume_name="Snapshot Data", incompat=VOL_INCOMPAT_CASE_INSENSITIVE | VOL_INCOMPAT_DATALLESS_SNAPS))
(samples_dir / "sealed-system.img").write_bytes(build_direct(volume_name="System", incompat=VOL_INCOMPAT_CASE_INSENSITIVE | VOL_INCOMPAT_SEALED, role=VOL_ROLE_SYSTEM))
(samples_dir / "explorer-large.img").write_bytes(build_explorer_stress())
(samples_dir / "link-behavior.img").write_bytes(build_link_behavior())
'@

$python | python - $samplesDir
