/*
 * Bluetooth: the two queues between bluetoothd and the controller (board/bt_host.c), as data alone -- no locks, no
 * hardware -- so that a test runs them on a host (workbench/bt_queue_test.c). The caller holds its lock around each.
 *
 *   a line   what bluetoothd wrote, bytes in the order they came, whole H4 packets taken from its front (and a
 *            run that cannot be a host's thrown away, board/bt_hci.h's bt_h4_from_host);
 *   a ring   whole packets for bluetoothd, each put in whole or not at all, taken out whole in the order they came.
 */
#ifndef BOARD_BT_QUEUE_H
#define BOARD_BT_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bt_hci.h"

struct bt_line {
	uint8_t *bytes;
	unsigned size, count;
};

/* N bytes at the line's end: false, and nothing of them put, when there is no room for all of them */
static inline bool
bt_line_put(struct bt_line *l, const uint8_t *bytes, unsigned n)
{
	if (n > l->size - l->count) {
		return false;
	}
	memcpy(l->bytes + l->count, bytes, n);
	l->count += n;
	return true;
}

/*
 * The whole packet at the line's front, taken into OUT (ROOM bytes): its length; 0 while none is whole; -1 when what is
 * there cannot be a host's packets, or is a packet longer than ROOM -- the line is emptied, *DROPPED counts its bytes.
 */
static inline int
bt_line_take(struct bt_line *l, uint8_t *out, unsigned room, uint32_t *dropped)
{
	int n = bt_h4_from_host(l->bytes, l->count, l->size);

	if (n < 0 || (unsigned)n > room) {
		*dropped += l->count;
		l->count = 0;
		return -1;
	}
	if (n == 0) {
		return 0;
	}
	memcpy(out, l->bytes, (unsigned)n);
	memmove(l->bytes, l->bytes + n, l->count - (unsigned)n);
	l->count -= (unsigned)n;
	return n;
}

struct bt_ring {
	uint8_t *bytes;
	unsigned size, head, count;
};

/* A whole packet of LENGTH bytes at the ring's end: false, and nothing of it put, when there is no room for all of it */
static inline bool
bt_ring_put(struct bt_ring *r, const uint8_t *packet, unsigned length)
{
	if (length == 0 || length > r->size - r->count) {
		return false;
	}
	for (unsigned i = 0; i < length; i++) {
		r->bytes[(r->head + r->count + i) % r->size] = packet[i];
	}
	r->count += length;
	return true;
}

/* The length of the packet at the ring's front, 0 when it is empty (its header read across the ring's end) */
static inline unsigned
bt_ring_next(const struct bt_ring *r)
{
	uint8_t header[5];
	unsigned have = r->count < 5 ? r->count : 5;

	for (unsigned i = 0; i < have; i++) {
		header[i] = r->bytes[(r->head + i) % r->size];
	}
	return bt_h4_length(header, have);
}

/* The packet at the ring's front, gone (copied into OUT unless it is NULL): its length, 0 when there was none */
static inline unsigned
bt_ring_take(struct bt_ring *r, uint8_t *out)
{
	unsigned length = bt_ring_next(r);

	if (length == 0 || length > r->count) {
		r->head = r->count = 0;         /* (cannot be: only whole packets are put; nothing is made of what is there) */
		return 0;
	}
	for (unsigned i = 0; out != NULL && i < length; i++) {
		out[i] = r->bytes[(r->head + i) % r->size];
	}
	r->head = (r->head + length) % r->size;
	r->count -= length;
	return length;
}

#endif /* BOARD_BT_QUEUE_H */
