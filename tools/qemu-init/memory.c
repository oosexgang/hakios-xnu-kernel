/* tools/qemu-init: memory -- a file's pages mapped and checked against its reads, written through a shared
 * mapping, written through a private one (the file left as it was), written and seen through a mapping, and
 * memory of its own: mmap, mmap-write, mmap-private, mmap-coherent, memory */
#include "init.h"

#define O_RDONLY                0
#define SYS_munmap              73
#define SYS_mmap                197
#define SYS_fstat64             339
#define SYS_lseek               199
#define PROT_READ               1
#define MAP_PRIVATE             2

/* FILE: the file mapped (its pages come from its file system), checked against what read() gives */
void
command_mmap(int argc, char **argv)
{
	static uint8_t buffer[16384];
	struct stat64_head st;

	if (argc != 2) {
		say("mmap FILE\n");
		return;
	}
	long fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) != 0 || st.size <= 0) {
		say("mmap: ");
		say(argv[1]);
		say(fd < 0 ? ": cannot be read\n" : ": nothing to map\n");
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	long mapped = syscall6(SYS_mmap, 0, st.size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped < 0) {
		say("mmap: not mapped, ");
		say_error(mapped);
		say("\n");
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	uint64_t by_pages = fnv1a(FNV1A_START, (const uint8_t *)mapped, st.size), by_reads = FNV1A_START;
	long all = 0;
	for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0; all += got) {
		by_reads = fnv1a(by_reads, buffer, got);
	}
	syscall6(SYS_munmap, mapped, st.size, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("mmap: ");
	say(argv[1]);
	say(", ");
	say_number((uint64_t)st.size);
	say(" bytes: ");
	say(all == st.size && by_pages == by_reads ? "its pages and its reads agree\n" : "its pages and its reads DIFFER\n");
}

/* ---- memory: pages of its own ---- */

#define PROT_WRITE              2
#define MAP_ANON                0x1000

/* the kernel's free memory, in megabytes (vm.page_free_count pages of hw.pagesize) */
static uint64_t
free_megabytes(void)
{
	uint64_t pages = 0, page_size = 0;

	sysctl_number("vm.page_free_count", &pages);
	sysctl_number("hw.pagesize", &page_size);
	return pages * page_size >> 20;
}

/* MB: that much memory of its own (anonymous, mmap), every page written and read back, then given back */
void
command_memory(int argc, char **argv)
{
	unsigned megabytes = argc > 1 ? parse_number(argv[1]) : 0;

	if (megabytes < 1 || megabytes > 1024) {
		say("memory MB: 1 to 1024\n");
		return;
	}
	uint64_t size = (uint64_t)megabytes << 20, before = free_megabytes();
	long mapped = syscall6(SYS_mmap, 0, (long)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (mapped < 0) {
		say("memory: not given, ");
		say_error(mapped);
		say("\n");
		return;
	}
	volatile uint64_t *words = (volatile uint64_t *)mapped;
	uint64_t count = size / sizeof(uint64_t), wrong = 0;
	int64_t start = now_us();
	for (uint64_t i = 0; i < count; i += 512) {             /* (a word every 4 KB: every page) */
		words[i] = i * 0x9e3779b97f4a7c15ull;
	}
	uint64_t during = free_megabytes();
	for (uint64_t i = 0; i < count; i += 512) {
		wrong += words[i] != i * 0x9e3779b97f4a7c15ull;
	}
	int64_t took = now_us() - start;
	syscall6(SYS_munmap, mapped, (long)size, 0, 0, 0, 0);
	say("memory: ");
	say_number(megabytes);
	say(" MB written and read back in ");
	say_number((uint64_t)took / 1000);
	say(wrong == 0 ? " ms, all as written" : " ms, SOME NOT AS WRITTEN");
	say("; free: ");
	say_number(before);
	say(" MB before, ");
	say_number(during);
	say(" MB with it, ");
	say_number(free_megabytes());
	say(" MB after\n");
}

#define SYS_msync               65
#define PROT_WRITE              2
#define MAP_SHARED              1
#define MS_SYNC                 0x10

/*
 * FILE: the file mapped shared and written through the mapping -- every byte turned over (b ^ 0xff) -- then
 * msync'd and unmapped: what reading it then gives must be the turned bytes (the file system's page-out)
 */
void
command_mmap_write(int argc, char **argv)
{
	static uint8_t buffer[16384];
	struct stat64_head st;
	uint64_t before = FNV1A_START, after = FNV1A_START, read_back = FNV1A_START;
	long fd, mapped, all = 0;

	if (argc != 2) {
		say("mmap-write FILE\n");
		return;
	}
	fd = syscall6(SYS_open, (long)argv[1], 2 /* O_RDWR */, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) != 0 || st.size <= 0) {
		say_failed("mmap-write", argv[1], fd < 0 ? fd : -22);
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	mapped = syscall6(SYS_mmap, 0, st.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped < 0) {
		say_failed("mmap-write", argv[1], mapped);
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	for (int64_t i = 0; i < st.size; i++) {
		uint8_t *b = (uint8_t *)mapped + i;

		before = fnv1a(before, b, 1);
		*b ^= 0xff;
		after = fnv1a(after, b, 1);
	}
	syscall6(SYS_msync, mapped, st.size, MS_SYNC, 0, 0, 0);
	syscall6(SYS_munmap, mapped, st.size, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	fd = syscall6(SYS_open, (long)argv[1], O_RDONLY, 0, 0, 0, 0);
	for (long got; fd >= 0 && (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0; all += got) {
		read_back = fnv1a(read_back, buffer, got);
	}
	if (fd >= 0) {
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	}
	say("mmap-write: ");
	say(argv[1]);
	say(", ");
	say_number((uint64_t)st.size);
	say(all == st.size && read_back == after && after != before ? " bytes turned through the mapping, and read so\n" :
	    " bytes: what is read is NOT what the mapping wrote\n");
}

/*
 * FILE: the file mapped private and every byte of it turned over through the mapping: the mapping has the
 * turned bytes (its own copies of the pages, made at the first write to each), the file what it had -- read
 * through another mapping, shared, and through read()
 */
void
command_mmap_private(int argc, char **argv)
{
	static uint8_t buffer[16384];
	struct stat64_head st;
	uint64_t file_before = FNV1A_START, mine = FNV1A_START, shared_after = FNV1A_START, read_after = FNV1A_START;
	long fd, private, shared;
	int turned = 1;

	if (argc != 2) {
		say("mmap-private FILE\n");
		return;
	}
	fd = syscall6(SYS_open, (long)argv[1], 2 /* O_RDWR */, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) != 0 || st.size <= 0) {
		say_failed("mmap-private", argv[1], fd < 0 ? fd : -22);
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	private = syscall6(SYS_mmap, 0, st.size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	shared = syscall6(SYS_mmap, 0, st.size, PROT_READ, MAP_SHARED, fd, 0);
	if (private < 0 || shared < 0) {
		say_failed("mmap-private", argv[1], private < 0 ? private : shared);
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	for (int64_t i = 0; i < st.size; i++) {
		uint8_t *b = (uint8_t *)private + i;

		file_before = fnv1a(file_before, b, 1);
		*b ^= 0xff;
	}
	for (int64_t i = 0; i < st.size; i++) {
		turned &= ((uint8_t *)private)[i] == (uint8_t)(((uint8_t *)shared)[i] ^ 0xff);
	}
	mine = fnv1a(mine, (const void *)private, st.size);
	shared_after = fnv1a(shared_after, (const void *)shared, st.size);
	syscall6(SYS_munmap, private, st.size, 0, 0, 0, 0);
	syscall6(SYS_munmap, shared, st.size, 0, 0, 0, 0);
	syscall6(SYS_lseek, fd, 0, 0, 0, 0, 0);
	for (long got; (got = syscall6(SYS_read, fd, (long)buffer, sizeof(buffer), 0, 0, 0)) > 0;) {
		read_after = fnv1a(read_after, buffer, got);
	}
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	say("mmap-private: ");
	say(argv[1]);
	say(turned && mine != file_before ? ": the private mapping turned" : ": the private mapping NOT turned");
	say(shared_after == file_before && read_after == file_before ? ", the file as it was\n" : ", the file CHANGED\n");
}

#define SYS_pwrite              154
#define SYS_pread               153

/*
 * FILE: the file mapped shared and read through the mapping (its pages brought in), then written with write()
 * in a page and across two: the mapping has what was written, at once, without being mapped again (the file
 * system brings the cached pages up to date in place); and what is written through the mapping, read() has at
 * once, no msync between
 */
void
command_mmap_coherent(int argc, char **argv)
{
	static const char middle[] = "written after the mapping", edge[] = "across a page's edge", back_way[] = "through the mapping";
	char got[sizeof(back_way)];
	int read_back;
	struct stat64_head st;
	uint64_t sum = 0;
	long fd, mapped;
	int64_t edge_at;
	int seen_middle, seen_edge;

	if (argc != 2) {
		say("mmap-coherent FILE (of 32 KB or more)\n");
		return;
	}
	fd = syscall6(SYS_open, (long)argv[1], 2 /* O_RDWR */, 0, 0, 0, 0);
	if (fd < 0 || syscall6(SYS_fstat64, fd, (long)&st, 0, 0, 0, 0) != 0 || st.size < 32768) {
		say_failed("mmap-coherent", argv[1], fd < 0 ? fd : -22);
		if (fd >= 0) {
			syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		}
		return;
	}
	if ((mapped = syscall6(SYS_mmap, 0, st.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) < 0) {
		say_failed("mmap-coherent", argv[1], mapped);
		syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
		return;
	}
	for (int64_t i = 0; i < st.size; i += 4096) {
		sum += ((volatile uint8_t *)mapped)[i];             /* (every page brought in, and cached) */
	}
	edge_at = 16384 - 5;
	syscall6(SYS_pwrite, fd, (long)middle, sizeof(middle) - 1, 1000, 0, 0);
	syscall6(SYS_pwrite, fd, (long)edge, sizeof(edge) - 1, edge_at, 0, 0);
	seen_middle = bytes_same((const void *)(mapped + 1000), middle, sizeof(middle) - 1);
	seen_edge = bytes_same((const void *)(mapped + edge_at), edge, sizeof(edge) - 1);
	/* ... and the other way: written through the mapping, read with pread, no msync between */
	copy_bytes((void *)(mapped + 20000), back_way, sizeof(back_way) - 1);
	read_back = syscall6(SYS_pread, fd, (long)got, sizeof(back_way) - 1, 20000, 0, 0) == (long)sizeof(back_way) - 1 &&
	    bytes_same(got, back_way, sizeof(back_way) - 1);
	syscall6(SYS_munmap, mapped, st.size, 0, 0, 0, 0);
	syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	(void)sum;
	say("mmap-coherent: ");
	say(argv[1]);
	say(seen_middle ? ": a write in a page seen through the mapping at once" : ": a write in a page NOT seen through the mapping");
	say(seen_edge ? ", and one across two pages" : ", one across two pages NOT");
	say(read_back ? "; written through it, read at once\n" : "; written through it, NOT read\n");
}
