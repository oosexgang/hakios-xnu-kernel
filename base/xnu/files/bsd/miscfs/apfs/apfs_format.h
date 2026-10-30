/*
 * APFS on-disk layout, read side.
 *
 * Written from Apple's "Apple File System Reference" (2020-06-22), which is the only
 * description of the format there is.  Names and field order follow that document, so
 * that a reader can hold the two side by side; what the document calls out but we never
 * read on the way to a file's bytes (the space manager, the reaper, fusion, encryption
 * rolling, snapshots) is left out rather than copied in unused.
 *
 * Everything here is little-endian on disk and is read straight out of a block, so the
 * structures must match the disk byte for byte.  tools/apfs/fmt_check.c prints every
 * size and field offset in this file and tools/apfs/apfs.py checks its own idea of the
 * layout against that output -- run tools/apfs/run-tests.sh after touching this file.
 *
 * Decompression of file contents (the com.apple.decmpfs attribute) is not part of APFS
 * and is not in the reference; it is at the end of this file, from bsd/sys/decmpfs.h and
 * from what the iOS 16.5 root file system actually contains.
 */

#ifndef _MISCFS_APFS_APFS_FORMAT_H_
#define _MISCFS_APFS_APFS_FORMAT_H_

#include <stdint.h>

typedef int64_t  apfs_paddr_t;   /* a block address within the container */
typedef uint64_t apfs_oid_t;     /* an object identifier */
typedef uint64_t apfs_xid_t;     /* a transaction identifier */

/* A run of blocks: pr_block_count blocks starting at pr_start_paddr. */
struct apfs_prange {
	apfs_paddr_t pr_start_paddr;
	uint64_t     pr_block_count;
};

/*
 * Objects
 *
 * Every object that occupies whole blocks starts with this header.  o_cksum is
 * Fletcher-64 over the rest of the block and is the only integrity check we have, so we
 * verify it on every block we read.
 */
#define APFS_MAX_CKSUM_SIZE 8

struct apfs_obj_phys {
	uint8_t    o_cksum[APFS_MAX_CKSUM_SIZE];
	apfs_oid_t o_oid;
	apfs_xid_t o_xid;
	uint32_t   o_type;      /* storage class and flags in the high half, type in the low */
	uint32_t   o_subtype;
};

/* o_type: the low half is the type, the high half says how the object is stored */
#define APFS_OBJECT_TYPE_MASK           0x0000ffff
#define APFS_OBJECT_TYPE_FLAGS_MASK     0xffff0000
#define APFS_OBJ_STORAGETYPE_MASK       0xc0000000
#define APFS_OBJ_VIRTUAL                0x00000000  /* found through an object map */
#define APFS_OBJ_EPHEMERAL              0x80000000  /* found through a checkpoint map */
#define APFS_OBJ_PHYSICAL               0x40000000  /* the identifier is the block address */
#define APFS_OBJ_NOHEADER               0x20000000  /* no apfs_obj_phys at the front */
#define APFS_OBJ_ENCRYPTED              0x10000000
#define APFS_OBJ_NONPERSISTENT          0x08000000

#define APFS_OBJECT_TYPE_NX_SUPERBLOCK  0x00000001
#define APFS_OBJECT_TYPE_BTREE          0x00000002  /* a tree's root node */
#define APFS_OBJECT_TYPE_BTREE_NODE     0x00000003  /* any other node */
#define APFS_OBJECT_TYPE_SPACEMAN       0x00000005
#define APFS_OBJECT_TYPE_SPACEMAN_BITMAP 0x00000008
#define APFS_OBJECT_TYPE_OMAP           0x0000000b
#define APFS_OBJECT_TYPE_CHECKPOINT_MAP 0x0000000c
#define APFS_OBJECT_TYPE_FS             0x0000000d  /* a volume superblock */
#define APFS_OBJECT_TYPE_FSTREE         0x0000000e  /* subtype: the file-system tree */
#define APFS_OBJECT_TYPE_BLOCKREFTREE   0x0000000f  /* subtype: the extent reference tree */
#define APFS_OBJECT_TYPE_SNAPMETATREE   0x00000010
#define APFS_OBJECT_TYPE_NX_REAPER      0x00000011
#define APFS_OBJECT_TYPE_EFI_JUMPSTART  0x00000014
#define APFS_OBJECT_TYPE_INTEGRITY_META 0x0000001e
#define APFS_OBJECT_TYPE_FEXT_TREE      0x0000001f
#define APFS_OBJECT_TYPE_INVALID        0x00000000

#define APFS_OID_INVALID        0ULL
#define APFS_OID_NX_SUPERBLOCK  1ULL
#define APFS_OID_RESERVED_COUNT 1024

/*
 * Container
 *
 * Block zero holds a copy of the container superblock, but it can be stale.  The live one
 * is the superblock with the largest transaction identifier in the checkpoint descriptor
 * area (nx_xp_desc_base, nx_xp_desc_blocks blocks long); that copy also names the
 * checkpoint's extent of descriptor blocks, nx_xp_desc_index..+nx_xp_desc_len.
 */
#define APFS_NX_MAGIC            0x4253584e  /* 'NXSB', little-endian on disk */
#define APFS_NX_MAX_FILE_SYSTEMS 100
#define APFS_NX_EPH_INFO_COUNT   4
#define APFS_NX_NUM_COUNTERS     32
#define APFS_NX_DEFAULT_BLOCK_SIZE 4096
#define APFS_NX_MINIMUM_BLOCK_SIZE 4096
#define APFS_NX_MAXIMUM_BLOCK_SIZE 65536

struct apfs_nx_superblock {
	struct apfs_obj_phys nx_o;
	uint32_t     nx_magic;
	uint32_t     nx_block_size;
	uint64_t     nx_block_count;

	uint64_t     nx_features;
	uint64_t     nx_readonly_compatible_features;
	uint64_t     nx_incompatible_features;

	uint8_t      nx_uuid[16];

	apfs_oid_t   nx_next_oid;
	apfs_xid_t   nx_next_xid;

	/* the checkpoint areas: a ring of descriptor blocks and a ring of data blocks */
	uint32_t     nx_xp_desc_blocks;
	uint32_t     nx_xp_data_blocks;
	apfs_paddr_t nx_xp_desc_base;
	apfs_paddr_t nx_xp_data_base;
	uint32_t     nx_xp_desc_next;
	uint32_t     nx_xp_data_next;
	uint32_t     nx_xp_desc_index;
	uint32_t     nx_xp_desc_len;
	uint32_t     nx_xp_data_index;
	uint32_t     nx_xp_data_len;

	apfs_oid_t   nx_spaceman_oid;
	apfs_oid_t   nx_omap_oid;      /* the container's object map, a physical object */
	apfs_oid_t   nx_reaper_oid;

	uint32_t     nx_test_type;

	uint32_t     nx_max_file_systems;
	apfs_oid_t   nx_fs_oid[APFS_NX_MAX_FILE_SYSTEMS];  /* volume superblocks, virtual */
	uint64_t     nx_counters[APFS_NX_NUM_COUNTERS];
	struct apfs_prange nx_blocked_out_prange;
	apfs_oid_t   nx_evict_mapping_tree_oid;
	uint64_t     nx_flags;
	apfs_paddr_t nx_efi_jumpstart;
	uint8_t      nx_fusion_uuid[16];
	struct apfs_prange nx_keylocker;
	uint64_t     nx_ephemeral_info[APFS_NX_EPH_INFO_COUNT];

	apfs_oid_t   nx_test_oid;

	apfs_oid_t   nx_fusion_mt_oid;
	apfs_oid_t   nx_fusion_wbc_oid;
	struct apfs_prange nx_fusion_wbc;

	uint64_t     nx_newest_mounted_version;

	struct apfs_prange nx_mkb_locker;
};

/* nx_incompatible_features: a container we would misread if we ignored these */
#define APFS_NX_INCOMPAT_VERSION1 0x0000000000000001ULL
#define APFS_NX_INCOMPAT_VERSION2 0x0000000000000002ULL
#define APFS_NX_INCOMPAT_FUSION   0x0000000000000100ULL

/*
 * Checkpoint maps
 *
 * Ephemeral objects live in the checkpoint data area rather than at a fixed place, so a
 * checkpoint's descriptor blocks carry a map from object identifier to block address.
 * We need this only for the space manager, so the read path mostly skips it.
 */
#define APFS_CHECKPOINT_MAP_LAST 0x00000001

struct apfs_checkpoint_mapping {
	uint32_t   cpm_type;
	uint32_t   cpm_subtype;
	uint32_t   cpm_size;
	uint32_t   cpm_pad;
	apfs_oid_t cpm_fs_oid;
	apfs_oid_t cpm_oid;
	apfs_oid_t cpm_paddr;
};

struct apfs_checkpoint_map_phys {
	struct apfs_obj_phys cpm_o;
	uint32_t             cpm_flags;
	uint32_t             cpm_count;
	struct apfs_checkpoint_mapping cpm_map[];
};

/*
 * Object maps
 *
 * A virtual object identifier means nothing by itself: it has to be looked up in an
 * object map, which is a B-tree keyed by (identifier, transaction) and whose values give
 * the block address.  The container's map resolves volume superblocks; each volume's map
 * resolves the nodes of that volume's file-system tree.
 */
struct apfs_omap_phys {
	struct apfs_obj_phys om_o;
	uint32_t   om_flags;
	uint32_t   om_snap_count;
	uint32_t   om_tree_type;
	uint32_t   om_snapshot_tree_type;
	apfs_oid_t om_tree_oid;            /* the map itself, a physical B-tree */
	apfs_oid_t om_snapshot_tree_oid;
	apfs_xid_t om_most_recent_snap;
	apfs_xid_t om_pending_revert_min;
	apfs_xid_t om_pending_revert_max;
};

struct apfs_omap_key {
	apfs_oid_t ok_oid;
	apfs_xid_t ok_xid;
};

struct apfs_omap_val {
	uint32_t     ov_flags;
	uint32_t     ov_size;
	apfs_paddr_t ov_paddr;
};

#define APFS_OMAP_VAL_DELETED           0x00000001
#define APFS_OMAP_VAL_SAVED             0x00000002
#define APFS_OMAP_VAL_ENCRYPTED         0x00000004
#define APFS_OMAP_VAL_NOHEADER          0x00000008
#define APFS_OMAP_VAL_CRYPTO_GENERATION 0x00000010

/*
 * Volumes
 */
#define APFS_MAGIC_VOL        0x42535041  /* 'APSB', little-endian on disk */
#define APFS_MAX_HIST         8
#define APFS_MODIFIED_NAMELEN 32
#define APFS_VOLNAME_LEN      256

struct apfs_modified_by {
	uint8_t    id[APFS_MODIFIED_NAMELEN];
	uint64_t   timestamp;
	apfs_xid_t last_xid;
};

/* The metadata encryption state; we only read it to check the volume is not encrypted. */
struct apfs_wrapped_meta_crypto_state {
	uint16_t major_version;
	uint16_t minor_version;
	uint32_t cpflags;
	uint32_t persistent_class;
	uint32_t key_os_version;
	uint16_t key_revision;
	uint16_t unused;
} __attribute__((aligned(2), packed));

struct apfs_superblock {
	struct apfs_obj_phys apfs_o;

	uint32_t   apfs_magic;
	uint32_t   apfs_fs_index;

	uint64_t   apfs_features;
	uint64_t   apfs_readonly_compatible_features;
	uint64_t   apfs_incompatible_features;

	uint64_t   apfs_unmount_time;

	uint64_t   apfs_fs_reserve_block_count;
	uint64_t   apfs_fs_quota_block_count;
	uint64_t   apfs_fs_alloc_count;

	struct apfs_wrapped_meta_crypto_state apfs_meta_crypto;

	uint32_t   apfs_root_tree_type;
	uint32_t   apfs_extentref_tree_type;
	uint32_t   apfs_snap_meta_tree_type;

	apfs_oid_t apfs_omap_oid;            /* this volume's object map, physical */
	apfs_oid_t apfs_root_tree_oid;       /* the file-system tree, virtual */
	apfs_oid_t apfs_extentref_tree_oid;
	apfs_oid_t apfs_snap_meta_tree_oid;

	apfs_xid_t apfs_revert_to_xid;
	apfs_oid_t apfs_revert_to_sblock_oid;

	uint64_t   apfs_next_obj_id;

	uint64_t   apfs_num_files;
	uint64_t   apfs_num_directories;
	uint64_t   apfs_num_symlinks;
	uint64_t   apfs_num_other_fsobjects;
	uint64_t   apfs_num_snapshots;

	uint64_t   apfs_total_blocks_alloced;
	uint64_t   apfs_total_blocks_freed;

	uint8_t    apfs_vol_uuid[16];
	uint64_t   apfs_last_mod_time;

	uint64_t   apfs_fs_flags;

	struct apfs_modified_by apfs_formatted_by;
	struct apfs_modified_by apfs_modified_by[APFS_MAX_HIST];

	uint8_t    apfs_volname[APFS_VOLNAME_LEN];
	uint32_t   apfs_next_doc_id;

	uint16_t   apfs_role;
	uint16_t   reserved;

	apfs_xid_t apfs_root_to_xid;
	apfs_oid_t apfs_er_state_oid;

	uint64_t   apfs_cloneinfo_id_epoch;
	uint64_t   apfs_cloneinfo_xid;

	apfs_oid_t apfs_snap_meta_ext_oid;

	uint8_t    apfs_volume_group_id[16];

	apfs_oid_t apfs_integrity_meta_oid;

	apfs_oid_t apfs_fext_tree_oid;
	uint32_t   apfs_fext_tree_type;

	uint32_t   reserved_type;
	apfs_oid_t reserved_oid;
};

/* apfs_incompatible_features: these change how we read names and extents */
#define APFS_INCOMPAT_CASE_INSENSITIVE          0x00000001ULL
#define APFS_INCOMPAT_DATALESS_SNAPS            0x00000002ULL
#define APFS_INCOMPAT_ENC_ROLLED                0x00000004ULL
#define APFS_INCOMPAT_NORMALIZATION_INSENSITIVE 0x00000008ULL
#define APFS_INCOMPAT_INCOMPLETE_RESTORE        0x00000010ULL
#define APFS_INCOMPAT_SEALED_VOLUME             0x00000020ULL

/* apfs_fs_flags */
#define APFS_FS_UNENCRYPTED 0x00000001ULL
#define APFS_FS_ONEKEY      0x00000008ULL
#define APFS_FS_SPILLEDOVER 0x00000010ULL

/* apfs_role */
#define APFS_VOL_ROLE_NONE      0x0000
#define APFS_VOL_ROLE_SYSTEM    0x0001
#define APFS_VOL_ROLE_USER      0x0002
#define APFS_VOL_ROLE_RECOVERY  0x0004
#define APFS_VOL_ROLE_VM        0x0008
#define APFS_VOL_ROLE_PREBOOT   0x0010
#define APFS_VOL_ROLE_INSTALLER 0x0020
#define APFS_VOLUME_ENUM_SHIFT  6
#define APFS_VOL_ROLE_DATA      (1 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_BASEBAND  (2 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_UPDATE    (3 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_XART      (4 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_HARDWARE  (5 << APFS_VOLUME_ENUM_SHIFT)
#define APFS_VOL_ROLE_BACKUP    (6 << APFS_VOLUME_ENUM_SHIFT)

/*
 * B-trees
 *
 * Every tree is one of these node blocks.  A node is laid out as: the header, then a
 * table of contents, then keys growing from the start of the key area forward, then free
 * space, then values growing from the end of the block backwards.  A root node keeps a
 * struct apfs_btree_info in the last bytes of the block, so the value area of a root node
 * stops short of it.
 *
 * Two layouts of the table of contents: with BTNODE_FIXED_KV_SIZE every key and value is
 * the same size and an entry is a struct apfs_kvoff (offsets only); otherwise an entry is
 * a struct apfs_kvloc (offset and length for each).
 *
 * Offsets in the table of contents are relative to the start of the key area for keys,
 * and backwards from the end of the value area for values.
 */
#define APFS_BTNODE_ROOT             0x0001
#define APFS_BTNODE_LEAF             0x0002
#define APFS_BTNODE_FIXED_KV_SIZE    0x0004
#define APFS_BTNODE_HASHED           0x0008
#define APFS_BTNODE_NOHEADER         0x0010
#define APFS_BTNODE_CHECK_KOFF_INVAL 0x8000

#define APFS_BTOFF_INVALID 0xffff

struct apfs_nloc {         /* a location inside a node: offset and length */
	uint16_t off;
	uint16_t len;
};

struct apfs_kvloc {        /* a table-of-contents entry, variable sizes */
	struct apfs_nloc k;
	struct apfs_nloc v;
};

struct apfs_kvoff {        /* a table-of-contents entry, fixed sizes */
	uint16_t k;
	uint16_t v;
};

struct apfs_btree_node_phys {
	struct apfs_obj_phys btn_o;
	uint16_t btn_flags;
	uint16_t btn_level;        /* 0 at the leaves */
	uint32_t btn_nkeys;
	struct apfs_nloc btn_table_space;   /* the table of contents, within btn_data */
	struct apfs_nloc btn_free_space;    /* the gap between keys and values */
	struct apfs_nloc btn_key_free_list;
	struct apfs_nloc btn_val_free_list;
	uint64_t btn_data[];
};

#define APFS_BTREE_UINT64_KEYS       0x00000001
#define APFS_BTREE_SEQUENTIAL_INSERT 0x00000002
#define APFS_BTREE_ALLOW_GHOSTS      0x00000004
#define APFS_BTREE_EPHEMERAL         0x00000008
#define APFS_BTREE_PHYSICAL          0x00000010  /* child identifiers are block addresses */
#define APFS_BTREE_NONPERSISTENT     0x00000020
#define APFS_BTREE_KV_NONALIGNED     0x00000040
#define APFS_BTREE_HASHED            0x00000080
#define APFS_BTREE_NOHEADER          0x00000100

#define APFS_BTREE_NODE_HASH_SIZE_MAX 64
#define APFS_BTREE_NODE_SIZE_DEFAULT  4096

struct apfs_btree_info_fixed {
	uint32_t bt_flags;
	uint32_t bt_node_size;
	uint32_t bt_key_size;      /* zero unless the keys are a fixed size */
	uint32_t bt_val_size;
};

struct apfs_btree_info {
	struct apfs_btree_info_fixed bt_fixed;
	uint32_t bt_longest_key;
	uint32_t bt_longest_val;
	uint64_t bt_key_count;
	uint64_t bt_node_count;
};

/* The value of a non-leaf entry: the child's identifier, and its hash on a sealed volume. */
struct apfs_btn_index_node_val {
	apfs_oid_t binv_child_oid;
	uint8_t    binv_child_hash[APFS_BTREE_NODE_HASH_SIZE_MAX];
};

/*
 * File-system objects
 *
 * A volume's file-system tree holds every kind of record in one tree, keyed by the
 * identifier of the object the record belongs to and the kind of record.  Records sort by
 * identifier first and kind second, so all the records of one file are together and the
 * kinds within a file are in the order of the APFS_TYPE_* numbers.
 */
struct apfs_j_key {
	uint64_t obj_id_and_type;   /* kind in the top four bits, identifier in the rest */
} __attribute__((packed));

#define APFS_OBJ_ID_MASK    0x0fffffffffffffffULL
#define APFS_OBJ_TYPE_MASK  0xf000000000000000ULL
#define APFS_OBJ_TYPE_SHIFT 60

#define APFS_TYPE_ANY           0
#define APFS_TYPE_SNAP_METADATA 1
#define APFS_TYPE_EXTENT        2
#define APFS_TYPE_INODE         3
#define APFS_TYPE_XATTR         4
#define APFS_TYPE_SIBLING_LINK  5
#define APFS_TYPE_DSTREAM_ID    6
#define APFS_TYPE_CRYPTO_STATE  7
#define APFS_TYPE_FILE_EXTENT   8
#define APFS_TYPE_DIR_REC       9
#define APFS_TYPE_DIR_STATS     10
#define APFS_TYPE_SNAP_NAME     11
#define APFS_TYPE_SIBLING_MAP   12
#define APFS_TYPE_FILE_INFO     13
#define APFS_TYPE_MAX_VALID     13
#define APFS_TYPE_INVALID       15

#define APFS_INVALID_INO_NUM   0
#define APFS_ROOT_DIR_PARENT   1
#define APFS_ROOT_DIR_INO_NUM  2
#define APFS_PRIV_DIR_INO_NUM  3
#define APFS_SNAP_DIR_INO_NUM  6
#define APFS_PURGEABLE_DIR_INO_NUM 7
#define APFS_MIN_USER_INO_NUM  16

/* inode */
struct apfs_j_inode_key {
	struct apfs_j_key hdr;
} __attribute__((packed));

struct apfs_j_inode_val {
	uint64_t parent_id;
	uint64_t private_id;        /* the data stream's identifier, for a regular file */

	uint64_t create_time;       /* nanoseconds since the Unix epoch */
	uint64_t mod_time;
	uint64_t change_time;
	uint64_t access_time;

	uint64_t internal_flags;

	union {
		int32_t nchildren;  /* a directory */
		int32_t nlink;      /* anything else */
	};

	uint32_t default_protection_class;
	uint32_t write_generation_counter;
	uint32_t bsd_flags;
	uint32_t owner;
	uint32_t group;
	uint16_t mode;
	uint16_t pad1;
	uint64_t uncompressed_size; /* only with INODE_HAS_UNCOMPRESSED_SIZE */
	uint8_t  xfields[];         /* a struct apfs_xf_blob, if there is anything in it */
} __attribute__((packed));

#define APFS_INODE_IS_APFS_PRIVATE       0x00000001
#define APFS_INODE_MAINTAIN_DIR_STATS    0x00000002
#define APFS_INODE_DIR_STATS_ORIGIN      0x00000004
#define APFS_INODE_PROT_CLASS_EXPLICIT   0x00000008
#define APFS_INODE_WAS_CLONED            0x00000010
#define APFS_INODE_HAS_SECURITY_EA       0x00000040
#define APFS_INODE_BEING_TRUNCATED       0x00000080
#define APFS_INODE_HAS_FINDER_INFO       0x00000100
#define APFS_INODE_IS_SPARSE             0x00000200
#define APFS_INODE_WAS_EVER_CLONED       0x00000400
#define APFS_INODE_ACTIVE_FILE_TRIMMED   0x00000800
#define APFS_INODE_PINNED_TO_MAIN        0x00001000
#define APFS_INODE_PINNED_TO_TIER2       0x00002000
#define APFS_INODE_HAS_RSRC_FORK         0x00004000
#define APFS_INODE_NO_RSRC_FORK          0x00008000
#define APFS_INODE_ALLOCATION_SPILLEDOVER 0x00010000
#define APFS_INODE_FAST_PROMOTE          0x00020000
#define APFS_INODE_HAS_UNCOMPRESSED_SIZE 0x00040000
#define APFS_INODE_IS_PURGEABLE          0x00080000
#define APFS_INODE_WANTS_TO_BE_PURGEABLE 0x00100000
#define APFS_INODE_IS_SYNC_ROOT          0x00200000
#define APFS_INODE_SNAPSHOT_COW_EXEMPTION 0x00400000

/* directory entry.  iOS uses the hashed key; the hash is of the name, case-folded. */
struct apfs_j_drec_key {
	struct apfs_j_key hdr;
	uint16_t name_len;          /* including the trailing zero byte */
	uint8_t  name[0];
} __attribute__((packed));

struct apfs_j_drec_hashed_key {
	struct apfs_j_key hdr;
	uint32_t name_len_and_hash;
	uint8_t  name[0];
} __attribute__((packed));

#define APFS_J_DREC_LEN_MASK   0x000003ff
#define APFS_J_DREC_HASH_MASK  0xfffff400
#define APFS_J_DREC_HASH_SHIFT 10

struct apfs_j_drec_val {
	uint64_t file_id;           /* the identifier of the inode this entry names */
	uint64_t date_added;
	uint16_t flags;             /* the file kind, DT_*, in the low four bits */
	uint8_t  xfields[];
} __attribute__((packed));

#define APFS_DREC_TYPE_MASK 0x000f

/* d_type values, as in a directory entry */
#define APFS_DT_UNKNOWN 0
#define APFS_DT_FIFO    1
#define APFS_DT_CHR     2
#define APFS_DT_DIR     4
#define APFS_DT_BLK     6
#define APFS_DT_REG     8
#define APFS_DT_LNK     10
#define APFS_DT_SOCK    12
#define APFS_DT_WHT     14

/* directory statistics, on directories that keep them */
struct apfs_j_dir_stats_val {
	uint64_t num_children;
	uint64_t total_size;
	uint64_t chained_key;
	uint64_t gen_count;
} __attribute__((packed));

/* extended attribute.  A symbolic link's target is the com.apple.fs.symlink attribute. */
struct apfs_j_xattr_key {
	struct apfs_j_key hdr;
	uint16_t name_len;          /* including the trailing zero byte */
	uint8_t  name[0];
} __attribute__((packed));

struct apfs_j_xattr_val {
	uint16_t flags;
	uint16_t xdata_len;
	uint8_t  xdata[0];          /* the value, or a struct apfs_j_xattr_dstream */
} __attribute__((packed));

#define APFS_XATTR_DATA_STREAM      0x00000001  /* xdata is an apfs_j_xattr_dstream */
#define APFS_XATTR_DATA_EMBEDDED    0x00000002  /* xdata is the value itself */
#define APFS_XATTR_FILE_SYSTEM_OWNED 0x00000004
#define APFS_XATTR_MAX_EMBEDDED_SIZE 3804

#define APFS_SYMLINK_EA_NAME  "com.apple.fs.symlink"
#define APFS_FIRMLINK_EA_NAME "com.apple.fs.firmlink"

/* data stream: the size of a file's contents, and what has been written */
struct apfs_j_dstream {
	uint64_t size;              /* the logical size, what stat reports */
	uint64_t alloced_size;      /* rounded up to whole blocks */
	uint64_t default_crypto_id;
	uint64_t total_bytes_written;
	uint64_t total_bytes_read;
} __attribute__((aligned(8), packed));

struct apfs_j_xattr_dstream {
	uint64_t xattr_obj_id;
	struct apfs_j_dstream dstream;
};

struct apfs_j_dstream_id_val {
	uint32_t refcnt;
} __attribute__((packed));

/*
 * file extent: one run of blocks of a file's contents.
 *
 * The key's logical_addr is the byte offset within the file, always a whole number of
 * blocks.  A file can have gaps between its extents -- that is a sparse file, and the gap
 * reads as zeros.  phys_block_num zero means a hole as well.
 */
struct apfs_j_file_extent_key {
	struct apfs_j_key hdr;
	uint64_t logical_addr;
} __attribute__((packed));

struct apfs_j_file_extent_val {
	uint64_t len_and_flags;     /* length in bytes in the low 56 bits */
	uint64_t phys_block_num;
	uint64_t crypto_id;
} __attribute__((packed));

#define APFS_J_FILE_EXTENT_LEN_MASK   0x00ffffffffffffffULL
#define APFS_J_FILE_EXTENT_FLAG_MASK  0xff00000000000000ULL
#define APFS_J_FILE_EXTENT_FLAG_SHIFT 56

/* physical extent, from the extent reference tree */
struct apfs_j_phys_ext_val {
	uint64_t len_and_kind;      /* block count in the low 60 bits, kind in the top four */
	uint64_t owning_obj_id;
	int32_t  refcnt;
} __attribute__((packed));

#define APFS_PEXT_LEN_MASK   0x0fffffffffffffffULL
#define APFS_PEXT_KIND_MASK  0xf000000000000000ULL
#define APFS_PEXT_KIND_SHIFT 60

/* hard links: the names an inode is known by */
struct apfs_j_sibling_key {
	struct apfs_j_key hdr;
	uint64_t sibling_id;
} __attribute__((packed));

struct apfs_j_sibling_val {
	uint64_t parent_id;
	uint16_t name_len;
	uint8_t  name[0];
} __attribute__((packed));

struct apfs_j_sibling_map_val {
	uint64_t file_id;
} __attribute__((packed));

/*
 * Extended fields
 *
 * The variable part at the end of an inode or a directory entry: a blob header, then that
 * many struct apfs_x_field describing the pieces, then the pieces themselves, each padded
 * out to a multiple of eight bytes.
 */
struct apfs_xf_blob {
	uint16_t xf_num_exts;
	uint16_t xf_used_data;
	uint8_t  xf_data[];
};

struct apfs_x_field {
	uint8_t  x_type;
	uint8_t  x_flags;
	uint16_t x_size;
};

#define APFS_INO_EXT_TYPE_SNAP_XID           1
#define APFS_INO_EXT_TYPE_DELTA_TREE_OID     2
#define APFS_INO_EXT_TYPE_DOCUMENT_ID        3
#define APFS_INO_EXT_TYPE_NAME               4   /* the name of a hard-linked inode */
#define APFS_INO_EXT_TYPE_PREV_FSIZE         5
#define APFS_INO_EXT_TYPE_FINDER_INFO        7
#define APFS_INO_EXT_TYPE_DSTREAM            8   /* a struct apfs_j_dstream */
#define APFS_INO_EXT_TYPE_DIR_STATS_KEY      10
#define APFS_INO_EXT_TYPE_FS_UUID            11
#define APFS_INO_EXT_TYPE_SPARSE_BYTES       13
#define APFS_INO_EXT_TYPE_RDEV               14  /* a device number */
#define APFS_INO_EXT_TYPE_PURGEABLE_FLAGS    15
#define APFS_INO_EXT_TYPE_ORIG_SYNC_ROOT_ID  16

#define APFS_DREC_EXT_TYPE_SIBLING_ID 1

#define APFS_XF_DATA_DEPENDENT    0x0001
#define APFS_XF_DO_NOT_COPY       0x0002
#define APFS_XF_CHILDREN_INHERIT  0x0008

/* file mode bits, as APFS stores them */
#define APFS_S_IFMT   0170000
#define APFS_S_IFIFO  0010000
#define APFS_S_IFCHR  0020000
#define APFS_S_IFDIR  0040000
#define APFS_S_IFBLK  0060000
#define APFS_S_IFREG  0100000
#define APFS_S_IFLNK  0120000
#define APFS_S_IFSOCK 0140000
#define APFS_S_IFWHT  0160000

/*
 * Compressed file contents
 *
 * Not part of APFS and not in Apple's reference.  A compressed file carries the attribute
 * com.apple.decmpfs, whose value starts with this header; where the compressed bytes
 * themselves are depends on the type.  The header is little-endian.
 *
 * XNU (bsd/sys/decmpfs.h) names only type 1; the rest live in Apple's closed
 * AppleFSCompression.  The types below are the ones the iOS 16.5 root file system
 * actually uses -- see tools/apfs/apfs.py, which counts them over the whole volume.
 *
 * In the attribute (types 3 and 4 the common pair): the bytes after the header are a
 * single zlib stream, except that a first byte of 0xff means the rest is the uncompressed
 * contents as they are.  In the resource fork (the com.apple.ResourceFork attribute): the
 * contents are cut into 64 KB pieces, each its own zlib stream, with a table of their
 * offsets at the front.
 */
#define APFS_DECMPFS_XATTR_NAME  "com.apple.decmpfs"
#define APFS_DECMPFS_MAGIC       0x636d7066   /* 'cmpf' */
#define APFS_RSRC_FORK_XATTR_NAME "com.apple.ResourceFork"

struct apfs_decmpfs_disk_header {
	uint32_t compression_magic;
	uint32_t compression_type;
	uint64_t uncompressed_size;
	uint8_t  attr_bytes[0];
} __attribute__((packed));

#define APFS_DECMPFS_TYPE_ZLIB_ATTR  3   /* zlib, bytes in the attribute */
#define APFS_DECMPFS_TYPE_ZLIB_RSRC  4   /* zlib, bytes in the resource fork */
#define APFS_DECMPFS_TYPE_RAW_ATTR   9   /* uncompressed, bytes in the attribute */
#define APFS_DECMPFS_TYPE_RAW_RSRC   10  /* uncompressed, bytes in the resource fork */
#define APFS_DECMPFS_TYPE_LZFSE_ATTR 11
#define APFS_DECMPFS_TYPE_LZFSE_RSRC 12
#define APFS_DECMPFS_TYPE_LZBITMAP_ATTR 13  /* LZBITMAP, bytes in the attribute */
#define APFS_DECMPFS_TYPE_LZBITMAP_RSRC 14  /* LZBITMAP, 64 KB pieces in the resource fork behind
                                               a table of n+1 little-endian offsets */

/* the pieces a resource-fork-compressed file is cut into */
#define APFS_DECMPFS_BLOCK_SIZE 65536

#endif /* _MISCFS_APFS_APFS_FORMAT_H_ */
