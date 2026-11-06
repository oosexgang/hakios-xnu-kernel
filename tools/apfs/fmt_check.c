/*
 * Print the size of every structure in apfs_format.h and the offset of every field the
 * reader depends on, so that the header and tools/apfs/apfs.py can be checked against
 * each other instead of being trusted to agree.  Built and run by run-tests.sh.
 *
 * Output is one line per item: "name<TAB>offset<TAB>size", and "name<TAB>sizeof" for a
 * whole structure.
 */

#include <stdio.h>
#include <stddef.h>

#include "apfs_format.h"

#define SIZE(t)       printf("%s\tsizeof\t%zu\n", #t, sizeof(struct t))
#define OFF(t, f)     printf("%s.%s\t%zu\t%zu\n", #t, #f, offsetof(struct t, f), \
                             sizeof(((struct t *)0)->f))
/* a flexible array member has no size of its own, so only its offset is printed */
#define OFFA(t, f)    printf("%s.%s\t%zu\t0\n", #t, #f, offsetof(struct t, f))

int
main(void)
{
	SIZE(apfs_prange);
	OFF(apfs_prange, pr_start_paddr);
	OFF(apfs_prange, pr_block_count);

	SIZE(apfs_obj_phys);
	OFF(apfs_obj_phys, o_cksum);
	OFF(apfs_obj_phys, o_oid);
	OFF(apfs_obj_phys, o_xid);
	OFF(apfs_obj_phys, o_type);
	OFF(apfs_obj_phys, o_subtype);

	SIZE(apfs_nx_superblock);
	OFF(apfs_nx_superblock, nx_magic);
	OFF(apfs_nx_superblock, nx_block_size);
	OFF(apfs_nx_superblock, nx_block_count);
	OFF(apfs_nx_superblock, nx_features);
	OFF(apfs_nx_superblock, nx_readonly_compatible_features);
	OFF(apfs_nx_superblock, nx_incompatible_features);
	OFF(apfs_nx_superblock, nx_uuid);
	OFF(apfs_nx_superblock, nx_next_oid);
	OFF(apfs_nx_superblock, nx_next_xid);
	OFF(apfs_nx_superblock, nx_xp_desc_blocks);
	OFF(apfs_nx_superblock, nx_xp_data_blocks);
	OFF(apfs_nx_superblock, nx_xp_desc_base);
	OFF(apfs_nx_superblock, nx_xp_data_base);
	OFF(apfs_nx_superblock, nx_xp_desc_next);
	OFF(apfs_nx_superblock, nx_xp_data_next);
	OFF(apfs_nx_superblock, nx_xp_desc_index);
	OFF(apfs_nx_superblock, nx_xp_desc_len);
	OFF(apfs_nx_superblock, nx_xp_data_index);
	OFF(apfs_nx_superblock, nx_xp_data_len);
	OFF(apfs_nx_superblock, nx_spaceman_oid);
	OFF(apfs_nx_superblock, nx_omap_oid);
	OFF(apfs_nx_superblock, nx_reaper_oid);
	OFF(apfs_nx_superblock, nx_max_file_systems);
	OFF(apfs_nx_superblock, nx_fs_oid);

	SIZE(apfs_checkpoint_mapping);
	OFF(apfs_checkpoint_mapping, cpm_type);
	OFF(apfs_checkpoint_mapping, cpm_oid);
	OFF(apfs_checkpoint_mapping, cpm_paddr);
	OFF(apfs_checkpoint_map_phys, cpm_flags);
	OFF(apfs_checkpoint_map_phys, cpm_count);
	OFFA(apfs_checkpoint_map_phys, cpm_map);

	SIZE(apfs_omap_phys);
	OFF(apfs_omap_phys, om_flags);
	OFF(apfs_omap_phys, om_snap_count);
	OFF(apfs_omap_phys, om_tree_type);
	OFF(apfs_omap_phys, om_snapshot_tree_type);
	OFF(apfs_omap_phys, om_tree_oid);
	OFF(apfs_omap_phys, om_snapshot_tree_oid);

	SIZE(apfs_omap_key);
	OFF(apfs_omap_key, ok_oid);
	OFF(apfs_omap_key, ok_xid);
	SIZE(apfs_omap_val);
	OFF(apfs_omap_val, ov_flags);
	OFF(apfs_omap_val, ov_size);
	OFF(apfs_omap_val, ov_paddr);

	SIZE(apfs_wrapped_meta_crypto_state);
	SIZE(apfs_modified_by);

	SIZE(apfs_superblock);
	OFF(apfs_superblock, apfs_magic);
	OFF(apfs_superblock, apfs_fs_index);
	OFF(apfs_superblock, apfs_features);
	OFF(apfs_superblock, apfs_readonly_compatible_features);
	OFF(apfs_superblock, apfs_incompatible_features);
	OFF(apfs_superblock, apfs_unmount_time);
	OFF(apfs_superblock, apfs_fs_reserve_block_count);
	OFF(apfs_superblock, apfs_fs_quota_block_count);
	OFF(apfs_superblock, apfs_fs_alloc_count);
	OFF(apfs_superblock, apfs_meta_crypto);
	OFF(apfs_superblock, apfs_root_tree_type);
	OFF(apfs_superblock, apfs_extentref_tree_type);
	OFF(apfs_superblock, apfs_snap_meta_tree_type);
	OFF(apfs_superblock, apfs_omap_oid);
	OFF(apfs_superblock, apfs_root_tree_oid);
	OFF(apfs_superblock, apfs_extentref_tree_oid);
	OFF(apfs_superblock, apfs_snap_meta_tree_oid);
	OFF(apfs_superblock, apfs_revert_to_xid);
	OFF(apfs_superblock, apfs_revert_to_sblock_oid);
	OFF(apfs_superblock, apfs_next_obj_id);
	OFF(apfs_superblock, apfs_num_files);
	OFF(apfs_superblock, apfs_num_directories);
	OFF(apfs_superblock, apfs_num_symlinks);
	OFF(apfs_superblock, apfs_num_other_fsobjects);
	OFF(apfs_superblock, apfs_num_snapshots);
	OFF(apfs_superblock, apfs_total_blocks_alloced);
	OFF(apfs_superblock, apfs_total_blocks_freed);
	OFF(apfs_superblock, apfs_vol_uuid);
	OFF(apfs_superblock, apfs_last_mod_time);
	OFF(apfs_superblock, apfs_fs_flags);
	OFF(apfs_superblock, apfs_formatted_by);
	OFF(apfs_superblock, apfs_modified_by);
	OFF(apfs_superblock, apfs_volname);
	OFF(apfs_superblock, apfs_next_doc_id);
	OFF(apfs_superblock, apfs_role);

	SIZE(apfs_nloc);
	OFF(apfs_nloc, off);
	OFF(apfs_nloc, len);
	SIZE(apfs_kvloc);
	SIZE(apfs_kvoff);

	SIZE(apfs_btree_node_phys);
	OFF(apfs_btree_node_phys, btn_flags);
	OFF(apfs_btree_node_phys, btn_level);
	OFF(apfs_btree_node_phys, btn_nkeys);
	OFF(apfs_btree_node_phys, btn_table_space);
	OFF(apfs_btree_node_phys, btn_free_space);
	OFF(apfs_btree_node_phys, btn_key_free_list);
	OFF(apfs_btree_node_phys, btn_val_free_list);
	OFFA(apfs_btree_node_phys, btn_data);

	SIZE(apfs_btree_info_fixed);
	SIZE(apfs_btree_info);
	OFF(apfs_btree_info, bt_fixed);
	OFF(apfs_btree_info, bt_longest_key);
	OFF(apfs_btree_info, bt_longest_val);
	OFF(apfs_btree_info, bt_key_count);
	OFF(apfs_btree_info, bt_node_count);

	SIZE(apfs_j_key);
	SIZE(apfs_j_inode_val);
	OFF(apfs_j_inode_val, parent_id);
	OFF(apfs_j_inode_val, private_id);
	OFF(apfs_j_inode_val, create_time);
	OFF(apfs_j_inode_val, mod_time);
	OFF(apfs_j_inode_val, change_time);
	OFF(apfs_j_inode_val, access_time);
	OFF(apfs_j_inode_val, internal_flags);
	OFF(apfs_j_inode_val, nchildren);
	OFF(apfs_j_inode_val, default_protection_class);
	OFF(apfs_j_inode_val, write_generation_counter);
	OFF(apfs_j_inode_val, bsd_flags);
	OFF(apfs_j_inode_val, owner);
	OFF(apfs_j_inode_val, group);
	OFF(apfs_j_inode_val, mode);
	OFF(apfs_j_inode_val, pad1);
	OFF(apfs_j_inode_val, uncompressed_size);
	OFFA(apfs_j_inode_val, xfields);

	OFF(apfs_j_drec_key, name_len);
	OFF(apfs_j_drec_key, name);
	OFF(apfs_j_drec_hashed_key, name_len_and_hash);
	OFF(apfs_j_drec_hashed_key, name);
	OFF(apfs_j_drec_val, file_id);
	OFF(apfs_j_drec_val, date_added);
	OFF(apfs_j_drec_val, flags);
	OFFA(apfs_j_drec_val, xfields);

	OFF(apfs_j_xattr_key, name_len);
	OFF(apfs_j_xattr_key, name);
	OFF(apfs_j_xattr_val, flags);
	OFF(apfs_j_xattr_val, xdata_len);
	OFF(apfs_j_xattr_val, xdata);

	SIZE(apfs_j_dstream);
	OFF(apfs_j_dstream, size);
	OFF(apfs_j_dstream, alloced_size);
	OFF(apfs_j_dstream, default_crypto_id);
	OFF(apfs_j_dstream, total_bytes_written);
	OFF(apfs_j_dstream, total_bytes_read);
	SIZE(apfs_j_xattr_dstream);
	OFF(apfs_j_xattr_dstream, xattr_obj_id);
	OFF(apfs_j_xattr_dstream, dstream);

	OFF(apfs_j_file_extent_key, logical_addr);
	OFF(apfs_j_file_extent_val, len_and_flags);
	OFF(apfs_j_file_extent_val, phys_block_num);
	OFF(apfs_j_file_extent_val, crypto_id);

	OFF(apfs_j_phys_ext_val, len_and_kind);
	OFF(apfs_j_phys_ext_val, owning_obj_id);
	OFF(apfs_j_phys_ext_val, refcnt);

	OFF(apfs_j_sibling_key, sibling_id);
	OFF(apfs_j_sibling_val, parent_id);
	OFF(apfs_j_sibling_val, name_len);
	OFF(apfs_j_sibling_val, name);

	SIZE(apfs_xf_blob);
	OFF(apfs_xf_blob, xf_num_exts);
	OFF(apfs_xf_blob, xf_used_data);
	OFFA(apfs_xf_blob, xf_data);
	SIZE(apfs_x_field);
	OFF(apfs_x_field, x_type);
	OFF(apfs_x_field, x_flags);
	OFF(apfs_x_field, x_size);

	OFF(apfs_decmpfs_disk_header, compression_magic);
	OFF(apfs_decmpfs_disk_header, compression_type);
	OFF(apfs_decmpfs_disk_header, uncompressed_size);
	OFF(apfs_decmpfs_disk_header, attr_bytes);

	return 0;
}
