/*
 * For the tests only: just enough of the names ath11k's qmi.h uses outside its message
 * definitions, so that the header can be read as it is by a compiler on this machine. Nothing here
 * does anything; the driver itself is not compiled by the tests.
 */
#ifndef QMI_TEST_SHIM_H
#define QMI_TEST_SHIM_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
#define __iomem

struct list_head {
	struct list_head *next, *prev;
};
struct work_struct {
	int unused;
};
struct workqueue_struct;
typedef struct {
	int unused;
} spinlock_t;
typedef struct {
	int unused;
} wait_queue_head_t;

#endif
