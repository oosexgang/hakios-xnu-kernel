#include "../fakekernel.h"
/*
 * Vnodes (sys/vnode.h), as far as a driver reads a file of the root whole (board/adsp.c): there is no file
 * system here -- no root, and a lookup finds nothing -- so a driver goes the way it goes before the root is
 * mounted.
 */
#ifndef FAKEKERNEL_SYS_VNODE_H
#define FAKEKERNEL_SYS_VNODE_H
typedef struct fake_vnode *vnode_t;
typedef struct fake_vfs_context *vfs_context_t;
#define NULLVP                          ((vnode_t)NULL)
struct vnode_attr {
	uint64_t va_data_size;
};
#define VATTR_INIT(attributes)          memset((attributes), 0, sizeof(*(attributes)))
#define VATTR_WANTED(attributes, field) ((void)(attributes))
enum uio_rw { UIO_READ, UIO_WRITE };
enum uio_seg { UIO_USERSPACE, UIO_SYSSPACE };
static vnode_t rootvnode __attribute__((unused)) = NULLVP;
static inline vfs_context_t vfs_context_kernel(void) { return NULL; }
static inline kauth_cred_t vfs_context_ucred(vfs_context_t context __unused) { return NULL; }
static inline proc_t vfs_context_proc(vfs_context_t context __unused) { return NULL; }
static inline int vnode_lookup(const char *path __unused, int flags __unused, vnode_t *vnode __unused, vfs_context_t context __unused)
{
	return ENOENT;
}
static inline int vnode_getattr(vnode_t vnode __unused, struct vnode_attr *attributes __unused, vfs_context_t context __unused)
{
	return ENOENT;
}
static inline int vnode_put(vnode_t vnode __unused) { return 0; }
static inline int
vn_rdwr(enum uio_rw rw __unused, vnode_t vnode __unused, caddr_t base __unused, int length __unused, off_t offset __unused,
    enum uio_seg segment __unused, int flags __unused, kauth_cred_t cred __unused, int *left __unused, proc_t proc __unused)
{
	return EIO;
}
#endif
