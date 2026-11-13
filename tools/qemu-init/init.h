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

/* text.c: the console (the descriptor what is said goes to), words and numbers, the time */
extern int console;
size_t length(const char *s);
int same(const char *a, const char *b);
void say(const char *s);
void say_number(uint64_t n);
size_t append(char *line, size_t at, size_t size, const char *text);       /* (a line said in one write, */
size_t append_number(char *line, size_t at, size_t size, uint64_t n);      /*  which the kernel's own lines do not cut) */
void say_padded(uint64_t n, int width);
unsigned parse_number(const char *text);
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
