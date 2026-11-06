/*
 * Remote processors: here the one that runs the radio's firmware. Starting and stopping it is
 * the kernel's business (lx_os_rproc_boot()); the driver only asks.
 */
#ifndef _LX_LINUX_REMOTEPROC_H
#define _LX_LINUX_REMOTEPROC_H

#include <linux/types.h>
#include <linux/of.h>

struct rproc {
	const char *name;
	phandle phandle;
};

struct rproc *rproc_get_by_phandle(phandle handle);
int rproc_boot(struct rproc *rproc);
int rproc_shutdown(struct rproc *rproc);

#endif
