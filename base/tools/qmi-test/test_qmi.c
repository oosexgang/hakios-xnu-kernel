/*
 * Tests of the QMI helpers (bsd/dev/redwood_qmi.c in the kernel tree) on the machine that builds
 * them; tools/qmi-test/run.sh compiles and runs this.
 *
 * Three kinds:
 *   1. the coding against byte sequences written out by hand from the format's public description
 *      (a header of 7 bytes, then type-length-value triples), with tables of our own that have
 *      every kind of element, and with ath11k's own tables for the messages of its "WLFW" service;
 *   2. the coding against itself and against what ath11k's header says: every one of ath11k's 36
 *      tables filled at random, encoded, decoded and compared; filled to the brim and the length
 *      compared with the ..._MAX_LEN its header gives; a hundred thousand damaged messages decoded
 *      under the address sanitizer;
 *   3. the helpers over a router made up here (struct redwood_qrtr_ops): lookup, a server coming
 *      and going, requests and replies as transactions (in time, late, never, crossed, undecodable),
 *      indications to handlers, and the whole exchange a WCN6750's firmware and ath11k have up to
 *      "firmware ready", played by a firmware made up here.
 * With -DNO_ATH11K (no copy of ath11k's sources at hand) only what needs no table of ath11k runs.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/redwood_qmi.h>
#include <sys/redwood_qrtr.h>

#define HZ 250                          /* what a Linux might have: qmi_txn_wait() must take it from here */
#define msecs_to_jiffies(ms)    ((unsigned long)(ms) * HZ / 1000)

#ifndef NO_ATH11K
#include "qmi.h"                        /* ath11k's, as it is */
#include "tables.inc"                   /* ath11k's tables, taken out of its qmi.c by extract.py */
#endif

static int checks, failures;

static void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

static void
check_bytes(const uint8_t *got, int got_length, const uint8_t *want, size_t want_length, const char *what)
{
	bool same = got_length == (int)want_length && memcmp(got, want, want_length) == 0;

	check(same, "%s: the encoded message is not the expected one", what);
	if (!same) {
		printf("  got  (%d):", got_length);
		for (int i = 0; i < got_length && i < 80; i++) {
			printf(" %02x", got[i]);
		}
		printf("\n  want (%zu):", want_length);
		for (size_t i = 0; i < want_length && i < 80; i++) {
			printf(" %02x", want[i]);
		}
		printf("\n");
	}
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- 1. the coding, against bytes written by hand ------------------------------------------------- */

/* A message of our own with every kind of element in it. */
struct inner {
	uint16_t a;
	uint32_t pairs_len;
	uint8_t pairs[4];
	char name[10];                  /* up to 9 characters: a 1-byte length before it in a message */
};
struct outer {
	uint8_t first;                  /* TLV 1 */
	uint8_t second_valid;
	uint32_t second;                /* TLV 0x10 */
	uint8_t list_valid;
	uint32_t list_len;
	uint16_t list[5];               /* TLV 0x11 */
	uint64_t fixed[2];              /* TLV 2: always two */
	uint8_t inners_valid;
	uint32_t inners_len;
	struct inner inners[3];         /* TLV 0x12 */
	uint8_t text_valid;
	char text[300];                 /* TLV 0x13: at the top level, no length before it */
	int16_t kind;                   /* TLV 3 */
	uint8_t big_valid;
	struct {
		char name[300];         /* over 255: a 2-byte length before it */
		uint32_t n;
	} big;                          /* TLV 0x14 */
};

static const struct qmi_elem_info inner_ei[] = {
	{ QMI_UNSIGNED_2_BYTE, 1, sizeof(uint16_t), NO_ARRAY, 0, offsetof(struct inner, a), NULL },
	{ QMI_DATA_LEN, 1, sizeof(uint8_t), NO_ARRAY, 0, offsetof(struct inner, pairs_len), NULL },
	{ QMI_UNSIGNED_1_BYTE, 4, sizeof(uint8_t), VAR_LEN_ARRAY, 0, offsetof(struct inner, pairs), NULL },
	{ QMI_STRING, 10, sizeof(char), NO_ARRAY, 0, offsetof(struct inner, name), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
static const struct qmi_elem_info big_ei[] = {
	{ QMI_STRING, 300, sizeof(char), NO_ARRAY, 0, 0, NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0, 300, NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
static const struct qmi_elem_info outer_ei[] = {
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(uint8_t), NO_ARRAY, 0x01, offsetof(struct outer, first), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x10, offsetof(struct outer, second_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x10, offsetof(struct outer, second), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x11, offsetof(struct outer, list_valid), NULL },
	{ QMI_DATA_LEN, 1, sizeof(uint16_t), NO_ARRAY, 0x11, offsetof(struct outer, list_len), NULL },
	{ QMI_UNSIGNED_2_BYTE, 5, sizeof(uint16_t), VAR_LEN_ARRAY, 0x11, offsetof(struct outer, list), NULL },
	{ QMI_UNSIGNED_8_BYTE, 2, sizeof(uint64_t), STATIC_ARRAY, 0x02, offsetof(struct outer, fixed), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x12, offsetof(struct outer, inners_valid), NULL },
	{ QMI_DATA_LEN, 1, sizeof(uint8_t), NO_ARRAY, 0x12, offsetof(struct outer, inners_len), NULL },
	{ QMI_STRUCT, 3, sizeof(struct inner), VAR_LEN_ARRAY, 0x12, offsetof(struct outer, inners), inner_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x13, offsetof(struct outer, text_valid), NULL },
	{ QMI_STRING, 300, sizeof(char), NO_ARRAY, 0x13, offsetof(struct outer, text), NULL },
	{ QMI_SIGNED_2_BYTE_ENUM, 1, sizeof(int16_t), NO_ARRAY, 0x03, offsetof(struct outer, kind), NULL },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x14, offsetof(struct outer, big_valid), NULL },
	{ QMI_STRUCT, 1, 304, NO_ARRAY, 0x14, offsetof(struct outer, big), big_ei },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};

static void
test_own_tables(void)
{
	/* everything there */
	static const uint8_t full[] = {
		0x00, 0x34, 0x12, 0xcd, 0xab, 0x51, 0x00,               /* a request, transaction 0x1234, message 0xabcd, 81 bytes follow */
		0x01, 0x01, 0x00, 0x7f,                                 /* first */
		0x10, 0x04, 0x00, 0x78, 0x56, 0x34, 0x12,               /* second */
		0x11, 0x06, 0x00, 0x02, 0x00, 0x01, 0x00, 0xff, 0xff,   /* list: a 2-byte count, two entries */
		0x02, 0x10, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 0, 0x80,    /* fixed */
		0x12, 0x0e, 0x00, 0x02,                                 /* inners: a 1-byte count, two of them */
		0x11, 0x22, 0x03, 0xa1, 0xa2, 0xa3, 0x02, 'h', 'i',     /*   a, a count and three bytes, a length and "hi" */
		0x33, 0x44, 0x00, 0x00,                                 /*   a, no pairs, an empty string */
		0x13, 0x05, 0x00, 'h', 'e', 'l', 'l', 'o',              /* text: the TLV's length is the string's */
		0x03, 0x02, 0x00, 0xfe, 0xff,                           /* kind = -2 */
		0x14, 0x09, 0x00, 0x03, 0x00, 'x', 'y', 'z', 0x2a, 0, 0, 0,     /* big: a 2-byte length, "xyz", n */
	};
	/* nothing optional: only the three that have no flag, in the table's order */
	static const uint8_t bare[] = {
		0x04, 0x00, 0x00, 0x01, 0x00, 0x1c, 0x00,               /* an indication */
		0x01, 0x01, 0x00, 0x00,
		0x02, 0x10, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0x03, 0x02, 0x00, 0x05, 0x00,
	};
	/* as `full` says it, but the TLVs in another order, one unknown among them, one given twice */
	static const uint8_t shuffled[] = {
		0x02, 0x01, 0x00, 0x02, 0x00, 0x26, 0x00,
		0x03, 0x02, 0x00, 0x09, 0x00,
		0xe0, 0x03, 0x00, 1, 2, 3,                              /* nobody knows 0xe0 */
		0x10, 0x04, 0x00, 1, 0, 0, 0,
		0x05, 0x01, 0x00, 0x99,                                 /* nor 5, which is below 0x10 */
		0x01, 0x01, 0x00, 0x11,
		0x10, 0x04, 0x00, 2, 0, 0, 0,                           /* the later one counts */
		0x11, 0x02, 0x00, 0x00, 0x00,                           /* a list of none */
	};
	struct outer o, back;
	uint8_t buffer[512];
	int n;

	memset(&o, 0, sizeof(o));
	o.first = 0x7f;
	o.second_valid = 1;
	o.second = 0x12345678;
	o.list_valid = 1;
	o.list_len = 2;
	o.list[0] = 1;
	o.list[1] = 0xffff;
	o.fixed[0] = 0x0102030405060708ULL;
	o.fixed[1] = 0x8000000000000000ULL;
	o.inners_valid = 1;
	o.inners_len = 2;
	o.inners[0].a = 0x2211;
	o.inners[0].pairs_len = 3;
	memcpy(o.inners[0].pairs, "\xa1\xa2\xa3", 3);
	strcpy(o.inners[0].name, "hi");
	o.inners[1].a = 0x4433;
	o.text_valid = 1;
	strcpy(o.text, "hello");
	o.kind = -2;
	o.big_valid = 1;
	strcpy(o.big.name, "xyz");
	o.big.n = 42;
	n = redwood_qmi_encode(QMI_REQUEST, 0xabcd, 0x1234, outer_ei, &o, buffer, sizeof(buffer));
	check_bytes(buffer, n, full, sizeof(full), "own table, everything there");

	memset(&back, 0, sizeof(back));
	n = redwood_qmi_decode(full, sizeof(full), outer_ei, &back);
	check(n == (int)sizeof(full), "own table: decoding gives %d, not the message's %zu bytes", n, sizeof(full));
	check(back.first == 0x7f && back.second_valid == 1 && back.second == 0x12345678 && back.list_valid == 1 &&
	    back.list_len == 2 && back.list[0] == 1 && back.list[1] == 0xffff && back.fixed[0] == 0x0102030405060708ULL &&
	    back.fixed[1] == 0x8000000000000000ULL && back.inners_valid == 1 && back.inners_len == 2 &&
	    back.inners[0].a == 0x2211 && back.inners[0].pairs_len == 3 && memcmp(back.inners[0].pairs, "\xa1\xa2\xa3", 3) == 0 &&
	    strcmp(back.inners[0].name, "hi") == 0 && back.inners[1].a == 0x4433 && back.inners[1].pairs_len == 0 &&
	    back.inners[1].name[0] == 0 && back.text_valid == 1 && strcmp(back.text, "hello") == 0 && back.kind == -2 &&
	    back.big_valid == 1 && strcmp(back.big.name, "xyz") == 0 && back.big.n == 42,
	    "own table: the decoded structure is not what the bytes say");

	memset(&o, 0, sizeof(o));
	o.kind = 5;
	o.second = 77;                  /* not valid: must not be sent */
	o.list_len = 3;
	n = redwood_qmi_encode(QMI_INDICATION, 1, 0, outer_ei, &o, buffer, sizeof(buffer));
	check_bytes(buffer, n, bare, sizeof(bare), "own table, nothing optional");

	memset(&back, 0xee, sizeof(back));
	memset(&back, 0, offsetof(struct outer, fixed));
	back.inners_valid = back.text_valid = back.big_valid = 0;
	n = redwood_qmi_decode(shuffled, sizeof(shuffled), outer_ei, &back);
	check(n == (int)sizeof(shuffled) && back.kind == 9 && back.first == 0x11 && back.second_valid == 1 && back.second == 2 &&
	    back.list_valid == 1 && back.list_len == 0 && back.inners_valid == 0 && back.text_valid == 0 && back.big_valid == 0,
	    "own table: TLVs out of order, unknown ones and a repeated one (%d)", n);
	check(back.fixed[0] == 0xeeeeeeeeeeeeeeeeULL, "own table: a TLV that is not in the message was written to");

	/* what must be refused */
	memset(&o, 0, sizeof(o));
	check(redwood_qmi_encode(QMI_REQUEST, 1, 1, outer_ei, &o, buffer, 34) == -EMSGSIZE, "a buffer one byte too small is not refused");
	check(redwood_qmi_encode(QMI_REQUEST, 1, 1, outer_ei, &o, buffer, 35) == 35, "a buffer of exactly the size is refused");
	o.list_valid = 1;
	o.list_len = 6;
	check(redwood_qmi_encode(QMI_REQUEST, 1, 1, outer_ei, &o, buffer, sizeof(buffer)) == -EINVAL, "a count over the array's size is encoded");
	o.list_len = 0;
	o.text_valid = 1;
	memset(o.text, 'a', sizeof(o.text));
	check(redwood_qmi_encode(QMI_REQUEST, 1, 1, outer_ei, &o, buffer, sizeof(buffer)) == -EINVAL, "a string with no end is encoded");
	{
		static const uint8_t count_too_big[] = { 0, 0, 0, 0, 0, 0x0f, 0, 0x11, 0x0c, 0, 0x06, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0 };
		static const uint8_t tlv_past_end[] = { 0, 0, 0, 0, 0, 0x04, 0, 0x01, 0x05, 0, 0x01 };
		static const uint8_t value_past_tlv[] = { 0, 0, 0, 0, 0, 0x05, 0, 0x10, 0x02, 0, 0x01, 0x02 };
		static const uint8_t length_lies[] = { 0, 0, 0, 0, 0, 0x40, 0, 0x01, 0x01, 0, 0x01 };
		static const uint8_t stray_byte[] = { 0, 0, 0, 0, 0, 0x05, 0, 0x01, 0x01, 0, 0x01, 0x10 };
		static const uint8_t inner_short[] = { 0, 0, 0, 0, 0, 0x07, 0, 0x12, 0x04, 0, 0x01, 0x11, 0x22, 0x03 };
		uint8_t long_string[7 + 3 + 300];

		memset(&back, 0, sizeof(back));
		check(redwood_qmi_decode(count_too_big, sizeof(count_too_big), outer_ei, &back) == -EINVAL, "a count over the array's size is decoded");
		check(redwood_qmi_decode(tlv_past_end, sizeof(tlv_past_end), outer_ei, &back) == -EINVAL, "a TLV longer than the message is decoded");
		check(redwood_qmi_decode(value_past_tlv, sizeof(value_past_tlv), outer_ei, &back) == -EINVAL, "a number longer than its TLV is decoded");
		check(redwood_qmi_decode(length_lies, sizeof(length_lies), outer_ei, &back) == -EINVAL, "a header that says more than there is, is believed");
		check(redwood_qmi_decode(stray_byte, sizeof(stray_byte), outer_ei, &back) == -EINVAL, "a stray byte after the last TLV is let pass");
		check(redwood_qmi_decode(inner_short, sizeof(inner_short), outer_ei, &back) == -EINVAL, "a structure cut short is decoded");
		memset(long_string, 'b', sizeof(long_string));
		memcpy(long_string, "\0\0\0\0\0\x2f\x01\x13\x2c\x01", 10);     /* a string of exactly 300 */
		check(redwood_qmi_decode(long_string, sizeof(long_string), outer_ei, &back) == -EINVAL, "a string that fills its buffer is decoded");
		long_string[5] = 0x2e;
		long_string[8] = 0x2b;                                          /* 299: fits, with its zero */
		check(redwood_qmi_decode(long_string, sizeof(long_string) - 1, outer_ei, &back) == 7 + 3 + 299 && strlen(back.text) == 299,
		    "a string one short of its buffer is not decoded");
	}
	/* the reply every response starts with */
	{
		static const uint8_t ok[] = { 0x02, 0x07, 0x00, 0x22, 0x00, 0x07, 0x00, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
		static const uint8_t no[] = { 0x02, 0x07, 0x00, 0x22, 0x00, 0x07, 0x00, 0x02, 0x04, 0x00, 0x01, 0x00, 0x5e, 0x00 };
		static const struct qmi_elem_info reply_ei[] = {
			{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02, 0, qmi_response_type_v01_ei },
			{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
		};
		struct qmi_response_type_v01 r = { 0xffff, 0xffff };

		check(redwood_qmi_decode(ok, sizeof(ok), reply_ei, &r) == 14 && r.result == QMI_RESULT_SUCCESS_V01 && r.error == QMI_ERR_NONE_V01,
		    "the common reply, success");
		check(redwood_qmi_decode(no, sizeof(no), reply_ei, &r) == 14 && r.result == QMI_RESULT_FAILURE_V01 &&
		    r.error == QMI_ERR_NOT_SUPPORTED_V01, "the common reply, failure");
		check_bytes(buffer, redwood_qmi_encode(QMI_RESPONSE, 0x22, 7, reply_ei, &r, buffer, sizeof(buffer)), no, sizeof(no), "the common reply");
	}
	/* a request with nothing in it */
	{
		static const uint8_t empty[] = { 0x00, 0x09, 0x00, 0x24, 0x00, 0x00, 0x00 };
		static const struct qmi_elem_info none_ei[] = { { QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL } };
		uint8_t nothing = 0;

		check_bytes(buffer, redwood_qmi_encode(QMI_REQUEST, 0x24, 9, none_ei, &nothing, buffer, sizeof(buffer)), empty, sizeof(empty), "an empty request");
		check_bytes(buffer, redwood_qmi_encode(QMI_REQUEST, 0x24, 9, NULL, NULL, buffer, sizeof(buffer)), empty, sizeof(empty), "a request with no table");
	}
}

#ifndef NO_ATH11K

/* ---- ... and ath11k's messages, by hand ------------------------------------------------------------- */

static void
test_wlfw_by_hand(void)
{
	uint8_t buffer[8192];

	/* "tell me when these happen": six flags and a client's number */
	{
		static const uint8_t want[] = {
			0x00, 0x05, 0x00, 0x20, 0x00, 0x1b, 0x00,
			0x10, 0x01, 0x00, 0x01,                                 /* fw_ready_enable */
			0x15, 0x04, 0x00, 0x4c, 0x45, 0x4e, 0x4b,               /* client_id */
			0x16, 0x01, 0x00, 0x01,                                 /* request_mem_enable */
			0x17, 0x01, 0x00, 0x01,                                 /* fw_mem_ready_enable */
			0x18, 0x01, 0x00, 0x01,                                 /* fw_init_done_enable */
			0x1b, 0x01, 0x00, 0x01,                                 /* cal_done_enable */
		};
		struct qmi_wlanfw_ind_register_req_msg_v01 req;

		memset(&req, 0, sizeof(req));
		req.client_id_valid = 1;
		req.client_id = 0x4b4e454c;
		req.fw_ready_enable_valid = 1;
		req.fw_ready_enable = 1;
		req.request_mem_enable_valid = 1;
		req.request_mem_enable = 1;
		req.fw_mem_ready_enable_valid = 1;
		req.fw_mem_ready_enable = 1;
		req.fw_init_done_enable_valid = 1;
		req.fw_init_done_enable = 1;
		req.cal_done_enable_valid = 1;
		req.cal_done_enable = 1;
		req.pin_connect_result_enable = 1;      /* (not valid: stays out) */
		check_bytes(buffer, redwood_qmi_encode(QMI_REQUEST, 0x20, 5, qmi_wlanfw_ind_register_req_msg_v01_ei, &req, buffer,
		    sizeof(buffer)), want, sizeof(want), "WLFW indication register request");
	}
	/* the mode: one number that is always there, one byte that may be */
	{
		static const uint8_t want[] = {
			0x00, 0x09, 0x00, 0x22, 0x00, 0x0b, 0x00,
			0x01, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,               /* mode 7: cold boot calibration */
			0x10, 0x01, 0x00, 0x00,                                 /* hw_debug */
		};
		struct qmi_wlanfw_wlan_mode_req_msg_v01 req = { .mode = 7, .hw_debug_valid = 1, .hw_debug = 0 };
		struct qmi_wlanfw_wlan_mode_req_msg_v01 back;

		check_bytes(buffer, redwood_qmi_encode(QMI_REQUEST, 0x22, 9, qmi_wlanfw_wlan_mode_req_msg_v01_ei, &req, buffer,
		    sizeof(buffer)), want, sizeof(want), "WLFW mode request");
		memset(&back, 0, sizeof(back));
		check(redwood_qmi_decode(want, sizeof(want), qmi_wlanfw_wlan_mode_req_msg_v01_ei, &back) == (int)sizeof(want) &&
		    back.mode == 7 && back.hw_debug_valid == 1 && back.hw_debug == 0, "WLFW mode request, decoded");
	}
	/* a piece of the board file: an array of bytes with a 2-byte count */
	{
		static const uint8_t want[] = {
			0x00, 0x02, 0x01, 0x25, 0x00, 0x2b, 0x00,
			0x01, 0x01, 0x00, 0x01,                                 /* valid */
			0x10, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,               /* file_id */
			0x11, 0x04, 0x00, 0x05, 0x00, 0x00, 0x00,               /* total_size */
			0x12, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00,               /* seg_id */
			0x13, 0x07, 0x00, 0x05, 0x00, 0xde, 0xad, 0xbe, 0xef, 0x01,     /* data */
			0x14, 0x01, 0x00, 0x01,                                 /* end */
			0x15, 0x01, 0x00, 0x01,                                 /* bdf_type: ELF */
		};
		static struct qmi_wlanfw_bdf_download_req_msg_v01 req, back;

		memset(&req, 0, sizeof(req));
		req.valid = 1;
		req.file_id_valid = 1;
		req.file_id = 0;
		req.total_size_valid = 1;
		req.total_size = 5;
		req.seg_id_valid = 1;
		req.seg_id = 2;
		req.data_valid = 1;
		req.data_len = 5;
		memcpy(req.data, "\xde\xad\xbe\xef\x01", 5);
		req.end_valid = 1;
		req.end = 1;
		req.bdf_type_valid = 1;
		req.bdf_type = 1;
		check_bytes(buffer, redwood_qmi_encode(QMI_REQUEST, 0x25, 0x0102, qmi_wlanfw_bdf_download_req_msg_v01_ei, &req, buffer,
		    sizeof(buffer)), want, sizeof(want), "WLFW board data request");
		memset(&back, 0, sizeof(back));
		check(redwood_qmi_decode(want, sizeof(want), qmi_wlanfw_bdf_download_req_msg_v01_ei, &back) == (int)sizeof(want) &&
		    back.data_valid == 1 && back.data_len == 5 && memcmp(back.data, "\xde\xad\xbe\xef\x01", 5) == 0 && back.seg_id == 2 &&
		    back.end == 1 && back.bdf_type == 1, "WLFW board data request, decoded");
	}
	/* what the firmware says of itself: structures, a string inside one, a string at the top level */
	{
		static const uint8_t reply[] = {
			0x02, 0x03, 0x00, 0x24, 0x00, 0x61, 0x00,
			0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,               /* result */
			0x10, 0x08, 0x00, 0x18, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,       /* chip_info: id 0x18, family 0x0b */
			0x11, 0x04, 0x00, 0xff, 0x00, 0x00, 0x00,               /* board_info: 0xff */
			0x12, 0x04, 0x00, 0x20, 0x03, 0x02, 0x40,               /* soc_info */
			0x13, 0x15, 0x00, 0x21, 0x43, 0x65, 0x87, 0x10,         /* fw_version_info: a number, then a string of 16 */
			'2', '0', '2', '3', '-', '0', '4', '-', '1', '5', ' ', '0', '1', ':', '2', '7',
			0x14, 0x22, 0x00,                                       /* fw_build_id: 34 characters, no length before them */
			'W', 'L', 'A', 'N', '.', 'M', 'S', 'L', '.', '1', '.', '0', '.', '1', '-', '0', '1', '2', '7', '7', '-',
			'Q', 'C', 'A', 'M', 'S', 'L', 'S', 'W', 'C', 'B', 'T', '-', '1',
			0x15, 0x01, 0x00, 0x02,                                 /* num_macs */
		};
		static struct qmi_wlanfw_cap_resp_msg_v01 cap;

		memset(&cap, 0, sizeof(cap));
		check(redwood_qmi_decode(reply, sizeof(reply), qmi_wlanfw_cap_resp_msg_v01_ei, &cap) == (int)sizeof(reply),
		    "WLFW capabilities reply is not decoded");
		check(cap.resp.result == 0 && cap.chip_info_valid == 1 && cap.chip_info.chip_id == 0x18 && cap.chip_info.chip_family == 0x0b &&
		    cap.board_info_valid == 1 && cap.board_info.board_id == 0xff && cap.soc_info_valid == 1 && cap.soc_info.soc_id == 0x40020320 &&
		    cap.fw_version_info_valid == 1 && cap.fw_version_info.fw_version == 0x87654321 &&
		    strcmp(cap.fw_version_info.fw_build_timestamp, "2023-04-15 01:27") == 0 && cap.fw_build_id_valid == 1 &&
		    strcmp(cap.fw_build_id, "WLAN.MSL.1.0.1-01277-QCAMSLSWCBT-1") == 0 && cap.num_macs_valid == 1 && cap.num_macs == 2 &&
		    cap.voltage_mv_valid == 0, "WLFW capabilities reply: the fields are not what the bytes say");
		check_bytes(buffer, redwood_qmi_encode(QMI_RESPONSE, 0x24, 3, qmi_wlanfw_cap_resp_msg_v01_ei, &cap, buffer, sizeof(buffer)),
		    reply, sizeof(reply), "WLFW capabilities reply, encoded again");
	}
	/* "give me memory": an array of structures, each with an array of structures in it */
	{
		static const uint8_t indication[] = {
			0x04, 0x00, 0x00, 0x35, 0x00, 0x23, 0x00,
			0x01, 0x20, 0x00, 0x02,                                 /* two segments */
			0x00, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,   /*   4 MB of type 1, one configuration: */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /*     offset 0 (8 bytes), size 0, secure 0 */
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,   /*   512 KB of type 4, no configuration */
		};
		static struct qmi_wlanfw_request_mem_ind_msg_v01 ind;

		memset(&ind, 0, sizeof(ind));
		check(redwood_qmi_decode(indication, sizeof(indication), qmi_wlanfw_request_mem_ind_msg_v01_ei, &ind) == (int)sizeof(indication),
		    "WLFW memory request is not decoded");
		check(ind.mem_seg_len == 2 && ind.mem_seg[0].size == 0x400000 && ind.mem_seg[0].type == 1 && ind.mem_seg[0].mem_cfg_len == 1 &&
		    ind.mem_seg[1].size == 0x80000 && ind.mem_seg[1].type == 4 && ind.mem_seg[1].mem_cfg_len == 0,
		    "WLFW memory request: the fields are not what the bytes say");
	}
}

/* ---- 2. the coding against itself, with every table of ath11k -------------------------------------- */

static uint64_t seed = 0x9e3779b97f4a7c15ULL;

static uint64_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

static void
put_native(uint8_t *to, uint64_t value, unsigned size)
{
	memcpy(to, &value, size);       /* this machine is little-endian, as the phone is */
}

static uint64_t
get_native(const uint8_t *from, unsigned size)
{
	uint64_t value = 0;

	memcpy(&value, from, size);
	return value;
}

enum fill { FILL_MOST, FILL_RANDOM };

/* Fill the structure a table describes; the table is walked the way the format's description walks it. */
static void
fill(const struct qmi_elem_info *ei, uint8_t *s, bool top, enum fill how)
{
	uint32_t count = 1;

	for (const struct qmi_elem_info *e = ei; e->data_type != QMI_EOTI; e++) {
		if (e->data_type == QMI_OPT_FLAG) {
			s[e->offset] = how == FILL_MOST || (rnd() & 1);
			if (!s[e->offset]) {
				while (e[1].data_type != QMI_EOTI && e[1].tlv_type == e->tlv_type && top) {
					e++;
				}
			}
			continue;
		}
		if (e->data_type == QMI_DATA_LEN) {
			count = how == FILL_MOST ? e[1].elem_len : (uint32_t)(rnd() % (e[1].elem_len + 1));
			put_native(s + e->offset, count, 4);
			continue;
		}
		if (e->array_type == NO_ARRAY) {
			count = 1;
		} else if (e->array_type == STATIC_ARRAY) {
			count = e->elem_len;
		}
		for (uint32_t i = 0; i < count; i++) {
			uint8_t *at = s + e->offset + (size_t)i * e->elem_size;

			if (e->data_type == QMI_STRUCT) {
				fill(e->ei_array, at, false, how);
			} else if (e->data_type == QMI_STRING) {
				size_t length = how == FILL_MOST ? e->elem_len - 1 : rnd() % e->elem_len;

				for (size_t k = 0; k < length; k++) {
					at[k] = (uint8_t)('a' + rnd() % 26);
				}
				at[length] = 0;
			} else {
				put_native(at, rnd(), e->elem_size);
			}
		}
	}
}

/* Do two structures say the same, as far as the table makes them say anything? */
static bool
same(const struct qmi_elem_info *ei, const uint8_t *a, const uint8_t *b, bool top)
{
	uint32_t count = 1;

	for (const struct qmi_elem_info *e = ei; e->data_type != QMI_EOTI; e++) {
		if (e->data_type == QMI_OPT_FLAG) {
			if (!a[e->offset] != !b[e->offset]) {
				return false;
			}
			if (!a[e->offset]) {
				while (e[1].data_type != QMI_EOTI && e[1].tlv_type == e->tlv_type && top) {
					e++;
				}
			}
			continue;
		}
		if (e->data_type == QMI_DATA_LEN) {
			count = (uint32_t)get_native(a + e->offset, 4);
			if (count != get_native(b + e->offset, 4)) {
				return false;
			}
			continue;
		}
		if (e->array_type == NO_ARRAY) {
			count = 1;
		} else if (e->array_type == STATIC_ARRAY) {
			count = e->elem_len;
		}
		for (uint32_t i = 0; i < count; i++) {
			const uint8_t *x = a + e->offset + (size_t)i * e->elem_size, *y = b + e->offset + (size_t)i * e->elem_size;

			if (e->data_type == QMI_STRUCT ? !same(e->ei_array, x, y, false) : e->data_type == QMI_STRING ?
			    strcmp((const char *)x, (const char *)y) != 0 : memcmp(x, y, e->elem_size) != 0) {
				return false;
			}
		}
	}
	return true;
}

/* What a table is expected to be like: said once, so that a table of another kind does not go unnoticed. */
static void
check_table(const struct test_message *m, const struct qmi_elem_info *ei, size_t size, bool top)
{
	for (const struct qmi_elem_info *e = ei; e->data_type != QMI_EOTI; e++) {
		check(e - ei < 100, "%s: a table without an end", m->name);
		check(top || e->data_type != QMI_OPT_FLAG, "%s: an optional element inside a structure", m->name);
		check(e->data_type != QMI_DATA_LEN || (e[1].array_type == VAR_LEN_ARRAY && (e->elem_size == 1 || e->elem_size == 2) &&
		    e->offset + 4 <= size), "%s: a count that is not a 32-bit field before a variable array", m->name);
		check(e->array_type != VAR_LEN_ARRAY || (e > ei && e[-1].data_type == QMI_DATA_LEN), "%s: a variable array without a count", m->name);
		check(e->data_type == QMI_OPT_FLAG || e->data_type == QMI_DATA_LEN ||
		    e->offset + (size_t)e->elem_size * (e->data_type == QMI_STRING || e->array_type == NO_ARRAY ? 1 : e->elem_len) *
		    (e->data_type == QMI_STRING ? e->elem_len : 1) <= size, "%s: an element beyond the end of its structure", m->name);
		if (e->data_type == QMI_STRUCT) {
			check_table(m, e->ei_array, e->elem_size, false);
		}
	}
}

static void
test_ath11k_tables(void)
{
	static uint8_t message[16384], again[16384], damaged[16384];
	unsigned with_limit = 0, exact = 0, messages = 0;

	for (unsigned t = 0; t < sizeof(ath11k_messages) / sizeof(ath11k_messages[0]); t++) {
		const struct test_message *m = &ath11k_messages[t];
		uint8_t *a = calloc(1, m->size + 16), *b = calloc(1, m->size + 16);
		int length, second;

		check_table(m, m->ei, m->size, true);
		if (!m->message) {
			free(a);
			free(b);
			continue;               /* a structure inside a message: gone through with the messages that have it */
		}
		messages++;

		/* filled to the brim: as long as ath11k's header says the message can get */
		fill(m->ei, a, true, FILL_MOST);
		length = redwood_qmi_encode(QMI_REQUEST, 1, 1, m->ei, a, message, sizeof(message));
		check(length >= QMI_HEADER_SIZE, "%s: cannot be encoded when full (%d)", m->name, length);
		if (m->limit >= 0) {
			/*
			 * Two of ath11k's tables say less than the interface they are of, and its header's figure
			 * is the interface's: "rejuvenate_enable" is a 32-bit number there and in the C structure
			 * and one byte in the table (3 bytes less), and the host capabilities' table has 14 of the
			 * interface's elements (72 bytes less). What counts on the wire is the table, for Linux too.
			 */
			int expected = strcmp(m->name, "qmi_wlanfw_ind_register_req_msg_v01") == 0 ? m->limit - 3 :
			    strcmp(m->name, "qmi_wlanfw_host_cap_req_msg_v01") == 0 ? m->limit - 72 : m->limit;

			with_limit++;
			exact += expected == m->limit;
			check(length - QMI_HEADER_SIZE == expected, "%s: full, it encodes to %d bytes, not %d (ath11k's header says %d at most)",
			    m->name, length - QMI_HEADER_SIZE, expected, m->limit);
		}
		check(redwood_qmi_decode(message, (size_t)length, m->ei, b) == length && same(m->ei, a, b, true), "%s: full, it does not come back the same",
		    m->name);
		/* (the bytes after a structure are guard bytes: nothing may have been written there) */
		check(memcmp(b + m->size, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0, "%s: decoding wrote past the structure", m->name);

		/* at random, 300 times: encode, decode, compare; encode what was decoded, compare the bytes */
		for (int round = 0; round < 300; round++) {
			memset(a, 0, m->size);
			memset(b, 0, m->size);
			fill(m->ei, a, true, FILL_RANDOM);
			length = redwood_qmi_encode(QMI_INDICATION, (uint16_t)rnd(), (uint16_t)rnd(), m->ei, a, message, sizeof(message));
			if (length < 0 || redwood_qmi_decode(message, (size_t)length, m->ei, b) != length || !same(m->ei, a, b, true)) {
				check(false, "%s: round %d: does not come back the same (%d)", m->name, round, length);
				break;
			}
			second = redwood_qmi_encode(message[0], (uint16_t)(message[3] | message[4] << 8), (uint16_t)(message[1] | message[2] << 8),
			    m->ei, b, again, sizeof(again));
			if (second != length || memcmp(message, again, (size_t)length) != 0) {
				check(false, "%s: round %d: encoded again, the bytes differ", m->name, round);
				break;
			}
			/* and the same message damaged: it may be refused, it may not be read or written out of bounds */
			for (int k = 0; k < 10 && length > QMI_HEADER_SIZE; k++) {
				size_t cut = (size_t)length;

				memcpy(damaged, message, (size_t)length);
				switch (rnd() % 4) {
				case 0: damaged[QMI_HEADER_SIZE + rnd() % (size_t)(length - QMI_HEADER_SIZE)] ^= (uint8_t)(1u << (rnd() % 8)); break;
				case 1: damaged[QMI_HEADER_SIZE + rnd() % (size_t)(length - QMI_HEADER_SIZE)] = (uint8_t)rnd(); break;
				case 2: cut = QMI_HEADER_SIZE + rnd() % (size_t)(length - QMI_HEADER_SIZE);
					damaged[5] = (uint8_t)(cut - QMI_HEADER_SIZE); damaged[6] = (uint8_t)((cut - QMI_HEADER_SIZE) >> 8); break;
				default: damaged[5] = (uint8_t)rnd(); damaged[6] = (uint8_t)rnd(); break;
				}
				memset(b, 0, m->size);
				{
					/* exactly as much memory as the message is long, so that the sanitizer sees a read past it */
					uint8_t *exact = malloc(cut ? cut : 1);

					memcpy(exact, damaged, cut);
					(void)redwood_qmi_decode(exact, cut, m->ei, b);
					free(exact);
				}
				checks++;
			}
		}
		check(memcmp(b + m->size, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0, "%s: decoding a damaged message wrote past the structure", m->name);
		free(a);
		free(b);
	}
	printf("  ath11k's tables: %zu, of them %u messages; %u have a largest length in ath11k's header, %u of them met to the byte\n",
	    sizeof(ath11k_messages) / sizeof(ath11k_messages[0]), messages, with_limit, exact);
}

#endif /* !NO_ATH11K */

/* ---- 3. the helpers, over a router made up here ------------------------------------------------------ */

#define SERVER_NODE     7
#define SERVER_PORT     0x4001

struct redwood_qrtr_port {
	redwood_qrtr_receive_t receive;
	redwood_qrtr_server_t server;
	void *context;
	bool open;
	unsigned lookups, sent, closed;
	uint32_t looked_service, looked_version, looked_instance;
	uint32_t last_node, last_port;
	uint8_t last[8192 + 16];
	size_t last_length;
	int send_error;                 /* what send answers */
	/* what the other side does with a datagram: called from send (as a router that delivers at once would) */
	void (*peer)(struct redwood_qrtr_port *port, const uint8_t *data, size_t length);
};
static struct redwood_qrtr_port router;

static struct redwood_qrtr_port *
router_open(redwood_qrtr_receive_t receive, redwood_qrtr_server_t server, void *context)
{
	memset(&router, 0, sizeof(router));
	router.receive = receive;
	router.server = server;
	router.context = context;
	router.open = true;
	return &router;
}

static int
router_send(struct redwood_qrtr_port *port, uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	if (port->send_error) {
		return port->send_error;
	}
	port->sent++;
	port->last_node = node;
	port->last_port = to_port;
	port->last_length = length;
	memcpy(port->last, data, length < sizeof(port->last) ? length : sizeof(port->last));
	if (port->peer != NULL) {
		port->peer(port, data, length);
	}
	return 0;
}

static int
router_lookup(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	port->lookups++;
	port->looked_service = service;
	port->looked_version = version;
	port->looked_instance = instance;
	return 0;
}

static void
router_close(struct redwood_qrtr_port *port)
{
	port->open = false;
	port->closed++;
}

static const struct redwood_qrtr_ops router_ops = { router_open, router_send, router_lookup, router_close };

/* A datagram from the server to the handle, as the router's thread would bring it. */
static void
deliver(const uint8_t *data, size_t length)
{
	router.receive(router.context, SERVER_NODE, SERVER_PORT, data, length);
}

/* ... a while later, from another thread */
struct later {
	uint8_t data[8192];
	size_t length;
	unsigned ms;
};

static void *
later_thread(void *argument)
{
	struct later *l = argument;

	usleep(l->ms * 1000);
	deliver(l->data, l->length);
	free(l);
	return NULL;
}

static pthread_t
deliver_later(const uint8_t *data, size_t length, unsigned ms)
{
	struct later *l = malloc(sizeof(*l));
	pthread_t thread;

	memcpy(l->data, data, length);
	l->length = length;
	l->ms = ms;
	pthread_create(&thread, NULL, later_thread, l);
	return thread;
}

/* A reply of 7 bytes of result behind a header that answers the request in `request`. */
static size_t
plain_reply(uint8_t *reply, const uint8_t *request, uint16_t result, uint16_t error)
{
	static const uint8_t body[] = { 0x02, 0x04, 0x00 };

	reply[0] = QMI_RESPONSE;
	memcpy(reply + 1, request + 1, 4);      /* the transaction and the message id */
	reply[5] = 7;
	reply[6] = 0;
	memcpy(reply + 7, body, 3);
	reply[10] = (uint8_t)result;
	reply[11] = (uint8_t)(result >> 8);
	reply[12] = (uint8_t)error;
	reply[13] = (uint8_t)(error >> 8);
	return 14;
}

/* a message of our own for this part: a request with one number, a reply with result and one number */
struct ask {
	uint32_t number;
};
struct answer {
	struct qmi_response_type_v01 resp;
	uint8_t twice_valid;
	uint32_t twice;
};
static const struct qmi_elem_info ask_ei[] = {
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x01, offsetof(struct ask, number), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
static const struct qmi_elem_info answer_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 0x02, offsetof(struct answer, resp), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x10, offsetof(struct answer, twice_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x10, offsetof(struct answer, twice), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
struct news {
	uint8_t what_valid;
	uint32_t what;
};
static const struct qmi_elem_info news_ei[] = {
	{ QMI_OPT_FLAG, 1, sizeof(uint8_t), NO_ARRAY, 0x10, offsetof(struct news, what_valid), NULL },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(uint32_t), NO_ARRAY, 0x10, offsetof(struct news, what), NULL },
	{ QMI_EOTI, 0, 0, NO_ARRAY, 0, 0, NULL },
};
#define MESSAGE_ASK     0x0077
#define MESSAGE_NEWS    0x0078

static struct {
	unsigned arrived, left, news;
	struct qmi_service last;
	struct qmi_service *last_pointer;
	int answer;                     /* what new_server says */
	struct news last_news;
	struct sockaddr_qrtr news_from;
	uint16_t news_transaction;
	bool news_sends;                /* the handler sends a request of its own, as ath11k's handlers may */
} seen;

static int
on_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct sockaddr_qrtr sq = { .sq_family = AF_QIPCRTR, .sq_node = service->node, .sq_port = service->port };

	seen.arrived++;
	seen.last = *service;
	seen.last_pointer = service;
	if (seen.answer < 0) {
		return seen.answer;
	}
	/* what ath11k does here, word for word but for the type its Linux has for an address */
	return kernel_connect(qmi->sock, (void *)&sq, sizeof(sq), 0);
}

static void
on_del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	(void)qmi;
	seen.left++;
	check(service == seen.last_pointer, "del_server is given another service than new_server was");
}

static void
on_news(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	seen.news++;
	seen.last_news = *(const struct news *)decoded;
	seen.news_from = *sq;
	seen.news_transaction = txn->id;
	if (seen.news_sends) {
		struct ask ask = { 5 };
		struct qmi_txn mine;

		check(qmi_txn_init(qmi, &mine, NULL, NULL) >= 0 && qmi_send_request(qmi, NULL, &mine, MESSAGE_ASK, 16, ask_ei, &ask) == 0,
		    "a handler cannot send");
		qmi_txn_cancel(&mine);
	}
}

static const struct qmi_ops handle_ops = { .new_server = on_new_server, .del_server = on_del_server };
static const struct qmi_msg_handler handle_handlers[] = {
	{ .type = QMI_INDICATION, .msg_id = MESSAGE_NEWS, .ei = news_ei, .decoded_size = sizeof(struct news), .fn = on_news },
	{ 0 },
};

/* The peer of the simplest kind: answers "ask" with twice the number, at once. */
static void
doubling_peer(struct redwood_qrtr_port *port, const uint8_t *data, size_t length)
{
	uint8_t reply[32];
	size_t n;

	(void)port;
	if (length != 14 || data[0] != QMI_REQUEST || (data[3] | data[4] << 8) != MESSAGE_ASK) {
		return;
	}
	n = plain_reply(reply, data, 0, 0);
	reply[5] = 14;
	reply[n++] = 0x10;
	reply[n++] = 4;
	reply[n++] = 0;
	{
		uint32_t twice = 2 * (uint32_t)(data[10] | data[11] << 8 | data[12] << 16 | (uint32_t)data[13] << 24);

		memcpy(reply + n, &twice, 4);
	}
	deliver(reply, n + 4);
}

struct asker {
	struct qmi_handle *qmi;
	uint32_t number;
	int result;
	struct answer answer;
	uint16_t id;
};

static void *
asker_thread(void *argument)
{
	struct asker *a = argument;
	struct ask ask = { a->number };
	struct qmi_txn txn;

	memset(&a->answer, 0, sizeof(a->answer));
	a->id = (uint16_t)qmi_txn_init(a->qmi, &txn, answer_ei, &a->answer);
	a->result = qmi_send_request(a->qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask);
	if (a->result == 0) {
		a->result = redwood_qmi_txn_wait_ms(&txn, 3000);
	}
	return NULL;
}

static void
test_helpers(void)
{
	struct qmi_handle qmi;
	struct qmi_txn txn, other;
	struct ask ask = { 21 };
	struct answer answer;
	uint8_t reply[64];
	uint64_t start;
	size_t n;
	int id, result;

	/* no router: no handle */
	redwood_qmi_set_transport(NULL);
	check(qmi_handle_init(&qmi, 8192, &handle_ops, handle_handlers) == -ENXIO && qmi.sock == NULL, "a handle is made with no router there");

	redwood_qmi_set_transport(&router_ops);
	memset(&seen, 0, sizeof(seen));
	check(qmi_handle_init(&qmi, 8192, &handle_ops, handle_handlers) == 0 && qmi.sock != NULL && router.open, "no handle with a router there");

	/* a request before anybody is connected to */
	id = qmi_txn_init(&qmi, &txn, answer_ei, &answer);
	check(id >= 0 && txn.id == id, "no transaction (%d)", id);
	check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == -ENOTCONN && router.sent == 0, "a request goes out with nowhere to go");
	qmi_txn_cancel(&txn);

	/* looking the service up; a server appears */
	check(qmi_add_lookup(&qmi, 0x45, 1, 3) == 0 && router.lookups == 1 && router.looked_service == 0x45 && router.looked_version == 1 &&
	    router.looked_instance == 3, "the lookup does not reach the router");
	router.server(router.context, true, 0x45, 1, 3, SERVER_NODE, SERVER_PORT);
	check(seen.arrived == 1 && seen.last.service == 0x45 && seen.last.version == 1 && seen.last.instance == 3 && seen.last.node == SERVER_NODE &&
	    seen.last.port == SERVER_PORT, "new_server is not told of the server");
	router.server(router.context, true, 0x45, 1, 3, SERVER_NODE, SERVER_PORT);
	check(seen.arrived == 1, "the same server arrives twice");

	/* a request and its reply, delivered at once (inside send) */
	router.peer = doubling_peer;
	memset(&answer, 0, sizeof(answer));
	id = qmi_txn_init(&qmi, &txn, answer_ei, &answer);
	check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == 0, "the request is not sent");
	{
		const uint8_t want[] = { 0x00, (uint8_t)id, (uint8_t)(id >> 8), 0x77, 0x00, 0x07, 0x00, 0x01, 0x04, 0x00, 21, 0, 0, 0 };

		check_bytes(router.last, (int)router.last_length, want, sizeof(want), "the request as the router got it");
		check(router.last_node == SERVER_NODE && router.last_port == SERVER_PORT, "the request goes to %u:%u", router.last_node, router.last_port);
	}
	result = redwood_qmi_txn_wait_ms(&txn, 1000);
	check(result == 21 && answer.resp.result == 0 && answer.twice_valid == 1 && answer.twice == 42, "the reply does not arrive (%d, %u)", result,
	    answer.twice);

	/* ... and from the router's own thread, 50 ms later, while the caller waits */
	router.peer = NULL;
	memset(&answer, 0, sizeof(answer));
	id = qmi_txn_init(&qmi, &txn, answer_ei, &answer);
	check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == 0, "the request is not sent");
	n = plain_reply(reply, router.last, 1, QMI_ERR_INTERNAL_V01);
	start = now_ms();
	{
		pthread_t thread = deliver_later(reply, n, 50);

		/* in the caller's jiffies: HZ is 250 here, so 250 of them are a second */
		result = qmi_txn_wait(&txn, 250);
		pthread_join(thread, NULL);
	}
	check(result == 14 && answer.resp.result == 1 && answer.resp.error == QMI_ERR_INTERNAL_V01 && answer.twice_valid == 0 &&
	    now_ms() - start >= 45 && now_ms() - start < 900, "the reply from another thread (%d after %llu ms)", result,
	    (unsigned long long)(now_ms() - start));

	/* no reply: the wait ends when it says it will; the reply that comes after is nobody's */
	id = qmi_txn_init(&qmi, &txn, answer_ei, &answer);
	check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == 0, "the request is not sent");
	n = plain_reply(reply, router.last, 0, 0);
	start = now_ms();
	result = qmi_txn_wait(&txn, 30);                /* 30 jiffies of 4 ms */
	check(result == -ETIMEDOUT && now_ms() - start >= 115 && now_ms() - start < 600, "the wait for nothing: %d after %llu ms", result,
	    (unsigned long long)(now_ms() - start));
	memset(&answer, 0x55, sizeof(answer));
	deliver(reply, n);
	check(answer.resp.result == 0x5555 && seen.news == 0, "a reply after its time is still taken");

	/* a transaction given up; a reply with another number; a reply that cannot be decoded */
	id = qmi_txn_init(&qmi, &txn, answer_ei, &answer);
	check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == 0, "the request is not sent");
	n = plain_reply(reply, router.last, 0, 0);
	qmi_txn_cancel(&txn);
	deliver(reply, n);
	check(answer.resp.result == 0x5555, "a reply to a transaction given up is taken");

	memset(&answer, 0, sizeof(answer));
	id = qmi_txn_init(&qmi, &txn, answer_ei, &answer);
	check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == 0, "the request is not sent");
	n = plain_reply(reply, router.last, 0, 0);
	reply[1] ^= 0x40;
	deliver(reply, n);                              /* not ours */
	reply[1] ^= 0x40;
	reply[8] = 9;                                   /* the TLV says more than the message has */
	deliver(reply, n);
	result = redwood_qmi_txn_wait_ms(&txn, 200);
	check(result == -EINVAL, "a reply that cannot be decoded gives %d", result);

	/* three at once have three numbers; two threads ask, the replies come in the other order */
	{
		struct qmi_txn t1, t2, t3;
		int a = qmi_txn_init(&qmi, &t1, NULL, NULL), b = qmi_txn_init(&qmi, &t2, NULL, NULL), c = qmi_txn_init(&qmi, &t3, NULL, NULL);
		struct asker one = { .qmi = &qmi, .number = 100 }, two = { .qmi = &qmi, .number = 200 };
		pthread_t first, second;
		uint8_t r1[32], r2[32];
		uint32_t value;

		check(a >= 0 && b >= 0 && c >= 0 && a != b && b != c && a != c, "three transactions: %d %d %d", a, b, c);
		qmi_txn_cancel(&t1);
		qmi_txn_cancel(&t2);
		qmi_txn_cancel(&t3);

		pthread_create(&first, NULL, asker_thread, &one);
		usleep(30000);
		memcpy(r1, router.last, 14);
		pthread_create(&second, NULL, asker_thread, &two);
		usleep(30000);
		memcpy(r2, router.last, 14);
		check(memcmp(r1 + 1, r2 + 1, 2) != 0, "two requests in the air have one number");
		/* the second one's reply first; each reply says the number that was asked */
		for (int k = 1; k >= 0; k--) {
			const uint8_t *request = k ? r2 : r1;

			n = plain_reply(reply, request, 0, 0);
			reply[5] = 14;
			memcpy(reply + n, "\x10\x04\x00", 3);
			memcpy(&value, request + 10, 4);
			memcpy(reply + n + 3, &value, 4);
			deliver(reply, n + 7);
		}
		pthread_join(first, NULL);
		pthread_join(second, NULL);
		check(one.result == 21 && two.result == 21 && one.answer.twice == 100 && two.answer.twice == 200, "crossed replies: %d %u, %d %u",
		    one.result, one.answer.twice, two.result, two.answer.twice);
	}

	/* an indication: to its handler, decoded; from the handler a request may be sent */
	{
		static const uint8_t news[] = { 0x04, 0x33, 0x00, 0x78, 0x00, 0x07, 0x00, 0x10, 0x04, 0x00, 0xef, 0xbe, 0xad, 0xde };
		static const uint8_t nobody[] = { 0x04, 0x00, 0x00, 0x99, 0x00, 0x00, 0x00 };
		static const uint8_t request_to_us[] = { 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00 };
		static const uint8_t bad_news[] = { 0x04, 0x00, 0x00, 0x78, 0x00, 0x04, 0x00, 0x10, 0x04, 0x00, 0xef };
		static uint8_t huge[8192 + 7 + 1];
		unsigned sent = router.sent;

		seen.news_sends = true;
		deliver(news, sizeof(news));
		check(seen.news == 1 && seen.last_news.what_valid == 1 && seen.last_news.what == 0xdeadbeef && seen.news_from.sq_family == AF_QIPCRTR &&
		    seen.news_from.sq_node == SERVER_NODE && seen.news_from.sq_port == SERVER_PORT && seen.news_transaction == 0x33,
		    "the indication does not reach its handler as sent");
		check(router.sent == sent + 1, "the handler's own request did not go out");
		seen.news_sends = false;
		deliver(nobody, sizeof(nobody));
		deliver(request_to_us, sizeof(request_to_us));          /* the same id, but a request: no handler is for that */
		deliver(bad_news, sizeof(bad_news));
		deliver(news, 5);                                       /* shorter than a header */
		memcpy(huge, news, sizeof(news));
		deliver(huge, sizeof(huge));                            /* more than the handle said it takes */
		check(seen.news == 1, "a message that is no indication we know reached the handler (%u)", seen.news);
	}

	/* to another address; the router refusing; the service going away */
	{
		struct sockaddr_qrtr elsewhere = { .sq_family = AF_QIPCRTR, .sq_node = 9, .sq_port = 99 };

		qmi_txn_init(&qmi, &txn, NULL, NULL);
		check(qmi_send_request(&qmi, &elsewhere, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == 0 && router.last_node == 9 && router.last_port == 99,
		    "a request to a given address goes to %u:%u", router.last_node, router.last_port);
		router.send_error = EHOSTUNREACH;
		check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 16, ask_ei, &ask) == -EHOSTUNREACH, "the router's refusal is not passed on");
		router.send_error = 0;
		check(qmi_send_request(&qmi, NULL, &txn, MESSAGE_ASK, 2, ask_ei, &ask) == -EMSGSIZE, "a request longer than its caller allows goes out");
		qmi_txn_cancel(&txn);
		qmi_txn_init(&qmi, &other, NULL, NULL);
		qmi_txn_cancel(&other);
	}
	router.server(router.context, false, 0x45, 1, 3, SERVER_NODE, SERVER_PORT);
	check(seen.left == 1, "del_server is not told (%u)", seen.left);
	router.server(router.context, false, 0x45, 1, 3, SERVER_NODE, SERVER_PORT);
	check(seen.left == 1, "the same server leaves twice");
	/* a server new_server does not want is forgotten: its leaving is not told */
	seen.answer = -EIO;
	router.server(router.context, true, 0x45, 1, 3, SERVER_NODE, SERVER_PORT + 1);
	router.server(router.context, false, 0x45, 1, 3, SERVER_NODE, SERVER_PORT + 1);
	check(seen.arrived == 2 && seen.left == 1, "a refused server: arrived %u, left %u", seen.arrived, seen.left);
	seen.answer = 0;
	router.server(router.context, true, 0x45, 1, 3, SERVER_NODE, SERVER_PORT);      /* (one left behind for release to free) */

	qmi_handle_release(&qmi);
	check(router.closed == 1 && !router.open && qmi.sock == NULL, "release does not close the port");
	redwood_qmi_set_transport(NULL);
}

#ifndef NO_ATH11K

/* ---- ... and the whole exchange of a WCN6750 with ath11k, both played here -------------------------- */

/*
 * The firmware's side: answers each request as the real one's replies are described (and checks
 * what it is asked), and sends the indications in between from its own thread. The host's side
 * below does what ath11k's qmi.c does for this chip, in its order, with its message ids, its
 * structures and its largest lengths.
 */
static struct {
	unsigned char file[20000];      /* the board file the host sends */
	unsigned char got[20000];
	size_t got_length;
	unsigned segments, requests[0x50];
	bool complete, bad;
	uint32_t mode[4];
	unsigned modes;
	pthread_t threads[8];
	unsigned thread_count;
} firmware;

static void
firmware_indicates(uint16_t message, unsigned ms)
{
	uint8_t indication[7] = { QMI_INDICATION, 0, 0, (uint8_t)message, (uint8_t)(message >> 8), 0, 0 };

	firmware.threads[firmware.thread_count++] = deliver_later(indication, sizeof(indication), ms);
}

static void
firmware_peer(struct redwood_qrtr_port *port, const uint8_t *data, size_t length)
{
	static uint8_t reply[512];
	uint16_t message = (uint16_t)(data[3] | data[4] << 8);
	int n;

	(void)port;
	if (data[0] != QMI_REQUEST || message >= 0x50) {
		firmware.bad = true;
		return;
	}
	firmware.requests[message]++;
	n = (int)plain_reply(reply, data, 0, 0);
	switch (message) {
	case 0x20: {                    /* indication register */
		static struct qmi_wlanfw_ind_register_req_msg_v01 req;
		struct qmi_wlanfw_ind_register_resp_msg_v01 resp = { .fw_status_valid = 1, .fw_status = 0 };

		memset(&req, 0, sizeof(req));
		firmware.bad |= redwood_qmi_decode(data, length, qmi_wlanfw_ind_register_req_msg_v01_ei, &req) < 0 || !req.fw_ready_enable ||
		    !req.fw_init_done_enable || !req.cal_done_enable || req.request_mem_enable_valid;
		n = redwood_qmi_encode(QMI_RESPONSE, message, (uint16_t)(data[1] | data[2] << 8), qmi_wlanfw_ind_register_resp_msg_v01_ei, &resp,
		    reply, sizeof(reply));
		break;
	}
	case 0x34: {                    /* host capabilities */
		static struct qmi_wlanfw_host_cap_req_msg_v01 req;

		memset(&req, 0, sizeof(req));
		firmware.bad |= redwood_qmi_decode(data, length, qmi_wlanfw_host_cap_req_msg_v01_ei, &req) < 0 || !req.num_clients_valid ||
		    req.num_clients != 1 || !req.mem_cfg_mode_valid || req.m3_support != 0;
		break;
	}
	case 0x24: {                    /* capabilities */
		static struct qmi_wlanfw_cap_resp_msg_v01 resp;

		memset(&resp, 0, sizeof(resp));
		resp.chip_info_valid = 1;
		resp.chip_info.chip_id = 0x18;
		resp.chip_info.chip_family = 0x0b;
		resp.board_info_valid = 1;
		resp.board_info.board_id = 0xff;
		resp.soc_info_valid = 1;
		resp.soc_info.soc_id = 0x40020320;
		resp.fw_version_info_valid = 1;
		resp.fw_version_info.fw_version = 0x10100504;
		strcpy(resp.fw_version_info.fw_build_timestamp, "2023-04-15 01:27");
		resp.fw_build_id_valid = 1;
		strcpy(resp.fw_build_id, "WLAN.MSL.1.0.1-01277-QCAMSLSWCBT-1");
		firmware.bad |= length != QMI_HEADER_SIZE;
		n = redwood_qmi_encode(QMI_RESPONSE, message, (uint16_t)(data[1] | data[2] << 8), qmi_wlanfw_cap_resp_msg_v01_ei, &resp, reply,
		    sizeof(reply));
		break;
	}
	case 0x4c: {                    /* device info: where the registers are */
		struct qmi_wlanfw_device_info_resp_msg_v01 resp = { .bar_addr = 0x17a00000, .bar_size = 0x200000, .bar_addr_valid = 1,
			                                            .bar_size_valid = 1 };

		n = redwood_qmi_encode(QMI_RESPONSE, message, (uint16_t)(data[1] | data[2] << 8), qmi_wlfw_device_info_resp_msg_v01_ei, &resp,
		    reply, sizeof(reply));
		break;
	}
	case 0x25: {                    /* a piece of a file */
		static struct qmi_wlanfw_bdf_download_req_msg_v01 req;

		memset(&req, 0, sizeof(req));
		if (redwood_qmi_decode(data, length, qmi_wlanfw_bdf_download_req_msg_v01_ei, &req) < 0 || !req.valid || !req.data_valid ||
		    req.seg_id != firmware.segments || req.total_size != sizeof(firmware.file) ||
		    firmware.got_length + req.data_len > sizeof(firmware.got)) {
			firmware.bad = true;
			break;
		}
		memcpy(firmware.got + firmware.got_length, req.data, req.data_len);
		firmware.got_length += req.data_len;
		firmware.segments++;
		if (req.end) {
			firmware.complete = true;
			firmware_indicates(QMI_WLFW_FW_INIT_DONE_IND_V01, 20);
		}
		break;
	}
	case 0x22: {                    /* mode */
		struct qmi_wlanfw_wlan_mode_req_msg_v01 req = { 0 };

		firmware.bad |= redwood_qmi_decode(data, length, qmi_wlanfw_wlan_mode_req_msg_v01_ei, &req) < 0 || firmware.modes >= 4;
		firmware.mode[firmware.modes++ & 3] = req.mode;
		if (req.mode == 7) {            /* cold boot calibration: done after a while, then ready */
			firmware_indicates(QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01, 30);
			firmware_indicates(QMI_WLFW_FW_READY_IND_V01, 60);
		}
		break;
	}
	case 0x23: {                    /* configuration: the pipes */
		static struct qmi_wlanfw_wlan_cfg_req_msg_v01 req;

		memset(&req, 0, sizeof(req));
		firmware.bad |= redwood_qmi_decode(data, length, qmi_wlanfw_wlan_cfg_req_msg_v01_ei, &req) < 0 || !req.tgt_cfg_valid ||
		    req.tgt_cfg_len != 9 || req.tgt_cfg[8].pipe_num != 8 || !req.svc_cfg_valid || req.svc_cfg_len != 20 ||
		    strcmp(req.host_version, "WIN") != 0;
		break;
	}
	default:
		firmware.bad = true;
		break;
	}
	if (n > 0) {
		deliver(reply, (size_t)n);
	}
}

static struct {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	bool server, init_done, cal_done, ready;
	struct qmi_service service;
} host = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, false, false, { 0 } };

static void
host_event(bool *what)
{
	pthread_mutex_lock(&host.lock);
	*what = true;
	pthread_cond_broadcast(&host.changed);
	pthread_mutex_unlock(&host.lock);
}

static bool
host_waits(const bool *what, unsigned ms)
{
	struct timespec until;
	bool happened;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += (long)(ms % 1000) * 1000000;
	if (until.tv_nsec >= 1000000000) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000;
	}
	pthread_mutex_lock(&host.lock);
	while (!*what && pthread_cond_timedwait(&host.changed, &host.lock, &until) == 0) {
	}
	happened = *what;
	pthread_mutex_unlock(&host.lock);
	return happened;
}

static int
host_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct sockaddr_qrtr sq = { .sq_family = AF_QIPCRTR, .sq_node = service->node, .sq_port = service->port };
	int error = kernel_connect(qmi->sock, (void *)&sq, sizeof(sq), 0);

	host.service = *service;
	host_event(&host.server);
	return error;
}

static void
host_init_done(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi; (void)sq; (void)txn; (void)decoded;
	host_event(&host.init_done);
}

static void
host_cal_done(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi; (void)sq; (void)txn; (void)decoded;
	host_event(&host.cal_done);
}

static void
host_ready(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded)
{
	(void)qmi; (void)sq; (void)txn; (void)decoded;
	host_event(&host.ready);
}

static const struct qmi_ops host_ops = { .new_server = host_new_server };
/* (the same list as ath11k's qmi.c has, with its tables and sizes) */
static const struct qmi_msg_handler host_handlers[] = {
	{ .type = QMI_INDICATION, .msg_id = QMI_WLFW_FW_READY_IND_V01, .ei = qmi_wlanfw_fw_ready_ind_msg_v01_ei,
	  .decoded_size = sizeof(struct qmi_wlanfw_fw_ready_ind_msg_v01), .fn = host_ready },
	{ .type = QMI_INDICATION, .msg_id = QMI_WLFW_COLD_BOOT_CAL_DONE_IND_V01, .ei = qmi_wlanfw_cold_boot_cal_done_ind_msg_v01_ei,
	  .decoded_size = sizeof(struct qmi_wlanfw_fw_cold_cal_done_ind_msg_v01), .fn = host_cal_done },
	{ .type = QMI_INDICATION, .msg_id = QMI_WLFW_FW_INIT_DONE_IND_V01, .ei = qmi_wlfw_fw_init_done_ind_msg_v01_ei,
	  .decoded_size = sizeof(struct qmi_wlfw_fw_init_done_ind_msg_v01), .fn = host_init_done },
	{ 0 },
};

/* One exchange the way every one of qmi.c's ten goes. Returns what qmi_txn_wait() gave. */
static int
exchange(struct qmi_handle *qmi, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply)
{
	struct qmi_txn txn;
	int ret;

	ret = qmi_txn_init(qmi, &txn, reply_ei, reply);
	if (ret < 0) {
		return ret;
	}
	ret = qmi_send_request(qmi, NULL, &txn, message, largest, request_ei, request);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		return ret;
	}
	return qmi_txn_wait(&txn, msecs_to_jiffies(ATH11K_QMI_WLANFW_TIMEOUT_MS));
}

static void
test_conversation(void)
{
	static struct qmi_wlanfw_bdf_download_req_msg_v01 bdf;
	static struct qmi_wlanfw_wlan_cfg_req_msg_v01 cfg;
	static struct qmi_wlanfw_cap_resp_msg_v01 cap;
	static struct qmi_wlanfw_host_cap_req_msg_v01 host_cap;
	struct qmi_wlanfw_ind_register_req_msg_v01 reg;
	struct qmi_wlanfw_ind_register_resp_msg_v01 reg_resp = { 0 };
	struct qmi_wlanfw_host_cap_resp_msg_v01 host_cap_resp = { 0 };
	struct qmi_wlanfw_cap_req_msg_v01 cap_req = { 0 };
	struct qmi_wlanfw_device_info_req_msg_v01 info_req = { 0 };
	struct qmi_wlanfw_device_info_resp_msg_v01 info = { 0 };
	struct qmi_wlanfw_bdf_download_resp_msg_v01 bdf_resp;
	struct qmi_wlanfw_wlan_mode_req_msg_v01 mode;
	struct qmi_wlanfw_wlan_mode_resp_msg_v01 mode_resp = { 0 };
	struct qmi_wlanfw_wlan_cfg_resp_msg_v01 cfg_resp = { 0 };
	struct qmi_handle qmi;
	size_t left = sizeof(firmware.file), longest = 0;
	const unsigned char *at = firmware.file;
	int ret;

	for (size_t i = 0; i < sizeof(firmware.file); i++) {
		firmware.file[i] = (unsigned char)(rnd() >> 11);
	}
	redwood_qmi_set_transport(&router_ops);
	check(qmi_handle_init(&qmi, ATH11K_QMI_RESP_LEN_MAX, &host_ops, host_handlers) == 0, "conversation: no handle");
	router.peer = firmware_peer;
	check(qmi_add_lookup(&qmi, ATH11K_QMI_WLFW_SERVICE_ID_V01, ATH11K_QMI_WLFW_SERVICE_VERS_V01,
	    ATH11K_QMI_WLFW_SERVICE_INS_ID_V01_WCN6750) == 0 && router.looked_service == 0x45 && router.looked_instance == 3, "conversation: lookup");
	router.server(router.context, true, 0x45, 1, 3, SERVER_NODE, SERVER_PORT);
	check(host_waits(&host.server, 1000), "conversation: the server does not arrive");

	/* indication register: fixed firmware memory, so no memory indications are asked for */
	memset(&reg, 0, sizeof(reg));
	reg.client_id_valid = 1;
	reg.client_id = 0x4b4e454c;
	reg.fw_ready_enable_valid = 1;
	reg.fw_ready_enable = 1;
	reg.cal_done_enable_valid = 1;
	reg.cal_done_enable = 1;
	reg.fw_init_done_enable_valid = 1;
	reg.fw_init_done_enable = 1;
	reg.pin_connect_result_enable_valid = 1;
	reg.pin_connect_result_enable = 1;
	ret = exchange(&qmi, 0x20, QMI_WLANFW_IND_REGISTER_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_ind_register_req_msg_v01_ei, &reg,
	    qmi_wlanfw_ind_register_resp_msg_v01_ei, &reg_resp);
	check(ret >= 0 && reg_resp.resp.result == QMI_RESULT_SUCCESS_V01 && reg_resp.fw_status_valid == 1, "conversation: indication register (%d)", ret);

	memset(&host_cap, 0, sizeof(host_cap));
	host_cap.num_clients_valid = 1;
	host_cap.num_clients = 1;
	host_cap.mem_cfg_mode = 0;
	host_cap.mem_cfg_mode_valid = 1;
	host_cap.bdf_support_valid = 1;
	host_cap.bdf_support = 1;
	host_cap.m3_support_valid = 1;
	host_cap.m3_support = 0;
	host_cap.m3_cache_support_valid = 1;
	host_cap.m3_cache_support = 0;
	host_cap.cal_done_valid = 1;
	host_cap.cal_done = 0;
	ret = exchange(&qmi, 0x34, QMI_WLANFW_HOST_CAP_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_host_cap_req_msg_v01_ei, &host_cap,
	    qmi_wlanfw_host_cap_resp_msg_v01_ei, &host_cap_resp);
	check(ret >= 0 && host_cap_resp.resp.result == 0, "conversation: host capabilities (%d)", ret);

	memset(&cap, 0, sizeof(cap));
	ret = exchange(&qmi, 0x24, QMI_WLANFW_CAP_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_cap_req_msg_v01_ei, &cap_req, qmi_wlanfw_cap_resp_msg_v01_ei, &cap);
	check(ret >= 0 && cap.resp.result == 0 && cap.chip_info.chip_id == 0x18 && cap.board_info.board_id == 0xff &&
	    cap.soc_info.soc_id == 0x40020320 && strcmp(cap.fw_build_id, "WLAN.MSL.1.0.1-01277-QCAMSLSWCBT-1") == 0 &&
	    strcmp(cap.fw_version_info.fw_build_timestamp, "2023-04-15 01:27") == 0, "conversation: capabilities (%d)", ret);

	ret = exchange(&qmi, 0x4c, QMI_WLANFW_DEVICE_INFO_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_device_info_req_msg_v01_ei, &info_req,
	    qmi_wlfw_device_info_resp_msg_v01_ei, &info);
	check(ret >= 0 && info.bar_addr_valid && info.bar_addr == 0x17a00000 && info.bar_size == ATH11K_QMI_DEVICE_BAR_SIZE,
	    "conversation: device info (%d)", ret);

	/* the board file, 6144 bytes at a time, as ath11k_qmi_load_file_target_mem() sends it */
	while (left > 0) {
		memset(&bdf, 0, sizeof(bdf));
		bdf.valid = 1;
		bdf.file_id_valid = 1;
		bdf.file_id = cap.board_info.board_id;
		bdf.total_size_valid = 1;
		bdf.total_size = sizeof(firmware.file);
		bdf.seg_id_valid = 1;
		bdf.seg_id = firmware.segments;
		bdf.data_valid = 1;
		bdf.bdf_type = ATH11K_QMI_BDF_TYPE_ELF;
		bdf.bdf_type_valid = 1;
		bdf.end_valid = 1;
		bdf.data_len = left > QMI_WLANFW_MAX_DATA_SIZE_V01 ? QMI_WLANFW_MAX_DATA_SIZE_V01 : (uint32_t)left;
		bdf.end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(bdf.data, at, bdf.data_len);
		memset(&bdf_resp, 0, sizeof(bdf_resp));
		ret = exchange(&qmi, 0x25, QMI_WLANFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_bdf_download_req_msg_v01_ei, &bdf,
		    qmi_wlanfw_bdf_download_resp_msg_v01_ei, &bdf_resp);
		if (ret < 0 || bdf_resp.resp.result != 0) {
			check(false, "conversation: board file, piece %u (%d)", firmware.segments, ret);
			break;
		}
		longest = router.last_length > longest ? router.last_length : longest;
		at += bdf.data_len;
		left -= bdf.data_len;
	}
	check(firmware.complete && firmware.segments == 4 && firmware.got_length == sizeof(firmware.file) &&
	    memcmp(firmware.got, firmware.file, sizeof(firmware.file)) == 0, "conversation: the board file did not arrive whole (%u pieces, %zu bytes)",
	    firmware.segments, firmware.got_length);
	check(longest == 6182 + QMI_HEADER_SIZE, "conversation: the longest datagram was %zu bytes, not the 6189 agreed", longest);

	check(host_waits(&host.init_done, 2000), "conversation: no \"firmware init done\"");
	mode.mode = 7;                          /* ATH11K_FIRMWARE_MODE_COLD_BOOT */
	mode.hw_debug_valid = 1;
	mode.hw_debug = 0;
	ret = exchange(&qmi, 0x22, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode,
	    qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_resp);
	check(ret >= 0 && mode_resp.resp.result == 0, "conversation: cold boot mode (%d)", ret);
	check(host_waits(&host.cal_done, 2000), "conversation: no \"cold boot calibration done\"");
	check(host_waits(&host.ready, 2000), "conversation: no \"firmware ready\"");

	/* the configuration: nine pipes, twenty services, as wcn6750's tables have them */
	memset(&cfg, 0, sizeof(cfg));
	strcpy(cfg.host_version, ATH11K_HOST_VERSION_STRING);
	cfg.host_version_valid = 1;
	cfg.tgt_cfg_valid = 1;
	cfg.tgt_cfg_len = 9;
	for (uint32_t i = 0; i < cfg.tgt_cfg_len; i++) {
		cfg.tgt_cfg[i].pipe_num = i;
		cfg.tgt_cfg[i].pipe_dir = i & 1;
		cfg.tgt_cfg[i].nentries = 32;
		cfg.tgt_cfg[i].nbytes_max = 2048;
	}
	cfg.svc_cfg_valid = 1;
	cfg.svc_cfg_len = 20;
	for (uint32_t i = 0; i < cfg.svc_cfg_len; i++) {
		cfg.svc_cfg[i].service_id = 0x100 + i;
		cfg.svc_cfg[i].pipe_dir = i & 1;
		cfg.svc_cfg[i].pipe_num = i % 9;
	}
	cfg.shadow_reg_v2_valid = 1;
	cfg.shadow_reg_v2_len = 36;
	ret = exchange(&qmi, 0x23, QMI_WLANFW_WLAN_CFG_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_wlan_cfg_req_msg_v01_ei, &cfg,
	    qmi_wlanfw_wlan_cfg_resp_msg_v01_ei, &cfg_resp);
	check(ret >= 0 && cfg_resp.resp.result == 0, "conversation: configuration (%d)", ret);

	mode.mode = 0;                          /* ATH11K_FIRMWARE_MODE_NORMAL */
	ret = exchange(&qmi, 0x22, QMI_WLANFW_WLAN_MODE_REQ_MSG_V01_MAX_LEN, qmi_wlanfw_wlan_mode_req_msg_v01_ei, &mode,
	    qmi_wlanfw_wlan_mode_resp_msg_v01_ei, &mode_resp);
	check(ret >= 0 && mode_resp.resp.result == 0 && firmware.modes == 2 && firmware.mode[0] == 7 && firmware.mode[1] == 0,
	    "conversation: mission mode (%d)", ret);

	check(!firmware.bad, "conversation: the firmware's side saw something it did not expect");
	check(firmware.requests[0x20] == 1 && firmware.requests[0x34] == 1 && firmware.requests[0x24] == 1 && firmware.requests[0x4c] == 1 &&
	    firmware.requests[0x25] == 4 && firmware.requests[0x22] == 2 && firmware.requests[0x23] == 1, "conversation: the requests were not the expected ones");
	for (unsigned i = 0; i < firmware.thread_count; i++) {
		pthread_join(firmware.threads[i], NULL);
	}
	qmi_handle_release(&qmi);
	redwood_qmi_set_transport(NULL);
}

#endif /* !NO_ATH11K */

int
main(void)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} parts[] = {
		{ "the coding, tables of our own, bytes by hand", test_own_tables },
#ifndef NO_ATH11K
		{ "the coding, ath11k's messages, bytes by hand", test_wlfw_by_hand },
		{ "the coding, every table of ath11k: full, at random, damaged", test_ath11k_tables },
#endif
		{ "the helpers over a made-up router", test_helpers },
#ifndef NO_ATH11K
		{ "a WCN6750's exchange with ath11k up to \"firmware ready\"", test_conversation },
#endif
	};

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		int before = failures, done = checks;

		parts[i].run();
		printf("%s  %s (%d checks)\n", failures == before ? "ok  " : "FAIL", parts[i].name, checks - done);
	}
	printf("%d checks, %d failed\n", checks, failures);
	return failures != 0;
}
