/*
 * The core of this port's Metal driver, in C++ (userland/src/metal/*.cpp), as C for the plug-in
 * (src/redwoodmetal.m). The shape is hakios-metal's (include/hakios-metal/hakios_metal.h on the
 * hakios-metal branch): the plug-in keeps Metal's own classes -- subclasses of Metal's private base
 * classes, which only Objective-C can make -- and the core keeps the work behind them, so that the
 * work can be built and read without the Objective-C runtime around it.
 *
 * Moved here so far: the compiler library (libredwoodair.dylib, the contract of
 * tools/mesa/air/air_metal.h) and buffers. The plug-in used to carry a copy of air_metal.h's
 * declarations "because build.sh knows nothing of Mesa's tree"; it is the same repository, so the
 * core includes the original, and a call's type is the library's own (__typeof__ of its prototype)
 * rather than a second spelling that can drift from it.
 */
#ifndef RWM_H
#define RWM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <mach/mach_types.h>
#include <IOSurface/IOSurfaceRef.h>

#include "air_metal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the driver says, core and plug-in alike: "redwoodmetal[process pid]: " and the line, to stdout,
 * flushed; the first of each sentence that says something went wrong also to kern.redwood.say.
 */
void rwm_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* the boot arguments, read once (src/metal/perf.cpp); rwm_boot_arg: whether `text` is in them */
const char *rwm_boot_args(void);
bool rwm_boot_arg(const char *text);

/*
 * Where a frame's time goes (src/metal/perf.cpp): counters added to where the work is, said as kept lines every
 * five seconds by rwm_perf_report and then set back to zero.
 */
struct rwm_perf {
	uint64_t since, cbs, draws, encode, wait, worst, pipes, compile;
	uint64_t recording, between, last_end;  /* buffer made to commit; one commit's end to the next's start */
	uint64_t copyins, copyin_bytes, copyin_ns;      /* surfaces copied into textures */
	/* where the threads that make command buffers wait for this driver: a full worker queue at -commit, a
	 * buffer's work at -waitUntilScheduled and the like, its GPU work at -waitUntilCompleted */
	uint64_t queue_waits, queue_ns, processed_waits, processed_ns, completed_waits, completed_ns;
	uint64_t drains, drain_ns;      /* a texture written or read at once, after the worker's queue */
	uint64_t worker_cbs, worker_ns;  /* the worker's whole time for a buffer, waits in it included */
	uint64_t worker_cpu;            /* ... and how much of it the worker ran */
	/* made to committed on the thread that makes them: the time, and how much of it that thread ran */
	uint64_t made_cbs, made_ns, made_cpu;
};
extern struct rwm_perf rwm_perf;
uint64_t rwm_ns(void);
uint64_t rwm_thread_cpu_ns(void);
void rwm_perf_report(uint64_t now);

/*
 * The compiler library's calls. Required: device_open, compute_create, dispatch, texture_create,
 * sampler_create -- rwa_dispatch's arguments changed when textures came into the contract, so a
 * library without rwa_texture_create is older than this driver and is not used at all. Everything
 * else may be NULL, and its user says so by name: a library that computes and does not yet draw is a
 * real state of this work.
 */
struct rwm_air {
	void *handle;
	__typeof__(&rwa_device_open) device_open;
	__typeof__(&rwa_device_close) device_close;
	__typeof__(&rwa_device_name) device_name;
	__typeof__(&rwa_buffer_create) buffer_create;
	__typeof__(&rwa_buffer_map) buffer_map;
	__typeof__(&rwa_buffer_size) buffer_size;
	__typeof__(&rwa_buffer_destroy) buffer_destroy;
	__typeof__(&rwa_compute_create) compute_create;
	__typeof__(&rwa_compute_destroy) compute_destroy;
	__typeof__(&rwa_texture_create) texture_create;
	__typeof__(&rwa_texture_write) texture_write;
	__typeof__(&rwa_texture_read) texture_read;
	__typeof__(&rwa_texture_copy) texture_copy;
	__typeof__(&rwa_buffer_copy) buffer_copy;
	__typeof__(&rwa_buffer_fill) buffer_fill;
	__typeof__(&rwa_texture_mipmaps) texture_mipmaps;
	__typeof__(&rwa_render_clear) render_clear;
	__typeof__(&rwa_depth_read) depth_read;
	__typeof__(&rwa_texture_destroy) texture_destroy;
	__typeof__(&rwa_sampler_create) sampler_create;
	__typeof__(&rwa_sampler_destroy) sampler_destroy;
	__typeof__(&rwa_dispatch) dispatch;
	__typeof__(&rwa_dispatch_buffers) dispatch_buffers;
	__typeof__(&rwa_module_argbufs) module_argbufs;
	__typeof__(&rwa_argbuf_encode_buffer) argbuf_encode_buffer;
	__typeof__(&rwa_argbuf_encode_texture) argbuf_encode_texture;
	__typeof__(&rwa_argbuf_encode_sampler) argbuf_encode_sampler;
	__typeof__(&rwa_argbuf_encoded_members) argbuf_encoded_members;
	__typeof__(&rwa_render_constants) render_constants;
	__typeof__(&rwa_render_create) render_create;
	__typeof__(&rwa_render_destroy) render_destroy;
	__typeof__(&rwa_render_draw) render_draw;
	__typeof__(&rwa_depth_stencil_create) depth_stencil_create;
	__typeof__(&rwa_depth_stencil_destroy) depth_stencil_destroy;
	__typeof__(&rwa_finish) finish;
	__typeof__(&rwa_flush_seq) flush_seq;     /* optional: sent off, not waited for */
	__typeof__(&rwa_wait_seq) wait_seq;
	__typeof__(&rwa_surface_seq) surface_seq;
};

/*
 * Loaded once, on the first call, and its absence is an answer rather than a crash: without the GPU
 * there is no library worth loading, and the device object still has to exist. rwm_air() is never
 * NULL; when rwm_air_available() is false every call in it is NULL.
 */
bool rwm_air_available(void);
const struct rwm_air *rwm_air(void);

/*
 * A buffer. With the compiler library loaded the memory is Mesa's (rwa_buffer_create), because only
 * such a buffer can be handed to a dispatch or a draw; without it, the kernel's GEM interface on
 * `gpu` (the device's RedwoodGPU client, not owned) gives memory a program can still write. Either
 * way it is write-combined and the GPU reads it through no cache of this processor.
 * NULL with the reason in `why` when neither would give it.
 */
struct rwm_buffer;

struct rwm_buffer *rwm_buffer_new(struct rwa_device *device, mach_port_t gpu, size_t length,
				  char *why, size_t why_size);
void rwm_buffer_free(struct rwm_buffer *buffer);
void *rwm_buffer_contents(const struct rwm_buffer *buffer);
size_t rwm_buffer_length(const struct rwm_buffer *buffer);
uint64_t rwm_buffer_gpu_address(const struct rwm_buffer *buffer);    /* the kernel path only, else 0 */
struct rwa_buffer *rwm_buffer_mesa(const struct rwm_buffer *buffer); /* the Mesa path only, else NULL */

/*
 * A texture: the library's texture (taken: destroyed with the record, and at once when the record
 * cannot be made, which is the only NULL) and its shape, and the two roads to an IOSurface.
 *
 * Copy-in: rwm_texture_copy_in_from names the surface another process draws (two_planes: YCbCr 4:2:0,
 * packed into RGBA8 as Y, Cb, Cr); rwm_texture_copy_in copies it when its seed has moved since the last
 * copy. 0 when it copied or had nothing to do, -1 with the reason when the library refused the texels;
 * *bytes is what was handed over (0: nothing was).
 * Copy-back: rwm_texture_copy_back_to names the surface the window server shows; rwm_texture_sync_surface
 * reads what was drawn and writes it into the surface's rows, -1 with the reason when it could not.
 */
struct rwm_texture;

struct rwm_texture *rwm_texture_new(struct rwa_texture *texture, const struct rwa_texture_shape *shape);
void rwm_texture_free(struct rwm_texture *texture);
struct rwa_texture *rwm_texture_mesa(const struct rwm_texture *texture);
const struct rwa_texture_shape *rwm_texture_shape(const struct rwm_texture *texture);
void rwm_texture_copy_in_from(struct rwm_texture *texture, IOSurfaceRef surface, bool two_planes);
int rwm_texture_copy_in(struct rwm_texture *texture, size_t *bytes, char *why, size_t why_size);
void rwm_texture_copy_back_to(struct rwm_texture *texture, IOSurfaceRef surface);
bool rwm_texture_has_surface(const struct rwm_texture *texture);
/* its texels written or read at once, outside any command buffer, after the buffers committed before it
 * (rwm_worker_drain); the library's refusal said, and false (rwm_texture_read_depth: by aspect, either may be NULL) */
bool rwm_texture_write(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		       const unsigned size[3], const void *bytes, size_t stride);
bool rwm_texture_read(struct rwm_texture *texture, unsigned level, unsigned slice, const unsigned origin[3],
		      const unsigned size[3], void *bytes, size_t stride);
bool rwm_texture_read_depth(struct rwm_texture *texture, const unsigned origin[3], const unsigned size[3],
			    float *depth, uint8_t *stencil);
/* the surface a texture laid over its memory lies over (src/metal/import.cpp), drawn in place; 0: none */
uint32_t rwm_texture_in_place(const struct rwm_texture *texture);
void rwm_texture_set_in_place(struct rwm_texture *texture, uint32_t surface);
int rwm_texture_sync_surface(struct rwm_texture *texture, char *why, size_t why_size);

/*
 * A blit encoder's work, as the command buffer runs it at -commit: the plug-in names the library's
 * objects (and, for a copy between a buffer and a texture, the buffer's memory as this processor sees
 * it), the core records it (rwm_blit_record) and does it.
 */
enum rwm_blit_kind {
	RWM_BLIT_TEXTURE_TO_TEXTURE = 1,
	RWM_BLIT_BUFFER_TO_BUFFER = 2,
	RWM_BLIT_BUFFER_TO_TEXTURE = 3,
	RWM_BLIT_TEXTURE_TO_BUFFER = 4,
	RWM_BLIT_FILL = 5,              /* to_buffer, to_offset, bytes, fill_value */
	RWM_BLIT_CLEAR = 6,             /* a pass that only clears: colors, depth and what each is cleared to */
	RWM_BLIT_MIPMAPS = 7,           /* to_texture's levels below the first */
};

struct rwm_blit {
	unsigned kind;
	struct rwa_texture *from_texture, *to_texture;
	struct rwa_buffer *from_buffer, *to_buffer;
	const uint8_t *from_contents;   /* RWM_BLIT_BUFFER_TO_TEXTURE: from_buffer's memory */
	uint8_t *to_contents;           /* RWM_BLIT_TEXTURE_TO_BUFFER: to_buffer's memory */
	unsigned from_level, from_slice, from_origin[3];
	unsigned to_level, to_slice, to_origin[3];
	unsigned size[3];
	size_t from_offset, to_offset, bytes, row_bytes;
	uint8_t fill_value;
	/* RWM_BLIT_CLEAR */
	struct rwa_device *device;
	struct rwa_texture *colors[8];
	unsigned color_count;
	const float *clear_color;       /* NULL: the colours are not cleared */
	struct rwa_texture *depth;
	const float *clear_depth;
	const unsigned *clear_stencil;
};

/* 0 when it was done (a clear or mipmaps the library has no call for count as done, as before), -1 with
 * the reason when the library refused it or has no copies at all */
int rwm_run_blit(const struct rwm_blit *blit, char *why, size_t why_size);

/*
 * A render encoder's draw, as the command buffer runs it: Metal's tables at Metal's own indices with
 * the library's objects in them (NULL where the object bound has none: a buffer from the kernel's GEM
 * path, from another driver) and, beside each, whether anything was bound there at all -- because
 * "bound, and not one of ours" is refused by name and "nothing bound" is not.
 */
#define RWM_MAX_BUFFERS 32

struct rwm_draw {
	struct rwa_render *render;
	unsigned stream_mask;           /* the pipeline's: which vertex buffer indices are streams */
	unsigned stream_count;          /* ... and the highest it fetches from, plus one */
	struct rwa_texture *color[8];
	unsigned color_count;
	const float *clear_color;       /* NULL: kept, not cleared */
	struct rwa_texture *depth;
	const float *clear_depth;
	const unsigned *clear_stencil;
	const struct rwa_depth_stencil *depth_stencil;
	unsigned stencil_reference[2];
	const float *depth_range, *viewport;
	const unsigned *scissor;
	unsigned vertex_count_bound;    /* Metal's vertex buffer table, so many entries */
	struct rwa_buffer *vertex[RWM_MAX_BUFFERS];
	size_t vertex_offset[RWM_MAX_BUFFERS];
	bool vertex_bound[RWM_MAX_BUFFERS];
	unsigned fragment_count_bound;  /* Metal's fragment buffer table */
	struct rwa_buffer *fragment[RWM_MAX_BUFFERS];
	size_t fragment_offset[RWM_MAX_BUFFERS];
	bool fragment_bound[RWM_MAX_BUFFERS];
	unsigned texture_count;
	struct rwa_texture *textures[RWM_MAX_BUFFERS];
	unsigned sampler_count;
	struct rwa_sampler *samplers[RWM_MAX_BUFFERS];
	unsigned primitive, vertex_start, vertex_count;   /* vertex_count: the INDEX count of an indexed draw */
	struct rwa_buffer *index_buffer;
	bool index_bound;
	unsigned index_type;
	size_t index_offset;
	int base_vertex;
	unsigned instance_count, base_instance;
};

/* 0 when it was drawn; -1 with the reason when it was refused before the library was asked (the
 * library cannot draw, a binding is not one of the library's, too many streams); -2 with the library's
 * own reason when the draw itself failed */
int rwm_run_draw(const struct rwm_draw *draw, char *why, size_t why_size);

/*
 * A compute encoder's dispatch: Metal's one [[buffer(n)]] table with the library's objects in it and
 * whether anything was bound at each index, the textures and samplers, the grid. A buffer bound and not
 * the library's is said and bound as nothing, as before; 0 when it ran, -1 with the library's reason.
 */
struct rwm_dispatch {
	struct rwa_compute *compute;
	unsigned count;
	struct rwa_buffer *buffers[RWM_MAX_BUFFERS];
	size_t offsets[RWM_MAX_BUFFERS];
	bool bound[RWM_MAX_BUFFERS];
	unsigned texture_count;
	struct rwa_texture *textures[RWM_MAX_BUFFERS];
	unsigned sampler_count;
	struct rwa_sampler *samplers[RWM_MAX_BUFFERS];
	unsigned groups[3], size[3];
};

int rwm_run_dispatch(const struct rwm_dispatch *dispatch, char *why, size_t why_size);

/*
 * A command buffer's end, once its work is recorded into the library: sent off and NOT waited for when
 * the library queues the GPU's jobs (rwa_flush_seq; *seq is then the job the display and the completed
 * handlers wait for) and nothing asks for the whole of it now -- `finish`: a surface drawn by the
 * copy-back road needs the frame at once, and boot-arg rwm_finish=1 asks for it always; otherwise the
 * GPU is waited for. 0, or -1 with the reason when the GPU did not finish.
 */
int rwm_submit(struct rwa_device *device, bool finish, uint64_t *seq, char *why, size_t why_size);

/*
 * A recorded command, made into the library's objects when it was encoded (src/metal/run.cpp runs them): a draw, a
 * dispatch or a blit, the textures it samples that are copied in from another process's surface before it runs,
 * and its colour targets -- those over a surface the window server shows are copied back after the buffer.
 */
enum {
	RWM_WORK_DRAW = 1,
	RWM_WORK_DISPATCH = 2,
	RWM_WORK_BLIT = 3,
};

#define RWM_WORK_KEEP (4 + 8 + 4 * RWM_MAX_BUFFERS)

struct rwm_work {
	unsigned kind;
	struct rwm_draw draw;
	struct rwm_dispatch dispatch;
	struct rwm_blit blit;
	struct rwm_texture *copy_in[RWM_MAX_BUFFERS + 1];
	unsigned copy_in_count;
	struct rwm_texture *targets[8];
	unsigned target_count;
	/* what the draw's and the clear's pointers point at, kept with them */
	float clear_color[4], clear_depth, depth_range[2], viewport[4];
	unsigned clear_stencil, scissor[4];
	/* the surfaces it reads in place (counted in use at -commit) and its colour attachments', by attachment (0: none
	 * there; marked pending for the display at -commit) */
	uint32_t sampled[RWM_MAX_BUFFERS + 1];
	unsigned sampled_count;
	uint32_t target_surface[8];
	unsigned target_surface_count;
	/* Metal's objects it was recorded from, which the plug-in keeps alive with it; the pipeline's, for its lines */
	void *keep[RWM_WORK_KEEP];
	unsigned keep_count;
	void *pipeline;
};

/*
 * A render encoder (src/metal/render.cpp): the pass it draws into, as the plug-in reads Metal's descriptor, and the
 * state bound so far -- each binding as Metal's object (not kept alive by this: the work that records a draw keeps
 * it), the library's and, for a texture, the core's (NULL: not one of ours). A draw records the state as it is into
 * a work and lists the objects it keeps; the pass's load actions go with its first draw, or with its end as a clear.
 */
struct rwm_bound_texture {
	void *object;
	struct rwm_texture *core;
};
struct rwm_bound_buffer {
	void *object;
	struct rwa_buffer *buffer;
	size_t offset;
};
struct rwm_render_state {
	struct rwa_device *device;
	struct rwm_bound_texture color[8];
	unsigned color_count;
	struct rwm_bound_texture depth;
	float clear_color[4], clear_depth;
	unsigned clear_stencil;
	bool clears, clears_depth, clears_stencil;
	void *pipeline;
	struct rwa_render *render;
	unsigned stream_mask, stream_count;
	bool writes_depth;
	void *depth_stencil_object;
	struct rwa_depth_stencil *depth_stencil;
	unsigned stencil_reference[2];
	float depth_range[2], viewport[4];
	unsigned scissor[4];
	bool has_depth_range, has_viewport, has_scissor;
	struct rwm_bound_buffer vertex[RWM_MAX_BUFFERS], fragment[RWM_MAX_BUFFERS];
	unsigned vertex_count, fragment_count;
	struct rwm_bound_texture textures[RWM_MAX_BUFFERS];
	unsigned texture_count;
	void *sampler_object[RWM_MAX_BUFFERS];
	struct rwa_sampler *samplers[RWM_MAX_BUFFERS];
	unsigned sampler_count;
	bool complained;                /* one complaint an encoder about what it cannot do */
};

/* the pass: colour attachment `index` (cleared: attachment 0's load action; a multisample resolve asked for by its
 * store action 2 or 3 or a resolve texture is said and not done), the depth and stencil attachments (one texture
 * carries both aspects; two different ones: said, neither) */
void rwm_render_begin(struct rwm_render_state *state, struct rwa_device *device);
void rwm_render_color(struct rwm_render_state *state, unsigned index, void *object, struct rwm_texture *core,
		      bool clears, const double color[4], unsigned long store_action, bool resolve_texture);
void rwm_render_depth(struct rwm_render_state *state, void *depth, struct rwm_texture *depth_core, bool clears_depth,
		      double clear_depth, void *stencil, struct rwm_texture *stencil_core, bool clears_stencil,
		      unsigned clear_stencil);
/* the state; an index past RWM_MAX_BUFFERS is said for a buffer and ignored for the rest, as before */
void rwm_render_set_pipeline(struct rwm_render_state *state, void *object, struct rwa_render *render,
			     unsigned stream_mask, unsigned stream_count, bool writes_depth);
void rwm_render_set_vertex_buffer(struct rwm_render_state *state, size_t index, void *object,
				  struct rwa_buffer *buffer, size_t offset);
void rwm_render_set_fragment_buffer(struct rwm_render_state *state, size_t index, void *object,
				    struct rwa_buffer *buffer, size_t offset);
void rwm_render_set_vertex_offset(struct rwm_render_state *state, size_t index, size_t offset);
void rwm_render_set_fragment_offset(struct rwm_render_state *state, size_t index, size_t offset);
void rwm_render_set_texture(struct rwm_render_state *state, size_t index, void *object, struct rwm_texture *core);
void rwm_render_set_sampler(struct rwm_render_state *state, size_t index, void *object, struct rwa_sampler *sampler);
void rwm_render_set_depth_stencil(struct rwm_render_state *state, void *object, struct rwa_depth_stencil *ds);
void rwm_render_set_stencil_reference(struct rwm_render_state *state, unsigned front, unsigned back);
/* x, y, width, height, znear, zfar: the rectangle kept when it is not the whole of the first attachment */
void rwm_render_set_viewport(struct rwm_render_state *state, const double viewport[6]);
void rwm_render_set_scissor(struct rwm_render_state *state, unsigned long x, unsigned long y, unsigned long width,
			    unsigned long height);

/* a work keeps `object` alive with it (the plug-in retains what it lists at -recordWork:) */
void rwm_work_keep(struct rwm_work *work, void *object);

/*
 * A compute encoder (src/metal/compute.cpp): the pipeline and Metal's one [[buffer(n)]] table, the textures and the
 * samplers, each as Metal's object beside the library's. An index past RWM_MAX_BUFFERS is said and not bound; a
 * per-binding lod clamp is said and drops the dispatches after it, as before. A dispatch records the state as it is.
 */
struct rwm_compute_state {
	void *pipeline;
	struct rwa_compute *compute;
	struct rwm_bound_buffer buffers[RWM_MAX_BUFFERS];
	unsigned buffer_count;
	struct rwm_bound_texture textures[RWM_MAX_BUFFERS];
	unsigned texture_count;
	void *sampler_object[RWM_MAX_BUFFERS];
	struct rwa_sampler *samplers[RWM_MAX_BUFFERS];
	unsigned sampler_count;
	bool complained;
};

void rwm_compute_set_pipeline(struct rwm_compute_state *state, void *object, struct rwa_compute *compute);
void rwm_compute_set_buffer(struct rwm_compute_state *state, size_t index, void *object, struct rwa_buffer *buffer,
			    size_t offset);
void rwm_compute_set_buffer_offset(struct rwm_compute_state *state, size_t index, size_t offset);
void rwm_compute_set_texture(struct rwm_compute_state *state, size_t index, void *object, struct rwm_texture *core);
void rwm_compute_set_sampler(struct rwm_compute_state *state, size_t index, void *object, struct rwa_sampler *sampler);
void rwm_compute_lod_clamp(struct rwm_compute_state *state, float lod_min, float lod_max);
/* the dispatch recorded into `work` (zeroed); false when it is dropped, said why -- by threads, a grid that does not
 * divide into the groups is refused rather than rounded */
bool rwm_compute_dispatch(struct rwm_compute_state *state, const unsigned long groups[3], const unsigned long size[3],
			  struct rwm_work *work);
bool rwm_compute_dispatch_threads(struct rwm_compute_state *state, const unsigned long threads[3],
				  const unsigned long size[3], struct rwm_work *work);

/* a blit encoder's copy recorded into `work` (zeroed): `blit` as the plug-in named the library's objects, and Metal's
 * objects it copies from and into, kept with it -- a texture of ours copied from is copied in first (and read in
 * place, counted in use), one copied into is drawn (copied back, or pending for the display) */
void rwm_blit_record(struct rwm_work *work, const struct rwm_blit *blit, void *from, struct rwm_texture *from_core,
		     void *to, struct rwm_texture *to_core);

/* a draw as Metal's calls give it: 64-bit numbers, which must fit the library's 32 */
struct rwm_draw_call {
	unsigned long primitive, start, count;
	void *index_object;             /* NULL: not indexed */
	struct rwa_buffer *index_buffer;
	unsigned long index_type, index_offset;
	long base_vertex;
	unsigned long instances, base_instance;
};

/* the draw recorded into `work` (zeroed), the pass's load actions with it; false when it is dropped, said why */
bool rwm_render_draw(struct rwm_render_state *state, const struct rwm_draw_call *call, struct rwm_work *work);
/* the pass's end: true with a clear in `work` (zeroed) when the pass clears and drew nothing */
bool rwm_render_end(struct rwm_render_state *state, struct rwm_work *work);

/*
 * A command buffer's surfaces (src/metal/surfaces.cpp): those it reads in place, counted in use until the GPU has
 * read them (32 at most, the rest said once), and those it draws into in place, pending for the display from -commit
 * (8 at most) and given the buffer's last GPU job when its work has been sent off.
 */
struct rwm_sampled {
	IOSurfaceRef at[32];
	unsigned count;
};
struct rwm_drawn {
	uint32_t at[8];
	unsigned count;
};
void rwm_sampled_add(struct rwm_sampled *sampled, uint32_t surface);
void rwm_sampled_release(struct rwm_sampled *sampled);
void rwm_drawn_add(struct rwm_drawn *drawn, uint32_t surface);
void rwm_drawn_done(const struct rwm_drawn *drawn, uint64_t seq);

/* at -commit, over a buffer's works: the surfaces they read counted in use, those they draw into pending for the
 * display, and the surfaces they sample copied in (the plug-in's -redwoodCopyInNow, counted in rwm_perf) */
void rwm_works_sampled(struct rwm_work *const *works, unsigned count, struct rwm_sampled *sampled);
void rwm_works_drawn(struct rwm_work *const *works, unsigned count, struct rwm_drawn *drawn);
void rwm_works_copy_in(struct rwm_work *const *works, unsigned count);
void rwm_copy_in(struct rwm_texture *texture);

/*
 * The driver's own thread (src/metal/worker.cpp; boot-arg rwm_async=1): rwm_worker_submit queues a job -- waiting while
 * rwm_queue_depth() are already waiting -- and starts the thread the first time; rwm_worker_drain waits for every job
 * queued before it (a texture touched at once); a job says it is done with rwm_worker_mark on a flag of its own, which
 * rwm_worker_wait waits for. rwm_bind_big and the "tl: w" trace lines as boot-args bigcore=1 and mali_trace=1 ask.
 */
typedef void (*rwm_job_fn)(void *context);
bool rwm_worker_on(void);
void rwm_worker_submit(rwm_job_fn run, void *context);
void rwm_worker_drain(const char *what);
void rwm_worker_mark(volatile bool *flag);
void rwm_worker_wait(const volatile bool *flag);
unsigned rwm_queue_depth(void);
void rwm_bind_big(int cpu);
bool rwm_trace(void);
void rwm_trace_line(const char *what, void *object);

/*
 * A command buffer's work, in Metal's order: copies in (copy_in: on this thread now -- with the driver's worker they
 * were made at -commit), then each command; a copy the library refuses is skipped and said, a draw whose shader reads
 * a buffer nobody bound is skipped and the frame kept (QuartzCore's specialized uber shader keeps its configuration
 * buffer among its arguments), any other draw that fails ends the buffer (RWM_RUN_DRAW_FAILED, the reason in `why`),
 * and so does a dispatch (RWM_RUN_DISPATCH_FAILED); then the work is sent off (rwm_submit: waited for when a target
 * is copied back or `finish`) and what was drawn into a surface copied back. *waited_ns is the time spent at the end.
 */
enum {
	RWM_RUN_DRAW_FAILED = -1,
	RWM_RUN_DISPATCH_FAILED = -2,
};

int rwm_run_works(struct rwa_device *device, struct rwm_work *const *works, unsigned count, bool copy_in, bool finish,
		  uint64_t *seq, uint64_t *waited_ns, char *why, size_t why_size);

/*
 * A command buffer (src/metal/cmdbuf.cpp): Metal's status numbers (MTLCommandBufferStatus -- until 21 October this
 * driver answered 3, 4 and 6, see the note above RedwoodMetalCommandBuffer), when it was made, its last GPU job, whether
 * the driver's worker has it and has done it, and its surfaces. The plug-in keeps its works alive, its NSError, its
 * handlers and the queue's slot.
 */
enum {
	RWM_CB_NOT_ENQUEUED = 0,
	RWM_CB_ENQUEUED = 1,
	RWM_CB_COMMITTED = 2,
	RWM_CB_SCHEDULED = 3,
	RWM_CB_COMPLETED = 4,
	RWM_CB_ERROR = 5,
};

struct rwm_command {
	unsigned status;
	uint64_t born, born_cpu;        /* when it was made, and the making thread's processor time then */
	uint64_t seq;                   /* its last GPU job, when it was sent off without a wait */
	volatile bool queued, processed;   /* handed to the driver's worker at -commit, and done there */
	struct rwm_drawn drawn;         /* the surfaces it draws into in place, pending meanwhile */
	struct rwm_sampled sampled;     /* the surfaces it reads in place, in use until the GPU is done */
};

void rwm_command_begin(struct rwm_command *command);
/* -commit, on the thread that makes the buffer: the making counted, that thread put on a big core once (bigcore=1),
 * the surfaces read in place counted in use; with the driver's worker (rwm_async=1) -- then true, and the plug-in
 * hands the buffer over -- also the copies in made now, the first buffer that draws into each surface said (`label`:
 * its pipeline's, NULL for "?"), and the surfaces drawn into marked pending */
bool rwm_command_commit(struct rwm_command *command, struct rwm_work *const *works, unsigned count,
			const char *(*label)(void *pipeline));
/* the work run (rwm_run_works) with its accounting: 0, the buffer scheduled; RWM_RUN_* with the reason, in error */
int rwm_command_run(struct rwm_command *command, struct rwa_device *device, struct rwm_work *const *works,
		    unsigned count, char *why, size_t why_size);
/* on the worker after the run: the surfaces given the buffer's last job, the buffer done */
void rwm_command_processed(struct rwm_command *command);
/* until the worker has done it (not on the worker, nor for a buffer it never had) */
void rwm_command_wait_processed(struct rwm_command *command);
/* -waitUntilScheduled: the worker is not waited for (boot-arg rwm_sched_wait=1: it is), a buffer it never had is */
void rwm_command_wait_scheduled(struct rwm_command *command);
/* -waitUntilCompleted: its own last job, or the whole GPU; 0, or -1 with the reason when the GPU did not finish */
int rwm_command_wait_completed(struct rwm_command *command, struct rwa_device *device, char *why, size_t why_size);
/* boot-arg rwm_copyin_late=1: the surfaces copied in on the driver's worker, as until 27 October */
bool rwm_copyin_late(void);
/* fn(context) on a thread of 16 MB, waited for: the compiler library's recursion (src/metal/worker.cpp) */
void rwm_on_big_stack(void (*fn)(void *context), void *context);

/*
 * A texture over an IOSurface (src/metal/import.cpp): in place when the plug-in's import can lay one over the
 * surface's memory, else copied in (two-plane YCbCr packed into RGBA8; one plane of 80, 81, 70, 30, 10) or
 * copied back (BGRA8 drawn into). `in_place` is the plug-in's import (src/metal/drawable.cpp); it fills `took`
 * with the surface it laid the texture over. NULL with the reason when no road serves; `road` says which did.
 */
enum {
	RWM_IMPORT_NONE,
	RWM_IMPORT_IN_PLACE,
	RWM_IMPORT_COPY_IN,
	RWM_IMPORT_COPY_BACK,
};

struct rwm_import {
	unsigned kind;
	uint32_t surface, width, height, row_bytes;   /* in place: the surface the texture lies over */
};

typedef struct rwa_texture *(*rwm_in_place_fn)(void *context, struct rwa_device *device, IOSurfaceRef surface,
					       unsigned plane, const struct rwa_texture_shape *shape,
					       struct rwm_import *took, char *why, size_t why_size);

/* Apple's extended-range 550-555 taken as BGRA8Unorm (80) for this 8-bit panel, said once; others as they are */
unsigned rwm_known_format(unsigned format);
struct rwm_texture *rwm_surface_import(struct rwa_device *device, IOSurfaceRef surface, unsigned plane,
				       const struct rwa_texture_shape *shape, rwm_in_place_fn in_place, void *context,
				       struct rwm_import *road, char *why, size_t why_size);

/*
 * An argument encoder, a recorder (src/metal/argenc.cpp): the set's layout, the destination -setArgumentBuffer:offset:
 * named (given and not the library's, or at an offset the encoder's alignment does not divide: refused, said, false),
 * whether an id may be recorded (said once for a missing destination, and for an id past the set's members), the
 * three records, and a value member's place in the buffer.
 */
struct rwm_argenc;
/* the layout of a set the application describes (-newArgumentEncoderWithArguments:: each member's index, MTLDataType
 * and array length, Metal's numbers), or of the set a function's module declares at `index` (its AIR and name); false
 * when it cannot be served or there is none, said why */
struct rwm_argument {
	unsigned long index, data_type, array_length;
};
bool rwm_argenc_layout_of(const struct rwm_argument *arguments, unsigned long count, struct rwa_argbuf_layout *layout);
bool rwm_argenc_layout_for(struct rwa_device *device, const void *air, size_t air_size, const char *name,
			   unsigned long index, struct rwa_argbuf_layout *layout);
struct rwm_argenc *rwm_argenc_new(const struct rwa_argbuf_layout *layout, bool from_module);
void rwm_argenc_free(struct rwm_argenc *encoder);
const struct rwa_argbuf_layout *rwm_argenc_layout(const struct rwm_argenc *encoder);
bool rwm_argenc_set_destination(struct rwm_argenc *encoder, bool given, struct rwa_buffer *buffer, void *contents,
				size_t offset);
bool rwm_argenc_ready(struct rwm_argenc *encoder, size_t index, const char *what);
void rwm_argenc_encode_buffer(struct rwm_argenc *encoder, size_t index, struct rwa_buffer *buffer, size_t offset);
void rwm_argenc_encode_texture(struct rwm_argenc *encoder, size_t index, struct rwa_texture *texture);
void rwm_argenc_encode_sampler(struct rwm_argenc *encoder, size_t index, struct rwa_sampler *sampler);
void *rwm_argenc_constant_data(struct rwm_argenc *encoder, size_t index);

/*
 * What a render pipeline asks for (src/metal/shape.cpp): each colour attachment's blend state as Metal's numbers,
 * deduplicated on content, the label of the first pipeline of a configuration, the plain fill's vertex layout once
 * and the attributes configured -- said before the library can refuse any of it; and what the library made of it.
 */
void rwm_shape_say(const char *label, const struct rwa_render_shape *shape, bool has_vertex_descriptor);
void rwm_blend_result(unsigned enabled_mask, unsigned write_masks);

/*
 * A render pipeline (src/metal/pipeline.cpp) from the shape the plug-in read out of Metal's descriptor. Prepared: what
 * it asks for said, CoreAnimation's in-shader blends given source-over, and one the warm-up built taken -- true, with
 * its render and info. Else compiled: the recipe recorded (CoreAnimation's), the modules built one at a time on a big
 * stack with the functions' constants; the render, or NULL with the library's reason (said the first, second, fourth
 * ... time). Modules: AIR, NULL for a vertex stage the library generates; names: for the recipe, NULL when the
 * modules are the pipeline library's.
 */
struct rwm_render_request {
	const void *vertex, *fragment;
	size_t vertex_size, fragment_size;
	const char *vertex_name, *fragment_name;
	const struct rwa_constant *vertex_constants, *fragment_constants;
	unsigned vertex_constant_count, fragment_constant_count;
};

bool rwm_render_prepare(const char *label, struct rwa_render_shape *shape, bool has_vertex_descriptor,
			struct rwa_render **render, struct rwa_render_info *info);
struct rwa_render *rwm_render_compile(struct rwa_device *device, const char *label,
				      const struct rwm_render_request *request, struct rwa_render_shape *shape,
				      struct rwa_render_info *info, char *why, size_t why_size);

/*
 * QuartzCore's pipeline library (src/metal/plib.cpp): the firmware's two specialized modules (AIR bitcode, in the
 * mapped file, never unmapped) for a pipeline of CoreAnimation's -- by its label, by its shader part, and with
 * allow_near (only once the pipeline's own shader has failed to build) the nearest one with its blend mode lost,
 * then the one whose label shares the longest beginning with it.
 */
bool rwm_plib_modules_for(const char *label, bool allow_near, const void **vertex, size_t *vertex_size,
			  const void **fragment, size_t *fragment_size);

/*
 * Pipeline recipes (src/metal/recipes.cpp). Builds go one at a time under the compile lock:
 * rwa_render_constants is global state in the library, and the warm-up thread builds too.
 */
void rwm_compile_lock(void);
void rwm_compile_unlock(void);
/* CoreAnimation's draw.PBGRAXm_ pipelines given the fixed function's premultiplied source-over (the Mali reads
 * [[color(0)]] as 0); boot-arg rwm_xm_blend=0 leaves them */
void rwm_xm_blend_apply(const char *label, struct rwa_render_shape *shape);
/* what this process built, once per label, at once to kern.redwood.host_keep as "rwr PROCESS N K/M text";
 * vertex and fragment: the functions' names for one whose shaders are not the pipeline library's, else NULL */
void rwm_recipe_record(const char *label, const struct rwa_render_shape *shape, const char *vertex,
		       const char *fragment, const struct rwa_constant *vc, unsigned nv, const struct rwa_constant *fc,
		       unsigned nf);
void rwm_recipes_say(void);       /* all of them to the log (the com.redwood.rwm_recipes notification) */
void rwm_recipes_say_new(void);   /* those built since the last time (once a minute) */
/* the warm-up: this process's recipes read from the first of `files` that opens; how many */
unsigned rwm_warm_load(const char *const *files, unsigned file_count);
struct rwm_warm_item {
	const char *label;
	const char *vertex, *fragment;   /* "" when the pipeline library has the shaders */
};
bool rwm_warm_item(unsigned index, struct rwm_warm_item *item);
/* the recipe built from the modules the caller found (found: it found them), unless taken meanwhile */
void rwm_warm_build(unsigned index, struct rwa_device *device, const void *vertex, size_t vertex_size,
		    const void *fragment, size_t fragment_size, bool found);
void rwm_warm_report(uint64_t elapsed_ns);
/* a built recipe for this label and shape, if one is ready: its render and info, and it is the caller's */
bool rwm_warm_take(const char *label, const struct rwa_render_shape *shape, struct rwa_render **render,
		   struct rwa_render_info *info);

#ifdef __cplusplus
}
#endif

#endif
