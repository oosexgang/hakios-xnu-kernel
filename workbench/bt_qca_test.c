/*
 * The firmware a Qualcomm Bluetooth controller is given (drivers/board/bt_qca.h), as it is: what a file is, the
 * segments that carry it, which of them the controller answers, and what its answers say.
 *
 *   bt_qca_test SCENARIO
 */
#include "fakekernel.h"
#include "board/bt_qca.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* a TLV file of TYPE whose LENGTH bytes after the header word are what MAKE writes */
static uint32_t
tlv(uint8_t *file, uint8_t type, uint32_t length)
{
	file[0] = type;
	file[1] = (uint8_t)length;
	file[2] = (uint8_t)(length >> 8);
	file[3] = (uint8_t)(length >> 16);
	for (uint32_t i = 0; i < length; i++) {
		file[4 + i] = (uint8_t)(i * 13 + 5);
	}
	return 4 + length;
}

static uint8_t file[4096], command[300];

/* ---- scenarios ---------------------------------------------------------------------------------- */

/* a patch, a configuration, an ELF file, and what is none of them */
static void
kinds(void)
{
	struct bt_qca_file f;
	uint32_t n;

	n = tlv(file, 1, 600);
	file[4 + 10] = BT_QCA_NO_EITHER;                /* the download mode */
	file[4 + 12] = 0x13;                            /* the product, the ROM build, the patch version */
	file[4 + 13] = 0x00;
	file[4 + 14] = 0x11;
	file[4 + 15] = 0x02;
	file[4 + 16] = 0x34;
	file[4 + 17] = 0x12;
	f = bt_qca_identify(file, n);
	CHECK(f.kind == BT_QCA_PATCH && f.mode == BT_QCA_NO_EITHER && f.product == 0x13 && f.rom_build == 0x211 && f.patch_version == 0x1234,
	    "a patch, its mode and its versions: %d %d %#x %#x %#x", f.kind, f.mode, f.product, f.rom_build, f.patch_version);
	file[4 + 10] = 9;
	CHECK(bt_qca_identify(file, n).mode == BT_QCA_ANSWERS_ALL, "a mode there is not: every answer waited for");
	CHECK(bt_qca_identify(file, n + 100).kind == BT_QCA_PATCH, "more bytes than the header says: still the patch");
	CHECK(bt_qca_identify(file, n - 1).kind == BT_QCA_NOT_ONE, "fewer than the header says: not one");
	n = tlv(file, 1, 21);
	CHECK(bt_qca_identify(file, n).kind == BT_QCA_NOT_ONE, "a patch without room for its own header");

	n = tlv(file, 2, 300);
	f = bt_qca_identify(file, n);
	CHECK(f.kind == BT_QCA_NVM && f.mode == BT_QCA_ANSWERS_ALL && f.product == 0, "a configuration");
	n = tlv(file, 2, 1);
	CHECK(bt_qca_identify(file, n).kind == BT_QCA_NVM, "a configuration of one byte");
	n = tlv(file, 2, 0);
	CHECK(bt_qca_identify(file, n).kind == BT_QCA_NOT_ONE, "one of nothing");
	n = tlv(file, 5, 30);
	CHECK(bt_qca_identify(file, n).kind == BT_QCA_NOT_ONE, "a type there is not");

	memset(file, 0, 64);
	memcpy(file, "\x7f" "ELF\x01\x01\x01", 7);
	CHECK(bt_qca_identify(file, 64).kind == BT_QCA_ELF && bt_qca_identify(file, 64).mode == BT_QCA_ANSWERS_ALL, "an ELF patch");
	CHECK(bt_qca_identify(file, 15).kind == BT_QCA_NOT_ONE, "too short to be one");
	CHECK(bt_qca_identify(file, 3).kind == BT_QCA_NOT_ONE && bt_qca_identify(NULL, 64).kind == BT_QCA_NOT_ONE, "nothing at all");
}

/* every byte of a file, in order, in segments of 243 at most, each the TLV request of 0xfc00 */
static void
segments(void)
{
	static const uint32_t sizes[] = { 1, 242, 243, 244, 486, 487, 1000 };

	for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		uint32_t size = sizes[s], got = 0, count = bt_qca_segments(size);

		tlv(file, 2, size > 4 ? size - 4 : 1);
		CHECK(count == (size + 242) / 243, "%u bytes: %u segments", size, count);
		for (uint32_t i = 0; i < count; i++) {
			unsigned n = bt_qca_segment(command, sizeof(command), file, size, i);
			unsigned length = command[5];

			CHECK(n == 6 + length && command[0] == 1 && command[1] == 0x00 && command[2] == 0xfc && command[3] == 2 + length &&
			    command[4] == 0x1e, "%u bytes, segment %u: a TLV request of %u bytes", size, i, length);
			CHECK(length == (i + 1 < count ? 243 : size - 243 * i), "%u bytes, segment %u: %u bytes", size, i, length);
			CHECK(memcmp(command + 6, file + got, length) == 0, "%u bytes, segment %u: the file's bytes", size, i);
			got += length;
		}
		CHECK(got == size, "%u bytes: all of them sent", size);
		CHECK(bt_qca_segment(command, sizeof(command), file, size, count) == 0, "%u bytes: no segment after the last", size);
	}
	CHECK(bt_qca_segment(command, 6 + 242, file, 1000, 0) == 0, "no room for the segment: none");
	CHECK(bt_qca_segment(command, 6 + 243, file, 1000, 0) == 249, "room for it to the byte: made");
	CHECK(bt_qca_segments(0) == 0 && bt_qca_segment(command, sizeof(command), file, 0, 0) == 0, "an empty file: no segment");
}

/* which segments are answered, by the patch's download mode: the last one always */
static void
answered(void)
{
	for (uint32_t i = 0; i < 5; i++) {
		bool last = i == 4;

		CHECK(bt_qca_answered(BT_QCA_ANSWERS_ALL, 1000, i), "all: segment %u", i);
		CHECK(bt_qca_answered(BT_QCA_NO_COMPLETE, 1000, i), "only the vendor event: segment %u", i);
		CHECK(bt_qca_answered(BT_QCA_NO_VENDOR_EVENT, 1000, i) == last, "no vendor event: segment %u", i);
		CHECK(bt_qca_answered(BT_QCA_NO_EITHER, 1000, i) == last, "neither: segment %u", i);
	}
	CHECK(bt_qca_answered(BT_QCA_NO_EITHER, 243, 0), "one segment: answered");
}

/* what an answer says: taken, refused, or no answer to a segment */
static void
answers(void)
{
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0xff, 3, 0x00, 0x1e, 0x00 }, 6) == 1, "the vendor event, status 0");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0xff, 3, 0x00, 0x1e, 0x05 }, 6) == -1, "the vendor event, an error");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 4, 1, 0x00, 0xfc, 0x00 }, 7) == 1, "a Command Complete, status 0");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 4, 1, 0x00, 0xfc, 0x0c }, 7) == -1, "a Command Complete, an error");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0xff, 3, 0x00, 0x19, 0x00 }, 6) == 0, "a vendor event of another request");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0xff, 3, 0x01, 0x1e, 0x00 }, 6) == 0, "a vendor event that is no response");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0xff, 2, 0x00, 0x1e }, 5) == 0, "one without a status");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0xff, 2, 0x00, 0x1e, 0x00 }, 6) == 0, "one whose length leaves its status out");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 4, 1, 0x03, 0x0c, 0x00 }, 7) == 0, "HCI_Reset's Command Complete");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 4, 1, 0x00, 0xfc }, 6) == 0, "one cut short");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 2, 0xff, 3, 0x00, 0x1e, 0x00 }, 6) == 0, "data, not an event");
	/* a Command Complete of 0xfc00 that says which request it answers (status, then the request): the TLV request's is a
	 * segment's; another's (the version's, 0x19) is not, whatever its status */
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 5, 1, 0x00, 0xfc, 0x00, 0x1e }, 8) == 1, "of the TLV request, status 0");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 5, 1, 0x00, 0xfc, 0x02, 0x1e }, 8) == -1, "of the TLV request, an error");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 5, 1, 0x00, 0xfc, 0x00, 0x19 }, 8) == 0, "of the version's request");
	CHECK(bt_qca_segment_answer((const uint8_t[]){ 4, 0x0e, 5, 1, 0x00, 0xfc, 0x00 }, 7) == 1, "one that says more than came: status");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "kinds", kinds }, { "segments", segments }, { "answered", answered }, { "answers", answers },
	};

	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
