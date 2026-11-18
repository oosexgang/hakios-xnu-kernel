/*
 * ramfs: a read-write filesystem that lives entirely in kernel memory.
 *
 * The open-source kernel has no writable filesystem of its own, and the
 * bring-up system has no storage driver, so /private/var and /tmp are kept
 * here. Everything is lost at shutdown.
 *
 * Nodes (files, directories, symlinks, socket names, named pipes) are plain kernel
 * structures; a directory is a list of (name, node) entries, so hard links
 * are just two entries naming one node. File contents are kept in
 * individually allocated pages hanging off the node. That copy is the truth:
 * read() and write() go straight to it, and the page cache is used only for
 * mmap, with page-in and page-out copying between the two. After a write(),
 * the pages of the page cache that hold what was written are brought up to
 * date in place (ramfs_refresh_cache()).
 *
 * One mutex per mount guards all of it. It is never held while user memory
 * is touched (a fault there could come back to this filesystem as a
 * page-in): data moves through a bounce buffer instead.
 *
 * A node lives while it is named by a directory or has a vnode; after the
 * last unlink it is freed when its vnode is reclaimed.
 */

#include <kern/assert.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <sys/attr.h>
#include <sys/content_protection.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode_internal.h>
#include <sys/xattr.h>
#include <uuid/uuid.h>
#include <vfs/vfs_support.h>
#include <miscfs/fifofs/fifo.h>          /* (after sys/vnode_internal.h: it needs vfs_context_t) */
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

#define RAMFS_MAX_NAME          255
#define RAMFS_MAX_XATTR         (16u * 1024 * 1024)  /* of a node's extended attributes together, names and values */
#define RAMFS_ROOT_INO          2
#define RAMFS_MAX_FILE          (1ull << 40)    /* 1 TB: a table of its pages is 2 GB, past any room there is */

struct ramfs_mount;
struct ramfs_node;

struct ramfs_dirent {
	TAILQ_ENTRY(ramfs_dirent) rd_link;
	struct ramfs_node *rd_node;
	char              *rd_name;             /* rd_namelen bytes and a terminator */
	uint32_t           rd_namelen;
	uint64_t           rd_cookie;           /* where readdir finds it: the directory's count of entries made */
};

/* An extended attribute: Darwin keeps container metadata, ACLs and much else in them. */
struct ramfs_xattr {
	TAILQ_ENTRY(ramfs_xattr) rx_link;
	char   *rx_name;                        /* rx_namelen bytes and a terminator */
	size_t  rx_namelen;
	void   *rx_data;
	size_t  rx_size;
};

struct ramfs_node {
	struct ramfs_mount *rn_mount;
	uint64_t            rn_ino;
	enum vtype          rn_type;
	uint32_t            rn_mode;            /* permission bits only */
	uid_t               rn_uid;
	gid_t               rn_gid;
	uint32_t            rn_bsdflags;
	uint32_t            rn_dpclass;         /* data protection class: a label only, nothing is encrypted */
	uint32_t            rn_nlink;
	uint32_t            rn_state;
	vnode_t             rn_vp;
	uint32_t            rn_vid;
	struct timespec     rn_atime, rn_mtime, rn_ctime, rn_btime;
	uint64_t            rn_size;            /* file: bytes; symlink: target length */

	/*
	 * Held across a write() and a change of size, which drop the mount lock while they copy and allocate:
	 * without it two appenders could both take the same end of the file, and a truncate could land in the
	 * middle of a write. Page-in and page-out never take it (a write's own copy from user memory can fault
	 * on a mapping of this very file); it is taken before the mount lock, never under it.
	 */
	lck_rw_t            rn_io_lock;

	/* directory */
	struct ramfs_node  *rn_parent;          /* NULL once the directory has been removed */
	TAILQ_HEAD(, ramfs_dirent) rn_entries;  /* in the order they were made: their cookies rising */
	uint32_t            rn_nentries;
	uint64_t            rn_cookies;         /* entries ever made in it; the next one's cookie is this and 2 */

	/* regular file */
	void              **rn_pages;           /* rn_nslots slots; NULL is a hole that reads as zeros */
	uint64_t            rn_nslots;
	uint64_t            rn_nalloc;          /* pages actually allocated */
	bool                rn_mapped_write;    /* has been mapped writable: the page cache may be ahead of us */

	/* symlink */
	char               *rn_target;

	struct ramfs_node  *rn_hnext;           /* the next node in rm_hash's chain */
	bool                rn_hashed;

	TAILQ_HEAD(, ramfs_xattr) rn_xattrs;
	size_t              rn_xattr_bytes;     /* their names and values: charged to the mount's pages */
};
#define RAMFS_N_CREATING        0x1         /* a thread is in vnode_create() for this node */
#define RAMFS_N_WANTED          0x2         /* somebody sleeps on the node */

struct ramfs_mount {
	mount_t             rm_mp;
	lck_mtx_t           rm_lock;
	struct ramfs_node  *rm_root;
	uint64_t            rm_next_ino;
	uint64_t            rm_nodes;
	uint64_t            rm_pages;           /* file pages in use, extended attributes' bytes in pages, rm_slot_bytes in pages */
	uint64_t            rm_slot_bytes;      /* the files' tables of their pages, together */
	uint64_t            rm_max_pages;
	uint64_t            rm_alloc_failures;
	struct ramfs_node **rm_hash;            /* every node by its number, for vfs_vget */
};

#define RAMFS_HASH_SIZE         4096u       /* chains, a power of two */

/* file pages in use on all the mounts together (each mount has its own lock): for the kernel's memory report */
_Atomic uint64_t ramfs_pages_in_use;
extern unsigned int vm_page_free_count;      /* osfmk/vm/vm_resident.c, for a refusal's line */

#define RAMFS_MP(mp)    ((struct ramfs_mount *)vfs_fsprivate(mp))
#define RAMFS_NODE(vp)  ((struct ramfs_node *)vnode_fsnode(vp))

static LCK_GRP_DECLARE(ramfs_lck_grp, "ramfs");

extern int(**ramfs_vnodeop_p)(void *);
extern int(**ramfs_fifoop_p)(void *);

/* ------------------------------------------------------------------------ */
/* Nodes                                                                    */

static struct ramfs_node *
ramfs_node_alloc(struct ramfs_mount *rm, enum vtype type, struct vnode_attr *vap)
{
	struct ramfs_node *node = kalloc_type(struct ramfs_node, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	struct timespec now;

	nanotime(&now);
	node->rn_mount = rm;
	node->rn_type = type;
	node->rn_mode = type == VLNK ? 0755 : (type == VDIR ? 0755 : 0644);
	/*
	 * hakios: a protection class for every file, D (none) unless one is asked for. Darwin 26 checks the
	 * class of what it stores (PosterBoard's data store must be NSFileProtectionNone throughout, and
	 * migrates it there when it is not); without one the check fails, setting it fails, and the
	 * wallpaper store is never made. The class is kept and told, nothing is encrypted.
	 */
	node->rn_dpclass = PROTECTION_CLASS_D;
	node->rn_atime = node->rn_mtime = node->rn_ctime = node->rn_btime = now;
	TAILQ_INIT(&node->rn_entries);
	TAILQ_INIT(&node->rn_xattrs);
	lck_rw_init(&node->rn_io_lock, &ramfs_lck_grp, LCK_ATTR_NULL);

	if (vap != NULL) {
		if (VATTR_IS_ACTIVE(vap, va_mode)) {
			node->rn_mode = vap->va_mode & ALLPERMS;
			VATTR_SET_SUPPORTED(vap, va_mode);
		}
		if (VATTR_IS_ACTIVE(vap, va_uid)) {
			node->rn_uid = vap->va_uid;
			VATTR_SET_SUPPORTED(vap, va_uid);
		}
		if (VATTR_IS_ACTIVE(vap, va_gid)) {
			node->rn_gid = vap->va_gid;
			VATTR_SET_SUPPORTED(vap, va_gid);
		}
		if (VATTR_IS_ACTIVE(vap, va_flags)) {
			node->rn_bsdflags = vap->va_flags;
			VATTR_SET_SUPPORTED(vap, va_flags);
		}
		if (VATTR_IS_ACTIVE(vap, va_create_time)) {
			node->rn_btime = vap->va_create_time;
			VATTR_SET_SUPPORTED(vap, va_create_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
			node->rn_mtime = vap->va_modify_time;
			VATTR_SET_SUPPORTED(vap, va_modify_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_access_time)) {
			node->rn_atime = vap->va_access_time;
			VATTR_SET_SUPPORTED(vap, va_access_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_change_time)) {
			node->rn_ctime = vap->va_change_time;
			VATTR_SET_SUPPORTED(vap, va_change_time);
		}
		if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
			node->rn_dpclass = vap->va_dataprotect_class;
			VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
		}
		VATTR_SET_SUPPORTED(vap, va_type);
	}
	return node;
}

/*
 * A page of a file's contents, and giving it back. Not from a kalloc zone, as it once was: all the
 * kernel's zones share one ceiling, and a few hundred MB of files in data.kalloc.16384 met it --
 * first writes failing with ENOSPC ("there isn't enough space" while most of this filesystem's own
 * allowance was free: dpkg's database could not be written), then, on an allocation that may not
 * fail, the kernel panicked with the zone map exhausted. Straight from the VM instead, a wired page
 * of kernel_map each, as the contents of a RAM disk are. wait=false: never wait for memory (the
 * page-out path, which may be the pageout daemon looking for some).
 */
/* the kernel's address ranges, for the line said when one of them is full (osfmk/vm/vm_kern.c) */
extern struct mach_vm_range *kmem_range(vm_map_range_id_t range_id);

static void
ramfs_say_ranges(void)
{
	static const char *const names[] = { "ptr0", "ptr1", "ptr2", "io", "data", "data-shared" };

	for (unsigned id = KMEM_RANGE_ID_FIRST; id <= KMEM_RANGE_ID_MAX; id++) {
		struct mach_vm_range *r = kmem_range((vm_map_range_id_t)id);

		printf("ramfs: kernel range %s: %#llx-%#llx (%llu MB)\n", names[id - KMEM_RANGE_ID_FIRST],
		    (unsigned long long)r->min_address, (unsigned long long)r->max_address,
		    (unsigned long long)((r->max_address - r->min_address) >> 20));
	}
}

/*
 * A page of file data, mapped in one of the kernel's two data ranges, which are as large and hold bags of
 * bytes; kmem_free() finds either by its address. Neither may be filled by files alone:
 *
 *   - kept in the private range (KMA_DATA) only, a Darwin session with a 4 GB system image filled it
 *     (KERN_NO_SPACE with free pages to spare: the addresses, not the memory, ran out), and the
 *     code-signature blobs that also live there could no longer be allocated -- launchd then stops the
 *     system on "unexpected exec of non-platform binary";
 *   - kept in the shared range (KMA_DATA_SHARED) first, a Darwin 27 session under QEMU filled that one
 *     (15 November: 927 MB of files), and what the kernel shares with other domains through it -- the
 *     data of Mach messages among them -- could not be allocated: SpringBoard and a dozen daemons died
 *     within a second ("Too many corpses being created") and the home screen went black.
 *
 * So the shared range takes files up to half of it, the private one after that, and the shared one
 * again, up to three quarters, only when the private one has no room either.
 */
static _Atomic uint64_t ramfs_shared_pages;

static bool
ramfs_in_shared(vm_offset_t addr)
{
	struct mach_vm_range *r = kmem_range(KMEM_RANGE_ID_DATA_SHARED);

	return addr >= r->min_address && addr < r->max_address;
}

static uint64_t
ramfs_shared_cap(unsigned quarters)
{
	struct mach_vm_range *r = kmem_range(KMEM_RANGE_ID_DATA_SHARED);

	return ((r->max_address - r->min_address) / PAGE_SIZE) * quarters / 4;
}

static void *
ramfs_page_alloc(bool wait)
{
	vm_offset_t addr = 0;
	kma_flags_t flags = KMA_KOBJECT | KMA_ZERO | (wait ? KMA_NONE : KMA_NOPAGEWAIT);
	kern_return_t kr = KERN_NO_SPACE;
	static _Atomic uint32_t private, refusals;

	if (os_atomic_load(&ramfs_shared_pages, relaxed) < ramfs_shared_cap(2)) {
		kr = kmem_alloc(kernel_map, &addr, PAGE_SIZE, flags | KMA_DATA_SHARED, VM_KERN_MEMORY_FILE);
	}
	if (kr == KERN_NO_SPACE) {
		if (os_atomic_inc(&private, relaxed) == 1) {
			printf("ramfs: files have half of the kernel's shared data range: pages go to the private "
			    "data range\n");
			ramfs_say_ranges();
		}
		kr = kmem_alloc(kernel_map, &addr, PAGE_SIZE, flags | KMA_DATA, VM_KERN_MEMORY_FILE);
	}
	if (kr == KERN_NO_SPACE && os_atomic_load(&ramfs_shared_pages, relaxed) < ramfs_shared_cap(3)) {
		kr = kmem_alloc(kernel_map, &addr, PAGE_SIZE, flags | KMA_DATA_SHARED, VM_KERN_MEMORY_FILE);
	}
	if (kr != KERN_SUCCESS) {
		/* Which refusal it was: no page (the free list), or no room in the kernel's data ranges. */
		uint32_t n = os_atomic_inc(&refusals, relaxed);
		if ((n & (n - 1)) == 0) {
			printf("ramfs: kmem_alloc refused a page (%u total): kern_return %d, waiting %d, free pages %u\n",
			    n, kr, wait, vm_page_free_count);
		}
		return NULL;
	}
	if (ramfs_in_shared(addr)) {
		os_atomic_inc(&ramfs_shared_pages, relaxed);
	}
	return (void *)addr;
}

static void
ramfs_page_free(void *page)
{
	if (ramfs_in_shared((vm_offset_t)page)) {
		os_atomic_dec(&ramfs_shared_pages, relaxed);
	}
	kmem_free(kernel_map, (vm_offset_t)page, PAGE_SIZE);
}

/*
 * A file's table of its pages is made for its size, holes and all: 8 bytes for every page (16 KB here), so a truncate to
 * terabytes would take the kernel's memory in the table alone. The tables are charged to the mount's room,
 * all of them together rounded up to pages (one table of a small file would otherwise cost a whole page). The
 * mount lock is held.
 */
static uint64_t
ramfs_slot_pages(uint64_t bytes)
{
	return (bytes + PAGE_MASK) / PAGE_SIZE;
}

/* whether a table of `slots` may take the place of one of `had` */
static bool
ramfs_slots_fit(struct ramfs_mount *rm, uint64_t had, uint64_t slots)
{
	uint64_t now = ramfs_slot_pages(rm->rm_slot_bytes);
	uint64_t then = ramfs_slot_pages(rm->rm_slot_bytes - had * sizeof(void *) + slots * sizeof(void *));

	return then <= now || rm->rm_pages + (then - now) <= rm->rm_max_pages;
}

static void
ramfs_slots_charge(struct ramfs_mount *rm, uint64_t had, uint64_t slots)
{
	uint64_t now = ramfs_slot_pages(rm->rm_slot_bytes);

	rm->rm_slot_bytes = rm->rm_slot_bytes - had * sizeof(void *) + slots * sizeof(void *);
	rm->rm_pages = rm->rm_pages - now + ramfs_slot_pages(rm->rm_slot_bytes);
}

/*
 * Make the page array big enough for page `index`, before the mount lock is taken: under it the
 * array can only grow by an allocation that does not wait (ramfs_grow_slots), and one that failed
 * under memory pressure failed the write with ENOSPC. Nothing is promised: ramfs_store looks again.
 */
static void
ramfs_reserve_slots(struct ramfs_mount *rm, struct ramfs_node *node, uint64_t index)
{
	for (int tries = 0; tries < 4; tries++) {
		uint64_t have, want;
		void **slots;

		lck_mtx_lock(&rm->rm_lock);
		have = node->rn_nslots;
		lck_mtx_unlock(&rm->rm_lock);
		if (index < have) {
			return;
		}
		for (want = have ? have : 4; want <= index; want *= 2) {
		}
		slots = kalloc_type(void *, want, Z_WAITOK | Z_ZERO);
		if (slots == NULL) {
			return;
		}
		lck_mtx_lock(&rm->rm_lock);
		if (node->rn_nslots == have && !ramfs_slots_fit(rm, have, want)) {
			/* (no room for it: ramfs_store finds the same and says ENOSPC) */
			lck_mtx_unlock(&rm->rm_lock);
			kfree_type(void *, want, slots);
			return;
		}
		if (node->rn_nslots == have) {
			ramfs_slots_charge(rm, have, want);
			if (node->rn_pages != NULL) {
				memcpy(slots, node->rn_pages, have * sizeof(void *));
				kfree_type(void *, have, node->rn_pages);
			}
			node->rn_pages = slots;
			node->rn_nslots = want;
			slots = NULL;
		}
		lck_mtx_unlock(&rm->rm_lock);
		if (slots == NULL) {
			return;
		}
		kfree_type(void *, want, slots);        /* it grew meanwhile: look again */
	}
}

/* Drop file pages from slot `from` on. The mount lock is held. */
static void
ramfs_free_pages(struct ramfs_node *node, uint64_t from)
{
	for (uint64_t i = from; i < node->rn_nslots; i++) {
		if (node->rn_pages[i] != NULL) {
			ramfs_page_free(node->rn_pages[i]);
			node->rn_pages[i] = NULL;
			node->rn_nalloc--;
			node->rn_mount->rm_pages--;
			os_atomic_dec(&ramfs_pages_in_use, relaxed);
		}
	}
}

/*
 * The pages a node's extended attributes are charged as, against the mount's budget: they are kernel memory
 * as much as a file's pages are, and with nothing counted anybody could set attributes until it ran out.
 */
static uint64_t
ramfs_xattr_pages(size_t bytes)
{
	return ((uint64_t)bytes + PAGE_MASK) / PAGE_SIZE;
}

static void
ramfs_xattr_free(struct ramfs_xattr *xa)
{
	kfree_data(xa->rx_name, xa->rx_namelen + 1);
	if (xa->rx_data != NULL) {
		kfree_data(xa->rx_data, xa->rx_size);
	}
	kfree_type(struct ramfs_xattr, xa);
}

/*
 * Nodes by their number. What asks for a file by number rather than by name -- fsgetpath(), and through
 * it LaunchServices, which keeps an application as its volume and file number and finds the path again
 * from them -- comes to vfs_vget, which this filesystem did not have: an application in /private/var
 * (the package manager's prefix, /var/jb/Applications) could not be registered at all ("Unable to
 * register"), while one on an APFS volume could. The mount lock is held.
 */
static void
ramfs_hash_insert(struct ramfs_mount *rm, struct ramfs_node *node)
{
	struct ramfs_node **chain = &rm->rm_hash[node->rn_ino & (RAMFS_HASH_SIZE - 1)];

	node->rn_hnext = *chain;
	*chain = node;
	node->rn_hashed = true;
}

static void
ramfs_hash_remove(struct ramfs_mount *rm, struct ramfs_node *node)
{
	struct ramfs_node **link = &rm->rm_hash[node->rn_ino & (RAMFS_HASH_SIZE - 1)];

	while (*link != NULL && *link != node) {
		link = &(*link)->rn_hnext;
	}
	if (*link == node) {
		*link = node->rn_hnext;
	}
	node->rn_hashed = false;
}

/* The node is named by nothing and has no vnode. The mount lock is held. */
static void
ramfs_node_free(struct ramfs_node *node)
{
	struct ramfs_xattr *xa;

	if (node->rn_hashed) {
		ramfs_hash_remove(node->rn_mount, node);
	}

	assert(TAILQ_EMPTY(&node->rn_entries));
	while ((xa = TAILQ_FIRST(&node->rn_xattrs)) != NULL) {
		TAILQ_REMOVE(&node->rn_xattrs, xa, rx_link);
		ramfs_xattr_free(xa);
	}
	node->rn_mount->rm_pages -= ramfs_xattr_pages(node->rn_xattr_bytes);
	ramfs_free_pages(node, 0);
	if (node->rn_pages != NULL) {
		kfree_type(void *, node->rn_nslots, node->rn_pages);
		ramfs_slots_charge(node->rn_mount, node->rn_nslots, 0);
	}
	if (node->rn_target != NULL) {
		kfree_data(node->rn_target, node->rn_size + 1);
	}
	node->rn_mount->rm_nodes--;
	lck_rw_destroy(&node->rn_io_lock, &ramfs_lck_grp);
	kfree_type(struct ramfs_node, node);
}

static void
ramfs_touch(struct ramfs_node *node, bool modified)
{
	struct timespec now;

	nanotime(&now);
	node->rn_ctime = now;
	if (modified) {
		node->rn_mtime = now;
	}
}

static struct ramfs_dirent *
ramfs_dirent_alloc(const char *name, size_t len)
{
	struct ramfs_dirent *de = kalloc_type(struct ramfs_dirent, Z_WAITOK | Z_ZERO | Z_NOFAIL);

	de->rd_name = kalloc_data(len + 1, Z_WAITOK | Z_NOFAIL);
	memcpy(de->rd_name, name, len);
	de->rd_name[len] = '\0';
	de->rd_namelen = (uint32_t)len;
	return de;
}

static void
ramfs_dirent_free(struct ramfs_dirent *de)
{
	kfree_data(de->rd_name, de->rd_namelen + 1);
	kfree_type(struct ramfs_dirent, de);
}

static struct ramfs_dirent *
ramfs_dir_find(struct ramfs_node *dir, const char *name, size_t len)
{
	struct ramfs_dirent *de;

	TAILQ_FOREACH(de, &dir->rn_entries, rd_link) {
		if (de->rd_namelen == len && memcmp(de->rd_name, name, len) == 0) {
			return de;
		}
	}
	return NULL;
}

static void
ramfs_dir_add(struct ramfs_node *dir, struct ramfs_dirent *de, struct ramfs_node *node)
{
	de->rd_node = node;
	de->rd_cookie = 2 + dir->rn_cookies++;  /* (0 and 1 are . and ..) */
	TAILQ_INSERT_TAIL(&dir->rn_entries, de, rd_link);
	dir->rn_nentries++;
	ramfs_touch(dir, true);
}

static void
ramfs_dir_remove(struct ramfs_node *dir, struct ramfs_dirent *de)
{
	TAILQ_REMOVE(&dir->rn_entries, de, rd_link);
	dir->rn_nentries--;
	ramfs_touch(dir, true);
	ramfs_dirent_free(de);
}

/*
 * Get the vnode of a node that is known to exist (it was just found in a
 * directory, or is the root). Called with the mount lock held; returns with
 * it dropped. 0: *vpp carries an iocount. EAGAIN: the node could not be
 * pinned (its vnode is on the way out, or another thread is creating one)
 * and may be gone by now; the caller must find it again.
 */
static int
ramfs_vget_locked(struct ramfs_mount *rm, struct ramfs_node *node, vnode_t dvp, struct componentname *cnp, vnode_t *vpp)
{
	struct vnode_fsparam param;
	vnode_t vp;
	int error;

	if (node->rn_state & RAMFS_N_CREATING) {
		node->rn_state |= RAMFS_N_WANTED;
		msleep(node, &rm->rm_lock, PINOD | PDROP, "ramfs_create", NULL);
		return EAGAIN;
	}
	if (node->rn_vp != NULLVP) {
		uint32_t vid = node->rn_vid;

		vp = node->rn_vp;
		lck_mtx_unlock(&rm->rm_lock);
		if (vnode_getwithvid(vp, vid) == 0) {
			*vpp = vp;
			return 0;
		}
		/* being reclaimed: give it a moment */
		tsleep(rm, PINOD, "ramfs_reclaim", 1);
		return EAGAIN;
	}

	/* The CREATING mark keeps the node in place: nobody can unlink what they cannot get a vnode for. */
	node->rn_state |= RAMFS_N_CREATING;
	lck_mtx_unlock(&rm->rm_lock);

	bzero(&param, sizeof(param));
	param.vnfs_mp = rm->rm_mp;
	param.vnfs_vtype = node->rn_type;
	param.vnfs_str = "ramfs";
	param.vnfs_dvp = dvp;
	param.vnfs_fsnode = node;
#if FIFO && SOCKETS
	/* a named pipe: fifofs does its opening, reading and writing; its attributes and its end are ramfs' */
	param.vnfs_vops = node->rn_type == VFIFO ? ramfs_fifoop_p : ramfs_vnodeop_p;
#else
	param.vnfs_vops = ramfs_vnodeop_p;
#endif
	param.vnfs_markroot = (node == rm->rm_root);
	param.vnfs_filesize = node->rn_type == VREG ? (off_t)node->rn_size : 0;
	param.vnfs_cnp = cnp;
	/* names change under rename and unlink at any time: keep them out of the name cache */
	param.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;
	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &param, &vp);

	lck_mtx_lock(&rm->rm_lock);
	if (!error) {
		node->rn_vp = vp;
		node->rn_vid = vnode_vid(vp);
	}
	if (node->rn_state & RAMFS_N_WANTED) {
		wakeup(node);
	}
	node->rn_state &= ~(RAMFS_N_CREATING | RAMFS_N_WANTED);
	lck_mtx_unlock(&rm->rm_lock);

	if (!error) {
		*vpp = vp;
	}
	return error;
}

/* ------------------------------------------------------------------------ */
/* File contents                                                            */

/* Make sure the page array has a slot for page `index`. The mount lock is held
 * on entry and return. A normal write/truncate may wait outside it: XNU's
 * kalloc_large always rejects Z_NOWAIT, even when plenty of RAM is free.
 * The pager cannot wait; truncate/write reserve its slots ahead of time. */
static int
ramfs_grow_slots(struct ramfs_node *node, uint64_t index, bool can_wait)
{
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t want = node->rn_nslots ? node->rn_nslots : 4;
	void **slots;

	if (index < node->rn_nslots) {
		return 0;
	}
	while (want <= index) {
		if (want > UINT64_MAX / 2) {
			return EFBIG;
		}
		want *= 2;
	}
	if (want > SIZE_MAX / sizeof(void *)) {
		return EFBIG;
	}
	if (!ramfs_slots_fit(rm, node->rn_nslots, want)) {
		return ENOSPC;
	}
	if (can_wait) {
		lck_mtx_unlock(&rm->rm_lock);
	}
	slots = kalloc_type(void *, want, (can_wait ? Z_WAITOK : Z_NOWAIT) | Z_ZERO);
	if (can_wait) {
		lck_mtx_lock(&rm->rm_lock);
	}
	if (slots == NULL) {
		printf("ramfs: slot allocation failed: inode %llu, index %llu, slots %llu, bytes %llu\n",
		    node->rn_ino, index, want, want * sizeof(void *));
		return can_wait ? ENOMEM : ENOSPC;
	}
	/* Another writer may have grown the table while the lock was dropped.
	 * The vnode keeps the node alive; copy the current table, never a snapshot. */
	if (index < node->rn_nslots) {
		kfree_type(void *, want, slots);
		return 0;
	}
	if (!ramfs_slots_fit(rm, node->rn_nslots, want)) {
		kfree_type(void *, want, slots);        /* (the room was taken meanwhile) */
		return ENOSPC;
	}
	ramfs_slots_charge(rm, node->rn_nslots, want);
	if (node->rn_pages != NULL) {
		memcpy(slots, node->rn_pages, node->rn_nslots * sizeof(void *));
		kfree_type(void *, node->rn_nslots, node->rn_pages);
	}
	node->rn_pages = slots;
	node->rn_nslots = want;
	return 0;
}

/*
 * After a file was cut: its table made as small as its size needs, and the room it took given back -- a
 * truncate to 32 GB and back to nothing must leave nothing charged. Only where a page of room is saved, so a
 * file cut and grown by a little does not make its table anew each time. The mount lock is held on entry and
 * return and dropped for the allocation; the node's I/O lock is held, so the size cannot move meanwhile.
 */
static void
ramfs_shrink_slots(struct ramfs_node *node)
{
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t need = (node->rn_size + PAGE_MASK) / PAGE_SIZE, have = node->rn_nslots, want = 4;
	void **slots = NULL;

	if (have == 0) {
		return;
	}
	if (need != 0) {
		while (want < need) {
			want *= 2;
		}
		if (ramfs_slot_pages(want * sizeof(void *)) >= ramfs_slot_pages(have * sizeof(void *))) {
			return;
		}
		lck_mtx_unlock(&rm->rm_lock);
		slots = kalloc_type(void *, want, Z_WAITOK | Z_ZERO);
		lck_mtx_lock(&rm->rm_lock);
		if (slots == NULL || node->rn_nslots != have) {
			/* (no memory for the smaller one: the big one stays, still charged) */
			if (slots != NULL) {
				kfree_type(void *, want, slots);
			}
			return;
		}
		/* (the slots past `need` are empty: ramfs_resize freed their pages) */
		memcpy(slots, node->rn_pages, want * sizeof(void *));
	} else {
		want = 0;
	}
	kfree_type(void *, have, node->rn_pages);
	node->rn_pages = slots;
	node->rn_nslots = want;
	ramfs_slots_charge(rm, have, want);
}

/*
 * Copy len bytes (within one page) into the file at offset off, extending
 * the file if needed. *spare is a zeroed page the caller allocated without
 * the lock; it is consumed if the slot was empty. The mount lock is held.
 */
static int
ramfs_store(struct ramfs_node *node, uint64_t off, const void *src, size_t len, void **spare, bool can_wait)
{
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t index = off / PAGE_SIZE;
	int error = ramfs_grow_slots(node, index, can_wait);

	if (error) {
		return error;
	}
	if (node->rn_pages[index] == NULL) {
		if (*spare == NULL || rm->rm_pages >= rm->rm_max_pages) {
			/* Count every failure without flooding the console under pressure. */
			uint64_t failures = ++rm->rm_alloc_failures;
			if ((failures & (failures - 1)) == 0) {
				printf("ramfs: page allocation failed (%llu total): inode %llu, offset %llu, spare %p, pages %llu/%llu\n",
				    failures, node->rn_ino, off, *spare, rm->rm_pages, rm->rm_max_pages);
			}
			return ENOSPC;
		}
		node->rn_pages[index] = *spare;
		*spare = NULL;
		node->rn_nalloc++;
		rm->rm_pages++;
		os_atomic_inc(&ramfs_pages_in_use, relaxed);
	}
	memcpy((char *)node->rn_pages[index] + (off & PAGE_MASK), src, len);
	if (off + len > node->rn_size) {
		node->rn_size = off + len;
	}
	return 0;
}

/* Copy len bytes (within one page) out of the file; holes read as zeros. The mount lock is held. */
static void
ramfs_fetch(struct ramfs_node *node, uint64_t off, void *dst, size_t len)
{
	uint64_t index = off / PAGE_SIZE;

	if (index < node->rn_nslots && node->rn_pages[index] != NULL) {
		memcpy(dst, (char *)node->rn_pages[index] + (off & PAGE_MASK), len);
	} else {
		bzero(dst, len);
	}
}

/* Change the file size; shrinking frees pages and clears the tail of the last one. The mount lock is held. */
static void
ramfs_resize(struct ramfs_node *node, uint64_t size)
{
	if (size < node->rn_size) {
		uint64_t keep = (size + PAGE_MASK) / PAGE_SIZE;

		ramfs_free_pages(node, keep);
		if ((size & PAGE_MASK) != 0 && keep - 1 < node->rn_nslots && node->rn_pages[keep - 1] != NULL) {
			bzero((char *)node->rn_pages[keep - 1] + (size & PAGE_MASK), PAGE_SIZE - (size & PAGE_MASK));
		}
	}
	node->rn_size = size;
	ramfs_touch(node, true);
}

/*
 * What a write() has changed, into the pages of the page cache that hold it, where there are such
 * pages: each is filled anew from the file, in place. Throwing them away instead, for the next
 * page-in to fetch again, is not enough: the page cache cannot throw away a page that somebody
 * holds wired, and that page goes on showing the file as it was. A surface made around memory that
 * maps a file wires the pages under it (MiniIOSurface.cpp; CoreAnimation's window server makes one
 * around every picture it shows, and the pictures of the home screen's icons are memory that maps
 * IconServices' cache, a file here). When SpringBoard wrote the Calendar's new icon into that
 * cache, the pages of the icon that it shares with the icons next to it were still held by those
 * icons' surfaces: they kept the old icon, in the top 16 rows and the bottom 4 of the new one, and
 * SpringBoard's own mapping of the cache, being of those very pages, showed the same.
 */
static void
ramfs_refresh_cache(vnode_t vp, struct ramfs_node *node, uint64_t start, uint64_t end)
{
	struct ramfs_mount *rm = node->rn_mount;

	for (uint64_t page = trunc_page_64(start); page < end; page += PAGE_SIZE) {
		upl_t upl = NULL;
		upl_page_info_t *pl = NULL;
		vm_offset_t addr;
		int flags = 0;

		if (ubc_page_op(vp, (off_t)page, 0, NULL, &flags) != KERN_SUCCESS) {
			continue;                       /* not in the page cache: the next page-in fetches it */
		}
		if (ubc_create_upl_kernel(vp, (off_t)page, PAGE_SIZE, &upl, &pl, UPL_SET_LITE | UPL_WILL_MODIFY,
		    VM_KERN_MEMORY_FILE) != KERN_SUCCESS || upl == NULL) {
			continue;
		}
		if (!upl_page_present(pl, 0) || ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
			/* (gone meanwhile: the page the request made is thrown away again) */
			ubc_upl_abort_range(upl, 0, PAGE_SIZE, UPL_ABORT_FREE_ON_EMPTY);
			continue;
		}
		lck_mtx_lock(&rm->rm_lock);
		if (page < node->rn_size) {
			size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - page);

			ramfs_fetch(node, page, (void *)addr, len);
			bzero((void *)(addr + len), PAGE_SIZE - len);
		} else {
			bzero((void *)addr, PAGE_SIZE);
		}
		lck_mtx_unlock(&rm->rm_lock);
		ubc_upl_unmap(upl);
		ubc_upl_commit_range(upl, 0, PAGE_SIZE, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
	}
}

/* A mapping may hold changes that have not been paged out to us yet: get them. */
static void
ramfs_sync_mapping(vnode_t vp, struct ramfs_node *node)
{
	if (node->rn_mapped_write) {
		(void)ubc_msync(vp, 0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY | UBC_SYNC);
	}
}

/* ------------------------------------------------------------------------ */
/* Vnode operations                                                         */

static int
ramfs_lookup(struct vnop_lookup_args *ap)
{
	vnode_t dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_node *dir = RAMFS_NODE(dvp);
	struct ramfs_mount *rm = dir->rn_mount;
	int error;

	*ap->a_vpp = NULLVP;
	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		error = vnode_get(dvp);
		if (!error) {
			*ap->a_vpp = dvp;
		}
		return error;
	}

	do {
		struct ramfs_node *target;
		bool dotdot = (cnp->cn_flags & ISDOTDOT) != 0;

		lck_mtx_lock(&rm->rm_lock);
		if (dotdot) {
			target = dir->rn_parent;
		} else {
			struct ramfs_dirent *de = ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen);

			target = de ? de->rd_node : NULL;
		}
		if (target == NULL) {
			lck_mtx_unlock(&rm->rm_lock);
			/* a name that may be created: tell VFS to go ahead (unless the directory itself is gone) */
			if (!dotdot && dir->rn_nlink != 0 && (cnp->cn_flags & ISLASTCN) &&
			    (cnp->cn_nameiop == CREATE || cnp->cn_nameiop == RENAME)) {
				return EJUSTRETURN;
			}
			return ENOENT;
		}
		error = ramfs_vget_locked(rm, target, dotdot ? NULLVP : dvp, dotdot ? NULL : cnp, ap->a_vpp);
	} while (error == EAGAIN);
	return error;
}

/* create, mkdir, symlink and mknod are one operation with different node types */
static int
ramfs_make(vnode_t dvp, vnode_t *vpp, struct componentname *cnp, struct vnode_attr *vap, enum vtype type, const char *target)
{
	struct ramfs_node *dir = RAMFS_NODE(dvp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_node *node;
	struct ramfs_dirent *de;
	int error;

	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	node = ramfs_node_alloc(rm, type, vap);
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);
	if (type == VLNK) {
		node->rn_size = strlen(target);
		node->rn_target = kalloc_data(node->rn_size + 1, Z_WAITOK | Z_NOFAIL);
		memcpy(node->rn_target, target, node->rn_size + 1);
	}

	lck_mtx_lock(&rm->rm_lock);
	rm->rm_nodes++;
	if (dir->rn_nlink == 0) {
		error = ENOENT;         /* the directory has been removed */
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		error = 0;
	}
	if (error) {
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
		ramfs_dirent_free(de);
		return error;
	}
	node->rn_ino = rm->rm_next_ino++;
	ramfs_hash_insert(rm, node);
	if (type == VDIR) {
		node->rn_nlink = 2;
		node->rn_parent = dir;
		dir->rn_nlink++;
	} else {
		node->rn_nlink = 1;
	}
	ramfs_dir_add(dir, de, node);

	/* a fresh node has no vnode and no other suitor: this cannot ask for a retry */
	error = ramfs_vget_locked(rm, node, dvp, cnp, vpp);
	if (error) {
		lck_mtx_lock(&rm->rm_lock);
		/*
		 * The name may no longer be this node's: a rename onto it while the lock was dropped takes the
		 * entry over and drops the node's link itself. The entry is found again by name rather than
		 * trusted: it may be gone. Nothing else can name the node (a link needs its vnode).
		 */
		de = ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen);
		if (de != NULL && de->rd_node == node) {
			ramfs_dir_remove(dir, de);
			if (type == VDIR) {
				dir->rn_nlink--;
			}
		}
		ramfs_node_free(node);
		lck_mtx_unlock(&rm->rm_lock);
	}
	return error;
}

static int
ramfs_create(struct vnop_create_args *ap)
{
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VREG, NULL);
}

static int
ramfs_mkdir(struct vnop_mkdir_args *ap)
{
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VDIR, NULL);
}

static int
ramfs_symlink(struct vnop_symlink_args *ap)
{
	if (strlen(ap->a_target) >= MAXPATHLEN) {
		return ENAMETOOLONG;
	}
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, VLNK, ap->a_target);
}

/* The name of a local socket (bind) and a named pipe (mkfifo) can be made here; devices live in devfs. */
static int
ramfs_mknod(struct vnop_mknod_args *ap)
{
	enum vtype type = ap->a_vap->va_type;

#if FIFO && SOCKETS
	if (type != VSOCK && type != VFIFO) {
#else
	if (type != VSOCK) {
#endif
		return ENOTSUP;
	}
	return ramfs_make(ap->a_dvp, ap->a_vpp, ap->a_cnp, ap->a_vap, type, NULL);
}

static int
ramfs_remove(struct vnop_remove_args *ap)
{
	struct ramfs_node *dir = RAMFS_NODE(ap->a_dvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;

	if (node->rn_type == VDIR) {
		return EPERM;
	}
	if ((ap->a_flags & VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(ap->a_vp, 0)) {
		return EBUSY;
	}
	lck_mtx_lock(&rm->rm_lock);
	de = ramfs_dir_find(dir, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen);
	if (de == NULL || de->rd_node != node) {
		lck_mtx_unlock(&rm->rm_lock);
		return ENOENT;
	}
	ramfs_dir_remove(dir, de);
	node->rn_nlink--;
	ramfs_touch(node, false);
	lck_mtx_unlock(&rm->rm_lock);
	/* with no links left the node goes away when its vnode does: see ramfs_inactive() */
	return 0;
}

static int
ramfs_rmdir(struct vnop_rmdir_args *ap)
{
	struct ramfs_node *dir = RAMFS_NODE(ap->a_dvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;
	int error = 0;

	if (node->rn_type != VDIR) {
		return ENOTDIR;
	}
	lck_mtx_lock(&rm->rm_lock);
	de = ramfs_dir_find(dir, ap->a_cnp->cn_nameptr, (size_t)ap->a_cnp->cn_namelen);
	if (de == NULL || de->rd_node != node) {
		error = ENOENT;
	} else if (node->rn_nentries != 0) {
		error = ENOTEMPTY;
	} else {
		ramfs_dir_remove(dir, de);
		dir->rn_nlink--;
		node->rn_nlink = 0;
		node->rn_parent = NULL;
	}
	lck_mtx_unlock(&rm->rm_lock);
	return error;
}

static int
ramfs_link(struct vnop_link_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_node *dir = RAMFS_NODE(ap->a_tdvp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct componentname *cnp = ap->a_cnp;
	struct ramfs_dirent *de;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EPERM;
	}
	if (cnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	de = ramfs_dirent_alloc(cnp->cn_nameptr, (size_t)cnp->cn_namelen);

	lck_mtx_lock(&rm->rm_lock);
	if (dir->rn_nlink == 0 || node->rn_nlink == 0) {
		error = ENOENT;
	} else if (ramfs_dir_find(dir, cnp->cn_nameptr, (size_t)cnp->cn_namelen) != NULL) {
		error = EEXIST;
	} else {
		ramfs_dir_add(dir, de, node);
		node->rn_nlink++;
		ramfs_touch(node, false);
		de = NULL;
	}
	lck_mtx_unlock(&rm->rm_lock);
	if (de != NULL) {
		ramfs_dirent_free(de);
	}
	return error;
}

static int
ramfs_rename(struct vnop_rename_args *ap)
{
	struct ramfs_node *fdir = RAMFS_NODE(ap->a_fdvp);
	struct ramfs_node *tdir = RAMFS_NODE(ap->a_tdvp);
	struct ramfs_node *node = RAMFS_NODE(ap->a_fvp);
	struct ramfs_mount *rm = node->rn_mount;
	struct componentname *fcnp = ap->a_fcnp, *tcnp = ap->a_tcnp;
	struct ramfs_dirent *fde, *tde, *spare;
	int error = 0;

	if (tcnp->cn_namelen > RAMFS_MAX_NAME) {
		return ENAMETOOLONG;
	}
	spare = ramfs_dirent_alloc(tcnp->cn_nameptr, (size_t)tcnp->cn_namelen);

	lck_mtx_lock(&rm->rm_lock);
	fde = ramfs_dir_find(fdir, fcnp->cn_nameptr, (size_t)fcnp->cn_namelen);
	tde = ramfs_dir_find(tdir, tcnp->cn_nameptr, (size_t)tcnp->cn_namelen);

	/*
	 * The names were looked up, and what they name authorized, before the lock was taken: if either now
	 * names something else (or the target names something where there was nothing), VFS looks them up
	 * again (ERECYCLE) rather than have this replace a file nobody was allowed to remove.
	 */
	if (fde == NULL || fde->rd_node != node ||
	    (tde == NULL ? ap->a_tvp != NULLVP : ap->a_tvp == NULLVP || tde->rd_node != RAMFS_NODE(ap->a_tvp))) {
		error = ERECYCLE;
		goto out;
	}
	if (tdir->rn_nlink == 0) {
		error = ENOENT;
		goto out;
	}
	if (node->rn_type == VDIR) {
		/* a directory cannot move into itself or below itself */
		for (struct ramfs_node *up = tdir; up != NULL; up = (up == rm->rm_root) ? NULL : up->rn_parent) {
			if (up == node) {
				error = EINVAL;
				goto out;
			}
		}
	}
	if (tde != NULL && tde->rd_node == node) {
		/* two names of the same file: POSIX wants the rename to do nothing */
		goto out;
	}

	if (tde != NULL) {
		struct ramfs_node *victim = tde->rd_node;

		if (victim->rn_type == VDIR) {
			if (node->rn_type != VDIR) {
				error = EISDIR;
			} else if (victim->rn_nentries != 0) {
				error = ENOTEMPTY;
			}
		} else if (node->rn_type == VDIR) {
			error = ENOTDIR;
		}
		if (error) {
			goto out;
		}
		if (victim->rn_type == VDIR) {
			victim->rn_nlink = 0;
			victim->rn_parent = NULL;
			tdir->rn_nlink--;
		} else {
			victim->rn_nlink--;
			ramfs_touch(victim, false);
		}
		tde->rd_node = node;
		ramfs_touch(tdir, true);
	} else {
		ramfs_dir_add(tdir, spare, node);
		spare = NULL;
	}
	ramfs_dir_remove(fdir, fde);
	if (node->rn_type == VDIR && fdir != tdir) {
		fdir->rn_nlink--;
		tdir->rn_nlink++;
		node->rn_parent = tdir;
	}
	ramfs_touch(node, false);
out:
	lck_mtx_unlock(&rm->rm_lock);
	if (spare != NULL) {
		ramfs_dirent_free(spare);
	}
	if (!error) {
		vnode_update_identity(ap->a_fvp, ap->a_tdvp, tcnp->cn_nameptr, tcnp->cn_namelen, tcnp->cn_hash,
		    VNODE_UPDATE_PARENT | VNODE_UPDATE_NAME);
	}
	return error;
}

/*
 * renamex_np(2): RENAME_SWAP exchanges two names, RENAME_EXCL refuses an existing target. Darwin writes
 * files "atomically" by swapping a new copy in -- usermanagerd's user sessions and persona manifest in
 * /private/var/keybags among them, and with ENOTSUP here it never saved them and aborted at every start.
 */
static int
ramfs_renamex(struct vnop_renamex_args *ap)
{
	struct vnop_rename_args plain = {
		.a_desc = &vnop_rename_desc, .a_fdvp = ap->a_fdvp, .a_fvp = ap->a_fvp, .a_fcnp = ap->a_fcnp,
		.a_tdvp = ap->a_tdvp, .a_tvp = ap->a_tvp, .a_tcnp = ap->a_tcnp, .a_context = ap->a_context,
	};
	struct ramfs_node *fdir = RAMFS_NODE(ap->a_fdvp);
	struct ramfs_node *tdir = RAMFS_NODE(ap->a_tdvp);
	struct ramfs_node *fnode, *tnode;
	struct ramfs_mount *rm;
	struct componentname *fcnp = ap->a_fcnp, *tcnp = ap->a_tcnp;
	struct ramfs_dirent *fde, *tde;
	int error = 0;

	if (ap->a_flags & ~(VFS_RENAME_SWAP | VFS_RENAME_EXCL)) {
		return ENOTSUP;
	}
	if (ap->a_flags == VFS_RENAME_EXCL) {
		if (ap->a_tvp != NULLVP) {
			return EEXIST;
		}
		/* (a target made since the lookup: ERECYCLE there, and the next lookup finds it: EEXIST) */
		return ramfs_rename(&plain);
	}
	if (ap->a_flags != VFS_RENAME_SWAP) {
		return EINVAL;
	}
	if (ap->a_tvp == NULLVP) {
		return ENOENT;
	}
	fnode = RAMFS_NODE(ap->a_fvp);
	tnode = RAMFS_NODE(ap->a_tvp);
	rm = fnode->rn_mount;
	if (fnode == tnode) {
		return 0;
	}

	lck_mtx_lock(&rm->rm_lock);
	fde = ramfs_dir_find(fdir, fcnp->cn_nameptr, (size_t)fcnp->cn_namelen);
	tde = ramfs_dir_find(tdir, tcnp->cn_nameptr, (size_t)tcnp->cn_namelen);
	if (fde == NULL || fde->rd_node != fnode || tde == NULL || tde->rd_node != tnode) {
		error = ERECYCLE;               /* changed since VFS looked them up: it looks again */
		goto out;
	}
	/* neither may end up inside itself */
	for (struct ramfs_node *up = tdir; fnode->rn_type == VDIR && up != NULL; up = (up == rm->rm_root) ? NULL : up->rn_parent) {
		if (up == fnode) {
			error = EINVAL;
			goto out;
		}
	}
	for (struct ramfs_node *up = fdir; tnode->rn_type == VDIR && up != NULL; up = (up == rm->rm_root) ? NULL : up->rn_parent) {
		if (up == tnode) {
			error = EINVAL;
			goto out;
		}
	}
	fde->rd_node = tnode;
	tde->rd_node = fnode;
	if (fdir != tdir) {
		if (fnode->rn_type == VDIR) {
			fdir->rn_nlink--;
			tdir->rn_nlink++;
			fnode->rn_parent = tdir;
		}
		if (tnode->rn_type == VDIR) {
			tdir->rn_nlink--;
			fdir->rn_nlink++;
			tnode->rn_parent = fdir;
		}
	}
	ramfs_touch(fdir, true);
	ramfs_touch(tdir, true);
	ramfs_touch(fnode, false);
	ramfs_touch(tnode, false);
out:
	lck_mtx_unlock(&rm->rm_lock);
	if (!error) {
		vnode_update_identity(ap->a_fvp, ap->a_tdvp, tcnp->cn_nameptr, tcnp->cn_namelen, tcnp->cn_hash,
		    VNODE_UPDATE_PARENT | VNODE_UPDATE_NAME);
		vnode_update_identity(ap->a_tvp, ap->a_fdvp, fcnp->cn_nameptr, fcnp->cn_namelen, fcnp->cn_hash,
		    VNODE_UPDATE_PARENT | VNODE_UPDATE_NAME);
	}
	return error;
}

static int
ramfs_open(__unused struct vnop_open_args *ap)
{
	return 0;
}

static int
ramfs_close(__unused struct vnop_close_args *ap)
{
	return 0;
}

static int
ramfs_getattr(struct vnop_getattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	uint64_t size, alloc;

	lck_mtx_lock(&rm->rm_lock);
	size = node->rn_type == VDIR ? (uint64_t)(node->rn_nentries + 2) * 32 : node->rn_size;
	alloc = node->rn_nalloc * PAGE_SIZE;

	VATTR_RETURN(vap, va_rdev, 0);
	VATTR_RETURN(vap, va_nlink, node->rn_nlink);
	VATTR_RETURN(vap, va_total_size, size);
	VATTR_RETURN(vap, va_total_alloc, alloc);
	VATTR_RETURN(vap, va_data_size, size);
	VATTR_RETURN(vap, va_data_alloc, alloc);
	VATTR_RETURN(vap, va_iosize, PAGE_SIZE);
	VATTR_RETURN(vap, va_uid, node->rn_uid);
	VATTR_RETURN(vap, va_gid, node->rn_gid);
	VATTR_RETURN(vap, va_mode, node->rn_mode);
	VATTR_RETURN(vap, va_flags, node->rn_bsdflags);
	VATTR_RETURN(vap, va_create_time, node->rn_btime);
	VATTR_RETURN(vap, va_access_time, node->rn_atime);
	VATTR_RETURN(vap, va_modify_time, node->rn_mtime);
	VATTR_RETURN(vap, va_change_time, node->rn_ctime);
	VATTR_RETURN(vap, va_fileid, node->rn_ino);
	VATTR_RETURN(vap, va_linkid, node->rn_ino);
	VATTR_RETURN(vap, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
	VATTR_RETURN(vap, va_filerev, 0);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_dataprotect_class, node->rn_dpclass);
	if (node->rn_type == VDIR && node->rn_parent != NULL) {
		VATTR_RETURN(vap, va_parentid, node->rn_parent->rn_ino);
		/*
		 * And its name, from its entry in the parent: what the kernel asks for to make a path out of a
		 * number alone (fsgetpath() of a vnode vfs_vget made, which has no name of its own).
		 */
		if (VATTR_IS_ACTIVE(vap, va_name) && vap->va_name != NULL && node != rm->rm_root) {
			struct ramfs_dirent *de;

			TAILQ_FOREACH(de, &node->rn_parent->rn_entries, rd_link) {
				if (de->rd_node == node) {
					strlcpy(vap->va_name, de->rd_name, MAXPATHLEN);
					VATTR_SET_SUPPORTED(vap, va_name);
					break;
				}
			}
		}
	}
	lck_mtx_unlock(&rm->rm_lock);
	return 0;
}

static int
ramfs_setattr(struct vnop_setattr_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	bool sizing = VATTR_IS_ACTIVE(vap, va_data_size), resized = false;
	uint64_t size = 0;

	if (sizing) {
		if (node->rn_type == VDIR) {
			return EISDIR;
		}
		if (node->rn_type != VREG) {
			return EINVAL;
		}
		/* (the table of a larger file's pages could not be charged, or even counted, safely) */
		if ((uint64_t)vap->va_data_size > RAMFS_MAX_FILE) {
			return EFBIG;
		}
		lck_rw_lock_exclusive(&node->rn_io_lock);
		/* what a mapping changed must reach us before the size moves under it */
		ramfs_sync_mapping(vp, node);
	}

	lck_mtx_lock(&rm->rm_lock);
	if (VATTR_IS_ACTIVE(vap, va_data_size)) {
		size = vap->va_data_size;
		/* A shared mapping cannot extend EOF. Reserve its pointer table here,
		 * while sleeping is possible, so pageout needs no large allocation. */
		if (size > node->rn_size) {
			int error = ramfs_grow_slots(node, (size - 1) / PAGE_SIZE, true);
			if (error) {
				lck_mtx_unlock(&rm->rm_lock);
				lck_rw_unlock_exclusive(&node->rn_io_lock);
				return error;
			}
		}
		if (size < node->rn_size) {
			ramfs_resize(node, size);
			ramfs_shrink_slots(node);
		} else {
			ramfs_resize(node, size);
		}
		resized = true;
		VATTR_SET_SUPPORTED(vap, va_data_size);
		/* (cut or grown by anybody but root: as a write, no longer run as its owner or its group) */
		if (!VATTR_IS_ACTIVE(vap, va_mode) && !kauth_cred_issuser(vfs_context_ucred(ap->a_context))) {
			node->rn_mode &= ~(S_ISUID | S_ISGID);
		}
	}
	if (VATTR_IS_ACTIVE(vap, va_mode)) {
		node->rn_mode = vap->va_mode & ALLPERMS;
		VATTR_SET_SUPPORTED(vap, va_mode);
	}
	if (VATTR_IS_ACTIVE(vap, va_uid)) {
		node->rn_uid = vap->va_uid;
		VATTR_SET_SUPPORTED(vap, va_uid);
	}
	if (VATTR_IS_ACTIVE(vap, va_gid)) {
		node->rn_gid = vap->va_gid;
		VATTR_SET_SUPPORTED(vap, va_gid);
	}
	if (VATTR_IS_ACTIVE(vap, va_flags)) {
		node->rn_bsdflags = vap->va_flags;
		VATTR_SET_SUPPORTED(vap, va_flags);
	}
	if (VATTR_IS_ACTIVE(vap, va_dataprotect_class)) {
		node->rn_dpclass = vap->va_dataprotect_class;
		VATTR_SET_SUPPORTED(vap, va_dataprotect_class);
	}
	if (VATTR_IS_ACTIVE(vap, va_create_time)) {
		node->rn_btime = vap->va_create_time;
		VATTR_SET_SUPPORTED(vap, va_create_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_access_time)) {
		node->rn_atime = vap->va_access_time;
		VATTR_SET_SUPPORTED(vap, va_access_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_modify_time)) {
		node->rn_mtime = vap->va_modify_time;
		VATTR_SET_SUPPORTED(vap, va_modify_time);
	}
	if (VATTR_IS_ACTIVE(vap, va_change_time)) {
		node->rn_ctime = vap->va_change_time;
		VATTR_SET_SUPPORTED(vap, va_change_time);
	} else {
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (resized) {
		(void)ubc_setsize(vp, (off_t)size);
	}
	if (sizing) {
		lck_rw_unlock_exclusive(&node->rn_io_lock);
	}
	return 0;
}

static int
ramfs_read(struct vnop_read_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	char *bounce;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EISDIR;
	}
	if (node->rn_type != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	ramfs_sync_mapping(vp, node);

	bounce = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0 && !error) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len;

		lck_mtx_lock(&rm->rm_lock);
		if (off >= node->rn_size) {
			lck_mtx_unlock(&rm->rm_lock);
			break;
		}
		len = MIN((size_t)MIN((uint64_t)uio_resid(uio), node->rn_size - off), PAGE_SIZE - (size_t)(off & PAGE_MASK));
		ramfs_fetch(node, off, bounce, len);
		lck_mtx_unlock(&rm->rm_lock);

		error = uiomove(bounce, (int)len, uio);
	}
	kfree_data(bounce, PAGE_SIZE);
	return error;
}

static int
ramfs_write(struct vnop_write_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	void *spare = NULL;
	char *bounce;
	uint64_t start, old_size;
	int error = 0;

	if (node->rn_type == VDIR) {
		return EISDIR;
	}
	if (node->rn_type != VREG || uio_offset(uio) < 0) {
		return EINVAL;
	}
	if (uio_resid(uio) == 0) {
		return 0;
	}
	/* (to the end: where an append starts, the pages it fills and the size it leaves are one writer's) */
	lck_rw_lock_exclusive(&node->rn_io_lock);
	ramfs_sync_mapping(vp, node);

	lck_mtx_lock(&rm->rm_lock);
	old_size = node->rn_size;
	if (ap->a_ioflag & IO_APPEND) {
		uio_setoffset(uio, (off_t)old_size);
	}
	lck_mtx_unlock(&rm->rm_lock);
	start = (uint64_t)uio_offset(uio);

	bounce = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);
	while (uio_resid(uio) > 0) {
		uint64_t off = (uint64_t)uio_offset(uio);
		size_t len = MIN((size_t)uio_resid(uio), PAGE_SIZE - (size_t)(off & PAGE_MASK));

		/* no further than a truncate may go (ramfs_setattr); the limit is a page boundary, so no piece crosses it */
		if (off >= RAMFS_MAX_FILE) {
			error = EFBIG;
			break;
		}
		error = uiomove(bounce, (int)len, uio);
		if (error) {
			break;
		}
		uint64_t index = off / PAGE_SIZE;
		lck_mtx_lock(&rm->rm_lock);
		if (index >= node->rn_nslots || node->rn_pages[index] == NULL) {
			lck_mtx_unlock(&rm->rm_lock);
			ramfs_reserve_slots(rm, node, index);
			if (spare == NULL) {
				spare = ramfs_page_alloc(true);
			}
			lck_mtx_lock(&rm->rm_lock);
		}
		error = ramfs_store(node, off, bounce, len, &spare, true);
		lck_mtx_unlock(&rm->rm_lock);
		if (error) {
			/* nothing of this piece was stored: hand it back to the caller's count */
			uio_setoffset(uio, (off_t)off);
			uio_setresid(uio, uio_resid(uio) + (user_ssize_t)len);
			break;
		}
	}
	kfree_data(bounce, PAGE_SIZE);
	if (spare != NULL) {
		ramfs_page_free(spare);
	}

	if ((uint64_t)uio_offset(uio) > start) {
		uint64_t new_size;

		lck_mtx_lock(&rm->rm_lock);
		ramfs_touch(node, true);
		/*
		 * Written by anybody but root: no longer run as its owner or its group. What was changed may not be what
		 * was made set-user-ID (HFS, APFS and UFS do the same on a write; a truncate does it in ramfs_setattr).
		 */
		if ((node->rn_mode & (S_ISUID | S_ISGID)) && !kauth_cred_issuser(vfs_context_ucred(ap->a_context))) {
			node->rn_mode &= ~(S_ISUID | S_ISGID);
		}
		new_size = node->rn_size;
		lck_mtx_unlock(&rm->rm_lock);

		if (new_size != old_size) {
			(void)ubc_setsize(vp, (off_t)new_size);
		}
		/* cached pages of the range are stale now: brought up to date in place */
		if (ubc_pages_resident(vp)) {
			ramfs_refresh_cache(vp, node, start, (uint64_t)uio_offset(uio));
		}
		/* a partial write is a success */
		if (error == ENOSPC || error == ENOMEM || error == EFBIG) {
			error = 0;
		}
	}
	lck_rw_unlock_exclusive(&node->rn_io_lock);
	return error;
}

/* Classic 32-bit-inode directory entries; the offset is a byte count of the entries before it. */
static int
ramfs_readdir(struct vnop_readdir_args *ap)
{
#define RAMFS_DIRSIZ(namlen)    ((sizeof(struct dirent) - (MAXNAMLEN + 1)) + (((namlen) + 1 + 3) & ~3))
	vnode_t vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct ramfs_node *dir = RAMFS_NODE(vp);
	struct ramfs_mount *rm = dir->rn_mount;
	struct ramfs_dirent *de;
	off_t start = uio_offset(uio), pos = 0;     /* (positions are cookies: . 0, .. 1, an entry its own) */
	size_t room = (size_t)MIN((user_ssize_t)PAGE_SIZE, uio_resid(uio));
	size_t used = 0;
	uint32_t entry = 0;
	bool eof = false;
	int count = 0, error;
	char *buf;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}
	if (dir->rn_type != VDIR) {
		return ENOTDIR;
	}
	/* the entries are collected under the lock and copied out after it is dropped */
	buf = kalloc_data(PAGE_SIZE, Z_WAITOK | Z_NOFAIL);

	lck_mtx_lock(&rm->rm_lock);
	/*
	 * A position is where an entry stays for as long as it is there: entries removed before it do not move it.
	 * A byte count from the first entry would: a program that removes what it has read, reading on, would pass
	 * over as many entries as it removed.
	 */
	de = TAILQ_FIRST(&dir->rn_entries);
	for (;; entry++) {
		struct dirent d;
		const char *name;

		bzero(&d, sizeof(d));
		if (entry == 0) {
			d.d_fileno = (ino_t)dir->rn_ino;
			d.d_type = DT_DIR;
			d.d_namlen = 1;
			name = ".";
		} else if (entry == 1) {
			d.d_fileno = (ino_t)(dir->rn_parent ? dir->rn_parent->rn_ino : dir->rn_ino);
			d.d_type = DT_DIR;
			d.d_namlen = 2;
			name = "..";
		} else {
			if (de == NULL) {
				eof = true;
				break;
			}
			d.d_fileno = (ino_t)de->rd_node->rn_ino;
			d.d_type = (uint8_t)IFTODT(VTTOIF(de->rd_node->rn_type));
			d.d_namlen = (uint8_t)de->rd_namelen;
			name = de->rd_name;
		}
		pos = entry < 2 ? (off_t)entry : (off_t)de->rd_cookie;
		if (entry >= 2) {
			de = TAILQ_NEXT(de, rd_link);
		}
		d.d_reclen = (uint16_t)RAMFS_DIRSIZ(d.d_namlen);
		if (pos >= start) {
			if (used + d.d_reclen > room) {
				break;                  /* (the next read begins with this one: pos) */
			}
			memcpy(d.d_name, name, d.d_namlen);
			memcpy(buf + used, &d, d.d_reclen);
			used += d.d_reclen;
			count++;
		}
	}
	if (eof) {
		pos = (off_t)(2 + dir->rn_cookies);     /* (past every entry there is, and any made later is after it) */
	}
	lck_mtx_unlock(&rm->rm_lock);

	error = used ? uiomove(buf, (int)used, uio) : 0;
	kfree_data(buf, PAGE_SIZE);
	if (!error) {
		uio_setoffset(uio, pos);
		if (ap->a_eofflag) {
			*ap->a_eofflag = eof;
		}
		if (ap->a_numdirent) {
			*ap->a_numdirent = count;
		}
	}
	return error;
#undef RAMFS_DIRSIZ
}

static int
ramfs_readlink(struct vnop_readlink_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);

	if (node->rn_type != VLNK) {
		return EINVAL;
	}
	/* a symlink never changes after it is made */
	return uiomove(node->rn_target, (int)node->rn_size, ap->a_uio);
}

/*
 * hakios: APFS's "mark purgeable" (APFSIOC_MARK_PURGEABLE, _IOWR('J', 68, uint64_t)), which Darwin 26's
 * PosterBoard asks of every directory and file of its wallpaper store and fails the store without
 * (errno -1, no poster is ever made). Purgeable files are what APFS may delete when space runs short;
 * this RAM disk deletes nothing on its own, so the mark is taken and nothing more is done.
 *
 * Every other request of APFS's own ('J', the group fsctl(2) and ffsctl(2) hand down here) is "not
 * supported": fileproviderd is told /private/var is APFS (ramfs_fstypename_shown() below) and may try
 * what only APFS does -- snapshots, sync roots, cloning a range -- and ENOTSUP is what a program that
 * asks an APFS feature of a volume without it is prepared for. Nothing is read or written for it.
 */
#define RAMFS_APFSIOC_MARK_PURGEABLE    _IOWR('J', 68, uint64_t)

static int
ramfs_ioctl(struct vnop_ioctl_args *ap)
{
	if (ap->a_command == RAMFS_APFSIOC_MARK_PURGEABLE) {
		return 0;
	}
	if (IOCGROUP(ap->a_command) == 'J') {
		return ENOTSUP;
	}
	return ENOTTY;
}

/* Last reference gone: a node without a name has no reason to linger in the vnode cache. */
static int
ramfs_inactive(struct vnop_inactive_args *ap)
{
	if (RAMFS_NODE(ap->a_vp)->rn_nlink == 0) {
		vnode_recycle(ap->a_vp);
	}
	return 0;
}

static int
ramfs_reclaim(struct vnop_reclaim_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;

	lck_mtx_lock(&rm->rm_lock);
	node->rn_vp = NULLVP;
	node->rn_vid = 0;
	if (node->rn_nlink == 0 && node != rm->rm_root) {
		ramfs_node_free(node);
	}
	lck_mtx_unlock(&rm->rm_lock);

	vnode_clearfsnode(vp);
	return 0;
}

static int
ramfs_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 32767;
		return 0;
	case _PC_NAME_MAX:
		*ap->a_retval = RAMFS_MAX_NAME;
		return 0;
	case _PC_PATH_MAX:
		*ap->a_retval = MAXPATHLEN;
		return 0;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return 0;
	case _PC_CHOWN_RESTRICTED:
	case _PC_NO_TRUNC:
		*ap->a_retval = 200112;
		return 0;
	case _PC_CASE_SENSITIVE:
	case _PC_CASE_PRESERVING:
		*ap->a_retval = 1;
		return 0;
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		return 0;
	default:
		return EINVAL;
	}
}

static int
ramfs_mmap(struct vnop_mmap_args *ap)
{
	if (ap->a_fflags & VM_PROT_WRITE) {
		RAMFS_NODE(ap->a_vp)->rn_mapped_write = true;
	}
	return 0;
}

static int
ramfs_mnomap(__unused struct vnop_mnomap_args *ap)
{
	return 0;
}

/* Fill pages of the page cache from the file. */
static int
ramfs_pagein(struct vnop_pagein_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	upl_size_t valid = 0;
	vm_offset_t addr;

	if (upl == NULL || node->rn_type != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit && upl != NULL) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	lck_mtx_lock(&rm->rm_lock);
	for (upl_size_t done = 0; done < size && off + done < node->rn_size; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - (off + done));

		ramfs_fetch(node, off + done, (void *)(addr + done), len);
		bzero((void *)(addr + done + len), PAGE_SIZE - len);
		valid = done + PAGE_SIZE;
	}
	lck_mtx_unlock(&rm->rm_lock);
	ubc_upl_unmap(upl);

	if (commit) {
		if (valid != 0) {
			ubc_upl_commit_range(upl, ap->a_pl_offset, valid, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
		/* pages wholly past the end of the file do not exist */
		if (valid < size) {
			ubc_upl_abort_range(upl, ap->a_pl_offset + valid, size - valid, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
	}
	return valid != 0 ? 0 : EINVAL;
}

/* Take back what a mapping changed: copy dirty pages of the page cache into the file. */
static int
ramfs_pageout(struct vnop_pageout_args *ap)
{
	vnode_t vp = ap->a_vp;
	struct ramfs_node *node = RAMFS_NODE(vp);
	struct ramfs_mount *rm = node->rn_mount;
	upl_t upl = ap->a_pl;
	upl_size_t size = (upl_size_t)ap->a_size;
	uint64_t off = (uint64_t)ap->a_f_offset;
	bool commit = (ap->a_flags & UPL_NOCOMMIT) == 0;
	vm_offset_t addr;
	int error = 0;

	if (upl == NULL) {
		return EINVAL;
	}
	if (node->rn_type != VREG || ap->a_f_offset < 0 || (off & PAGE_MASK) != 0 ||
	    ubc_upl_map(upl, &addr) != KERN_SUCCESS) {
		if (commit) {
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		}
		return EINVAL;
	}
	addr += ap->a_pl_offset;

	lck_mtx_lock(&rm->rm_lock);
	for (upl_size_t done = 0; done < size && off + done < node->rn_size && !error; done += PAGE_SIZE) {
		size_t len = (size_t)MIN((uint64_t)PAGE_SIZE, node->rn_size - (off + done));
		uint64_t index = (off + done) / PAGE_SIZE;
		/* this may be the pageout daemon looking for memory: never wait for more of it here */
		void *spare = index < node->rn_nslots && node->rn_pages[index] != NULL ? NULL : ramfs_page_alloc(false);

		error = ramfs_store(node, off + done, (const void *)(addr + done), len, &spare, false);
		if (spare != NULL) {
			ramfs_page_free(spare);
		}
	}
	if (!error) {
		ramfs_touch(node, true);
	}
	lck_mtx_unlock(&rm->rm_lock);
	ubc_upl_unmap(upl);

	if (commit) {
		if (error) {
			/* the pages stay dirty and will be offered again */
			ubc_upl_abort_range(upl, ap->a_pl_offset, size, UPL_ABORT_FREE_ON_EMPTY);
		} else {
			ubc_upl_commit_range(upl, ap->a_pl_offset, size, UPL_COMMIT_CLEAR_DIRTY | UPL_COMMIT_FREE_ON_EMPTY);
		}
	}
	return error;
}

/* ---- extended attributes ---- */

static struct ramfs_xattr *
ramfs_xattr_find(struct ramfs_node *node, const char *name)
{
	struct ramfs_xattr *xa;
	size_t len = strlen(name);

	TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
		if (xa->rx_namelen == len && memcmp(xa->rx_name, name, len) == 0) {
			return xa;
		}
	}
	return NULL;
}

static int
ramfs_getxattr(struct vnop_getxattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa;
	void *copy = NULL;
	size_t size = 0;
	int error = 0;

	lck_mtx_lock(&rm->rm_lock);
	xa = ramfs_xattr_find(node, ap->a_name);
	if (xa == NULL) {
		error = ENOATTR;
	} else {
		size = xa->rx_size;
		if (ap->a_uio != NULL) {
			if ((size_t)uio_resid(ap->a_uio) < size) {
				error = ERANGE;
			} else if (size != 0) {
				/* copied out after the lock is dropped: the attribute may be replaced meanwhile */
				copy = kalloc_data(size, Z_NOWAIT);
				if (copy == NULL) {
					error = ENOMEM;
				} else {
					memcpy(copy, xa->rx_data, size);
				}
			}
		}
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (!error) {
		if (ap->a_uio == NULL) {
			*ap->a_size = size;
		} else if (copy != NULL) {
			error = uiomove(copy, (int)size, ap->a_uio);
		}
	}
	if (copy != NULL) {
		kfree_data(copy, size);
	}
	return error;
}

static int
ramfs_setxattr(struct vnop_setxattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa, *fresh, *old = NULL;
	size_t size = (size_t)uio_resid(ap->a_uio);
	size_t namelen = strlen(ap->a_name);
	void *data = NULL;
	int error = 0;

	if (size > RAMFS_MAX_XATTR || namelen > XATTR_MAXNAMELEN) {
		return E2BIG;
	}
	if (size != 0) {
		data = kalloc_data(size, Z_WAITOK);
		if (data == NULL) {
			return ENOMEM;
		}
		error = uiomove(data, (int)size, ap->a_uio);
		if (error) {
			kfree_data(data, size);
			return error;
		}
	}
	fresh = kalloc_type(struct ramfs_xattr, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	fresh->rx_name = kalloc_data(namelen + 1, Z_WAITOK | Z_NOFAIL);
	memcpy(fresh->rx_name, ap->a_name, namelen + 1);
	fresh->rx_namelen = namelen;
	fresh->rx_data = data;
	fresh->rx_size = size;

	lck_mtx_lock(&rm->rm_lock);
	xa = ramfs_xattr_find(node, ap->a_name);
	if (xa != NULL && (ap->a_options & XATTR_CREATE)) {
		error = EEXIST;
	} else if (xa == NULL && (ap->a_options & XATTR_REPLACE)) {
		error = ENOATTR;
	} else {
		/* what the node's attributes come to with this one in place of the one it replaces */
		size_t bytes = node->rn_xattr_bytes - (xa != NULL ? xa->rx_namelen + xa->rx_size : 0) + namelen + size;
		uint64_t had = ramfs_xattr_pages(node->rn_xattr_bytes), need = ramfs_xattr_pages(bytes);

		/* a cap on each node as well as the mount's room: one file may not hold any number of large attributes */
		if (bytes > RAMFS_MAX_XATTR) {
			error = E2BIG;
		} else if (need > had && rm->rm_pages + (need - had) > rm->rm_max_pages) {
			error = ENOSPC;
		} else {
			if (xa != NULL) {
				TAILQ_REMOVE(&node->rn_xattrs, xa, rx_link);
				old = xa;
			}
			TAILQ_INSERT_TAIL(&node->rn_xattrs, fresh, rx_link);
			fresh = NULL;
			node->rn_xattr_bytes = bytes;
			rm->rm_pages = rm->rm_pages - had + need;
			ramfs_touch(node, false);
		}
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (fresh != NULL) {
		ramfs_xattr_free(fresh);
	}
	if (old != NULL) {
		ramfs_xattr_free(old);
	}
	return error;
}

static int
ramfs_removexattr(struct vnop_removexattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa;

	lck_mtx_lock(&rm->rm_lock);
	xa = ramfs_xattr_find(node, ap->a_name);
	if (xa != NULL) {
		uint64_t had = ramfs_xattr_pages(node->rn_xattr_bytes);

		TAILQ_REMOVE(&node->rn_xattrs, xa, rx_link);
		node->rn_xattr_bytes -= xa->rx_namelen + xa->rx_size;
		rm->rm_pages -= had - ramfs_xattr_pages(node->rn_xattr_bytes);
		ramfs_touch(node, false);
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (xa == NULL) {
		return ENOATTR;
	}
	ramfs_xattr_free(xa);
	return 0;
}

/* The names, each with its terminator, one after another. */
static int
ramfs_listxattr(struct vnop_listxattr_args *ap)
{
	struct ramfs_node *node = RAMFS_NODE(ap->a_vp);
	struct ramfs_mount *rm = node->rn_mount;
	struct ramfs_xattr *xa;
	char *copy = NULL;
	size_t total = 0, at = 0;
	int error = 0;

	lck_mtx_lock(&rm->rm_lock);
	TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
		total += xa->rx_namelen + 1;
	}
	if (ap->a_uio != NULL && total != 0) {
		if ((size_t)uio_resid(ap->a_uio) < total) {
			error = ERANGE;
		} else if ((copy = kalloc_data(total, Z_NOWAIT)) == NULL) {
			error = ENOMEM;
		} else {
			TAILQ_FOREACH(xa, &node->rn_xattrs, rx_link) {
				memcpy(copy + at, xa->rx_name, xa->rx_namelen + 1);
				at += xa->rx_namelen + 1;
			}
		}
	}
	lck_mtx_unlock(&rm->rm_lock);

	if (!error) {
		if (ap->a_uio == NULL) {
			*ap->a_size = total;
		} else if (copy != NULL) {
			error = uiomove(copy, (int)total, ap->a_uio);
		}
	}
	if (copy != NULL) {
		kfree_data(copy, total);
	}
	return error;
}

static int
ramfs_blktooff(struct vnop_blktooff_args *ap)
{
	*ap->a_offset = (off_t)ap->a_lblkno * PAGE_SIZE;
	return 0;
}

static int
ramfs_offtoblk(struct vnop_offtoblk_args *ap)
{
	*ap->a_lblkno = (daddr64_t)(ap->a_offset / PAGE_SIZE);
	return 0;
}

#define VOPFUNC int (*)(void *)

int(**ramfs_vnodeop_p)(void *);
const struct vnodeopv_entry_desc ramfs_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)ramfs_lookup },
	{ .opve_op = &vnop_create_desc, .opve_impl = (VOPFUNC)ramfs_create },
	{ .opve_op = &vnop_mknod_desc, .opve_impl = (VOPFUNC)ramfs_mknod },
	{ .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)ramfs_open },
	{ .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)ramfs_close },
	{ .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)ramfs_getattr },
	{ .opve_op = &vnop_setattr_desc, .opve_impl = (VOPFUNC)ramfs_setattr },
	{ .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)ramfs_read },
	{ .opve_op = &vnop_write_desc, .opve_impl = (VOPFUNC)ramfs_write },
	{ .opve_op = &vnop_ioctl_desc, .opve_impl = (VOPFUNC)ramfs_ioctl },
	{ .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)nop_fsync },
	{ .opve_op = &vnop_remove_desc, .opve_impl = (VOPFUNC)ramfs_remove },
	{ .opve_op = &vnop_link_desc, .opve_impl = (VOPFUNC)ramfs_link },
	{ .opve_op = &vnop_rename_desc, .opve_impl = (VOPFUNC)ramfs_rename },
	{ .opve_op = &vnop_renamex_desc, .opve_impl = (VOPFUNC)ramfs_renamex },
	{ .opve_op = &vnop_mkdir_desc, .opve_impl = (VOPFUNC)ramfs_mkdir },
	{ .opve_op = &vnop_rmdir_desc, .opve_impl = (VOPFUNC)ramfs_rmdir },
	{ .opve_op = &vnop_symlink_desc, .opve_impl = (VOPFUNC)ramfs_symlink },
	{ .opve_op = &vnop_readdir_desc, .opve_impl = (VOPFUNC)ramfs_readdir },
	{ .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)ramfs_readlink },
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)ramfs_inactive },
	{ .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)ramfs_reclaim },
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)ramfs_pathconf },
	{ .opve_op = &vnop_mmap_desc, .opve_impl = (VOPFUNC)ramfs_mmap },
	{ .opve_op = &vnop_mnomap_desc, .opve_impl = (VOPFUNC)ramfs_mnomap },
	{ .opve_op = &vnop_pagein_desc, .opve_impl = (VOPFUNC)ramfs_pagein },
	{ .opve_op = &vnop_pageout_desc, .opve_impl = (VOPFUNC)ramfs_pageout },
	{ .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)ramfs_getxattr },
	{ .opve_op = &vnop_setxattr_desc, .opve_impl = (VOPFUNC)ramfs_setxattr },
	{ .opve_op = &vnop_removexattr_desc, .opve_impl = (VOPFUNC)ramfs_removexattr },
	{ .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)ramfs_listxattr },
	{ .opve_op = &vnop_blktooff_desc, .opve_impl = (VOPFUNC)ramfs_blktooff },
	{ .opve_op = &vnop_offtoblk_desc, .opve_impl = (VOPFUNC)ramfs_offtoblk },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};
const struct vnodeopv_desc ramfs_vnodeop_opv_desc = {
	.opv_desc_vector_p = &ramfs_vnodeop_p,
	.opv_desc_ops = ramfs_vnodeop_entries
};

#if FIFO && SOCKETS
/* a named pipe's vnode: its data goes through fifofs' socket pair; its name, attributes and end are ours */
int(**ramfs_fifoop_p)(void *);
const struct vnodeopv_entry_desc ramfs_fifoop_entries[] = {
	{ .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)fifo_lookup },
	{ .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)fifo_open },
	{ .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)fifo_close },
	{ .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)ramfs_getattr },
	{ .opve_op = &vnop_setattr_desc, .opve_impl = (VOPFUNC)ramfs_setattr },
	{ .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)fifo_read },
	{ .opve_op = &vnop_write_desc, .opve_impl = (VOPFUNC)fifo_write },
	{ .opve_op = &vnop_ioctl_desc, .opve_impl = (VOPFUNC)fifo_ioctl },
	{ .opve_op = &vnop_select_desc, .opve_impl = (VOPFUNC)fifo_select },
	{ .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)nop_fsync },
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)ramfs_inactive },
	{ .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)ramfs_reclaim },
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)fifo_pathconf },
	{ .opve_op = &vnop_advlock_desc, .opve_impl = (VOPFUNC)err_advlock },
	{ .opve_op = &vnop_getxattr_desc, .opve_impl = (VOPFUNC)ramfs_getxattr },
	{ .opve_op = &vnop_setxattr_desc, .opve_impl = (VOPFUNC)ramfs_setxattr },
	{ .opve_op = &vnop_removexattr_desc, .opve_impl = (VOPFUNC)ramfs_removexattr },
	{ .opve_op = &vnop_listxattr_desc, .opve_impl = (VOPFUNC)ramfs_listxattr },
	{ .opve_op = (struct vnodeop_desc *)NULL, .opve_impl = (VOPFUNC)NULL }
};
const struct vnodeopv_desc ramfs_fifoop_opv_desc = {
	.opv_desc_vector_p = &ramfs_fifoop_p,
	.opv_desc_ops = ramfs_fifoop_entries
};
#endif

/* ------------------------------------------------------------------------ */
/* Mount operations                                                         */

/* mount(2) with no arguments: an empty filesystem appears. */
static int
ramfs_mount(mount_t mp, __unused vnode_t devvp, __unused user_addr_t data, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm;
	struct vfsstatfs *sp;

	if (vfs_isupdate(mp)) {
		return 0;
	}
	rm = kalloc_type(struct ramfs_mount, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	lck_mtx_init(&rm->rm_lock, &ramfs_lck_grp, LCK_ATTR_NULL);
	rm->rm_mp = mp;
	rm->rm_next_ino = RAMFS_ROOT_INO + 1;
	/*
	 * A runaway writer must not be able to take the whole machine: half of memory at most (pages are taken
	 * only as files are written). A quarter was too little once /private/var was kept between boots
	 * (var_store): its 580 MB left some 470 MB of 1 GB free, and Camera.app said "iPhone Storage Full"
	 * and would not capture.
	 */
	rm->rm_max_pages = (max_mem / 2) / PAGE_SIZE;

	rm->rm_hash = kalloc_type(struct ramfs_node *, RAMFS_HASH_SIZE, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	rm->rm_root = ramfs_node_alloc(rm, VDIR, NULL);
	rm->rm_root->rn_ino = RAMFS_ROOT_INO;
	ramfs_hash_insert(rm, rm->rm_root);
	rm->rm_root->rn_nlink = 2;
	rm->rm_root->rn_parent = rm->rm_root;
	rm->rm_nodes = 1;

	vfs_setfsprivate(mp, rm);
	vfs_setlocklocal(mp);
	/* ACLs and owner UUIDs: VFS keeps them in an extended attribute of the file */
	vfs_setextendedsecurity(mp);
	vfs_getnewfsid(mp);

	sp = vfs_statfs(mp);
	sp->f_bsize = PAGE_SIZE;
	sp->f_iosize = PAGE_SIZE;
	sp->f_blocks = rm->rm_max_pages;
	sp->f_bfree = sp->f_bavail = rm->rm_max_pages;
	sp->f_bused = 0;
	/* there is no table of nodes to run out of; report as many as there could be pages */
	sp->f_files = rm->rm_max_pages;
	sp->f_ffree = rm->rm_max_pages - 1;
	strlcpy(sp->f_mntfromname, "ramfs", sizeof(sp->f_mntfromname));
	return 0;
}

/* Free a directory's subtree; every vnode is gone, so nothing else can be looking. */
static void
ramfs_free_tree(struct ramfs_node *dir)
{
	struct ramfs_dirent *de;

	while ((de = TAILQ_FIRST(&dir->rn_entries)) != NULL) {
		struct ramfs_node *node = de->rd_node;

		TAILQ_REMOVE(&dir->rn_entries, de, rd_link);
		ramfs_dirent_free(de);
		if (node->rn_type == VDIR) {
			ramfs_free_tree(node);
			ramfs_node_free(node);
		} else if (--node->rn_nlink == 0) {
			ramfs_node_free(node);
		}
	}
}

static int
ramfs_unmount(mount_t mp, int mntflags, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	int error = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0);

	if (error) {
		return error;
	}
	lck_mtx_lock(&rm->rm_lock);
	ramfs_free_tree(rm->rm_root);
	ramfs_node_free(rm->rm_root);
	lck_mtx_unlock(&rm->rm_lock);

	vfs_setfsprivate(mp, NULL);
	lck_mtx_destroy(&rm->rm_lock, &ramfs_lck_grp);
	kfree_type(struct ramfs_node *, RAMFS_HASH_SIZE, rm->rm_hash);
	kfree_type(struct ramfs_mount, rm);
	return 0;
}

static int
ramfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	int error;

	do {
		lck_mtx_lock(&rm->rm_lock);
		error = ramfs_vget_locked(rm, rm->rm_root, NULLVP, NULL, vpp);
	} while (error == EAGAIN);
	return error;
}

/* A node by its number (fsgetpath, and whoever else asks by file ID): see ramfs_hash_insert(). */
static int
ramfs_vfs_vget(mount_t mp, ino64_t ino, vnode_t *vpp, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	int error;

	do {
		struct ramfs_node *node;

		lck_mtx_lock(&rm->rm_lock);
		for (node = rm->rm_hash[ino & (RAMFS_HASH_SIZE - 1)]; node != NULL && node->rn_ino != ino;
		    node = node->rn_hnext) {
		}
		if (node == NULL || node->rn_nlink == 0) {
			lck_mtx_unlock(&rm->rm_lock);
			return ENOENT;
		}
		error = ramfs_vget_locked(rm, node, NULLVP, NULL, vpp);
	} while (error == EAGAIN);
	return error;
}

static int
ramfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, __unused vfs_context_t ctx)
{
	struct ramfs_mount *rm = RAMFS_MP(mp);
	uint64_t used, nodes;

	lck_mtx_lock(&rm->rm_lock);
	used = rm->rm_pages;
	nodes = rm->rm_nodes;
	lck_mtx_unlock(&rm->rm_lock);

	VFSATTR_RETURN(fsap, f_objcount, nodes);
	VFSATTR_RETURN(fsap, f_bsize, PAGE_SIZE);
	VFSATTR_RETURN(fsap, f_iosize, PAGE_SIZE);
	VFSATTR_RETURN(fsap, f_blocks, rm->rm_max_pages);
	VFSATTR_RETURN(fsap, f_bfree, rm->rm_max_pages - MIN(used, rm->rm_max_pages));
	VFSATTR_RETURN(fsap, f_bavail, rm->rm_max_pages - MIN(used, rm->rm_max_pages));
	VFSATTR_RETURN(fsap, f_bused, used);
	VFSATTR_RETURN(fsap, f_files, rm->rm_max_pages);
	VFSATTR_RETURN(fsap, f_ffree, rm->rm_max_pages - MIN(nodes, rm->rm_max_pages));

	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		vol_capabilities_attr_t *cap = &fsap->f_capabilities;

		bzero(cap, sizeof(*cap));
		cap->capabilities[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    /*
		     * A file keeps its number for as long as it exists (the life of the mount, a boot), a node is
		     * found by it (vfs_vget) and a path made from it (fsgetpath): LaunchServices keeps an
		     * application by number, and takes none from a volume that does not say so -- everything a
		     * package installed under the prefix was "Unable to register", with nothing tried on its files.
		     */
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->capabilities[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY |
		    VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_RENAME_SWAP |
		    VOL_CAP_INT_RENAME_EXCL;
		cap->valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    VOL_CAP_FMT_SYMBOLICLINKS |
		    VOL_CAP_FMT_HARDLINKS |
		    VOL_CAP_FMT_JOURNAL |
		    VOL_CAP_FMT_JOURNAL_ACTIVE |
		    VOL_CAP_FMT_NO_ROOT_TIMES |
		    VOL_CAP_FMT_SPARSE_FILES |
		    VOL_CAP_FMT_ZERO_RUNS |
		    VOL_CAP_FMT_CASE_SENSITIVE |
		    VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    VOL_CAP_FMT_OPENDENYMODES |
		    VOL_CAP_FMT_HIDDEN_FILES |
		    VOL_CAP_FMT_PATH_FROM_ID;
		cap->valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_SEARCHFS |
		    VOL_CAP_INT_ATTRLIST |
		    VOL_CAP_INT_NFSEXPORT |
		    VOL_CAP_INT_READDIRATTR |
		    VOL_CAP_INT_EXCHANGEDATA |
		    VOL_CAP_INT_COPYFILE |
		    VOL_CAP_INT_ALLOCATE |
		    VOL_CAP_INT_VOL_RENAME |
		    VOL_CAP_INT_ADVLOCK |
		    VOL_CAP_INT_FLOCK |
		    VOL_CAP_INT_EXTENDED_SECURITY |
		    VOL_CAP_INT_USERACCESS |
		    VOL_CAP_INT_MANLOCK |
		    VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_NAMEDSTREAMS |
		    VOL_CAP_INT_RENAME_SWAP |
		    VOL_CAP_INT_RENAME_EXCL;
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	/*
	 * The rest of what getattrlist(2) may be asked about a volume. Unless the caller also asks
	 * which attributes came back, one that the filesystem leaves out fails the whole call with
	 * EINVAL, and callers take that badly: CacheDelete then hands assetsd a nil volume, which
	 * ends in an uncaught exception.
	 */
	VFSATTR_RETURN(fsap, f_filecount, nodes);
	VFSATTR_RETURN(fsap, f_dircount, 0);
	VFSATTR_RETURN(fsap, f_maxobjcount, rm->rm_max_pages);
	VFSATTR_RETURN(fsap, f_owner, 0);
	VFSATTR_RETURN(fsap, f_fssubtype, 0);
	VFSATTR_RETURN(fsap, f_signature, 0x4244);              /* 'BD', what the VFS layer defaults to */
	VFSATTR_RETURN(fsap, f_carbon_fsid, 0);
	VFSATTR_RETURN(fsap, f_quota, 0);
	VFSATTR_RETURN(fsap, f_reserved, 0);
	if (VFSATTR_IS_ACTIVE(fsap, f_fsid)) {
		fsap->f_fsid = vfs_statfs(mp)->f_fsid;
		VFSATTR_SET_SUPPORTED(fsap, f_fsid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_create_time) || VFSATTR_IS_ACTIVE(fsap, f_modify_time) ||
	    VFSATTR_IS_ACTIVE(fsap, f_access_time) || VFSATTR_IS_ACTIVE(fsap, f_backup_time)) {
		struct timeval boot;
		struct timespec since = { .tv_sec = 0, .tv_nsec = 0 };

		/* nothing here is older than this boot */
		boottime_timeval(&boot);
		since.tv_sec = boot.tv_sec;
		VFSATTR_RETURN(fsap, f_create_time, since);
		VFSATTR_RETURN(fsap, f_modify_time, since);
		VFSATTR_RETURN(fsap, f_access_time, since);
		since.tv_sec = 0;
		VFSATTR_RETURN(fsap, f_backup_time, since);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_uuid)) {
		/* the same at every boot as long as the mounts are made in the same order: programs key caches by it */
		static const uuid_t base = { 'r', 'e', 'd', 'w', 'o', 'o', 0x50, 'd', 0x80, 'R', 0, 0, 0, 0, 0, 0 };
		fsid_t fsid = vfs_statfs(mp)->f_fsid;

		memcpy(fsap->f_uuid, base, sizeof(uuid_t));
		memcpy(&fsap->f_uuid[10], &fsid.val[0], 4);
		fsap->f_uuid[14] = (uint8_t)fsid.val[1];
		VFSATTR_SET_SUPPORTED(fsap, f_uuid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
		vol_attributes_attr_t *attr = &fsap->f_attributes;

		attr->validattr.commonattr = ATTR_CMN_VALIDMASK;
		attr->validattr.volattr = ATTR_VOL_VALIDMASK;
		attr->validattr.dirattr = ATTR_DIR_VALIDMASK;
		attr->validattr.fileattr = ATTR_FILE_VALIDMASK;
		attr->validattr.forkattr = 0;
		attr->nativeattr = attr->validattr;
		VFSATTR_SET_SUPPORTED(fsap, f_attributes);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		strlcpy(fsap->f_vol_name, "ramfs", MAXPATHLEN);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	return 0;
}

/*
 * "On My iPhone" in Files: fileproviderd keeps its library only on a volume it takes for APFS -- local, writable and
 * of the type name "apfs" (-[FPDVolume isEligibleForFPFS:]); on any other it says "Non-eligible device ... non-apfs",
 * skips it, and the local domain is never made, so Files shows no "On My iPhone" at all. /private/var is this memory
 * filesystem, local and writable but named "ramfs". So to fileproviderd, and to nobody else, it is named "apfs":
 * statfs(2), getfsstat(2) and getattrlist(2) take the name from here (bsd/vfs/vfs_subr.c,
 * vfs_getfstypenameref_locked), while the kernel's own checks read f_fstypename and still see "ramfs" -- the APFS
 * driver never takes this volume for one of its own -- and every other program sees "ramfs" too, so none of the
 * daemons that do something else on APFS (cloning, snapshots, purgeable space) is sent down that way. What APFS alone
 * does, fileproviderd is refused cleanly (ramfs_ioctl() above, VNOP_CLONEFILE not implemented: ENOTSUP).
 * After the OnePlus Nord 2 port of iOS (frxncyyy/ios-denniz, the idea of its piece 0734), which found the check and
 * named its root volume "apfs" to every program once fileproviderd had started.
 *
 * Boot-arg var_apfs=0: /private/var is "ramfs" to fileproviderd as well, and Files has no "On My iPhone".
 */
static TUNABLE(uint32_t, ramfs_var_apfs, "var_apfs", 1);
static char ramfs_apfs_name[] = "apfs";
static bool ramfs_apfs_said;            /* (under the mount's lock, as the one mount it is said of) */
extern struct vfsops ramfs_vfsops;

/* (called by vfs_getfstypenameref_locked() with the mount's spin lock held: nothing here waits) */
char *ramfs_fstypename_shown(mount_t mp, char *name);

char *
ramfs_fstypename_shown(mount_t mp, char *name)
{
	proc_t p = current_proc();

	if (!ramfs_var_apfs || mp->mnt_op != &ramfs_vfsops || p == PROC_NULL ||
	    strcmp(mp->mnt_vfsstat.f_mntonname, "/private/var") != 0 || strcmp(proc_best_name(p), "fileproviderd") != 0) {
		return name;
	}
	if (!ramfs_apfs_said) {
		ramfs_apfs_said = true;
		printf("ramfs: /private/var named \"apfs\" to %s[%d], for \"On My iPhone\" (var_apfs=0: not)\n",
		    proc_best_name(p), proc_getpid(p));
	}
	return ramfs_apfs_name;
}

static int
ramfs_sync(__unused mount_t mp, __unused int waitfor, __unused vfs_context_t ctx)
{
	return 0;
}

static int
ramfs_init(__unused struct vfsconf *vfsc)
{
	return 0;
}

struct vfsops ramfs_vfsops = {
	.vfs_mount = ramfs_mount,
	.vfs_unmount = ramfs_unmount,
	.vfs_root = ramfs_root,
	.vfs_vget = ramfs_vfs_vget,
	.vfs_getattr = ramfs_vfs_getattr,
	.vfs_sync = ramfs_sync,
	.vfs_init = ramfs_init,
};
