/*
 * Darwin-on-hakios: the kernel's side of the Wi-Fi driver's object.
 *
 * The driver of the phone's radio is Linux's ath11k, compiled unmodified outside this tree on a
 * layer that gives it the kernel interfaces it expects, and tied into one object that the
 * kernel links when it is there (tools/mk-ath11k.sh and wifi/lx in the repository; the kernel
 * is built with that object when xnu-build.sh finds it). The object shows one name, lx_start(),
 * and asks for the calls of sys/hakios.lx.h, which are here: memory, time, threads, one lock
 * to sleep under, the kernel's options. What it asks of the radio itself -- its firmware's
 * files, its view of memory, the processor its firmware runs on -- is hakios.wifi.c's to
 * answer, which knows the phone.
 *
 * A kernel built without the object has the lx_start() at the end of this file, which says so.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.net.h>
#include <sys/hakios.wifi.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);

#define say hakios_wifi_say

static LCK_GRP_DECLARE(lx_lock_group, "hakios-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);

/* ---- memory ------------------------------------------------------------------------------------------ */

void *
lx_os_alloc(unsigned long size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
lx_os_free(void *memory, unsigned long size)
{
	kfree_data(memory, size);
}

/*
 * In one physical piece, so that it can be mapped a second time without this processor's cache
 * in between (as hakios.adreno.c does with what it shares with the GPU); the first, cached
 * view is written out and never used again.
 */
void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("lx: no %lu bytes of memory in one piece\n", size);
		return NULL;
	}
	physical = kvtophys(cached);
	bzero((void *)cached, size);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !hakios_wifi_radio_map(physical, size, address)) {
		return NULL;
	}
	return (void *)uncached;
}

void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);

	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

/* ---- saying things ----------------------------------------------------------------------------------- */

void
lx_os_print(const char *text)
{
	say("%s", text);
}

void
lx_os_tell(const char *text)
{
	hakios_wifi_tell("%s", text);
}

void
lx_os_panic(const char *text)
{
	panic("%s", text);
}

/* ---- time, threads, sleeping --------------------------------------------------------------------------- */

unsigned long long
lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
lx_os_thread_self(void)
{
	return current_thread();
}

struct lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct lx_thread start = *(struct lx_thread *)parameter;

	kfree_type(struct lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
lx_os_lock(void)
{
	lck_mtx_lock(&lx_lock);
}

void
lx_os_unlock(void)
{
	lck_mtx_unlock(&lx_lock);
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;

	if (nanoseconds == 0) {
		return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)address);
}

/* ---- the rest of the machine ------------------------------------------------------------------------- */

void
lx_os_random(void *buffer, unsigned long size)
{
	read_random(buffer, (u_int)size);
}

unsigned
lx_os_option(const char *name)
{
	return hakios_wifi_option(name);
}

int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	uint64_t length = 0;
	const uint8_t *file = hakios_wifi_driver_file(name, &length);

	if (file == NULL) {
		return ENOENT;
	}
	*data = file;
	*size = (unsigned long)length;
	return 0;
}

int
lx_os_rproc_boot(void)
{
	return hakios_wifi_radio_boot() ? 0 : ENXIO;
}

/*
 * A radio on PCIe (sys/hakios.wifi.h): the radio's driver answers. These weak answers are a kernel's whose radio
 * is not on PCIe (the WCN6750's driver has none of them): the driver's object then never asks the others.
 */
__attribute__((weak)) uint32_t
hakios_wifi_radio_bus(void)
{
	return HAKIOS_WIFI_BUS_AHB;
}

__attribute__((weak)) bool
hakios_wifi_radio_pci(__unused uint16_t *vendor, __unused uint16_t *device, __unused uint16_t *subsystem_vendor,
    __unused uint16_t *subsystem_device)
{
	return false;
}

__attribute__((weak)) bool
hakios_wifi_radio_bar(__unused uint64_t *physical, __unused uint64_t *size)
{
	return false;
}

__attribute__((weak)) uint32_t
hakios_wifi_radio_read32(__unused uint32_t offset)
{
	return 0xffffffff;
}

__attribute__((weak)) void
hakios_wifi_radio_write32(__unused uint32_t offset, __unused uint32_t value)
{
}

__attribute__((weak)) void
hakios_wifi_radio_wake(__unused bool awake)
{
}

__attribute__((weak)) void
hakios_wifi_radio_halt(void)
{
}

__attribute__((weak)) bool
hakios_wifi_radio_irq_attach(__unused void (*arrived)(uint32_t vectors))
{
	return false;
}

__attribute__((weak)) void
hakios_wifi_radio_irq_mask(__unused uint32_t vector, __unused bool masked)
{
}

__attribute__((weak)) void
hakios_wifi_radio_irq_detach(void)
{
}

unsigned
lx_os_radio_bus(void)
{
	return hakios_wifi_radio_bus() == HAKIOS_WIFI_BUS_PCI ? LX_OS_BUS_PCI : LX_OS_BUS_AHB;
}

int
lx_os_radio_pci_id(unsigned short *vendor, unsigned short *device, unsigned short *subsystem_vendor,
    unsigned short *subsystem_device)
{
	uint16_t v, d, sv, sd;

	if (!hakios_wifi_radio_pci(&v, &d, &sv, &sd)) {
		return 0;
	}
	*vendor = v;
	*device = d;
	*subsystem_vendor = sv;
	*subsystem_device = sd;
	return 1;
}

int
lx_os_radio_bar(unsigned long long *physical, unsigned long *size)
{
	uint64_t p, s;

	if (!hakios_wifi_radio_bar(&p, &s)) {
		return 0;
	}
	*physical = p;
	*size = (unsigned long)s;
	return 1;
}

unsigned
lx_os_radio_read32(unsigned offset)
{
	return hakios_wifi_radio_read32(offset);
}

void
lx_os_radio_write32(unsigned offset, unsigned value)
{
	hakios_wifi_radio_write32(offset, value);
}

void
lx_os_radio_wake(int awake)
{
	hakios_wifi_radio_wake(awake != 0);
}

void
lx_os_radio_halt(void)
{
	hakios_wifi_radio_halt();
}

int
lx_os_radio_address(unsigned char *address)
{
	return hakios_wifi_radio_address(address);
}

int
lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
	return hakios_wifi_network(name, name_length, passphrase, passphrase_length);
}

/* ---- frames: the radio as the link behind en0 (sys/hakios.net.h) ------------------------------------ */

/*
 * A frame from the network stack comes on whatever thread sends it, and the driver's side keeps
 * a record of every thread that enters it: so frames are put in a queue here, and one thread of
 * ours takes them to the driver. A queue that is full drops what comes (TCP sends it again).
 */
#define LX_QUEUE        256

struct lx_frame {
	uint16_t length;
	uint8_t bytes[HAKIOS_NET_FRAME_MAX];
};

static bool lx_link_attached;
static LCK_MTX_DECLARE(lx_queue_lock, &lx_lock_group);
static struct lx_frame *lx_queue;
static uint32_t lx_queue_in, lx_queue_out;      /* frames put in and taken out, counted from the start */
static uint64_t lx_queue_dropped;

/*
 * The first frames of the network's own kinds that pass, said: ARP (who has an address, and the answer) and DHCP
 * (DISCOVER, OFFER, REQUEST, ACK; the address handed out). Enough to see that data moves once a network is joined;
 * no hardware address is said, and nothing else of any frame.
 */
#define LX_NOTES_MOST   24
static volatile SInt32 lx_notes;

static void
lx_frame_note(const uint8_t *f, size_t length, bool out)
{
	static const char *const kinds[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM" };
	unsigned type;

	if (length < 14 || lx_notes >= LX_NOTES_MOST) {
		return;
	}
	type = (unsigned)f[12] << 8 | f[13];
	if (type == 0x0806 && length >= 42) {
		unsigned op = (unsigned)f[20] << 8 | f[21];

		if (OSIncrementAtomic(&lx_notes) < LX_NOTES_MOST) {
			say("lx: %s: ARP %s: %u.%u.%u.%u %s %u.%u.%u.%u\n", out ? "sent" : "received", op == 1 ? "request" : op == 2 ?
			    "reply" : "?", f[28], f[29], f[30], f[31], op == 1 ? "asks who has" : "says it is at its address, to",
			    f[38], f[39], f[40], f[41]);
		}
	} else if (type == 0x0800 && length >= 34 && f[23] == 17) {
		size_t ip = (size_t)(f[14] & 0xf) * 4, udp = 14 + ip, bootp = udp + 8;
		unsigned from, to, kind = 0;

		if (bootp + 240 > length) {
			return;
		}
		from = (unsigned)f[udp] << 8 | f[udp + 1];
		to = (unsigned)f[udp + 2] << 8 | f[udp + 3];
		if (!((from == 67 && to == 68) || (from == 68 && to == 67))) {
			return;
		}
		for (size_t o = bootp + 240; o + 2 < length && f[o] != 255;) {
			if (f[o] == 0) {
				o++;
				continue;
			}
			if (f[o] == 53 && f[o + 1] == 1) {
				kind = f[o + 2];
			}
			o += 2 + f[o + 1];
		}
		if (OSIncrementAtomic(&lx_notes) < LX_NOTES_MOST) {
			say("lx: %s: DHCP %s%s", out ? "sent" : "received", kind < sizeof(kinds) / sizeof(kinds[0]) ? kinds[kind] : "?",
			    kind == 2 || kind == 5 ? "" : "\n");
			if (kind == 2 || kind == 5) {
				say(": THE ADDRESS %u.%u.%u.%u, from the server %u.%u.%u.%u\n", f[bootp + 16], f[bootp + 17],
				    f[bootp + 18], f[bootp + 19], f[26], f[27], f[28], f[29]);
			}
		}
	}
}

static void
lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		struct lx_frame *frame;

		lck_mtx_lock(&lx_queue_lock);
		while (lx_queue_in == lx_queue_out) {
			lck_mtx_sleep(&lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_queue_in, THREAD_UNINT);
		}
		frame = &lx_queue[lx_queue_out % LX_QUEUE];     /* (stays ours until lx_queue_out moves) */
		lck_mtx_unlock(&lx_queue_lock);
		lx_transmit(frame->bytes, frame->length);
		lck_mtx_lock(&lx_queue_lock);
		lx_queue_out++;
		lck_mtx_unlock(&lx_queue_lock);
	}
}

static bool
lx_link_transmit(mbuf_t packet, size_t length)
{
	struct lx_frame *frame;

	lck_mtx_lock(&lx_queue_lock);
	if (length > HAKIOS_NET_FRAME_MAX || lx_queue_in - lx_queue_out == LX_QUEUE) {
		lx_queue_dropped++;
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame = &lx_queue[lx_queue_in % LX_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	lx_frame_note(frame->bytes, length, true);
	lx_queue_in++;
	lck_mtx_unlock(&lx_queue_lock);
	thread_wakeup((event_t)&lx_queue_in);
	return true;
}

static struct hakios_net_link lx_link = {
	.name = "the phone's radio (ath11k)",
	.wireless = true,
	.transmit = lx_link_transmit,
};

void
lx_os_net_attach(const unsigned char *address)
{
	thread_t thread;

	if (lx_link_attached) {
		return;
	}
	lx_queue = kalloc_data(LX_QUEUE * sizeof(*lx_queue), Z_WAITOK | Z_ZERO);
	if (lx_queue == NULL || kernel_thread_start(lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
		say("lx: no memory or no thread for the frames to send: the radio's frames go nowhere\n");
		return;
	}
	thread_set_thread_name(thread, "wifi-send");
	thread_deallocate(thread);
	memcpy(lx_link.address, address, sizeof(lx_link.address));
	lx_link_attached = hakios_net_attach(&lx_link);
	say("lx: %s\n", lx_link_attached ? "the radio is the link behind en0" : "EN0 HAS A LINK ALREADY: the radio's frames go "
	    "nowhere");
}

/*
 * With wifi=1 the interface is made when the kernel starts, before the radio is up: Darwin's Wi-Fi
 * side looks for it and for its registry entry as soon as it runs, once, and a radio that
 * turns up twelve seconds later is one it has already failed to open.
 */
void
hakios_lx_net_early(void)
{
	uint8_t address[6];

	if (hakios_wifi_radio_address(address)) {
		lx_os_net_attach(address);
	}
}

void
lx_os_net_link(int up)
{
	if (lx_link_attached) {
		hakios_net_link_state(up != 0);
	}
}

void
lx_os_net_input(const unsigned char *frame, unsigned long length)
{
	if (lx_link_attached && length <= HAKIOS_NET_FRAME_MAX) {
		lx_frame_note(frame, length, false);
		hakios_net_input(frame, length);
	}
}

/* ---- the radio's interrupts ------------------------------------------------------------------------------- */

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);

#define LX_VECTORS      32

static struct {
	void (*function)(void *);
	void *argument;
	bool attached;
} lx_vector[LX_VECTORS];
static uint32_t lx_irq_first;                   /* the GIC's number of vector 0; 0: no interrupts (or on one line) */
static bool lx_irq_line;                        /* the radio's vectors come on one line, told apart by the machine (zeus) */
static bool lx_irq_line_attached;
static uint64_t lx_irq_counts[LX_VECTORS];      /* (on one line: what came, by vector) */
static uint64_t lx_irq_cpu;                     /* the thread's time in the vectors' functions, mach units */
static uint64_t lx_irq_wakes;
static uint32_t lx_irq_pending;                 /* the vectors that have fired and are not dealt with yet */
static uint32_t lx_irq_running;                 /* the vectors whose function the thread is in */
static thread_t lx_irq_thread;
static LCK_MTX_DECLARE(lx_irq_lock, &lx_lock_group);

/* In interrupt context: the vector masked, and the thread told. */
static void
lx_irq_arrived(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	os_atomic_or(&lx_irq_pending, 1u << (intid - lx_irq_first), relaxed);
	thread_wakeup((event_t)&lx_irq_pending);
}

/* In interrupt context, on one line: the vectors that fired, masked by the machine already. */
static void
lx_irq_line_arrived(uint32_t vectors)
{
	for (uint32_t v = vectors; v != 0; v &= v - 1) {
		os_atomic_inc(&lx_irq_counts[__builtin_ctz(v)], relaxed);
	}
	os_atomic_or(&lx_irq_pending, vectors, relaxed);
	thread_wakeup((event_t)&lx_irq_pending);
}

static void
lx_irq_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint32_t pending;

		for (;;) {
			assert_wait((event_t)&lx_irq_pending, THREAD_UNINT);
			if ((pending = os_atomic_xchg(&lx_irq_pending, 0, acquire)) != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
		}
		uint64_t cpu = thread_get_runtime_self();
		lx_irq_wakes++;
		for (unsigned v = 0; v < LX_VECTORS; v++) {
			if (!(pending & 1u << v)) {
				continue;
			}
			lck_mtx_lock(&lx_irq_lock);
			if (!lx_vector[v].attached) {
				lck_mtx_unlock(&lx_irq_lock);
				continue;
			}
			lx_irq_running |= 1u << v;
			lck_mtx_unlock(&lx_irq_lock);
			lx_vector[v].function(lx_vector[v].argument);
			lck_mtx_lock(&lx_irq_lock);
			lx_irq_running &= ~(1u << v);
			lck_mtx_unlock(&lx_irq_lock);
			thread_wakeup((event_t)&lx_irq_running);
		}
		lx_irq_cpu += thread_get_runtime_self() - cpu;
	}
}

/* The CPU time of the thread that calls (nanoseconds), and of the interrupts' thread in the vectors' functions. */
unsigned long long
lx_os_thread_cpu(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(thread_get_runtime_self(), &ns);
	return ns;
}

unsigned long long
lx_os_irq_cpu(unsigned long long *wakes)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(lx_irq_cpu, &ns);
	*wakes = lx_irq_wakes;
	return ns;
}

int
lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
	uint32_t first = 0;

	if (!hakios_wifi_radio_msi(address, data, &first)) {
		return 0;
	}
	lx_irq_first = first;
	lx_irq_line = first == 0;
	return 1;
}

int
lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
	kern_return_t kr;

	if (vector >= LX_VECTORS || (lx_irq_first == 0 && !lx_irq_line) || function == NULL) {
		return EINVAL;
	}
	if (lx_irq_thread == THREAD_NULL && lx_irq_line) {
		if (kernel_thread_start(lx_irq_runs, NULL, &lx_irq_thread) != KERN_SUCCESS) {
			lx_irq_thread = THREAD_NULL;
			return ENOMEM;
		}
		thread_set_thread_name(lx_irq_thread, "wifi-irq");
	} else if (lx_irq_thread == THREAD_NULL) {
		hakios_wifi_tell("lx: wifi_irq: THE GIC'S DISTRIBUTOR IS WRITTEN for the radio's interrupts: each vector's SPI (%u + v) edge-"
		    "triggered, group 1, routed to this processor, then enabled\n", lx_irq_first);
		if (kernel_thread_start(lx_irq_runs, NULL, &lx_irq_thread) != KERN_SUCCESS) {
			lx_irq_thread = THREAD_NULL;
			return ENOMEM;
		}
		thread_set_thread_name(lx_irq_thread, "wifi-irq");
	}
	lck_mtx_lock(&lx_irq_lock);
	lx_vector[vector].function = function;
	lx_vector[vector].argument = argument;
	lx_vector[vector].attached = true;
	lck_mtx_unlock(&lx_irq_lock);
	if (lx_irq_line) {
		hakios_wifi_radio_irq_mask(vector, true);
		if (!lx_irq_line_attached) {
			if (!hakios_wifi_radio_irq_attach(lx_irq_line_arrived)) {
				lck_mtx_lock(&lx_irq_lock);
				lx_vector[vector].attached = false;
				lck_mtx_unlock(&lx_irq_lock);
				return ENXIO;
			}
			lx_irq_line_attached = true;
		}
		return 0;
	}
	if ((kr = gic_v3_spi_attach(lx_irq_first + vector, true, lx_irq_arrived, NULL)) != KERN_SUCCESS) {
		lck_mtx_lock(&lx_irq_lock);
		lx_vector[vector].attached = false;
		lck_mtx_unlock(&lx_irq_lock);
		return kr == KERN_NAME_EXISTS ? EBUSY : EINVAL;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	return 0;
}

void
lx_os_irq_enable(unsigned vector)
{
	if (vector < LX_VECTORS && lx_vector[vector].attached) {
		if (lx_irq_line) {
			hakios_wifi_radio_irq_mask(vector, false);
		} else {
			gic_v3_spi_unmask(lx_irq_first + vector);
		}
	}
}

void
lx_os_irq_disable(unsigned vector)
{
	if (vector >= LX_VECTORS || !lx_vector[vector].attached) {
		return;
	}
	if (lx_irq_line) {
		hakios_wifi_radio_irq_mask(vector, true);
	} else {
		gic_v3_spi_mask(lx_irq_first + vector);
	}
	if (current_thread() == lx_irq_thread) {
		return;                         /* (from the vector's own function, or another's: nothing to wait for) */
	}
	lck_mtx_lock(&lx_irq_lock);
	while (lx_irq_running & 1u << vector) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
}

void
lx_os_irq_detach_all(void)
{
	if (lx_irq_line && lx_irq_line_attached) {
		hakios_wifi_radio_irq_detach();
		lx_irq_line_attached = false;
	}
	for (unsigned v = 0; !lx_irq_line && v < LX_VECTORS; v++) {
		if (lx_vector[v].attached) {
			gic_v3_spi_detach(lx_irq_first + v);
		}
	}
	lck_mtx_lock(&lx_irq_lock);
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		lx_vector[v].attached = false;
	}
	while (lx_irq_running != 0 && current_thread() != lx_irq_thread) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
	os_atomic_store(&lx_irq_pending, 0, relaxed);
}

unsigned long long
lx_os_irq_count(unsigned vector)
{
	if (vector >= LX_VECTORS) {
		return 0;
	}
	return lx_irq_line ? os_atomic_load(&lx_irq_counts[vector], relaxed) : lx_irq_first != 0 ?
	       gic_v3_spi_count(lx_irq_first + vector) : 0;
}

/* ---- the station (sys/hakios.sta.h) --------------------------------------------------------------------- */

void
lx_os_station(const struct hakios_sta_ops *ops)
{
	hakios_sta_attach(ops);
}

void
lx_os_station_lost(int error)
{
	hakios_sta_lost(error);
}

/* ---- the driver ---------------------------------------------------------------------------------------- */

/* The object has its own; this is the one of a kernel built without it. */
__attribute__((weak)) int
lx_start(void)
{
	say("lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built (tools/mk-ath11k.sh, then "
	    "build the kernel again)\n");
	return ENXIO;
}

__attribute__((weak)) void
lx_stop(void)
{
}

__attribute__((weak)) int
lx_prepare(void)
{
	return 0;
}

__attribute__((weak)) int
lx_transmit(__unused const unsigned char *frame, __unused unsigned long length)
{
	return ENXIO;
}

int
hakios_lx_start(void)
{
	return lx_start();
}

void
hakios_lx_stop(void)
{
	lx_stop();
}

int
hakios_lx_prepare(void)
{
	return lx_prepare();
}
