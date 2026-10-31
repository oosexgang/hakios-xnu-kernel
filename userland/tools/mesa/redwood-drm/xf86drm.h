/*
 * What Mesa's freedreno takes from libdrm's xf86drm.h, for a system without Linux's DRM: the
 * declarations here, the functions in redwood_drm.c, which carry the requests to the kernel's
 * GPU driver (the service RedwoodGPU; iokit/Kernel/RedwoodGPU.cpp) as calls of its user client.
 * tools/mk-mesa.sh puts both into Mesa's tree, where they are the "libdrm" freedreno is built
 * against. The request numbers and their structures are Linux's (Mesa's include/drm-uapi).
 */
#ifndef REDWOOD_XF86DRM_H
#define REDWOOD_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/*
 * Buffers and fences as file descriptors: there are none here. But memory that somebody else
 * has made does come in, as Linux's comes in by file descriptor: an IOSurface, by its ID, where
 * freedreno expects the descriptor (see redwood_drm_surface() below).
 */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/* Not libdrm's: a buffer's memory in this process, in place of mmap() on the device (freedreno_bo.c is patched to call these). */
void *redwood_drm_map(int fd, uint32_t handle, uint64_t size);
void redwood_drm_unmap(int fd, uint32_t handle, void *address);

/*
 * Not libdrm's either: what the memory of an IOSurface would be like as a buffer (the kernel's
 * request and its answer: struct redwood_surface in iokit/Kernel/RedwoodGPU.cpp); 0, or -1 with
 * errno. The buffer itself is made by drmPrimeFDToHandle(), which freedreno calls with its
 * table of buffers locked, so that a buffer on its way out and the same surface on its way in
 * cannot pass each other.
 */
struct redwood_drm_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* (the kernel's: which of the two is asked) */
	uint64_t size;                          /* back: how big the buffer is, */
	uint32_t offset, row_bytes;             /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixel_format, planes;   /* and what they are */
};
int redwood_drm_surface(int fd, struct redwood_drm_surface *surface);

#endif
