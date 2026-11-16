/*
 * The lookup of a file in the port's RAM disks (drivers/board/image.c), run as a host
 * program on an image tools/apfs/mkapfs.py made: the way the GPU's, the Wi-Fi DSP's and the audio DSP's
 * firmware reach their drivers before there is a root. The reader under it is the kernel's own
 * (bsd/miscfs/apfs/apfs_lib.c), built for this host as apfs-tool is. workbench/apfs_image_test.sh makes the
 * images and runs this.
 *
 *   image_file_test IMAGE1 IMAGE0 DIR     the image that travels with the kernel (md1), the resident one
 *                                         (md0), and the directory IMAGE1's first volume was made of
 */
#include <sys/stat.h>
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];
void loader_log_putc(char c);

static void
said(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

/* the RAM disks: md1 and md0, each a file read into memory, as the loader leaves them */
static uint8_t *disk[2];
static uint64_t disk_size[2];
static bool no_md1;

dev_t
mdevlookup(int devid)
{
	return devid < 0 || devid > 1 || disk[devid] == NULL || (devid == 1 && no_md1) ? (dev_t)-1 : makedev(3, devid);
}

int
mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size)
{
	int which = (int)minor(dev);

	*base = (vm_offset_t)disk[which];
	*size = disk_size[which];
	return 0;
}

#define printf said
#include "board/image.c"
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static uint8_t *
read_file(const char *path, uint64_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data;
	long length;

	if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	data = malloc((size_t)length + 1);
	if (data == NULL || fread(data, 1, (size_t)length, f) != (size_t)length) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(2);
	}
	fclose(f);
	*size = (uint64_t)length;
	return data;
}

/* the file's bytes are those of DIR/dir/name */
static bool
same(const char *root, const char *dir, const char *name, const uint8_t *data, uint64_t size)
{
	char path[512];
	uint64_t want_size;
	uint8_t *want;
	bool equal;

	snprintf(path, sizeof(path), "%s/%s/%s", root, dir, name);
	want = read_file(path, &want_size);
	equal = want_size == size && memcmp(want, data, size) == 0;
	free(want);
	return equal;
}

static bool
inside(int which, const uint8_t *data, uint64_t size)
{
	return data >= disk[which] && data + size <= disk[which] + disk_size[which];
}

int
main(int argc, char **argv)
{
	const uint8_t *data, *again;
	uint64_t size = 0, size2 = 0;
	int from = -1;
	char longname[200];

	if (argc != 4) {
		fprintf(stderr, "usage: image_file_test IMAGE1 IMAGE0 DIR\n");
		return 2;
	}
	disk[1] = read_file(argv[1], &disk_size[1]);
	disk[0] = read_file(argv[2], &disk_size[0]);

	/* firmware stored as it is, in one piece: used where it lies, no copy */
	data = hakios_image_file("fw", "a660_zap.mbn", &size, &from);
	CHECK(data != NULL && from == 1 && same(argv[3], "fw", "a660_zap.mbn", data, size), "fw/a660_zap.mbn of md1: %p, %llu bytes",
	    (const void *)data, (unsigned long long)size);
	CHECK(data != NULL && inside(1, data, size), "where it lies in md1's memory");

	/* a file stored compressed: read out once into memory of its own, and kept */
	data = hakios_image_file("etc", "big.txt", &size, &from);
	CHECK(data != NULL && same(argv[3], "etc", "big.txt", data, size), "etc/big.txt, decompressed: %llu bytes", (unsigned long long)size);
	CHECK(data != NULL && !inside(1, data, size), "in memory of its own");
	again = hakios_image_file("etc", "big.txt", &size2, NULL);
	CHECK(again == data && size2 == size, "asked again: the same copy, not another");

	/* one in a directory below, and a small one whole in its attribute */
	data = hakios_image_file("etc/deep/er", "random.bin", &size, NULL);
	CHECK(data != NULL && same(argv[3], "etc/deep/er", "random.bin", data, size), "etc/deep/er/random.bin");
	data = hakios_image_file("etc", "small", &size, NULL);
	CHECK(data != NULL && same(argv[3], "etc", "small", data, size), "etc/small, in its attribute");

	/* what is not a file of some size is nothing */
	CHECK(hakios_image_file("fw", "missing.bin", &size, NULL) == NULL, "a file that is not there");
	CHECK(hakios_image_file("etc", "deep", &size, NULL) == NULL, "a directory");
	CHECK(hakios_image_file("etc", "empty", &size, NULL) == NULL, "an empty file");

	/* md0 when md1 does not have it, md1 first when both do */
	data = hakios_image_file("fw", "only0.bin", &size, &from);
	CHECK(data != NULL && from == 0 && inside(0, data, size), "a file only md0 has comes from md0: %d", from);
	no_md1 = true;
	data = hakios_image_file("fw", "a660_zap.mbn", &size, &from);
	CHECK(data != NULL && from == 0 && inside(0, data, size), "without md1, md0's own copy: %d", from);
	no_md1 = false;

	/* a path longer than the lookup holds is not cut short into another file's */
	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = 0;
	CHECK(hakios_image_file("fw", longname, &size, NULL) == NULL && strstr(log_text, "is not looked up") != NULL,
	    "a path too long: nothing, and said: %s", log_text);

	/* a RAM disk that is not an APFS container */
	memset(disk[0] + 32, 0, 4);
	disk[1] = disk[0];
	disk_size[1] = disk_size[0];
	CHECK(hakios_image_file("fw", "only0.bin", &size, NULL) == NULL, "neither disk is a container: nothing");

	fprintf(stderr, "image_file_test: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
