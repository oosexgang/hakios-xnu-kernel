/*
 * A camera on a MediaTek phone: the skeleton. It is the part of redwood_mtkcam.c (the Infinix Hot 20's working driver, MT6768 + Samsung S5K JN1,
 * which has the same node and the same ABI) that does not depend on the phone: /dev/redwood-camera for iOS (sys/redwood_camera.h, RWFRAME1), a
 * session that starts when something reads and ends when nothing has for a while, the newest-frame handover, the EXPOSURE command. What is
 * different on every phone -- the rails and clocks, the sensor and its tables, the CSI receiver and the DMA that writes the frames -- is a table of
 * operations, struct mtkcam_platform, that a port fills in. Nothing here knows a register.
 *
 * It does nothing until it is told to (boot-arg mtkcam_skel=1) AND the platform is complete: every field the driver needs that is still missing
 * is said (redwood_host_keep_add) and the node is not made. There are no default values for what only the phone can tell.
 *
 * Not in bsd/conf/files, and not to be built together with redwood_mtkcam.c (both make /dev/redwood-camera). To try it: add
 *     bsd/dev/redwood_mtkcam_skel.c   standard
 * to bsd/conf/files, call redwood_mtkcam_skel_init() where redwood_mtkcam_init() is called (redwood_input.c), and put your platform in
 * mtkcam_platform below. docs/camera-mtk-porting.md has the order to find things out in and the tools for it; the proven sequence for one phone is
 * redwood_mtkcam.c and mtkloader/mtkcam.h of this tree (docs/camera-x6826b.md).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/redwood_camera.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);

#define say(...) redwood_host_keep_add("mtkcam-skel: " __VA_ARGS__)

#define SKEL_MAX_BUFFERS        4u
#define SKEL_MAX_STRIDE         8192u           /* of the RAW10 lines handed out: 4 pixels in 5 bytes, 6528 pixels at most */
#define SKEL_CHUNK_LINES        8u
#define SKEL_IDLE_STOP_MS       4000u           /* the session ends when nothing has read for this long */

/* What one frame the hardware has finished says about itself. */
struct mtkcam_frame_meta {
	uint32_t        exposure_lines;         /* the sensor's exposure it was taken with */
	uint32_t        gain;                   /* the analog gain register's value it was taken with */
	uint32_t        frame_lines;            /* the frame length in lines it was taken with */
};

/* What the frames are. Zero is "not known": a port that does not know a field leaves it zero and the driver refuses to start. */
struct mtkcam_geometry {
	uint32_t        width, height;          /* pixels (height even: whole Bayer cells) */
	uint32_t        bayer;                  /* the order of a 2x2 cell: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR (sys/redwood_camera.h) */
	uint32_t        black;                  /* the black level in 10-bit steps */
	uint32_t        line_ns;                /* one sensor line's time */
	uint32_t        gain_unity, gain_max;   /* the gain register's value for 1x and the most it may be given */
	uint32_t        frame_lines_min, frame_lines_max, margin;       /* the mode's own frame length, the longest, and exposure_lines <= frame_lines - margin */
	uint32_t        nbuf;                   /* frame buffers the DMA cycles through (2..SKEL_MAX_BUFFERS) */
};

/*
 * The operations of one phone. All of them are required (the skeleton checks); each returns 0 or an errno, and says (say()) what it is about to
 * do BEFORE a step that could hang a bus: the last line of a log that ends is the step that hung.
 */
struct mtkcam_platform {
	const char              *name;          /* "mt6768 / x6826b / s5k-jn1": what the log calls it */
	struct mtkcam_geometry  geometry;
	/* rails, the camera power domain and its bus protection, clocks, reset and MCLK pins. Undone by power_off. On a phone whose PMIC is
	 * reached through a wrapper shared with other drivers (MT6358 here), take that driver lock around every transaction (redwood_pwrap_lock). */
	int     (*power_on)(void);
	void    (*power_off)(void);
	/* the sensor: chip ID read and checked, its table written, streaming on; and off again (sensor_stop must leave the bus idle) */
	int     (*sensor_start)(void);
	void    (*sensor_stop)(void);
	/* the CSI-2 receiver and the DMA: set up, buffers queued, running; and stopped (the DMA must be quiet when this returns) */
	int     (*rx_start)(void);
	void    (*rx_stop)(void);
	/* the buffer (0..nbuf-1) of a finished frame, or -1 if none is done yet; called in a loop with a short sleep between, from the session's thread */
	int     (*frame_done)(struct mtkcam_frame_meta *meta);
	/* `lines` lines from `first` of buffer `buf`, as MIPI RAW10 (4 pixels in 5 bytes, stride = width * 5 / 4), into dst. A DMA that writes another
	 * layout is converted here (the MT6768's writes a byte a pixel and is widened here); the cache is invalidated here if the buffer is cached. */
	void    (*read_lines)(unsigned buf, uint32_t first, uint32_t lines, uint8_t *dst);
	/* the buffer is the hardware's again */
	void    (*frame_release)(unsigned buf);
	/* the sensor's exposure, gain and frame length for the frames to come (frame_lines 0: the shortest that holds the exposure) */
	int     (*set_exposure)(uint32_t lines, uint32_t gain, uint32_t frame_lines);
};

/*
 * ---- THE PORT: fill this in ----
 * Nothing is assumed. For the Infinix Hot 20 the values are in redwood_mtkcam.c (CAM_PITCH, CAM_LINES, CAM_BAYER, CAM_BLACK10, CAM_GAIN_UNITY, ...)
 * and each one says there how it was found.
 */
static const struct mtkcam_platform mtkcam_platform = {
	.name = NULL,
};

/* ---- the platform check ---- */
static bool
skel_platform_complete(const struct mtkcam_platform *p)
{
	const struct mtkcam_geometry *g = &p->geometry;
	bool ok = true;

#define NEED(cond, what) do { if (!(cond)) { say("the platform lacks: %s", what); ok = false; } } while (0)
	NEED(p->name != NULL, "name");
	NEED(g->width >= 2 && !(g->width & 3), "geometry.width (a multiple of 4: RAW10)");
	NEED(g->width * 5 / 4 <= SKEL_MAX_STRIDE, "geometry.width at most 6528");
	NEED(g->height >= 2 && !(g->height & 1), "geometry.height (even)");
	NEED(g->nbuf >= 2 && g->nbuf <= SKEL_MAX_BUFFERS, "geometry.nbuf (2..4)");
	NEED(g->line_ns != 0, "geometry.line_ns");
	NEED(g->gain_unity != 0 && g->gain_max >= g->gain_unity, "geometry.gain_unity, gain_max");
	NEED(g->frame_lines_min != 0 && g->frame_lines_max >= g->frame_lines_min, "geometry.frame_lines_min, frame_lines_max");
	NEED(g->black != 0, "geometry.black (the black level; 0 would be a guess)");
	NEED(p->power_on && p->power_off, "power_on, power_off");
	NEED(p->sensor_start && p->sensor_stop, "sensor_start, sensor_stop");
	NEED(p->rx_start && p->rx_stop, "rx_start, rx_stop");
	NEED(p->frame_done && p->read_lines && p->frame_release, "frame_done, read_lines, frame_release");
	NEED(p->set_exposure, "set_exposure");
#undef NEED
	return ok;
}

/* ---- the session ---- */
static LCK_GRP_DECLARE(skel_lck_grp, "redwood-mtkcam-skel");
static LCK_MTX_DECLARE(skel_lock, &skel_lck_grp);

enum skel_slot_state { SLOT_FREE, SLOT_READY, SLOT_READING };

static struct {
	enum skel_slot_state    state;
	uint64_t                sequence, time;
	struct mtkcam_frame_meta meta;
} skel_slot[SKEL_MAX_BUFFERS];

static uint8_t skel_chunk[SKEL_CHUNK_LINES * SKEL_MAX_STRIDE];

static uint32_t skel_opens;
static bool skel_wanted, skel_streaming, skel_reading, skel_broken, skel_node_made;
static uint64_t skel_seq, skel_given, skel_last_read_ms;
static struct {
	bool            pending;
	uint32_t        lines, gain, frame_lines;
} skel_ask;

static uint64_t
skel_ms(void)
{
	uint64_t t, ns;

	t = mach_absolute_time();
	absolutetime_to_nanoseconds(t, &ns);
	return ns / NSEC_PER_MSEC;
}

/* One session: power, sensor, receiver; publish what the DMA finishes until nobody has read for SKEL_IDLE_STOP_MS; everything back off. */
static bool
skel_session(void)
{
	const struct mtkcam_platform *p = &mtkcam_platform;
	uint64_t last_frame_ms;
	int error;

	say("session: power on");
	if ((error = p->power_on()) != 0) {
		say("power_on failed (%d)", error);
		return false;
	}
	say("session: sensor start");
	if ((error = p->sensor_start()) != 0) {
		say("sensor_start failed (%d)", error);
		p->power_off();
		return false;
	}
	say("session: receiver start");
	if ((error = p->rx_start()) != 0) {
		say("rx_start failed (%d)", error);
		p->sensor_stop();
		p->power_off();
		return false;
	}
	lck_mtx_lock(&skel_lock);
	for (unsigned i = 0; i < SKEL_MAX_BUFFERS; i++) {
		skel_slot[i].state = SLOT_FREE;
	}
	skel_given = 0;
	skel_streaming = true;
	skel_last_read_ms = last_frame_ms = skel_ms();
	lck_mtx_unlock(&skel_lock);

	for (;;) {
		struct mtkcam_frame_meta meta;
		int b = p->frame_done(&meta);
		bool stop = false;

		if (b >= 0 && (unsigned)b < p->geometry.nbuf) {
			lck_mtx_lock(&skel_lock);
			if (skel_slot[b].state == SLOT_FREE) {
				/* the older frame nobody read is given back: the newest one is the one wanted */
				for (unsigned i = 0; i < p->geometry.nbuf; i++) {
					if (skel_slot[i].state == SLOT_READY) {
						skel_slot[i].state = SLOT_FREE;
						lck_mtx_unlock(&skel_lock);
						p->frame_release(i);
						lck_mtx_lock(&skel_lock);
					}
				}
				skel_slot[b].state = SLOT_READY;
				skel_slot[b].sequence = ++skel_seq;
				skel_slot[b].time = mach_absolute_time();
				skel_slot[b].meta = meta;
				wakeup(&skel_seq);
				lck_mtx_unlock(&skel_lock);
			} else {
				lck_mtx_unlock(&skel_lock);
				p->frame_release((unsigned)b);          /* (a buffer the driver still holds is not the DMA's to finish: give it back) */
			}
			last_frame_ms = skel_ms();
		}
		lck_mtx_lock(&skel_lock);
		if (skel_ask.pending) {
			struct { uint32_t lines, gain, frame_lines; } ask = { skel_ask.lines, skel_ask.gain, skel_ask.frame_lines };

			skel_ask.pending = false;
			lck_mtx_unlock(&skel_lock);
			if (p->set_exposure(ask.lines, ask.gain, ask.frame_lines) != 0) {
				say("set_exposure(%u, %u, %u) failed", ask.lines, ask.gain, ask.frame_lines);
			}
			lck_mtx_lock(&skel_lock);
		}
		if (skel_opens == 0 || skel_ms() - skel_last_read_ms > SKEL_IDLE_STOP_MS) {
			stop = true;
		} else if (skel_ms() - last_frame_ms > 3000u) {
			say("no frame for 3 s: the camera is given up (reads fail with EIO)");
			skel_broken = true;
			stop = true;
		}
		lck_mtx_unlock(&skel_lock);
		if (stop) {
			break;
		}
		if (b < 0) {
			delay_for_interval(2, NSEC_PER_MSEC);
		}
	}
	lck_mtx_lock(&skel_lock);
	skel_streaming = false;
	lck_mtx_unlock(&skel_lock);
	p->rx_stop();
	p->sensor_stop();
	p->power_off();
	say("session: stopped after %llu frames", skel_seq);
	return !skel_broken;
}

/* ---- /dev/redwood-camera ---- */
static int
skel_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (skel_broken) {
		return EIO;
	}
	lck_mtx_lock(&skel_lock);
	skel_opens++;
	lck_mtx_unlock(&skel_lock);
	return 0;
}

static int
skel_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	lck_mtx_lock(&skel_lock);
	if (skel_opens > 0) {
		skel_opens--;
	}
	lck_mtx_unlock(&skel_lock);
	return 0;
}

/* A struct redwood_camera_frame, then as many of the frame's bytes as there is room for (a buffer of the header's size learns the frame and gets nothing more). */
static int
skel_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	const struct mtkcam_platform *p = &mtkcam_platform;
	const struct mtkcam_geometry *g = &p->geometry;
	struct redwood_camera_frame f;
	uint64_t since = skel_ms();
	uint32_t stride = g->width / 4 * 5;
	int b, error = 0;

	lck_mtx_lock(&skel_lock);
	if (skel_reading) {
		lck_mtx_unlock(&skel_lock);
		return EBUSY;
	}
	skel_reading = true;
	skel_last_read_ms = skel_ms();
	for (;;) {
		if (skel_broken) {
			error = EIO;
			break;
		}
		b = -1;
		for (int i = 0; i < (int)g->nbuf; i++) {
			if (skel_slot[i].state == SLOT_READY && skel_slot[i].sequence > skel_given && (b < 0 || skel_slot[i].sequence > skel_slot[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0 && skel_streaming) {
			break;
		}
		if (!skel_streaming && !skel_wanted) {
			skel_wanted = true;
			wakeup(&skel_wanted);
		}
		if (skel_ms() - since > (skel_streaming ? 3000u : 20000u)) {
			error = EAGAIN;
			break;
		}
		struct timespec ts = { 0, 50 * NSEC_PER_MSEC };

		error = msleep(&skel_seq, &skel_lock, PZERO | PCATCH, "redwood-mtkcam-skel", &ts);
		if (error == EWOULDBLOCK) {
			error = 0;
		}
		if (error) {
			break;
		}
		skel_last_read_ms = skel_ms();
	}
	if (error) {
		skel_reading = false;
		lck_mtx_unlock(&skel_lock);
		return error;
	}
	skel_slot[b].state = SLOT_READING;
	skel_given = skel_slot[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, REDWOOD_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = g->width;
	f.height = g->height;
	f.stride = stride;
	f.format = 1;                           /* MIPI RAW10 */
	f.bayer = g->bayer;
	f.black = g->black;
	f.sequence = skel_slot[b].sequence;
	f.time = skel_slot[b].time;
	f.exposure_lines = skel_slot[b].meta.exposure_lines;
	f.line_ns = g->line_ns;
	f.gain = skel_slot[b].meta.gain;
	f.gain_unity = g->gain_unity;
	f.gain_max = g->gain_max;
	f.frame_lines = skel_slot[b].meta.frame_lines;
	f.frame_lines_min = g->frame_lines_min;
	f.frame_lines_max = g->frame_lines_max;
	f.margin = g->margin;
	f.sensor = 1;                           /* the wide (main) camera: the only one the skeleton knows */
	lck_mtx_unlock(&skel_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	for (uint32_t y = 0; y < g->height && error == 0 && uio_resid(uio) > 0; y += SKEL_CHUNK_LINES) {
		uint32_t n = MIN(SKEL_CHUNK_LINES, g->height - y);

		p->read_lines((unsigned)b, y, n, skel_chunk);
		error = uiomove((caddr_t)skel_chunk, (int)MIN((user_ssize_t)(n * stride), uio_resid(uio)), uio);
	}
	lck_mtx_lock(&skel_lock);
	skel_slot[b].state = SLOT_FREE;
	skel_reading = false;
	skel_last_read_ms = skel_ms();
	lck_mtx_unlock(&skel_lock);
	p->frame_release((unsigned)b);
	return error;
}

static int
skel_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case REDWOOD_CAMERA_EXPOSURE:
		lck_mtx_lock(&skel_lock);
		skel_ask.lines = c.a;
		skel_ask.gain = c.b;
		skel_ask.frame_lines = c.c;
		skel_ask.pending = true;
		lck_mtx_unlock(&skel_lock);
		return 0;
	case REDWOOD_CAMERA_KEEP:
		return ENOSPC;                  /* no memory in one piece is kept for the host */
	case REDWOOD_CAMERA_SENSOR:
		return c.a == 1 ? 0 : EINVAL;   /* only the wide (main) camera */
	default:
		return EINVAL;
	}
}

static const struct cdevsw skel_cdevsw = {
	.d_open = skel_open, .d_close = skel_close, .d_read = skel_read, .d_write = skel_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* A session whenever a read wants frames; after three sessions that never got to a frame the camera is given up. */
static void
skel_thread(__unused void *arg, __unused wait_result_t w)
{
	uint32_t failed = 0;
	int major;

	if (!skel_platform_complete(&mtkcam_platform)) {
		say("the platform is not complete: no camera");
		thread_terminate(current_thread());
		return;
	}
	major = cdevsw_add(-1, &skel_cdevsw);
	skel_node_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-camera") != NULL;
	say("%s: /dev/redwood-camera %s", mtkcam_platform.name, skel_node_made ? "made" : "CANNOT BE MADE");
	while (skel_node_made && !skel_broken) {
		struct timespec ts = { 1, 0 };

		lck_mtx_lock(&skel_lock);
		while (!skel_wanted) {
			(void)msleep(&skel_wanted, &skel_lock, PZERO, "redwood-mtkcam-skel-idle", &ts);
		}
		skel_wanted = false;
		if (skel_opens == 0) {
			lck_mtx_unlock(&skel_lock);
			continue;               /* (a read that came while the last session was ending) */
		}
		lck_mtx_unlock(&skel_lock);
		if (!skel_session()) {
			if (++failed >= 3) {
				say("three sessions failed: the camera is given up");
				skel_broken = true;
			}
		} else {
			failed = 0;
		}
	}
	thread_terminate(current_thread());
}

void redwood_mtkcam_skel_init(void);

void
redwood_mtkcam_skel_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkcam_skel", &level, sizeof(level)) || level < 1) {
		return;
	}
	if (kernel_thread_start_priority(skel_thread, NULL, 81 /* BASEPRI_KERNEL */, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
