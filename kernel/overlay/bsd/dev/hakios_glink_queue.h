/*
 * The two queues of bytes GLINK runs over in the processors' shared memory (Linux:
 * qcom_glink_smem.c): the arithmetic of their marks, once. Each side moves the head of its own queue
 * and the tail of the other's. A mark the other side moves is read from memory it can write at any
 * time: nothing here trusts one to be inside its queue.
 *
 * What goes through the queues is a message of a command and two words, and what follows some of them:
 * their numbers, and how long each is, are here too. Whose doorbell is rung, and what is done with each,
 * is each driver's.
 */
#ifndef HAKIOS_GLINK_QUEUE_H
#define HAKIOS_GLINK_QUEUE_H

#include <mach/mach_types.h>

#define GLINK_OUR_TAIL          0               /* the descriptor's words: how far the other side has read our queue (its to move) */
#define GLINK_OUR_HEAD          4               /* how far we have written it */
#define GLINK_THEIR_TAIL        8               /* how far we have read the other side's queue */
#define GLINK_THEIR_HEAD        12              /* how far it has written it (its to move) */
#define GLINK_DESCRIPTOR_SIZE   32
#define GLINK_RESERVE           16              /* of our queue, never filled (Linux's reserve, and room for one short message) */
#define GLINK_SMALLEST_QUEUE    1024

/* Are two queues of those sizes of a shape this knows: large enough, and whole messages of eight bytes? */
static inline bool
glink_queues_usable(uint32_t ours_size, uint32_t theirs_size)
{
	return ours_size >= GLINK_SMALLEST_QUEUE && theirs_size >= GLINK_SMALLEST_QUEUE && !(ours_size & 7) && !(theirs_size & 7);
}

/* How much of a queue is written and not yet read; 0 if a mark is not inside it. */
static inline uint32_t
glink_queue_unread(uint32_t head, uint32_t tail, uint32_t size)
{
	if (head >= size || tail >= size) {
		return 0;
	}
	return head >= tail ? head - tail : size - tail + head;
}

/* How much may be written into a queue now, the reserve left out; 0 if a mark is not inside it. */
static inline uint32_t
glink_queue_room(uint32_t head, uint32_t tail, uint32_t size)
{
	uint32_t room;

	if (head >= size || tail >= size) {
		return 0;
	}
	room = tail <= head ? size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

/* `count' bytes out of a queue, from `offset' behind its tail on, around its end. */
static inline void
glink_queue_peek(const volatile uint8_t *queue, uint32_t size, uint32_t tail, uint32_t offset, void *to, uint32_t count)
{
	uint32_t at = (tail % size + offset % size) % size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = queue[(at + i) % size];
	}
}

/* Where a mark is after `count' more bytes. */
static inline uint32_t
glink_queue_after(uint32_t mark, uint32_t count, uint32_t size)
{
	return (mark % size + count % size) % size;
}

/*
 * A message into a queue at its head, in two pieces, filled up with zeros to eight bytes. Returns
 * how many bytes that was: what the head is to be moved by, once the bytes are there.
 */
static inline uint32_t
glink_queue_put(volatile uint8_t *queue, uint32_t size, uint32_t head, const void *first, uint32_t first_length,
    const void *second, uint32_t second_length)
{
	uint32_t length = first_length + second_length, padded = (length + 7) & ~7u;

	for (uint32_t i = 0; i < padded; i++) {
		queue[(head % size + i) % size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	return padded;
}

/* What goes through the queues: messages of a command and two words, and what follows some of them. */
enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};

/*
 * How many bytes the message at the head of the other side's queue takes, filled up to eight: its eight,
 * and a channel's name after an OPEN (param2 bytes), the intents after an INTENT (param2 of eight bytes),
 * a piece of data after DATA (eight more, then `piece' bytes: the first word after the message). The
 * counts are the other side's: the sum is 64 bits wide, for in 32 it wraps -- 2^29 intents are 0 bytes.
 */
static inline uint64_t
glink_message_length(const struct glink_message *message, uint32_t piece)
{
	uint64_t whole = sizeof(*message);

	if (message->command == GLINK_OPEN) {
		whole += message->param2;
	} else if (message->command == GLINK_INTENT) {
		whole += 8ull * message->param2;
	} else if (message->command == GLINK_DATA || message->command == GLINK_DATA_MORE) {
		whole = 16ull + piece;
	}
	return (whole + 7) & ~7ull;
}

#endif /* HAKIOS_GLINK_QUEUE_H */
