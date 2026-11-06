#!/usr/bin/env python3
# Check that apfs.py and apfs_format.h describe the same bytes.
#
# apfs.py reads the disk with struct.unpack_from and literal offsets.  The table below is
# those offsets, written out in one place; fmt_check.c prints what the C header says the
# same offsets are.  If the header and the reader ever drift apart, this fails.
#
# usage: test_format.py FMT_CHECK_OUTPUT

import sys

# (structure, field) -> (offset, size).  Size 0 means a trailing variable-length field,
# where only the offset is checked.
EXPECTED = {
    ("apfs_obj_phys", "o_cksum"): (0, 8),
    ("apfs_obj_phys", "o_oid"): (8, 8),
    ("apfs_obj_phys", "o_xid"): (16, 8),
    ("apfs_obj_phys", "o_type"): (24, 4),
    ("apfs_obj_phys", "o_subtype"): (28, 4),

    ("apfs_nx_superblock", "nx_magic"): (32, 4),
    ("apfs_nx_superblock", "nx_block_size"): (36, 4),
    ("apfs_nx_superblock", "nx_block_count"): (40, 8),
    ("apfs_nx_superblock", "nx_features"): (48, 8),
    ("apfs_nx_superblock", "nx_readonly_compatible_features"): (56, 8),
    ("apfs_nx_superblock", "nx_incompatible_features"): (64, 8),
    ("apfs_nx_superblock", "nx_uuid"): (72, 16),
    ("apfs_nx_superblock", "nx_next_oid"): (88, 8),
    ("apfs_nx_superblock", "nx_next_xid"): (96, 8),
    ("apfs_nx_superblock", "nx_xp_desc_blocks"): (104, 4),
    ("apfs_nx_superblock", "nx_xp_data_blocks"): (108, 4),
    ("apfs_nx_superblock", "nx_xp_desc_base"): (112, 8),
    ("apfs_nx_superblock", "nx_xp_data_base"): (120, 8),
    ("apfs_nx_superblock", "nx_xp_desc_next"): (128, 4),
    ("apfs_nx_superblock", "nx_spaceman_oid"): (152, 8),
    ("apfs_nx_superblock", "nx_omap_oid"): (160, 8),
    ("apfs_nx_superblock", "nx_reaper_oid"): (168, 8),
    ("apfs_nx_superblock", "nx_max_file_systems"): (180, 4),
    ("apfs_nx_superblock", "nx_fs_oid"): (184, 800),

    ("apfs_omap_phys", "om_flags"): (32, 4),
    ("apfs_omap_phys", "om_tree_type"): (40, 4),
    ("apfs_omap_phys", "om_tree_oid"): (48, 8),
    ("apfs_omap_phys", "om_snapshot_tree_oid"): (56, 8),
    ("apfs_omap_key", "ok_oid"): (0, 8),
    ("apfs_omap_key", "ok_xid"): (8, 8),
    ("apfs_omap_val", "ov_flags"): (0, 4),
    ("apfs_omap_val", "ov_size"): (4, 4),
    ("apfs_omap_val", "ov_paddr"): (8, 8),

    ("apfs_superblock", "apfs_magic"): (32, 4),
    ("apfs_superblock", "apfs_fs_index"): (36, 4),
    ("apfs_superblock", "apfs_features"): (40, 8),
    ("apfs_superblock", "apfs_incompatible_features"): (56, 8),
    ("apfs_superblock", "apfs_unmount_time"): (64, 8),
    ("apfs_superblock", "apfs_fs_alloc_count"): (88, 8),
    ("apfs_superblock", "apfs_meta_crypto"): (96, 20),
    ("apfs_superblock", "apfs_root_tree_type"): (116, 4),
    ("apfs_superblock", "apfs_omap_oid"): (128, 8),
    ("apfs_superblock", "apfs_root_tree_oid"): (136, 8),
    ("apfs_superblock", "apfs_extentref_tree_oid"): (144, 8),
    ("apfs_superblock", "apfs_snap_meta_tree_oid"): (152, 8),
    ("apfs_superblock", "apfs_revert_to_xid"): (160, 8),
    ("apfs_superblock", "apfs_next_obj_id"): (176, 8),
    ("apfs_superblock", "apfs_num_files"): (184, 8),
    ("apfs_superblock", "apfs_num_directories"): (192, 8),
    ("apfs_superblock", "apfs_num_symlinks"): (200, 8),
    ("apfs_superblock", "apfs_num_other_fsobjects"): (208, 8),
    ("apfs_superblock", "apfs_num_snapshots"): (216, 8),
    ("apfs_superblock", "apfs_total_blocks_alloced"): (224, 8),
    ("apfs_superblock", "apfs_vol_uuid"): (240, 16),
    ("apfs_superblock", "apfs_last_mod_time"): (256, 8),
    ("apfs_superblock", "apfs_fs_flags"): (264, 8),
    ("apfs_superblock", "apfs_formatted_by"): (272, 48),
    ("apfs_superblock", "apfs_modified_by"): (320, 384),
    ("apfs_superblock", "apfs_volname"): (704, 256),
    ("apfs_superblock", "apfs_next_doc_id"): (960, 4),
    ("apfs_superblock", "apfs_role"): (964, 2),

    ("apfs_btree_node_phys", "btn_flags"): (32, 2),
    ("apfs_btree_node_phys", "btn_level"): (34, 2),
    ("apfs_btree_node_phys", "btn_nkeys"): (36, 4),
    ("apfs_btree_node_phys", "btn_table_space"): (40, 4),
    ("apfs_btree_node_phys", "btn_free_space"): (44, 4),
    ("apfs_btree_node_phys", "btn_data"): (56, 0),
    ("apfs_btree_info", "bt_fixed"): (0, 16),
    ("apfs_btree_info", "bt_longest_key"): (16, 4),
    ("apfs_btree_info", "bt_longest_val"): (20, 4),
    ("apfs_btree_info", "bt_key_count"): (24, 8),
    ("apfs_btree_info", "bt_node_count"): (32, 8),

    ("apfs_j_inode_val", "parent_id"): (0, 8),
    ("apfs_j_inode_val", "private_id"): (8, 8),
    ("apfs_j_inode_val", "create_time"): (16, 8),
    ("apfs_j_inode_val", "mod_time"): (24, 8),
    ("apfs_j_inode_val", "change_time"): (32, 8),
    ("apfs_j_inode_val", "access_time"): (40, 8),
    ("apfs_j_inode_val", "internal_flags"): (48, 8),
    ("apfs_j_inode_val", "nchildren"): (56, 4),
    ("apfs_j_inode_val", "default_protection_class"): (60, 4),
    ("apfs_j_inode_val", "write_generation_counter"): (64, 4),
    ("apfs_j_inode_val", "bsd_flags"): (68, 4),
    ("apfs_j_inode_val", "owner"): (72, 4),
    ("apfs_j_inode_val", "group"): (76, 4),
    ("apfs_j_inode_val", "mode"): (80, 2),
    ("apfs_j_inode_val", "uncompressed_size"): (84, 8),
    ("apfs_j_inode_val", "xfields"): (92, 0),

    ("apfs_j_drec_hashed_key", "name_len_and_hash"): (8, 4),
    ("apfs_j_drec_hashed_key", "name"): (12, 0),
    ("apfs_j_drec_val", "file_id"): (0, 8),
    ("apfs_j_drec_val", "date_added"): (8, 8),
    ("apfs_j_drec_val", "flags"): (16, 2),

    ("apfs_j_xattr_key", "name_len"): (8, 2),
    ("apfs_j_xattr_key", "name"): (10, 0),
    ("apfs_j_xattr_val", "flags"): (0, 2),
    ("apfs_j_xattr_val", "xdata_len"): (2, 2),
    ("apfs_j_xattr_val", "xdata"): (4, 0),

    ("apfs_j_dstream", "size"): (0, 8),
    ("apfs_j_dstream", "alloced_size"): (8, 8),
    ("apfs_j_dstream", "default_crypto_id"): (16, 8),
    ("apfs_j_dstream", "total_bytes_written"): (24, 8),
    ("apfs_j_dstream", "total_bytes_read"): (32, 8),
    ("apfs_j_xattr_dstream", "xattr_obj_id"): (0, 8),
    ("apfs_j_xattr_dstream", "dstream"): (8, 40),

    ("apfs_j_file_extent_key", "logical_addr"): (8, 8),
    ("apfs_j_file_extent_val", "len_and_flags"): (0, 8),
    ("apfs_j_file_extent_val", "phys_block_num"): (8, 8),
    ("apfs_j_file_extent_val", "crypto_id"): (16, 8),

    ("apfs_xf_blob", "xf_num_exts"): (0, 2),
    ("apfs_xf_blob", "xf_used_data"): (2, 2),
    ("apfs_xf_blob", "xf_data"): (4, 0),
    ("apfs_x_field", "x_type"): (0, 1),
    ("apfs_x_field", "x_flags"): (1, 1),
    ("apfs_x_field", "x_size"): (2, 2),

    ("apfs_decmpfs_disk_header", "compression_magic"): (0, 4),
    ("apfs_decmpfs_disk_header", "compression_type"): (4, 4),
    ("apfs_decmpfs_disk_header", "uncompressed_size"): (8, 8),
    ("apfs_decmpfs_disk_header", "attr_bytes"): (16, 0),
}

# structure -> size in bytes, as apfs.py steps over it
EXPECTED_SIZES = {
    "apfs_obj_phys": 32,
    "apfs_prange": 16,
    "apfs_omap_key": 16,
    "apfs_omap_val": 16,
    "apfs_wrapped_meta_crypto_state": 20,
    "apfs_modified_by": 48,
    "apfs_nloc": 4,
    "apfs_kvloc": 8,
    "apfs_kvoff": 4,
    "apfs_btree_node_phys": 56,
    "apfs_btree_info_fixed": 16,
    "apfs_btree_info": 40,
    "apfs_j_key": 8,
    "apfs_j_inode_val": 92,
    "apfs_j_dstream": 40,
    "apfs_j_xattr_dstream": 48,
    "apfs_xf_blob": 4,
    "apfs_x_field": 4,
    "apfs_checkpoint_mapping": 40,
}


def main(argv):
    if len(argv) < 2:
        print("usage: test_format.py FMT_CHECK_OUTPUT", file=sys.stderr)
        return 2
    got_off = {}
    got_size = {}
    with open(argv[1]) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3:
                continue
            name, a, b = parts
            if a == "sizeof":
                got_size[name] = int(b)
            else:
                st, _, fld = name.partition(".")
                got_off[(st, fld)] = (int(a), int(b))

    fails = []
    for key, (off, size) in sorted(EXPECTED.items()):
        if key not in got_off:
            fails.append("%s.%s: not printed by fmt_check" % key)
            continue
        c_off, c_size = got_off[key]
        if c_off != off:
            fails.append("%s.%s: apfs.py reads at %d, the header puts it at %d"
                         % (key[0], key[1], off, c_off))
        elif size and c_size != size:
            fails.append("%s.%s: apfs.py reads %d bytes, the header has %d"
                         % (key[0], key[1], size, c_size))
    for name, size in sorted(EXPECTED_SIZES.items()):
        if name not in got_size:
            fails.append("%s: no size printed by fmt_check" % name)
        elif got_size[name] != size:
            fails.append("%s: apfs.py assumes %d bytes, the header is %d"
                         % (name, size, got_size[name]))

    print("checked %d field offsets and %d structure sizes against apfs_format.h"
          % (len(EXPECTED), len(EXPECTED_SIZES)))
    if fails:
        for m in fails:
            print("  FAIL %s" % m)
        print("FAILED: %d mismatches" % len(fails))
        return 1
    print("OK: the header and apfs.py agree on every one")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
