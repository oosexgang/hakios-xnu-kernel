/*
 * A debugger inside the kernel, reached over the phone's Wi-Fi: boot-arg dbg=1. A thread listens on TCP
 * port 2323 (any address, so en0's once Darwin's networking has given it one); `nc PHONE 2323` gives a prompt.
 * Nothing here writes to the phone's hardware: the commands read.
 *
 *   help               the list
 *   log [KB]           the kernel's own log, the last KB kilobytes (default 16)
 *   spmi SID ADDR      one register of a PMIC (hex), through the arbiter's observer channel (read only)
 *   battery            what the battery driver knows (drivers/power/hakios.battery.c)
 *   uptime             seconds since this thread started listening, and how many clients it has had
 *   quit               close
 *
 * Meant for the questions that otherwise cost a boot with the cable: what did the kernel say a minute ago,
 * what does that register say now, is the charger charging.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <netinet/in.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/msgbuf.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern int hakios_pmic_read(unsigned sid, unsigned addr, uint8_t *out);
extern bool hakios_khaje_joined(void);
extern int hakios_battery_summary(char *buffer, size_t size);
extern int hakios_pmic_set_reboot_reason(unsigned mode);
extern char hakios_console_ring[];
extern volatile uint32_t hakios_console_head;
extern const uint32_t hakios_console_size;
extern void thread_set_thread_name(thread_t thread, const char *name);
extern kern_return_t thread_terminate(thread_t thread);

#define DBG_PORT        2323
/*
 * The password of the debugger (the first line a client sends: "auth TOKEN"). It is not in the repository: tools/make-kernel.sh writes
 * bsd/dev/dbg_token_local.h into the kernel tree from the file HAKIOS_DBG_TOKEN_FILE (default ~/hakios-work/dbg-token), and without
 * that file the password is empty and the debugger lets nobody in.
 */
#if __has_include("dbg_token_local.h")
#include "dbg_token_local.h"
#endif
#ifndef DBG_TOKEN
#define DBG_TOKEN ""
#endif

static uint64_t
dbg_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

static unsigned dbg_clients;
static uint64_t dbg_began_ms;

static int
dbg_send(socket_t so, const char *p, size_t n)
{
	while (n != 0) {
		struct iovec iov = { .iov_base = (void *)(uintptr_t)p, .iov_len = n };
		struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
		size_t sent = 0;
		errno_t error = sock_send(so, &msg, 0, &sent);

		if (error != 0 || sent == 0) {
			return -1;
		}
		p += sent;
		n -= sent;
	}
	return 0;
}

static int
dbg_say(socket_t so, const char *format, ...) __printflike(2, 3);

static int
dbg_say(socket_t so, const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	return dbg_send(so, line, strlen(line));
}

/* one line from the client, without its end of line; false when it has gone */
static bool
dbg_line(socket_t so, char *line, size_t size)
{
	size_t n = 0;

	for (;;) {
		char c;
		struct iovec iov = { .iov_base = &c, .iov_len = 1 };
		struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
		size_t got = 0;

		if (sock_receive(so, &msg, 0, &got) != 0 || got == 0) {
			return false;
		}
		if (c == '\n') {
			line[n] = 0;
			return true;
		}
		if (c != '\r' && n + 1 < size) {
			line[n++] = c;
		}
	}
}

static void
dbg_log(socket_t so, unsigned kb)
{
	struct msgbuf *mb = msgbufp;
	uint32_t size, start;
	uint64_t want = (uint64_t)kb * 1024;
	char chunk[512];
	size_t n = 0;

	if (mb == NULL || mb->msg_magic != MSG_MAGIC || mb->msg_bufc == NULL || mb->msg_size == 0) {
		dbg_say(so, "the kernel's log is not there\n");
		return;
	}
	size = mb->msg_size;
	start = mb->msg_bufx;                   /* the oldest byte is where the next one goes */
	/* the newest `want` bytes of the ring */
	uint32_t skip = (uint32_t)(want < size ? size - want : 0);

	for (uint32_t i = skip; i < size; i++) {
		char c = mb->msg_bufc[(start + i) % size];

		if (c == 0) {
			continue;
		}
		chunk[n++] = c;
		if (n == sizeof(chunk)) {
			if (dbg_send(so, chunk, n) != 0) {
				return;
			}
			n = 0;
		}
	}
	if (n != 0) {
		dbg_send(so, chunk, n);
	}
}

/* the newest `kb` kilobytes of everything the console said: the kernel's lines and user space's (iOS's own messages) */
static void
dbg_console(socket_t so, unsigned kb)
{
	uint32_t head = hakios_console_head, size = hakios_console_size;
	uint64_t have = head < size ? head : size, want = (uint64_t)kb * 1024;
	uint64_t take = want < have ? want : have;
	char chunk[512];
	size_t n = 0;

	for (uint64_t i = head - take; i < head; i++) {
		char c = hakios_console_ring[i % size];

		if (c == 0) {
			continue;
		}
		chunk[n++] = c;
		if (n == sizeof(chunk)) {
			if (dbg_send(so, chunk, n) != 0) {
				return;
			}
			n = 0;
		}
	}
	if (n != 0) {
		dbg_send(so, chunk, n);
	}
}

static void
dbg_restart(socket_t so, unsigned mode, const char *where)
{
	int error = hakios_pmic_set_reboot_reason(mode);

	dbg_say(so, "reboot reason write: %d\n", error);
	if (error == 0) {
		dbg_say(so, "restarting into %s now\n", where);
		delay_for_interval(300, NSEC_PER_MSEC);
		register uint64_t x0 __asm__("x0") = 0x84000009ULL;        /* PSCI SYSTEM_RESET */
		__asm__ volatile ("smc #0" : "+r" (x0) : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		    "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	}
}

static bool dbg_authenticated;

static bool
dbg_command(socket_t so, char *line)
{
	char *words[4] = { NULL, NULL, NULL, NULL };
	unsigned count = 0;
	char *c = line;

	while (*c != 0 && count < 4) {
		while (*c == ' ' || *c == '\t') {
			*c++ = 0;
		}
		if (*c == 0) {
			break;
		}
		words[count++] = c;
		while (*c != 0 && *c != ' ' && *c != '\t') {
			c++;
		}
	}
	if (count == 0) {
		return true;
	}
	if (strcmp(words[0], "auth") == 0) {
		dbg_authenticated = count == 2 && DBG_TOKEN[0] != 0 && strcmp(words[1], DBG_TOKEN) == 0;
		dbg_say(so, dbg_authenticated ? "ok\n" : "no\n");
		return dbg_authenticated;                    /* a wrong password ends the session */
	}
	if (!dbg_authenticated) {
		dbg_say(so, "auth TOKEN first\n");
		return false;
	}
	if (strcmp(words[0], "quit") == 0 || strcmp(words[0], "exit") == 0) {
		return false;
	} else if (strcmp(words[0], "help") == 0) {
		dbg_say(so, "console [KB] | log [KB] | spmi SID ADDR | battery | uptime | fastboot | reboot | hang | quit\n");
	} else if (strcmp(words[0], "log") == 0) {
		dbg_log(so, count > 1 ? (unsigned)strtoul(words[1], NULL, 10) : 16);
	} else if (strcmp(words[0], "spmi") == 0 && count == 3) {
		uint8_t value = 0;
		unsigned sid = (unsigned)strtoul(words[1], NULL, 16), addr = (unsigned)strtoul(words[2], NULL, 16);
		int error = hakios_pmic_read(sid, addr, &value);

		if (error == 0) {
			dbg_say(so, "sid %u %#x = %#x\n", sid, addr, value);
		} else {
			dbg_say(so, "sid %u %#x: no (%d: -1 arbiter not ready, -2 no such peripheral, -3 transfer failed)\n", sid, addr,
			    error);
		}
	} else if (strcmp(words[0], "battery") == 0) {
		char text[256];

		hakios_battery_summary(text, sizeof(text));
		dbg_say(so, "%s\n", text);
	} else if (strcmp(words[0], "fastboot") == 0) {
		dbg_restart(so, 2, "fastboot");              /* the boot firmware is told "the bootloader" (mode 2) */
	} else if (strcmp(words[0], "reboot") == 0) {
		dbg_restart(so, 0, "the system");            /* mode 0: the system, from the active slot */
	} else if (strcmp(words[0], "console") == 0) {
		dbg_console(so, count > 1 ? (unsigned)strtoul(words[1], NULL, 10) : 32);
	} else if (strcmp(words[0], "hang") == 0) {
		/* a test of the guard: the processor stops with interrupts off; the watchdog resets the phone and the boot firmware,
		 * told "the bootloader", should come up in fastboot */
		dbg_say(so, "hanging now\n");
		delay_for_interval(200, NSEC_PER_MSEC);
		__asm__ volatile ("msr daifset, #0xf" ::: "memory");
		for (;;) {
		}
	} else if (strcmp(words[0], "uptime") == 0) {
		dbg_say(so, "listening for %llu s; %u clients so far\n", (dbg_ms() - dbg_began_ms) / 1000,
		    dbg_clients);
	} else {
		dbg_say(so, "unknown command; help\n");
	}
	return true;
}

static void
dbg_serve(socket_t so)
{
	char line[128];

	dbg_clients++;
	dbg_authenticated = false;
	dbg_say(so, "hakios kernel debugger; auth TOKEN\n");
	for (;;) {
		if (dbg_send(so, "> ", 2) != 0 || !dbg_line(so, line, sizeof(line)) || !dbg_command(so, line)) {
			return;
		}
	}
}

static void
dbg_thread(__unused void *parameter, __unused wait_result_t result)
{
	socket_t listener = NULL;
	struct sockaddr_in sin;
	int one = 1;

	/* Darwin's networking and Wi-Fi come up in the first minute; the socket does not need them, only the clients */
	delay_for_interval(20000, NSEC_PER_MSEC);
	if (sock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &listener) != 0) {
		printf("dbg: no socket\n");
		thread_terminate(current_thread());
		return;
	}
	sock_setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	bzero(&sin, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(DBG_PORT);
	if (sock_bind(listener, (struct sockaddr *)&sin) != 0 || sock_listen(listener, 2) != 0) {
		printf("dbg: cannot listen on %d\n", DBG_PORT);
		sock_close(listener);
		thread_terminate(current_thread());
		return;
	}
	dbg_began_ms = dbg_ms();
	printf("dbg: listening on TCP port %d\n", DBG_PORT);
	for (;;) {
		socket_t client = NULL;

		if (sock_accept(listener, NULL, 0, 0, NULL, NULL, &client) == 0 && client != NULL) {
			dbg_serve(client);
			sock_close(client);
		} else {
			delay_for_interval(500, NSEC_PER_MSEC);
		}
	}
}

/*
 * The guard (dbg=2). A system that hangs, or one whose radio never comes up, would leave nobody a way in but the
 * buttons. So: as soon as the PMIC can be written the boot firmware is told that the next restart is "to the
 * bootloader"; once the system has shown that it lives -- the radio joined and a minute gone, or five minutes gone
 * -- that is taken back (the system). A hang before that ends, by the watchdog, in fastboot.
 */
static void
dbg_guard_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool armed = false;
	uint8_t probe;

	for (unsigned wait = 0; wait < 240; wait++) {
		if (hakios_pmic_read(0, 0x088f, &probe) == 0 && hakios_pmic_set_reboot_reason(2) == 0) {
			armed = true;
			printf("dbg: guard armed: a restart now goes to fastboot\n");
			break;
		}
		delay_for_interval(500, NSEC_PER_MSEC);
	}
	if (!armed) {
		printf("dbg: guard NOT armed (the PMIC could not be written)\n");
		thread_terminate(current_thread());
		return;
	}
	for (;;) {
		uint64_t up = dbg_ms() / 1000;

		if ((hakios_khaje_joined() && up >= 60) || up >= 300) {
			int error = hakios_pmic_set_reboot_reason(0);

			printf("dbg: guard released after %llu s (radio %s): write %d\n", up, hakios_khaje_joined() ? "joined" : "not joined",
			    error);
			if (error == 0) {
				break;
			}
		}
		delay_for_interval(2000, NSEC_PER_MSEC);
	}
	thread_terminate(current_thread());
}

void hakios_dbg_init(void);
void
hakios_dbg_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("dbg", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (on == 2 && kernel_thread_start(dbg_guard_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "hakios-dbg-guard");
		thread_deallocate(thread);
	}
	if (kernel_thread_start(dbg_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "hakios-dbg");
		thread_deallocate(thread);
	}
}
