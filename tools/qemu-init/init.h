/*
 * The first program of the root tools/qemu-root.sh makes: what the kernel starts as /sbin/launchd when
 * it has no Darwin to start. It shows that the kernel runs a program of its own user space -- exec, the
 * system calls, devfs, the console -- and says what the kernel says of itself: its version, the board
 * the loader named, the processors and the memory. Then it is a small shell on the console (the serial
 * port is its keyboard in QEMU): `help` lists what it can do.
 *
 * Nothing of Apple's: no libSystem, no dyld. A static arm64 executable that makes its system calls
 * itself (svc 0x80, the number in x16, the carry flag set on an error), built and linked by
 * tools/qemu-root.sh with clang and ld64.lld.
 *
 * Its parts: main.c (the start and the shell), text.c (the console, numbers, the time), and the
 * commands by what of the kernel they try: kernel.c, files.c, net.c, processes.c, devices.c, iokit.c.
 */
#ifndef QEMU_INIT_H
#define QEMU_INIT_H

#include <stddef.h>
#include <stdint.h>

/* bsd/kern/syscalls.master: the calls more than one part makes (each part names its own) */
#define SYS_exit                1
#define SYS_read                3
#define SYS_write               4
#define SYS_open                5
#define SYS_close               6
#define SYS_getpid              20
#define SYS_sigsuspend          111
#define SYS_gettimeofday        116
#define SYS_sysctlbyname        274
#define SYS_proc_info           336
#define O_WRONLY                1
#define WORDS                   16              /* the most words a line is split into */

/* a system call: the number in x16, the carry flag set on an error (then the result is -errno) */
static inline long
syscall6(long number, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	register long x16 __asm__("x16") = number;
	long failed;

	__asm__ volatile ("svc #0x80\n\tcset %[failed], cs"
	    : "+r"(x0), "+r"(x1), [failed] "=r"(failed)
	    : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
	    : "memory", "cc");
	return failed ? -x0 : x0;
}

/* a Mach trap (a negative number): eight arguments in x0 to x7, a kern_return_t (or a port's name) back in x0 */
static inline long
trap8(long number, long a, long b, long c, long d, long e, long f, long g, long h)
{
	register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e, x5 __asm__("x5") = f, x6 __asm__("x6") = g, x7 __asm__("x7") = h;
	register long x16 __asm__("x16") = number;

	__asm__ volatile ("svc #0x80"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
	    : "r"(x16)
	    : "memory", "cc");
	return x0;
}

/* pipe(): the two ends come back in x0 and x1, not in memory; 0, or -errno */
#define SYS_pipe                42
static inline long
syscall_pipe(int ends[2])
{
	register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0;
	register long x16 __asm__("x16") = SYS_pipe;
	long failed;

	__asm__ volatile ("svc #0x80\n\tcset %[failed], cs" : "+r"(x0), "+r"(x1), [failed] "=r"(failed) : "r"(x16) : "memory", "cc");
	ends[0] = (int)x0;
	ends[1] = (int)x1;
	return failed ? -x0 : 0;
}

/* lstat64 and fstat64 answer with this (the start of Darwin's struct stat64; the rest is room) */
struct stat64_head {                    /* the start of Darwin's struct stat64, up to st_size */
	int32_t         dev;
	uint16_t        mode, nlink;
	uint64_t        ino;
	uint32_t        uid, gid, rdev, pad;
	int64_t         times[8];       /* access, modification, change, birth: seconds and nanoseconds */
	int64_t         size;
	char            rest[64];
};

#define SYS_fork                2
/* fork() answers in x0 (the other's pid) and x1 (1 in the child); -errno if it failed */
static inline long
syscall_fork(int *in_child)
{
	register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0;
	register long x16 __asm__("x16") = SYS_fork;
	long failed;

	__asm__ volatile ("svc #0x80\n\tcset %[failed], cs" : "+r"(x0), "+r"(x1), [failed] "=r"(failed) : "r"(x16) : "memory", "cc");
	*in_child = !failed && x1 != 0;
	return failed ? -x0 : x0;
}

/* text.c: the console (the descriptor what is said goes to), words and numbers, the time */
extern int console;
size_t length(const char *s);
int same(const char *a, const char *b);
int bytes_same(const void *a, const void *b, size_t n);
#define FNV1A_START             0xcbf29ce484222325ull
uint64_t fnv1a(uint64_t hash, const void *bytes, long n);
void say(const char *s);
void say_number(uint64_t n);
void say_error(long error);             /* "error N (ENAME)", of a -errno */
void say_failed(const char *what, const char *path, long error);       /* "WHAT: PATH: error N (ENAME)" */
size_t append(char *line, size_t at, size_t size, const char *text);       /* (a line said in one write, */
size_t append_number(char *line, size_t at, size_t size, uint64_t n);      /*  which the kernel's own lines do not cut) */
void say_padded(uint64_t n, int width);
unsigned parse_number(const char *text);
uint64_t parse_number64(const char *text);
int split(char *text, char **words);
int64_t now_us(void);
void sleep_us(int64_t us);
void copy_bytes(void *to, const void *from, size_t n);

/* main.c: a value of the kernel's by its name (sysctlbyname); whether there is one */
int sysctl_string(const char *name, char *out, size_t size);
int sysctl_number(const char *name, uint64_t *value);

/* the commands: argc words, the command's name first (main.c lists them for the shell) */
void command_about(int argc, char **argv);
void command_alarm(int argc, char **argv);
void command_cat(int argc, char **argv);
void command_crash(int argc, char **argv);
void command_date(int argc, char **argv);
void command_dmesg(int argc, char **argv);
void command_draw(int argc, char **argv);
void command_echo(int argc, char **argv);
void command_fork(int argc, char **argv);
void command_get(int argc, char **argv);
void command_help(int argc, char **argv);
void command_ifconfig(int argc, char **argv);
void command_hash(int argc, char **argv);
void command_input(int argc, char **argv);
void command_ioprop(int argc, char **argv);
void command_ioreg(int argc, char **argv);
void command_kevent(int argc, char **argv);
void command_kill(int argc, char **argv);
void command_locks(int argc, char **argv);
void command_ls(int argc, char **argv);
void command_mach(int argc, char **argv);
void command_memory(int argc, char **argv);
void command_mkdir(int argc, char **argv);
void command_mmap(int argc, char **argv);
void command_mount(int argc, char **argv);
void command_ping(int argc, char **argv);
void command_pipe(int argc, char **argv);
void command_ps(int argc, char **argv);
void command_rm(int argc, char **argv);
void command_echo_udp(int argc, char **argv);
void command_unix_name(int argc, char **argv);
void command_fill(int argc, char **argv);
void command_df(int argc, char **argv);
void command_mask(int argc, char **argv);
void command_fds(int argc, char **argv);
void command_cloexec(int argc, char **argv);
void command_umask(int argc, char **argv);
void command_chown(int argc, char **argv);
void command_chflags(int argc, char **argv);
void command_access(int argc, char **argv);
void command_cp(int argc, char **argv);
void command_fsize(int argc, char **argv);
void command_appends(int argc, char **argv);
void command_opens(int argc, char **argv);
void command_dir_walk(int argc, char **argv);
void command_id(int argc, char **argv);
void command_watch(int argc, char **argv);
void command_truncate(int argc, char **argv);
void command_vectors(int argc, char **argv);
void command_touch(int argc, char **argv);
void command_poll(int argc, char **argv);
void command_pipe_size(int argc, char **argv);
void command_dgram(int argc, char **argv);
void command_fifo(int argc, char **argv);
void command_flock(int argc, char **argv);
void command_byte_locks(int argc, char **argv);
void command_descriptors(int argc, char **argv);
void command_mmap_write(int argc, char **argv);
void command_mmap_private(int argc, char **argv);
void command_mmap_coherent(int argc, char **argv);
void command_shm(int argc, char **argv);
void command_vm(int argc, char **argv);
void command_random(int argc, char **argv);
void command_stat(int argc, char **argv);
void command_ln(int argc, char **argv);
void command_mv(int argc, char **argv);
void command_chmod(int argc, char **argv);
void command_xattr(int argc, char **argv);
void command_files(int argc, char **argv);
void command_run(int argc, char **argv);
void command_serve(int argc, char **argv);
void command_sleep(int argc, char **argv);
void command_spawn(int argc, char **argv);
void command_sysctl(int argc, char **argv);
void command_threads(int argc, char **argv);
void command_tone(int argc, char **argv);
void command_udp(int argc, char **argv);
void command_unix(int argc, char **argv);
void command_uptime(int argc, char **argv);
void command_wait(int argc, char **argv);
void command_write(int argc, char **argv);

#endif
