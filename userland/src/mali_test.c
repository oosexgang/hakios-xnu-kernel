/*
 * mali_test: the Mali through the kernel's RedwoodMali service, the way Mesa's panfrost will reach
 * it, before there is any Mesa: open the service, ask a few parameters, make a buffer, map it,
 * write a job into it, submit it, and see the GPU's work in the buffer.
 *
 * The job is the smallest there is -- write a 32-bit value -- and the buffer is both the job and
 * the place it writes to, at GPU addresses the kernel's page tables gave it. Every step prints what
 * it got; the last line says whether the GPU did the work.
 *
 * The request numbers and structures are Linux's panfrost ones (include/uapi/drm/panfrost_drm.h),
 * as tools/mesa/redwood-drm will send them for Mesa.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>

struct drm_panfrost_get_param { uint32_t param, pad; uint64_t value; };
struct drm_panfrost_create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct drm_panfrost_submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct drm_gem_close { uint32_t handle, pad; };

static io_connect_t connection;

/* The outcome for the kernel too, which repeats it where a run's log is kept (kern.redwood.mali_user). */
static int
said(uint32_t what)
{
	sysctlbyname("kern.redwood.mali_user", NULL, NULL, &what, sizeof(what));
	return what == 1 ? 0 : 1;
}

static kern_return_t
call(uint32_t number, void *data, size_t size)
{
	size_t returned = size;

	return IOConnectCallStructMethod(connection, number, data, size, data, &returned);
}

int
main(void)
{
	io_service_t service;
	struct drm_panfrost_get_param param;
	struct drm_panfrost_create_bo bo;
	struct drm_panfrost_submit submit;
	struct drm_gem_close close_bo;
	mach_vm_address_t where = 0;
	mach_vm_size_t size = 0;
	volatile uint32_t *job;
	kern_return_t kr;
	int ok;

	setvbuf(stdout, NULL, _IONBF, 0);       /* every line out as it is printed: a crash must not take them along */
	printf("mali_test: started\n");
	service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodMali"));
	if (service == 0) {
		printf("mali_test: there is no RedwoodMali service (boot-arg mali=1?)\n");
		return said(2);
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		printf("mali_test: cannot open the service: %#x\n", kr);
		return said(3);
	}
	for (uint32_t which = 0; which < 3; which++) {
		memset(&param, 0, sizeof(param));
		param.param = which;
		kr = call(0x44, &param, sizeof(param));
		printf("mali_test: parameter %u = %#llx (%#x)\n", which, (unsigned long long)param.value, kr);
	}

	memset(&bo, 0, sizeof(bo));
	bo.size = 4096;
	kr = call(0x42, &bo, sizeof(bo));
	printf("mali_test: a buffer: handle %u at GPU address %#llx (%#x)\n", bo.handle, (unsigned long long)bo.offset, kr);
	if (kr != KERN_SUCCESS) {
		return said(4);
	}
	kr = IOConnectMapMemory64(connection, bo.handle, mach_task_self(), &where, &size, kIOMapAnywhere);
	printf("mali_test: mapped at %#llx, %llu bytes (%#x)\n", (unsigned long long)where, (unsigned long long)size, kr);
	if (kr != KERN_SUCCESS) {
		return said(5);
	}

	job = (volatile uint32_t *)where;
	job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
	job[8] = (uint32_t)(bo.offset + 0x800);
	job[9] = (uint32_t)((bo.offset + 0x800) >> 32);
	job[10] = 6;                            /* a 32-bit immediate */
	job[12] = 0x6d616c69u;                  /* "mali" */
	job[0x200] = 0;
	__asm__ volatile ("dsb sy");

	memset(&submit, 0, sizeof(submit));
	submit.jc = bo.offset;
	kr = call(0x40, &submit, sizeof(submit));
	ok = job[0x200] == 0x6d616c69u;
	printf("mali_test: submitted (%#x); the job says %#x, the word reads %#x%s\n", kr, job[0], job[0x200],
	    ok ? " -- THE GPU DID IT, FROM USER SPACE" : " -- not written");

	close_bo.handle = bo.handle;
	close_bo.pad = 0;
	IOConnectUnmapMemory64(connection, bo.handle, mach_task_self(), where);
	call(0x09, &close_bo, sizeof(close_bo));
	IOServiceClose(connection);
	return said(ok ? 1 : 6);
}
