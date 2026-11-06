/*
 * QMI: the helpers of sys/redwood_qmi.h -- messages coded by tables, a handle on the IPC router,
 * requests as transactions, indications to handlers.
 *
 * The interface is that of Linux's QMI helpers, because the driver that uses it (ath11k's qmi.c)
 * was written for them and is compiled as it is. The code is not Linux's: it is written from the
 * interface's meaning and from the public descriptions of the wire format (a 7-byte header, then
 * type-length-value triples; see below at "the coding").
 *
 * Where it deliberately does not behave as Linux's does, each of them for a message that is not
 * well formed (a message Linux decodes right is decoded the same here):
 *   - a TLV is taken to be as long as it says: what its element does not use is skipped, and an
 *     element that needs more than its TLV has is an error. (Linux reads on past the TLV.)
 *   - a TLV of a type the table does not have is skipped, whatever its type. (Linux refuses the
 *     message if the type is below 0x10.)
 *   - a string that would fill its C buffer to the last byte is an error: the buffer is elem_len
 *     bytes and ends with a zero. (Linux writes the zero one byte past it.)
 *
 * The same file is compiled into the kernel and, for its tests, on the machine that builds it
 * (tools/qmi-test): the few things it needs from an operating system are in the first section.
 */
#include <sys/redwood_qmi.h>
#include <sys/redwood_qrtr.h>

/* ---- what is needed of the operating system ---------------------------------------------------- */

#ifdef KERNEL

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>

static LCK_GRP_DECLARE(qmi_lck_grp, "redwood-qmi");

struct os_lock {
	lck_mtx_t mutex;
};

static void *
os_alloc(size_t size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

static void
os_free(void *memory, size_t size)
{
	kfree_data(memory, size);
}

static void
os_lock_init(struct os_lock *l)
{
	lck_mtx_init(&l->mutex, &qmi_lck_grp, LCK_ATTR_NULL);
}

static void
os_lock_destroy(struct os_lock *l)
{
	lck_mtx_destroy(&l->mutex, &qmi_lck_grp);
}

static void
os_lock(struct os_lock *l)
{
	lck_mtx_lock(&l->mutex);
}

static void
os_unlock(struct os_lock *l)
{
	lck_mtx_unlock(&l->mutex);
}

static uint64_t
os_now_ms(void)
{
	uint64_t now, ns;

	clock_get_uptime(&now);
	absolutetime_to_nanoseconds(now, &ns);
	return ns / NSEC_PER_MSEC;
}

/* With the lock held: sleep until os_wake(what) or for that long. */
static void
os_wait(struct os_lock *l, void *what, uint64_t ms)
{
	struct timespec ts = { .tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)(ms % 1000) * NSEC_PER_MSEC };

	msleep(what, &l->mutex, PZERO, "redwood-qmi", &ts);
}

static void
os_wake(__unused struct os_lock *l, void *what)
{
	wakeup(what);
}

#define os_log(...)     printf(__VA_ARGS__)

#else /* a program on the machine that builds this: the tests */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct os_lock {
	pthread_mutex_t mutex;
	pthread_cond_t woken;
};

static void *
os_alloc(size_t size)
{
	return calloc(1, size ? size : 1);
}

static void
os_free(void *memory, size_t size)
{
	(void)size;
	free(memory);
}

static void
os_lock_init(struct os_lock *l)
{
	pthread_mutex_init(&l->mutex, NULL);
	pthread_cond_init(&l->woken, NULL);
}

static void
os_lock_destroy(struct os_lock *l)
{
	pthread_mutex_destroy(&l->mutex);
	pthread_cond_destroy(&l->woken);
}

static void
os_lock(struct os_lock *l)
{
	pthread_mutex_lock(&l->mutex);
}

static void
os_unlock(struct os_lock *l)
{
	pthread_mutex_unlock(&l->mutex);
}

static uint64_t
os_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void
os_wait(struct os_lock *l, void *what, uint64_t ms)
{
	struct timespec until;

	(void)what;                     /* one condition for all: whoever is woken looks again */
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += (time_t)(ms / 1000);
	until.tv_nsec += (long)(ms % 1000) * 1000000;
	if (until.tv_nsec >= 1000000000) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000;
	}
	pthread_cond_timedwait(&l->woken, &l->mutex, &until);
}

static void
os_wake(struct os_lock *l, void *what)
{
	(void)what;
	pthread_cond_broadcast(&l->woken);
}

#define os_log(...)     fprintf(stderr, __VA_ARGS__)

#endif

/* ---- the coding -------------------------------------------------------------------------------- */

/*
 * A message: the header, then TLVs -- type (1 byte), length (2), value -- in any order. Numbers
 * are little-endian. A table describes the C structure a message is coded from or into, top
 * level element by element; elements of one TLV follow each other and share its type:
 *
 *   [QMI_OPT_FLAG]   a byte in C that says whether the TLV is in the message at all
 *   [QMI_DATA_LEN]   the count of the array that follows. In C it is always a 32-bit number,
 *                    whatever the element's elem_size says (the tables are made by a tool that
 *                    writes "uint32_t x_len" for every count, and Linux reads and writes four
 *                    bytes there); elem_size is its size in the message: 1 byte if it is 1, else 2
 *   the value        numbers of 1, 2, 4 or 8 bytes; a string (at the top level just its
 *                    characters: the TLV's length is the string's); a structure, which is its
 *                    own elements one after the other with nothing in between; or an array of
 *                    any of these
 *
 * Inside a structure there are no TLVs and nothing is optional: a count if the element is a
 * variable array, a length before a string (1 byte if the string's C buffer is at most 255
 * bytes, else 2), then the values.
 */
#define TLV_HEADER      3

struct writer {
	uint8_t *at, *end;
};

struct reader {
	const uint8_t *at, *end;
};

static bool
put(struct writer *w, const void *bytes, size_t count)
{
	if ((size_t)(w->end - w->at) < count) {
		return false;
	}
	memcpy(w->at, bytes, count);
	w->at += count;
	return true;
}

static bool
put_number(struct writer *w, uint64_t value, unsigned size)
{
	uint8_t bytes[8];

	for (unsigned i = 0; i < size; i++) {
		bytes[i] = (uint8_t)(value >> (8 * i));
	}
	return put(w, bytes, size);
}

static bool
get_number(struct reader *r, uint64_t *value, unsigned size)
{
	if ((size_t)(r->end - r->at) < size) {
		return false;
	}
	*value = 0;
	for (unsigned i = 0; i < size; i++) {
		*value |= (uint64_t)r->at[i] << (8 * i);
	}
	r->at += size;
	return true;
}

/* A number of the C structure, of 1, 2, 4 or 8 bytes, as the machine keeps it. */
static uint64_t
load(const void *from, unsigned size)
{
	uint8_t v8;
	uint16_t v16;
	uint32_t v32;
	uint64_t v64;

	switch (size) {
	case 1: memcpy(&v8, from, 1); return v8;
	case 2: memcpy(&v16, from, 2); return v16;
	case 4: memcpy(&v32, from, 4); return v32;
	default: memcpy(&v64, from, 8); return v64;
	}
}

static void
store(void *to, uint64_t value, unsigned size)
{
	uint8_t v8 = (uint8_t)value;
	uint16_t v16 = (uint16_t)value;
	uint32_t v32 = (uint32_t)value;

	switch (size) {
	case 1: memcpy(to, &v8, 1); break;
	case 2: memcpy(to, &v16, 2); break;
	case 4: memcpy(to, &v32, 4); break;
	default: memcpy(to, &value, 8); break;
	}
}

static bool
is_number(const struct qmi_elem_info *e)
{
	return e->data_type >= QMI_UNSIGNED_1_BYTE && e->data_type <= QMI_SIGNED_4_BYTE_ENUM &&
	       (e->elem_size == 1 || e->elem_size == 2 || e->elem_size == 4 || e->elem_size == 8);
}

static unsigned
count_size(const struct qmi_elem_info *count)
{
	return count->elem_size == 1 ? 1 : 2;
}

static int encode_structure(struct writer *w, const struct qmi_elem_info *ei, const uint8_t *structure);

/* `count` entries of one element. */
static int
encode_value(struct writer *w, const struct qmi_elem_info *e, const uint8_t *from, uint32_t count, bool top)
{
	if (is_number(e)) {
		for (uint32_t i = 0; i < count; i++) {
			if (!put_number(w, load(from + (size_t)i * e->elem_size, e->elem_size), e->elem_size)) {
				return -EMSGSIZE;
			}
		}
		return 0;
	}
	if (e->data_type == QMI_STRUCT && e->ei_array != NULL) {
		for (uint32_t i = 0; i < count; i++) {
			int error = encode_structure(w, e->ei_array, from + (size_t)i * e->elem_size);

			if (error) {
				return error;
			}
		}
		return 0;
	}
	if (e->data_type == QMI_STRING && count == 1) {
		size_t length = strnlen((const char *)from, e->elem_len);

		if (length >= e->elem_len) {
			return -EINVAL;                 /* no zero in the buffer */
		}
		if (!top && !put_number(w, length, e->elem_len <= 255 ? 1 : 2)) {
			return -EMSGSIZE;
		}
		return put(w, from, length) ? 0 : -EMSGSIZE;
	}
	return -EINVAL;
}

/*
 * The count and the entries of the element at *e (a QMI_DATA_LEN first, if the element is a variable
 * array); leaves *e at the last element used.
 */
static int
encode_counted(struct writer *w, const struct qmi_elem_info **e, const uint8_t *structure, bool top)
{
	uint32_t count = 1;

	if ((*e)->data_type == QMI_DATA_LEN) {
		const struct qmi_elem_info *length = (*e)++;

		count = (uint32_t)load(structure + length->offset, sizeof(uint32_t));
		if ((*e)->array_type != VAR_LEN_ARRAY || count > (*e)->elem_len || (count_size(length) == 1 && count > 255)) {
			return -EINVAL;
		}
		if (!put_number(w, count, count_size(length))) {
			return -EMSGSIZE;
		}
	} else if ((*e)->array_type == STATIC_ARRAY) {
		count = (*e)->elem_len;
	} else if ((*e)->array_type != NO_ARRAY) {
		return -EINVAL;                         /* a variable array nothing counts */
	}
	return encode_value(w, *e, structure + (*e)->offset, count, top);
}

static int
encode_structure(struct writer *w, const struct qmi_elem_info *ei, const uint8_t *structure)
{
	for (const struct qmi_elem_info *e = ei; e->data_type != QMI_EOTI; e++) {
		int error = e->data_type == QMI_OPT_FLAG ? -EINVAL : encode_counted(w, &e, structure, false);

		if (error) {
			return error;
		}
	}
	return 0;
}

int
redwood_qmi_encode(uint8_t type, uint16_t message, uint16_t transaction, const struct qmi_elem_info *ei, const void *structure,
    void *buffer, size_t size)
{
	struct writer w = { .at = buffer, .end = (uint8_t *)buffer + size };
	uint8_t *start = buffer, *tlv;
	const struct qmi_elem_info *e;

	if (size < QMI_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	w.at += QMI_HEADER_SIZE;
	for (e = structure != NULL ? ei : NULL; e != NULL && e->data_type != QMI_EOTI; e++) {
		uint8_t tlv_type = e->tlv_type;
		int error;

		if (e->data_type == QMI_OPT_FLAG) {
			if (!load((const uint8_t *)structure + e->offset, 1)) {
				/* not there: on to the next element of another TLV */
				while (e[1].data_type != QMI_EOTI && e[1].tlv_type == tlv_type) {
					e++;
				}
				continue;
			}
			e++;
			if (e->data_type == QMI_EOTI || e->tlv_type != tlv_type) {
				return -EINVAL;
			}
		}
		tlv = w.at;
		if ((size_t)(w.end - w.at) < TLV_HEADER) {
			return -EMSGSIZE;
		}
		w.at += TLV_HEADER;
		error = encode_counted(&w, &e, structure, true);
		if (error) {
			return error;
		}
		if (w.at - tlv - TLV_HEADER > 0xffff) {
			return -EMSGSIZE;
		}
		tlv[0] = tlv_type;
		tlv[1] = (uint8_t)(w.at - tlv - TLV_HEADER);
		tlv[2] = (uint8_t)((w.at - tlv - TLV_HEADER) >> 8);
	}
	if (w.at - start - QMI_HEADER_SIZE > 0xffff) {
		return -EMSGSIZE;
	}
	start[0] = type;
	start[1] = (uint8_t)transaction;
	start[2] = (uint8_t)(transaction >> 8);
	start[3] = (uint8_t)message;
	start[4] = (uint8_t)(message >> 8);
	start[5] = (uint8_t)(w.at - start - QMI_HEADER_SIZE);
	start[6] = (uint8_t)((w.at - start - QMI_HEADER_SIZE) >> 8);
	return (int)(w.at - start);
}

static int decode_structure(struct reader *r, const struct qmi_elem_info *ei, uint8_t *structure);

static int
decode_value(struct reader *r, const struct qmi_elem_info *e, uint8_t *to, uint32_t count, bool top)
{
	uint64_t value;

	if (is_number(e)) {
		for (uint32_t i = 0; i < count; i++) {
			if (!get_number(r, &value, e->elem_size)) {
				return -EINVAL;
			}
			store(to + (size_t)i * e->elem_size, value, e->elem_size);
		}
		return 0;
	}
	if (e->data_type == QMI_STRUCT && e->ei_array != NULL) {
		for (uint32_t i = 0; i < count; i++) {
			int error = decode_structure(r, e->ei_array, to + (size_t)i * e->elem_size);

			if (error) {
				return error;
			}
		}
		return 0;
	}
	if (e->data_type == QMI_STRING && count == 1) {
		value = (uint64_t)(r->end - r->at);             /* at the top level: all of the TLV */
		if (!top && !get_number(r, &value, e->elem_len <= 255 ? 1 : 2)) {
			return -EINVAL;
		}
		if (value >= e->elem_len || value > (uint64_t)(r->end - r->at)) {
			return -EINVAL;
		}
		memcpy(to, r->at, (size_t)value);
		to[value] = 0;
		r->at += value;
		return 0;
	}
	return -EINVAL;
}

static int
decode_counted(struct reader *r, const struct qmi_elem_info **e, uint8_t *structure, bool top)
{
	uint32_t count = 1;

	if ((*e)->data_type == QMI_DATA_LEN) {
		const struct qmi_elem_info *length = (*e)++;
		uint64_t value;

		if (!get_number(r, &value, count_size(length))) {
			return -EINVAL;
		}
		if ((*e)->array_type != VAR_LEN_ARRAY || value > (*e)->elem_len) {
			return -EINVAL;
		}
		store(structure + length->offset, value, sizeof(uint32_t));
		count = (uint32_t)value;
	} else if ((*e)->array_type == STATIC_ARRAY) {
		count = (*e)->elem_len;
	} else if ((*e)->array_type != NO_ARRAY) {
		return -EINVAL;
	}
	return decode_value(r, *e, structure + (*e)->offset, count, top);
}

static int
decode_structure(struct reader *r, const struct qmi_elem_info *ei, uint8_t *structure)
{
	for (const struct qmi_elem_info *e = ei; e->data_type != QMI_EOTI; e++) {
		int error = e->data_type == QMI_OPT_FLAG ? -EINVAL : decode_counted(r, &e, structure, false);

		if (error) {
			return error;
		}
	}
	return 0;
}

int
redwood_qmi_decode(const void *buffer, size_t length, const struct qmi_elem_info *ei, void *structure)
{
	const uint8_t *start = buffer;
	struct reader r;
	size_t said;

	if (buffer == NULL || length < QMI_HEADER_SIZE || ei == NULL || structure == NULL) {
		return -EINVAL;
	}
	said = start[5] | (size_t)start[6] << 8;
	if (said > length - QMI_HEADER_SIZE) {
		return -EINVAL;
	}
	r.at = start + QMI_HEADER_SIZE;
	r.end = r.at + said;
	while (r.at < r.end) {
		const struct qmi_elem_info *e;
		struct reader value;
		uint8_t tlv_type;
		int error;

		if (r.end - r.at < TLV_HEADER) {
			return -EINVAL;
		}
		tlv_type = r.at[0];
		value.at = r.at + TLV_HEADER;
		value.end = value.at + (r.at[1] | (size_t)r.at[2] << 8);
		if (value.end > r.end) {
			return -EINVAL;
		}
		r.at = value.end;
		for (e = ei; e->data_type != QMI_EOTI && e->tlv_type != tlv_type; e++) {
		}
		if (e->data_type == QMI_EOTI) {
			continue;                       /* nothing we know: a newer firmware's, say */
		}
		if (e->data_type == QMI_OPT_FLAG) {
			store((uint8_t *)structure + e->offset, 1, 1);
			e++;
			if (e->data_type == QMI_EOTI || e->tlv_type != tlv_type) {
				return -EINVAL;
			}
		}
		error = decode_counted(&value, &e, structure, true);
		if (error) {
			return error;
		}
	}
	return (int)(QMI_HEADER_SIZE + said);
}

const struct qmi_elem_info qmi_response_type_v01_ei[] = {
	{
		.data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
		.tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct qmi_response_type_v01, result),
	},
	{
		.data_type = QMI_SIGNED_2_BYTE_ENUM, .elem_len = 1, .elem_size = sizeof(uint16_t), .array_type = NO_ARRAY,
		.tlv_type = QMI_COMMON_TLV_TYPE, .offset = offsetof(struct qmi_response_type_v01, error),
	},
	{ .data_type = QMI_EOTI },
};

/* ---- handles ----------------------------------------------------------------------------------- */

/* What stands behind a handle (its `sock` points here). */
struct qmi_state {
	struct qmi_handle *handle;
	const struct redwood_qrtr_ops *transport;
	struct redwood_qrtr_port *port;
	struct os_lock lock;                    /* the lists below and the transactions in them */
	struct qmi_ops ops;
	const struct qmi_msg_handler *handlers;
	size_t largest;
	struct qmi_txn *transactions;
	uint16_t last_id;
	struct qmi_service *services;
	bool connected;
	uint32_t peer_node, peer_port;
	/* said in the log once each, so that a firmware that talks a lot cannot fill it */
	bool said_malformed, said_undecoded, said_unknown;
};

static const struct redwood_qrtr_ops *qmi_transport;

void
redwood_qmi_set_transport(const struct redwood_qrtr_ops *ops)
{
	qmi_transport = ops;
}

static struct qmi_state *
state_of(struct qmi_handle *qmi)
{
	return qmi != NULL ? (struct qmi_state *)(void *)qmi->sock : NULL;
}

/* With the lock held. */
static struct qmi_txn **
find_transaction(struct qmi_state *s, const struct qmi_txn *txn, uint16_t id)
{
	struct qmi_txn **p;

	for (p = &s->transactions; *p != NULL; p = &(*p)->next) {
		if (txn != NULL ? *p == txn : (*p)->id == id) {
			return p;
		}
	}
	return NULL;
}

/* A message that is not the reply somebody waits for: to its handler, if it has one. */
static void
to_handler(struct qmi_state *s, uint8_t type, uint16_t id, uint16_t message, uint32_t node, uint32_t port, const void *data,
    size_t length)
{
	const struct qmi_msg_handler *h;
	struct sockaddr_qrtr from = { .sq_family = AF_QIPCRTR, .sq_node = node, .sq_port = port };
	struct qmi_txn txn = { .qmi = s->handle, .id = id };
	void *decoded;
	int result;

	for (h = s->handlers; h != NULL && h->fn != NULL; h++) {
		if (h->type == type && h->msg_id == message) {
			break;
		}
	}
	if (h == NULL || h->fn == NULL) {
		if (!s->said_unknown) {
			s->said_unknown = true;
			os_log("qmi: a message nobody handles: type %u, id %#x, %zu bytes (said once)\n", type, message, length);
		}
		return;
	}
	decoded = os_alloc(h->decoded_size);
	if (decoded == NULL) {
		return;
	}
	result = redwood_qmi_decode(data, length, h->ei, decoded);
	if (result < 0) {
		if (!s->said_undecoded) {
			s->said_undecoded = true;
			os_log("qmi: message %#x (type %u, %zu bytes) cannot be decoded: %d (said once)\n", message, type, length, result);
		}
	} else {
		h->fn(s->handle, &from, &txn, decoded);
	}
	os_free(decoded, h->decoded_size);
}

/* The router has a datagram for us. */
static void
qmi_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct qmi_state *s = context;
	const uint8_t *header = data;
	struct qmi_txn **found, *txn;
	uint16_t id, message;
	uint8_t type;

	if (length < QMI_HEADER_SIZE || length > s->largest + QMI_HEADER_SIZE ||
	    (size_t)(header[5] | header[6] << 8) > length - QMI_HEADER_SIZE) {
		if (!s->said_malformed) {
			s->said_malformed = true;
			os_log("qmi: a datagram of %zu bytes from %u:%u is no message we take (said once)\n", length, node, port);
		}
		return;
	}
	type = header[0];
	id = (uint16_t)(header[1] | header[2] << 8);
	message = (uint16_t)(header[3] | header[4] << 8);
	if (type == QMI_RESPONSE) {
		os_lock(&s->lock);
		found = find_transaction(s, NULL, id);
		txn = found != NULL ? *found : NULL;
		if (txn != NULL && !txn->done && txn->ei != NULL && txn->dest != NULL) {
			txn->result = redwood_qmi_decode(data, length, txn->ei, txn->dest);
			txn->done = true;
			os_wake(&s->lock, txn);
			os_unlock(&s->lock);
			return;
		}
		os_unlock(&s->lock);
		if (txn == NULL) {
			return;                         /* a reply to nothing: late, after its transaction was given up */
		}
	}
	to_handler(s, type, id, message, node, port, data, length);
}

/* The router says a server we asked about is there, or is gone. */
static void
qmi_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port)
{
	struct qmi_state *s = context;
	struct qmi_service *svc, **p;

	os_lock(&s->lock);
	for (p = &s->services; *p != NULL; p = &(*p)->next) {
		if ((*p)->node == node && (*p)->port == port) {
			break;
		}
	}
	svc = *p;
	if (!arrived && svc != NULL) {
		*p = svc->next;
	}
	os_unlock(&s->lock);

	if (arrived && svc == NULL) {
		svc = os_alloc(sizeof(*svc));
		if (svc == NULL) {
			return;
		}
		svc->service = service;
		svc->version = version;
		svc->instance = instance;
		svc->node = node;
		svc->port = port;
		if (s->ops.new_server != NULL && s->ops.new_server(s->handle, svc) < 0) {
			os_free(svc, sizeof(*svc));
			return;
		}
		os_lock(&s->lock);
		svc->next = s->services;
		s->services = svc;
		os_unlock(&s->lock);
	} else if (!arrived && svc != NULL) {
		if (s->ops.del_server != NULL) {
			s->ops.del_server(s->handle, svc);
		}
		os_free(svc, sizeof(*svc));
	}
}

int
qmi_handle_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *ops, const struct qmi_msg_handler *handlers)
{
	const struct redwood_qrtr_ops *transport = qmi_transport;
	struct qmi_state *s;

	qmi->sock = NULL;
	if (transport == NULL) {
		return -ENXIO;
	}
	s = os_alloc(sizeof(*s));
	if (s == NULL) {
		return -ENOMEM;
	}
	s->handle = qmi;
	s->transport = transport;
	s->largest = largest;
	s->handlers = handlers;
	if (ops != NULL) {
		s->ops = *ops;
	}
	os_lock_init(&s->lock);
	qmi->sock = (struct socket *)(void *)s;         /* before the port: callbacks may come at once */
	s->port = transport->open(qmi_receive, qmi_server, s);
	if (s->port == NULL) {
		qmi->sock = NULL;
		os_lock_destroy(&s->lock);
		os_free(s, sizeof(*s));
		return -ENXIO;
	}
	return 0;
}

/* Nobody may be waiting in qmi_txn_wait() on this handle any more. */
void
qmi_handle_release(struct qmi_handle *qmi)
{
	struct qmi_state *s = state_of(qmi);
	struct qmi_service *svc;

	if (s == NULL) {
		return;
	}
	s->transport->close(s->port);                   /* no callback after this */
	while ((svc = s->services) != NULL) {
		s->services = svc->next;
		os_free(svc, sizeof(*svc));
	}
	qmi->sock = NULL;
	os_lock_destroy(&s->lock);
	os_free(s, sizeof(*s));
}

int
qmi_add_lookup(struct qmi_handle *qmi, unsigned int service, unsigned int version, unsigned int instance)
{
	struct qmi_state *s = state_of(qmi);
	int error;

	if (s == NULL) {
		return -EINVAL;
	}
	error = s->transport->lookup(s->port, service, version, instance);
	return error > 0 ? -error : error;
}

int
redwood_qmi_connect(struct socket *sock, uint32_t node, uint32_t port)
{
	struct qmi_state *s = (struct qmi_state *)(void *)sock;

	if (s == NULL) {
		return -EINVAL;
	}
	os_lock(&s->lock);
	s->peer_node = node;
	s->peer_port = port;
	s->connected = true;
	os_unlock(&s->lock);
	return 0;
}

int
qmi_txn_init(struct qmi_handle *qmi, struct qmi_txn *txn, const struct qmi_elem_info *ei, void *structure)
{
	struct qmi_state *s = state_of(qmi);
	uint16_t id;

	memset(txn, 0, sizeof(*txn));
	if (s == NULL) {
		return -EINVAL;
	}
	txn->qmi = qmi;
	txn->ei = ei;
	txn->dest = structure;
	os_lock(&s->lock);
	/* the next number nobody else has (0 to 65534, round and round) */
	for (id = (uint16_t)(s->last_id + 1);; id++) {
		if (id == 0xffff) {
			id = 0;
		}
		if (find_transaction(s, NULL, id) == NULL) {
			break;
		}
		if (id == s->last_id) {
			os_unlock(&s->lock);
			return -ENOMEM;                 /* 65535 of them at once */
		}
	}
	s->last_id = id;
	txn->id = id;
	txn->next = s->transactions;
	s->transactions = txn;
	os_unlock(&s->lock);
	return id;
}

int
qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	struct qmi_state *s = state_of(qmi);
	uint32_t node, port;
	bool connected;
	size_t size = largest + QMI_HEADER_SIZE;
	void *buffer;
	int length, error;

	if (s == NULL || txn == NULL) {
		return -EINVAL;
	}
	os_lock(&s->lock);
	connected = s->connected;
	node = s->peer_node;
	port = s->peer_port;
	os_unlock(&s->lock);
	if (sq != NULL) {
		node = sq->sq_node;
		port = sq->sq_port;
	} else if (!connected) {
		return -ENOTCONN;
	}
	buffer = os_alloc(size);
	if (buffer == NULL) {
		return -ENOMEM;
	}
	length = redwood_qmi_encode(QMI_REQUEST, (uint16_t)msg_id, txn->id, ei, structure, buffer, size);
	if (length < 0) {
		os_free(buffer, size);
		return length;
	}
	error = s->transport->send(s->port, node, port, buffer, (size_t)length);
	os_free(buffer, size);
	return error > 0 ? -error : error;
}

void
qmi_txn_cancel(struct qmi_txn *txn)
{
	struct qmi_state *s = state_of(txn->qmi);
	struct qmi_txn **found;

	if (s == NULL) {
		return;
	}
	os_lock(&s->lock);
	found = find_transaction(s, txn, 0);
	if (found != NULL) {
		*found = txn->next;
	}
	os_unlock(&s->lock);
}

int
redwood_qmi_txn_wait_ms(struct qmi_txn *txn, uint64_t milliseconds)
{
	struct qmi_state *s = state_of(txn->qmi);
	uint64_t now = os_now_ms(), until = now + milliseconds;
	struct qmi_txn **found;
	int result;

	if (s == NULL) {
		return -EINVAL;
	}
	os_lock(&s->lock);
	while (!txn->done && now < until) {
		os_wait(&s->lock, txn, until - now);
		now = os_now_ms();
	}
	found = find_transaction(s, txn, 0);
	if (found != NULL) {
		*found = txn->next;
	}
	result = txn->done ? txn->result : -ETIMEDOUT;
	os_unlock(&s->lock);
	return result;
}
