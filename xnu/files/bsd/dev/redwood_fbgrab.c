/*
 * sysctl kern.redwood.fbgrab: the picture on the panel, whole (what fbshot puts in the log 60 to 130 pixels wide).
 * Read it with a buffer big enough: 16 bytes -- width, height, bytes a row, 'BGRA' -- and the rows, B G R A in memory
 * (src/fbgrab.m reads it, makes a PNG and sends it over the network to tools/fbrecv.py). EINVAL if no frame has been shown yet.
 */
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern bool RedwoodFramebufferGrab(uint8_t *out, uint32_t max, uint32_t *width, uint32_t *height, uint32_t *stride);        /* iokit/Kernel/MiniIOMobileFramebuffer.cpp */

#define FBGRAB_MAX      (3000u * 3200u * 4u)

static int
fbgrab_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	uint32_t header[4], width = 0, height = 0, stride = 0;
	uint8_t *pixels;
	int error;

	pixels = kalloc_data(FBGRAB_MAX, Z_WAITOK);
	if (pixels == NULL) {
		return ENOMEM;
	}
	if (!RedwoodFramebufferGrab(pixels, FBGRAB_MAX, &width, &height, &stride)) {
		kfree_data(pixels, FBGRAB_MAX);
		return EINVAL;
	}
	header[0] = width;
	header[1] = height;
	header[2] = stride;
	header[3] = 0x41524742;         /* "BGRA" */
	error = SYSCTL_OUT(req, header, sizeof(header));
	if (error == 0) {
		error = SYSCTL_OUT(req, pixels, (size_t) stride * height);
	}
	kfree_data(pixels, FBGRAB_MAX);
	return error;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, fbgrab, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, fbgrab_sysctl, "S",
    "the picture on the panel, whole: width, height, stride, 'BGRA', rows");
