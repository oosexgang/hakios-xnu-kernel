/*
 * The driver's core as C, for hakios's Metal plug-in (userland/src/HakiosMetal.m), which is C and Objective-C and does
 * not take C++ classes. Every object is an opaque pointer, every call says by its result whether it worked, and a
 * failure leaves its reason for hakios_metal_error().
 *
 *   device          the GPU, or NULL when the kernel has none to give (no gpu=1, no firmware, QEMU)
 *   buffer          GPU memory with its CPU mapping (write-combined) and its GPU address
 *   library         a .metallib taken apart: its functions by name, each one's AIR
 *   kernel          a compute function compiled for this GPU, in this process (the compiler is built in with MESA=1)
 *   queue           command buffers complete in commit order; their callbacks run on the queue's own thread
 *   command buffer  copy and fill, done by the GPU's command processor; a kernel dispatched over a grid; commit; wait
 */
#ifndef HAKIOS_METAL_H
#define HAKIOS_METAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hakios_metal_device hakios_metal_device;
typedef struct hakios_metal_buffer hakios_metal_buffer;
typedef struct hakios_metal_library hakios_metal_library;
typedef struct hakios_metal_kernel hakios_metal_kernel;
typedef struct hakios_metal_queue hakios_metal_queue;
typedef struct hakios_metal_cb hakios_metal_cb;

enum hakios_metal_status {
	HAKIOS_METAL_NOT_ENQUEUED = 0,  /* as MTLCommandBufferStatus numbers them */
	HAKIOS_METAL_ENQUEUED = 1,
	HAKIOS_METAL_COMMITTED = 2,
	HAKIOS_METAL_SCHEDULED = 3,
	HAKIOS_METAL_COMPLETED = 4,
	HAKIOS_METAL_ERROR = 5,
};

/* a function's type in a .metallib, as the container's TYPE tag numbers them (MTLFunctionType is these plus one) */
enum hakios_metal_function_type {
	HAKIOS_METAL_FUNCTION_VERTEX = 0,
	HAKIOS_METAL_FUNCTION_FRAGMENT = 1,
	HAKIOS_METAL_FUNCTION_KERNEL = 2,
};

/* what a compiled kernel binds (the plug-in's reflection, and what the encoder must be given) */
enum hakios_metal_resource_kind {
	HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER = 0,   /* [[buffer(n)]] of constant address space */
	HAKIOS_METAL_RESOURCE_DEVICE_BUFFER = 1,     /* [[buffer(n)]] of device address space */
	HAKIOS_METAL_RESOURCE_THREADGROUP_BUFFER = 2,
	HAKIOS_METAL_RESOURCE_TEXTURE = 3,           /* not served yet: such a kernel compiles and is refused at dispatch */
	HAKIOS_METAL_RESOURCE_IMAGE = 4,
	HAKIOS_METAL_RESOURCE_SAMPLER = 5,
};

/* the last failure on this thread, in words */
const char *hakios_metal_error(void);

hakios_metal_device *hakios_metal_open(void);
/* (for the tests: a device on the fake GPU, which runs the packets in this process) */
hakios_metal_device *hakios_metal_open_fake(void);
void hakios_metal_close(hakios_metal_device *device);
/* the chip id (0x06030500 on the phone: the kernel's GPU_ID is 0, the chip id is what names the GPU) */
uint64_t hakios_metal_gpu_id(hakios_metal_device *device);
/* whether kernels can be compiled in this build (MESA=1); without it hakios_metal_kernel_compile* refuse by name */
int hakios_metal_has_compiler(void);

hakios_metal_buffer *hakios_metal_buffer_new(hakios_metal_device *device, uint64_t length);
void hakios_metal_buffer_free(hakios_metal_buffer *buffer);
void *hakios_metal_buffer_contents(hakios_metal_buffer *buffer);
uint64_t hakios_metal_buffer_length(hakios_metal_buffer *buffer);
uint64_t hakios_metal_buffer_gpu_address(hakios_metal_buffer *buffer);

/* a .metallib's bytes (thin or universal), copied; NULL when the file does not hold together, with the reason */
hakios_metal_library *hakios_metal_library_open(const void *data, size_t size);
void hakios_metal_library_free(hakios_metal_library *library);
unsigned hakios_metal_library_count(hakios_metal_library *library);
const char *hakios_metal_library_name(hakios_metal_library *library, unsigned i);
int hakios_metal_library_type(hakios_metal_library *library, unsigned i);   /* enum hakios_metal_function_type */

/* FUNCTION of LIBRARY compiled for this GPU (AIR to NIR to ir3, here); NULL with the reason: no such function, not a
 * compute kernel, the front end's or ir3's refusal by name, or no compiler in this build */
hakios_metal_kernel *hakios_metal_kernel_compile(hakios_metal_library *library, const char *function);
/* the same from a module's bitcode (a .bc, or a function's AIR as Metal hands it out: -[_MTLFunction bitcodeData]) */
hakios_metal_kernel *hakios_metal_kernel_compile_bitcode(const void *bitcode, size_t size);
void hakios_metal_kernel_free(hakios_metal_kernel *kernel);
const char *hakios_metal_kernel_name(hakios_metal_kernel *kernel);
unsigned hakios_metal_kernel_resource_count(hakios_metal_kernel *kernel);
/* resource I: its kind, Metal index and declared size in bytes (0 when the metadata gives none); -1 past the count */
int hakios_metal_kernel_resource(hakios_metal_kernel *kernel, unsigned i, unsigned *kind, unsigned *index, unsigned *size_bytes);
/* the threadgroup size the kernel fixed, or 0 0 0 when it is the dispatch's to choose (every Metal kernel so far) */
void hakios_metal_kernel_threadgroup_size(hakios_metal_kernel *kernel, unsigned size[3]);
/* MTLComputePipelineState's numbers: the most threads a threadgroup may have, and a SIMD group's width */
unsigned hakios_metal_kernel_max_threads(hakios_metal_kernel *kernel);
unsigned hakios_metal_kernel_execution_width(hakios_metal_kernel *kernel);
unsigned hakios_metal_kernel_threadgroup_memory(hakios_metal_kernel *kernel);   /* bytes it declares statically */

hakios_metal_queue *hakios_metal_queue_new(hakios_metal_device *device);
void hakios_metal_queue_free(hakios_metal_queue *queue);

hakios_metal_cb *hakios_metal_cb_new(hakios_metal_queue *queue);
/* the command buffer is released by its owner; one still in flight is waited for first. It may be freed inside its
 * own callback (the callback is the last thing to use the pointer). */
void hakios_metal_cb_free(hakios_metal_cb *cb);
int hakios_metal_cb_copy(hakios_metal_cb *cb, hakios_metal_buffer *from, uint64_t from_offset, hakios_metal_buffer *to,
    uint64_t to_offset, uint64_t length);
int hakios_metal_cb_fill(hakios_metal_cb *cb, hakios_metal_buffer *buffer, uint64_t offset, uint64_t length, uint8_t value);
/* KERNEL over GROUPS threadgroups of THREADS threads each (MTLComputeCommandEncoder's dispatchThreadgroups:
 * threadsPerThreadgroup:), with BUFFERS[i] at OFFSETS[i] bound at Metal index i for i < COUNT (NULL: nothing there).
 * -1 with the reason when the kernel and the bindings do not fit together; the kernel and the buffers stay alive
 * until the command buffer completes, whatever the caller frees meanwhile. */
int hakios_metal_cb_dispatch(hakios_metal_cb *cb, hakios_metal_kernel *kernel, const unsigned groups[3], const unsigned threads[3],
    hakios_metal_buffer *const *buffers, const uint64_t *offsets, unsigned count);
/* something the caller could not record: the command buffer will end in HAKIOS_METAL_ERROR, saying WHY */
void hakios_metal_cb_fail(hakios_metal_cb *cb, const char *why);
/* CALLBACK(CB, CONTEXT) runs once, after completion or error, on the queue's thread; may be NULL */
void hakios_metal_cb_commit(hakios_metal_cb *cb, void (*callback)(hakios_metal_cb *cb, void *context), void *context);
void hakios_metal_cb_wait(hakios_metal_cb *cb);
enum hakios_metal_status hakios_metal_cb_status(hakios_metal_cb *cb);
/* why it ended in HAKIOS_METAL_ERROR, or "" */
const char *hakios_metal_cb_error(hakios_metal_cb *cb);
/* how long the GPU had it, in nanoseconds (commit to fence), once completed */
int64_t hakios_metal_cb_gpu_nanoseconds(hakios_metal_cb *cb);

#ifdef __cplusplus
}
#endif

#endif
