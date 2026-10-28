/*
 * The driver's core as C, for hakios's Metal plug-in (userland/src/HakiosMetal.m), which is C and Objective-C and does
 * not take C++ classes. Every object is an opaque pointer, every call says by its result whether it worked, and a
 * failure leaves its reason for hakios_metal_error().
 *
 *   device          the GPU, or NULL when the kernel has none to give (no gpu=1, no firmware, QEMU)
 *   buffer          GPU memory with its CPU mapping (write-combined) and its GPU address
 *   queue           command buffers complete in commit order; their callbacks run on the queue's own thread
 *   command buffer  copy and fill, done by the GPU's command processor; commit; wait
 */
#ifndef HAKIOS_METAL_H
#define HAKIOS_METAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hakios_metal_device hakios_metal_device;
typedef struct hakios_metal_buffer hakios_metal_buffer;
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

/* the last failure on this thread, in words */
const char *hakios_metal_error(void);

hakios_metal_device *hakios_metal_open(void);
/* (for the tests: a device on the fake GPU, which runs the packets in this process) */
hakios_metal_device *hakios_metal_open_fake(void);
void hakios_metal_close(hakios_metal_device *device);
uint64_t hakios_metal_gpu_id(hakios_metal_device *device);

hakios_metal_buffer *hakios_metal_buffer_new(hakios_metal_device *device, uint64_t length);
void hakios_metal_buffer_free(hakios_metal_buffer *buffer);
void *hakios_metal_buffer_contents(hakios_metal_buffer *buffer);
uint64_t hakios_metal_buffer_length(hakios_metal_buffer *buffer);
uint64_t hakios_metal_buffer_gpu_address(hakios_metal_buffer *buffer);

hakios_metal_queue *hakios_metal_queue_new(hakios_metal_device *device);
void hakios_metal_queue_free(hakios_metal_queue *queue);

hakios_metal_cb *hakios_metal_cb_new(hakios_metal_queue *queue);
/* the command buffer is released by its owner; one still in flight is waited for first. It may be freed inside its
 * own callback (the callback is the last thing to use the pointer). */
void hakios_metal_cb_free(hakios_metal_cb *cb);
int hakios_metal_cb_copy(hakios_metal_cb *cb, hakios_metal_buffer *from, uint64_t from_offset, hakios_metal_buffer *to,
    uint64_t to_offset, uint64_t length);
int hakios_metal_cb_fill(hakios_metal_cb *cb, hakios_metal_buffer *buffer, uint64_t offset, uint64_t length, uint8_t value);
/* something the caller could not record: the command buffer will end in HAKIOS_METAL_ERROR, saying WHY */
void hakios_metal_cb_fail(hakios_metal_cb *cb, const char *why);
/* CALLBACK(CB, CONTEXT) runs once, after completion or error, on the queue's thread; may be NULL */
void hakios_metal_cb_commit(hakios_metal_cb *cb, void (*callback)(hakios_metal_cb *cb, void *context), void *context);
void hakios_metal_cb_wait(hakios_metal_cb *cb);
enum hakios_metal_status hakios_metal_cb_status(hakios_metal_cb *cb);
/* why it ended in HAKIOS_METAL_ERROR, or "" */
const char *hakios_metal_cb_error(hakios_metal_cb *cb);

#ifdef __cplusplus
}
#endif

#endif
