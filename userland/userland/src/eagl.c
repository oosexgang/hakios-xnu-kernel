/*
 * libredwood-gl.dylib: an OpenGL ES implementation behind iOS's EAGLContext, inside the window
 * server (backboardd; libredwood.dylib loads this when the image has it: build.sh GL=1).
 *
 * CoreAnimation's render server draws with Metal, or else with OpenGL ES, or else with its own
 * software rasterizer (CA::WindowServer::EAGLServer::renderer()). For OpenGL ES it makes an
 * EAGLContext of API 3 and asks six things of it, all of which are replaced here:
 *
 *   -initWithAPI:properties:   ours makes a context of the OpenGL ES library behind us
 *   -API                       3
 *   -setParameter:to:          noted
 *   -getMacroContextPrivate    a table of functions, called as entry(table[0], arguments...) at
 *                              the offsets of iOS's own OpenGLES (src/eagl_slots.txt); ours is
 *                              made by mkeagl.py: each entry counts the call and passes it on,
 *                              except the ones written by hand below (my_gl...)
 *   -texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:
 *                              the texture bound to `target` is to show what the IOSurface holds
 *   -attachImage:toCoreSurface:invertedRender:
 *                              the texture bound to the target ("image") is to BE the IOSurface:
 *                              what is drawn into it (it becomes a framebuffer's colour
 *                              attachment) is the screen; or it is a picture to be shown
 *
 * A GPU's driver does the last two by using the IOSurface's memory as the texture's, and so it
 * is done here where the library can (redwood_gl_surface_texture(); surface_shared()): nothing
 * is copied, and a frame's end waits for the GPU. Where it cannot (a software driver, a surface
 * the GPU cannot address, a picture to be turned over) the texture is an ordinary one, filled
 * from the IOSurface when that has changed (its seed) and read back into it when a frame ends
 * (glFlush, glFinish), with nothing but OpenGL ES calls. Either way this file does not know
 * what draws: the library is reached through its gl* functions and the functions of redwood_gl.h.
 *
 * Where CoreAnimation relies on iOS's library being what it is, this stands in between:
 * blue-first pixels into a texture called GL_RGBA (internal_format()), shaders that read
 * textures at coordinates in pixels (my_glShaderSource()), and draws made with no program
 * (plain_draw_begin()).
 *
 * What CoreAnimation asks of OpenGL ES is written down as it goes (the log lines "redwood-gl:"):
 * strings and limits it reads, texture formats and sizes, calls and triangles per frame. More
 * with the kernel's boot-args: gltrace=FIRST-LAST writes down every call of those frames (lines
 * "gl:"; with each draw what it is made of -- the texture, the first corners -- and after it a
 * pixel or two of the framebuffer; gltrace_lit=1: and how much of the framebuffer is not black);
 * gltrace_at=SECONDS does so for the first frame that long after the window server started,
 * gltrace_draws=N for the first frame after one of N draws, gltrace_function=N[,K] for the frame
 * after the Kth draw CoreAnimation makes of its function N with no program, gltrace_frames=N for
 * N frames from any of them on; gltrace_place=X0,Y0,X1,Y1 writes down, of those frames, only the draws whose corners
 * reach into that rectangle, a line each (lines "gl-at:"; writing everything down is slow, and a
 * window server that is slow draws other frames), gltrace_look=1 adds the pixel each of them
 * left in the middle of itself; gltrace_shaders=1 writes down the text of the shaders of every
 * program a traced draw uses, the first time it draws, gltrace_uniforms=1 what every traced draw
 * gave its program, by the names the shaders use, gltrace_corners=N how many of a draw's corners
 * to write down (twelve by default); glcensus=1 keeps count of the draws CoreAnimation makes with no
 * program and of the functions it asks for in vain (see census_draw()); glshaders=1 writes down
 * the text of every shader, glshow=TEXTURE (or WxH, with glshow_part=W,H and glshow_zoom=N) puts that texture over the corner of the picture, glplain=0 leaves the
 * draws without a program alone, glskip=N[,N...] leaves out the draws of those programs (see skip_draw()), glcopy=1 copies every IOSurface as if the library could not
 * share one, glwait=1 waits for the GPU at the end of every frame and glwait=0 never (see
 * wait_for_frames), glfetch=0 keeps EXT_shader_framebuffer_fetch from CoreAnimation (see
 * fetch_offered), glgroup=0 leaves vibrant layers inside groups as they were (see group_draw_begin()), glprecision=0 leaves the shaders' "lowp" and "mediump" as they are (see all_highp), glenv=NAME=VALUE puts that into the environment for the library to find
 * (its own switches: FD_MESA_DEBUG=sysmem, say), vblur=0 blurs nothing whose radius varies and
 * vblur=2 paints such blurs by their masks instead (see vblur_on), glframes=1 writes a line for
 * every frame (see frame_end()).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "redwood_gl.h"

/* ---- OpenGL ES: the types, the few names used here, the library's functions --------------------- */

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei, GLfixed, GLclampx;
typedef unsigned char GLboolean, GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort, GLhalf;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef char GLchar;
typedef void GLvoid;
typedef long GLintptr, GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
typedef GLDEBUGPROC GLDEBUGPROCKHR;

#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006
#define GL_UNPACK_ROW_LENGTH            0x0cf2
#define GL_UNPACK_ALIGNMENT             0x0cf5
#define GL_PACK_ROW_LENGTH              0x0d02
#define GL_PACK_ALIGNMENT               0x0d05
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_EXTENSIONS                   0x1f03
#define GL_TEXTURE_BINDING_2D           0x8069
#define GL_BGRA                         0x80e1
#define GL_INFO_LOG_LENGTH              0x8b84
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_TEXTURE                      0x1702
#define GL_READ_FRAMEBUFFER             0x8ca8
#define GL_DRAW_FRAMEBUFFER             0x8ca9
#define GL_READ_FRAMEBUFFER_BINDING     0x8caa
#define GL_DRAW_FRAMEBUFFER_BINDING     0x8ca6
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_NEAREST                      0x2600
#define GL_LINES                        0x0001
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8cd0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8cd1
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_FRAMEBUFFER                  0x8d40
#define GL_SYNC_GPU_COMMANDS_COMPLETE   0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT      0x00000001

#define VBLUR_LEVELS    8               /* levels of a blur whose radius varies, the picture itself included */
#define VBLUR_CHAINS    6               /* ... such blurs kept track of at a time */

struct eagl;
static int eagl_tracing;                /* this frame's calls are written down (boot-arg gltrace=FIRST-LAST) */
static int frame_lines;                 /* boot-arg glframes=1: a line for every frame (see frame_end()) */
static int latency_lines;               /* boot-arg touch_lat=1: when every frame began and ended (see frame_end()) */
static uint64_t slow_calls_us;          /* boot-arg glslow=US: a line for every call that takes longer (eagl_leave()) */
static uint64_t call_began_us;          /* (when the call in progress began) */
static uint64_t now_us(void);
static void eagl_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void eagl_trace_floats(const float *values, int count);
static void eagl_enter(struct eagl *e, unsigned offset);
static void eagl_leave(struct eagl *e, unsigned offset);
static long eagl_missing(void *context, unsigned offset);
#define EAGL_ENTER(context, offset)     eagl_enter(context, offset)
#define EAGL_LEAVE(context, offset)     eagl_leave(context, offset)

#define EAGL_GEN_DECLARATIONS
#include "eagl_gen.h"
#undef EAGL_GEN_DECLARATIONS

static void
say(const char *format, ...)
{
	char line[1024];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

static void
eagl_trace(const char *format, ...)
{
	char line[512] = "gl: ";
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line + 4, sizeof(line) - 4, format, ap) + 4;
	va_end(ap);
	write(2, line, n > (int)sizeof(line) - 1 ? sizeof(line) - 1 : (size_t)n);
}

static void
eagl_trace_floats(const float *values, int count)
{
	char line[512] = "gl:    ";
	int n = 7;

	for (int i = 0; i < count && i < 16 && n < 480; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %g", (double)values[i]);
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

/* ---- a context -------------------------------------------------------------------------------- */

#define SLOTS           0x440           /* entries of the table: more than iOS's has (checked below) */

/* (a vibrant layer inside a group: see group_draw_begin()) */
#define GROUP_TEXTURES  8192
enum { GROUP_NONE, GROUP_FRESH, GROUP_VIBRANT };
struct group_texture {
	uint8_t state;                  /* GROUP_FRESH: cleared, nothing drawn in it since; GROUP_VIBRANT: a vibrant layer's coverage */
	GLfloat colours[20];            /* ... and that layer's colour matrix */
};
static int group_vibrant_on = 1;        /* boot-arg glgroup=0 */
static unsigned group_covered, group_laid, group_flattened;    /* (glframes=1) this frame's */

/* A texture that stands for an IOSurface. */
struct image {
	GLuint texture;
	IOSurfaceRef surface;
	bool target;                    /* drawn into (attachImage) rather than shown (texImageIOSurface) */
	bool invert, drawn;
	bool shared;                    /* the texture is the surface's memory: nothing to copy */
	GLuint mirror;                  /* a texture that is the surface's memory, which the GPU copies the frame into (blit_targets) */
	bool wrapped;                   /* the surface is memory of somebody's with a surface made around it */
	bool alpha_first;               /* its pixels are opacity, blue, green, red in memory: see my_attachImage() */
	bool painted;                   /* something has been drawn into it (it is a target in fact, not only in name) */
	unsigned looked;                /* ... the frame in which that memory was last looked at, */
	uint64_t sum;                   /* ... and what it added up to */
	GLenum to, internal, format, type;
	uint32_t plane, seed;
	GLsizei width, height;
};

struct eagl {
	void *table[SLOTS];             /* [0] is the context itself: what every entry gets as its first argument */
	id object;                      /* the EAGLContext */
	struct redwood_gl *gl;
	unsigned long api;
	struct image images[256];
	unsigned image_count;
	GLuint framebuffer;             /* the one bound */
	GLuint colour;                  /* ... and the texture that is its colour attachment (0: none, or no texture) */
	GLuint reader;                  /* a framebuffer of ours, to read a texture back */
	GLuint writer;                  /* ... and one to copy a frame into its surface with (blit_targets) */
	GLuint program;                 /* the one in use, as CoreAnimation has it */
	GLuint plain, lookup;           /* programs of ours for when that is none (plain_draw_begin()) */
	GLint plain_matrix, plain_textured, lookup_matrix;
	GLuint vibrant, masking;        /* two more: what a colour matrix does to what is behind, and to the layer */
	int blended;                    /* blending is off for such a draw, to be put back after it */
	bool no_vibrant;                /* the first of the two could not be made */
	GLint vibrant_matrix, vibrant_colours, masking_matrix, masking_colours;
	GLbitfield unclear;             /* a glClear put off until the next draw (clear_now()), and what it was to be made with */
	GLint unclear_box[4];
	GLfloat unclear_colour[4];
	GLboolean unclear_boxed;
	struct group_texture *groups;   /* CoreAnimation's textures: which are groups with a vibrant layer in them (group_draw_begin()) */
	GLuint group_cover, group_over, group_flat;     /* ... and our programs for them */
	bool group_failed;
	GLint group_cover_matrix, group_cover_texmat, group_over_matrix, group_over_texmat, group_over_colours;
	GLint group_flat_matrix, group_flat_colour;
	GLboolean group_blend;          /* the blending a draw of those had, to be put back after it */
	GLint group_blend_func[4];
	GLuint box;                     /* and one that makes a picture eight times smaller */
	GLint box_matrix, box_places;
	GLuint blur4;                   /* and for the four samples of a shadow's blur (blur4_fragment) */
	GLint blur4_matrix, blur4_places;
	GLuint halve, vblur;            /* and two for the blur whose radius varies (vblur_level(), vblur_draw_begin()) */
	GLint halve_texel, vblur_matrix, vblur_radius, vblur_levels, vblur_normalize, vblur_paint;
	struct vblur_chain {
		GLuint picture;         /* CoreAnimation's texture, the first level (0: unused) */
		GLsizei width, height;
		GLuint level[VBLUR_LEVELS], framebuffer[VBLUR_LEVELS];  /* ours, from the second on (index 1 up) */
		GLsizei level_width[VBLUR_LEVELS], level_height[VBLUR_LEVELS];
		int made;               /* how many levels are made of the present picture, the first included */
		unsigned used;          /* the frame it was last used in */
	} chains[VBLUR_CHAINS];
	bool vblur_bound;               /* a draw of ours has textures of its own on units 2 to 5 */
	unsigned unit;                  /* the texture unit being spoken of */
	GLuint bound[8];                /* the 2D texture bound to each of the first units */
	const GLfloat *matrix;          /* where CoreAnimation keeps the matrix it gives its programs */
	struct {
		const void *pointer;
		GLsizei stride;
		GLint size;
		GLenum type;
	} arrays[8];                    /* the vertex arrays, as given */
	bool in_frame;
	struct {                        /* what the line a frame gets with glframes=1 says (frame_end()) */
		bool taken, boxed;      /* the first draw or clear into the display's picture seen; scissored */
		GLint box[4];
		uint32_t surface;       /* that picture's IOSurface */
		unsigned vrd, vrb, into_display;
		unsigned solid_spans, blended_spans;    /* line draws into it with blending off and on (glyphs' spans) */
		unsigned solid_ends, blended_ends;      /* ... and their lines' ends */
		unsigned other_spans, other_ends;       /* line draws into other targets */
	} line;
	/* a frame's worth */
	unsigned calls, draws, triangles, uploads, clears, last_draws;
	uint64_t upload_bytes, began, attaching, flushing;
	/* everything so far */
	unsigned frames, shaders, programs, errors, shared, refused;
	uint64_t all_calls, all_draws, all_triangles, readback_us, wait_us, attach_us, flush_us, frame_us;
	unsigned most_calls, most_triangles;
	const char *sources[256];       /* the text of the shaders being made, by name (modulo this) */
	bool fragment[256];             /* ... and which of the two kinds each is */
	unsigned plain_said;
};

static unsigned trace_first, trace_last;        /* the frames whose calls are written down */
static uint64_t trace_at;                       /* ... or the first frame from this time on (now_us()) */
static unsigned trace_draws;                    /* ... or the first frame after one of so many draws */
static unsigned trace_frames = 1, trace_left;   /* ... and how many frames from there on */
static int trace_lit;                           /* boot-arg gltrace_lit=1: count the lit pixels after every traced draw (slow) */
static int trace_function = -1, trace_function_after = 1;       /* boot-arg gltrace_function=N[,K]: the frames after the Kth draw ... */
static int trace_function_met;                                  /* ... CoreAnimation makes of its function N with no program */
static float trace_place[4];                    /* boot-arg gltrace_place=X0,Y0,X1,Y1: of those frames, only the draws that touch this */
static int trace_placed, trace_placing;         /* ... is set; ... and this frame is one of them */
static int trace_look;                          /* boot-arg gltrace_look=1: ... and what each of them left in the middle of itself */
static int trace_shaders;                       /* boot-arg gltrace_shaders=1: the text of the shaders of every program that draws */
static int trace_uniforms_on;                   /* boot-arg gltrace_uniforms=1: and what every traced draw gave its program */
static GLsizei trace_corners = 12;              /* boot-arg gltrace_corners=N: how many corners of a traced draw to write down */
static float trace_middle[2];                   /* ... which is here, as CoreAnimation counts */
static int trace_listed;                        /* the draw at hand is one of them */
static int all_shaders;                         /* boot-arg glshaders=1: the text of every shader, not of the first two */
static struct group_texture *group_of(struct eagl *e, GLuint texture);
static __thread struct eagl *current;
static struct eagl *contexts[8];
static uint32_t slot_calls[SLOTS];
static int check_errors = 1;            /* ask the library for an error after every call (the first frames) */
static int share_surfaces = 1;          /* boot-arg glcopy=1: 0 */
/*
 * A frame is not drawn into its surface's memory but into a texture of the library's own, and the
 * GPU copies it into the surface at the frame's end (one blit; the display's driver waits for that
 * submission, as it waited for the frame's last draw). Drawn into the surface's memory directly,
 * frames came out with parts of them missing, every other frame for a while -- Control Center's
 * modules, the home screen's icons -- where CoreAnimation reads the picture it is drawing to draw
 * over it (EXT_shader_framebuffer_fetch, the colour matrix over what is behind: our vibrant program);
 * a texture of the library's own does not show that, as a copy by the processor did not (glcopy=1).
 * Boot-arg glblit=0: the surface's memory, as before.
 */
static int blit_targets = 1;
/*
 * A frame that is handed to the display must have been drawn. The display's driver waits for
 * the GPU itself before it puts a surface on the panel, on a thread of its own, and says so in
 * the registry ("RedwoodWaitsForGPU"); the window server is then at its next frame meanwhile.
 * With a driver that does not, the frame is waited for here (glFinish). Boot-arg glwait=1:
 * here in any case; glwait=0: nowhere, to see what the waiting costs (and frames half drawn).
 */
static int wait_for_frames = 1;
static int sync_frames = 1;             /* boot-arg glsync=0 (a test): a frame's end does not push what Mesa holds back (frame_end()) */
/*
 * EXT_shader_framebuffer_fetch -- a fragment shader reads the pixel it is about to write -- is
 * what CoreAnimation draws a number of things with where the library offers it: the shadows
 * under the home screen's labels, when they are of the kind that darkens only what is light,
 * are quads that read the picture and write it back, and a colour matrix over what is there
 * (the Search field's text) is one draw instead of a copy and a draw. Without the extension
 * CoreAnimation does the same with what OpenGL ES always has, at four times the submissions
 * for a home screen. (Mesa's driver for this GPU drew the first triangle of each such quad
 * with pixels of another tile, fragments of other icons under the labels, until it was given a
 * flush before the first draw that reads the picture after one that does not:
 * tools/mesa/freedreno-darwin.patch, fd6_draw.cc.) Boot-arg glfetch=0 keeps the extension from
 * CoreAnimation, to see the difference.
 */
static int fetch_offered = 1;

#define GL_SCISSOR_TEST                 0x0c11
#define GL_SCISSOR_BOX                  0x0c10
#define GL_COLOR_CLEAR_VALUE            0x0c22

static const char *eagl_slot_name(unsigned offset);
static void census_report(void);
static void clear_now(struct eagl *e);

static void
eagl_enter(struct eagl *e, unsigned offset)
{
	if (current != e) {
		if (!redwood_gl_make_current(e->gl)) {
			say("redwood-gl: the context cannot be made this thread's\n");
		}
		current = e;
	}
	slot_calls[offset / 8]++;
	e->calls++;
	if (slow_calls_us != 0) {
		call_began_us = now_us();
	}
}

static void
eagl_leave(struct eagl *e, unsigned offset)
{
	GLenum error;

	if (check_errors && offset != 0x338 && (error = glGetError()) != 0) {
		if (e->errors++ < 60) {
			say("redwood-gl: %s: error %#x\n", eagl_slot_name(offset), error);
		}
	}
	if (slow_calls_us != 0) {
		uint64_t now = now_us();

		/* "tl: slow NAME US at US, frame N": a call that took long, and when it ended (tools/frames.py) */
		if (now - call_began_us >= slow_calls_us) {
			say("tl: slow %s %llu at %llu, frame %u%s\n", eagl_slot_name(offset), now - call_began_us, now, e->frames + 1,
			    e->in_frame ? "" : " (before its first draw)");
		}
	}
}

/* An entry the library has no function for: say so, once an entry. */
static long
eagl_missing(void *context, unsigned offset)
{
	struct eagl *e = context;

	eagl_enter(e, offset);
	if (slot_calls[offset / 8] == 1) {
		say("redwood-gl: CALLED AND NOT THERE: entry %#x, %s\n", offset, eagl_slot_name(offset));
	}
	return 0;
}

static struct eagl *
context_of(id object)
{
	for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
		if (contexts[i] != NULL && contexts[i]->object == object) {
			return contexts[i];
		}
	}
	return NULL;
}

/* ---- what CoreAnimation asks for: kept for the summary ------------------------------------------- */

struct format_use {
	GLenum internal, format, type;
	unsigned count, widest, tallest;
	const char *how;
};
static struct format_use formats[48];
static unsigned widest_texture, tallest_texture;

static void
note_format(const char *how, GLenum internal, GLenum format, GLenum type, GLsizei width, GLsizei height)
{
	struct format_use *use = NULL;

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (formats[i].count == 0 || (formats[i].how == how && formats[i].internal == internal &&
		    formats[i].format == format && formats[i].type == type)) {
			use = &formats[i];
			break;
		}
	}
	if ((unsigned)width > widest_texture) {
		widest_texture = (unsigned)width;
	}
	if ((unsigned)height > tallest_texture) {
		tallest_texture = (unsigned)height;
	}
	if (use == NULL) {
		return;
	}
	if (use->count++ == 0) {
		use->how = how;
		use->internal = internal;
		use->format = format;
		use->type = type;
		say("redwood-gl: a new kind of texture: %s, internal format %#x, format %#x, type %#x, %d x %d\n", how, internal,
		    format, type, width, height);
	}
	if ((unsigned)width > use->widest) {
		use->widest = (unsigned)width;
	}
	if ((unsigned)height > use->tallest) {
		use->tallest = (unsigned)height;
	}
}

static void
summary(struct eagl *e)
{
	unsigned frames = e->frames ? e->frames : 1;

	say("redwood-gl: after %u frames: %llu calls, %llu draws and %llu triangles a frame on average; the most in one: "
	    "%u calls, %u triangles; %llu us a frame, of which %llu handing the commands on, %llu waiting for the picture "
	    "to be drawn and %llu copying it out, and before each %llu making textures of IOSurfaces; %u IOSurfaces were "
	    "the textures' memory, %u had to be copied; %u shaders, %u programs; biggest texture %u x %u; %u errors\n",
	    e->frames, e->all_calls / frames, e->all_draws / frames, e->all_triangles / frames, e->most_calls,
	    e->most_triangles, e->frame_us / frames, e->flush_us / frames, e->wait_us / frames, e->readback_us / frames,
	    e->attach_us / frames, e->shared, e->refused, e->shaders, e->programs, widest_texture, tallest_texture, e->errors);
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]) && formats[i].count; i++) {
		say("redwood-gl:   textures: %s %#x/%#x/%#x: %u times, up to %u x %u\n", formats[i].how, formats[i].internal,
		    formats[i].format, formats[i].type, formats[i].count, formats[i].widest, formats[i].tallest);
	}
	for (unsigned offset = 8; offset < SLOTS * 8; offset += 8) {
		if (slot_calls[offset / 8]) {
			say("redwood-gl:   %-28s %u\n", eagl_slot_name(offset), slot_calls[offset / 8]);
		}
	}
}

/* ---- IOSurfaces ----------------------------------------------------------------------------------- */

static struct image *
image_of(struct eagl *e, GLuint texture)
{
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].texture == texture) {
			return &e->images[i];
		}
	}
	return NULL;
}

static struct image *
image_new(struct eagl *e, GLuint texture, IOSurfaceRef surface)
{
	struct image *image = image_of(e, texture);

	if (image == NULL) {
		if (e->image_count == sizeof(e->images) / sizeof(e->images[0])) {
			say("redwood-gl: more textures with IOSurfaces than there is room for\n");
			return NULL;
		}
		image = &e->images[e->image_count++];
	} else {
		if (image->mirror != 0) {
			glDeleteTextures(1, &image->mirror);
		}
		CFRelease(image->surface);
	}
	memset(image, 0, sizeof(*image));
	image->texture = texture;
	image->surface = (IOSurfaceRef)CFRetain(surface);
	return image;
}

static void
image_forget(struct eagl *e, GLuint texture)
{
	struct image *image = image_of(e, texture);

	if (image != NULL) {
		if (image->mirror != 0) {
			glDeleteTextures(1, &image->mirror);
		}
		CFRelease(image->surface);
		*image = e->images[--e->image_count];
	}
}

/* Apple's OpenGL ES takes blue-first pixels into a texture called GL_RGBA; everybody else's calls the texture so too. */
static GLenum
internal_format(GLenum internal, GLenum format)
{
	return format == GL_BGRA && internal == GL_RGBA ? GL_BGRA : internal;
}

static unsigned
bytes_a_pixel(GLenum format, GLenum type)
{
	unsigned parts = format == GL_RGBA || format == GL_BGRA ? 4 : format == 0x1907 ? 3 :        /* RGB */
	    format == 0x8227 || format == 0x190a ? 2 : 1;                                           /* RG, luminance + alpha */

	switch (type) {
	case GL_UNSIGNED_BYTE:
		return parts;
	case 0x140b: case 0x8d61: case 0x1403:          /* half floats, 16 bits */
		return 2 * parts;
	case 0x1406:                                    /* floats */
		return 4 * parts;
	case 0x8368:                                    /* 2:10:10:10 in a word */
		return 4;
	default:
		return 2;                               /* the packed 16-bit kinds */
	}
}

/*
 * For a surface drawn into (blit_targets): a second texture that is the surface's memory, for the
 * frame to be copied into by the GPU; the texture CoreAnimation draws into stays the library's own.
 * The texture that is bound is bound again after.
 */
static bool
surface_mirrored(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	GLint bound = 0;

	if (!blit_targets || !share_surfaces || !image->target || image->alpha_first ||
	    image->format != GL_BGRA || image->type != GL_UNSIGNED_BYTE || image->plane != 0 ||
	    (size_t)image->width != IOSurfaceGetWidth(surface) || (size_t)image->height != IOSurfaceGetHeight(surface)) {
		return false;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glGenTextures(1, &image->mirror);
	glBindTexture(GL_TEXTURE_2D, image->mirror);
	if (!redwood_gl_surface_texture(e->gl, IOSurfaceGetID(surface))) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
		glDeleteTextures(1, &image->mirror);
		image->mirror = 0;
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	e->shared++;
	return true;
}

/*
 * Make a texture, which is bound, of its IOSurface's own memory, if that is what is asked for
 * (the whole of a surface of blue-first pixels, the same way up) and the library can do it.
 */
static bool
surface_shared(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;

	if (!share_surfaces || image->invert || image->alpha_first || image->format != GL_BGRA || image->type != GL_UNSIGNED_BYTE ||
	    image->plane != 0 || (size_t)image->width != IOSurfaceGetWidth(surface) ||
	    (size_t)image->height != IOSurfaceGetHeight(surface) ||
	    !redwood_gl_surface_texture(e->gl, IOSurfaceGetID(surface))) {
		e->refused++;
		return false;
	}
	image->shared = true;
	e->shared++;
	return true;
}

/*
 * A surface that is shown goes on being drawn into after the texture has been made of it -- a
 * sign an application draws for one of its layers, a picture CoreAnimation has made a surface
 * around -- and not everybody who draws says so: often no seed moves, and the texture is not
 * made again. A GPU that has the memory itself shows what is there. A copy has to notice: once
 * a frame, when the texture is about to be drawn with, the memory is added up, and copied again
 * if the sum has changed.
 */
static bool
surface_changed(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t size = IOSurfaceGetBytesPerRow(surface) * (size_t)(image->height - 1) +
	    (size_t)image->width * bytes_a_pixel(image->format, image->type);
	const uint8_t *base = IOSurfaceGetBaseAddress(surface);
	uint64_t sum = size, word;

	if (image->looked == e->frames + 1 || base == NULL || IOSurfaceGetPlaneCount(surface) > 0) {
		return false;
	}
	image->looked = e->frames + 1;
	for (size_t at = 0; at + 8 <= size; at += 8) {
		memcpy(&word, base + at, 8);
		sum = (sum ^ word) * 0x9e3779b97f4a7c15ull;
		sum ^= sum >> 29;
	}
	if (sum == image->sum) {
		return false;
	}
	image->sum = sum;
	return true;
}

/* Fill a texture, which is bound, from its IOSurface, unless it is the surface. */
static void
surface_to_texture(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	size_t row = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, image->plane) : IOSurfaceGetBytesPerRow(surface);
	unsigned pixel = bytes_a_pixel(image->format, image->type);
	GLint row_length = 0, alignment = 4;
	uint8_t *base, *flipped = NULL;
	bool alpha_first = image->alpha_first && !planar && pixel == 4;

	if (image->shared) {
		return;
	}
	if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != 0) {
		say("redwood-gl: an IOSurface cannot be locked\n");
		return;
	}
	base = planar ? IOSurfaceGetBaseAddressOfPlane(surface, image->plane) : IOSurfaceGetBaseAddress(surface);
	if ((image->invert || alpha_first) && (flipped = calloc(row, (size_t)image->height)) != NULL) {
		for (GLsizei y = 0; y < image->height; y++) {
			const uint8_t *from = base + row * (size_t)(image->invert ? image->height - 1 - y : y);
			uint8_t *to = flipped + row * (size_t)y;
			size_t used = (size_t)image->width * pixel;     /* (the last row may end where its pixels do) */

			if (!alpha_first) {
				memcpy(to, from, used < row ? used : row);
				continue;
			}
			/* opacity, blue, green, red in memory: into the order OpenGL ES was told */
			for (size_t at = 0; at + 4 <= used && at + 4 <= row; at += 4) {
				to[at] = from[at + (image->format == GL_BGRA ? 1 : 3)];
				to[at + 1] = from[at + 2];
				to[at + 2] = from[at + (image->format == GL_BGRA ? 3 : 1)];
				to[at + 3] = from[at];
			}
		}
		base = flipped;
	}
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(row / pixel));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(image->to, 0, (GLint)internal_format(image->internal, image->format), image->width, image->height, 0,
	    image->format, image->type, base);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
	if (trace_placing) {
		size_t set[4] = { 0, 0, 0, 0 }, size = row * (size_t)(image->height - 1) + (size_t)image->width * pixel;

		for (size_t at = 0; at < size; at++) {
			set[at & 3] += base[at] != 0;
		}
		say("gl-at: frame %u: texture %u filled from surface %u (pixel format %#x, %d x %d, %zu bytes a row, %zu a pixel by the "
		    "surface, %u by the texture: internal format %#x, format %#x, type %#x): of %zu bytes as they go to the texture, "
		    "not zero: %zu %zu %zu %zu (first, second, third, fourth of every four)\n", e->frames + 1, image->texture,
		    IOSurfaceGetID(surface),
		    (unsigned)IOSurfaceGetPixelFormat(surface), image->width, image->height, row,
		    IOSurfaceGetBytesPerElement(surface), pixel, image->internal, image->format, image->type, size, set[0], set[1],
		    set[2], set[3]);
		if (image->height == 1 && pixel == 2 && image->width >= 16) {
			char entries[200] = "";
			size_t used = 0;

			for (int i = 0; i < 16; i++) {
				size_t at = (size_t)(i * (image->width - 1) / 15) * 2;

				used += (size_t)snprintf(entries + used, sizeof(entries) - used, " %u/%u", base[at], base[at + 1]);
			}
			say("gl-at:     a table; sixteen of its entries, first to last:%s\n", entries);
		}
	}
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	free(flipped);
	image->seed = IOSurfaceGetSeed(surface);
	e->uploads++;
	e->upload_bytes += row * (size_t)image->height;
}

/* What has been drawn into a texture, into its IOSurface, unless it is there (frame_end() waits for it). */
static void
target_to_surface(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t row = IOSurfaceGetBytesPerRow(surface);
	uint8_t *base;

	image->drawn = false;
	if (image->mirror != 0) {
		GLint read = 0, draw = 0;

		/* the frame into the surface's memory, by the GPU (blit_targets); upside down if it is drawn so */
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
		if (e->reader == 0) {
			glGenFramebuffers(1, &e->reader);
		}
		if (e->writer == 0) {
			glGenFramebuffers(1, &e->writer);
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, e->reader);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->writer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->mirror, 0);
		glBlitFramebuffer(0, 0, image->width, image->height, 0, image->invert ? image->height : 0, image->width,
		    image->invert ? 0 : image->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
		if (IOSurfaceLock(surface, 0, NULL) == 0) {
			IOSurfaceUnlock(surface, 0, NULL);
		}
		return;
	}
	if (image->shared) {
		/* whoever looks at the surface's seed is to see that it has changed */
		if (IOSurfaceLock(surface, 0, NULL) == 0) {
			IOSurfaceUnlock(surface, 0, NULL);
		}
		return;
	}
	if (e->reader == 0) {
		glGenFramebuffers(1, &e->reader);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
	if (IOSurfaceLock(surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(surface);
		glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(row / 4));
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		if (!image->invert) {
			glReadPixels(0, 0, image->width, image->height, GL_BGRA, GL_UNSIGNED_BYTE, base);
		} else {
			for (GLsizei y = 0; y < image->height; y++) {
				glReadPixels(0, y, image->width, 1, GL_BGRA, GL_UNSIGNED_BYTE, base + row * (size_t)(image->height - 1 - y));
			}
		}
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		IOSurfaceUnlock(surface, 0, NULL);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
}

/*
 * For looking at what is drawn on the way to the picture (boot-arg glshow=TEXTURE): that texture's
 * pixels are put over the top left corner of the picture as it goes out. glshow=WxH takes, instead
 * of a name that is a new one every run, whichever texture is of that size; glshow_part=W,H only
 * that much of its corner and glshow_zoom=N every pixel of it as a square of N, for a texture
 * whose few pixels in a corner are what matters (a material's backdrop, say).
 */
#define GL_TEXTURE_WIDTH        0x1000
#define GL_TEXTURE_HEIGHT       0x1001

static GLuint shown_texture;
/*
 * Boot-arg glskip=N[,N...]: the draws of those programs are left out of the picture, which says
 * what in it is their work. A program's number is the same from run to run, because CoreAnimation
 * builds its programs in the same order every time (the trace prints it: "program 111").
 */
static GLuint skipped[8];
static unsigned skipping;

static bool
skip_draw(struct eagl *e)
{
	for (unsigned i = 0; i < skipping; i++) {
		if (skipped[i] == e->program) {
			static GLuint told[8];
			static unsigned telling;

			for (unsigned k = 0; k < telling; k++) {
				if (told[k] == e->program) {
					return true;
				}
			}
			if (telling < sizeof(told) / sizeof(told[0])) {
				told[telling++] = e->program;
			}
			say("redwood-gl: the draws of program %u are left out\n", e->program);
			return true;
		}
	}
	return false;
}

static GLint shown_wanted[2];                   /* boot-arg glshow=WxH: the size to look for instead */
static GLint shown_part[2];                     /* boot-arg glshow_part=W,H: that much of its corner */
static GLint shown_zoom = 1;                    /* boot-arg glshow_zoom=N */

static GLuint shown_told;                       /* (the one written down as being shown) */

/* What sizes the textures are, when the one asked for is not among them. */
static void
say_sizes(void)
{
	GLint bound = 0;
	char line[600] = "";
	size_t used = 0;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	for (GLuint name = 1; name < 512 && used < sizeof(line) - 24; name++) {
		GLint width = 0, height = 0;

		if (!glIsTexture(name)) {
			continue;
		}
		glBindTexture(GL_TEXTURE_2D, name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %u:%dx%d", name, width, height);
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	say("redwood-gl: glshow: the textures there are:%s\n", line);
}

/* glshow=WxH: the first texture of that size there is (names are handed out afresh every run). */
static GLuint
texture_of_size(GLint want_width, GLint want_height)
{
	GLint bound = 0;
	GLuint found = 0;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	for (GLuint name = 1; name < 512 && found == 0; name++) {
		GLint width = 0, height = 0;

		if (!glIsTexture(name)) {
			continue;
		}
		glBindTexture(GL_TEXTURE_2D, name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		/* within a few pixels: the size of a backdrop's texture is a pixel or two different every run */
		if (width - want_width <= 8 && want_width - width <= 8 && height - want_height <= 8 && want_height - height <= 8) {
			found = name;
		}
	}
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	return found;
}

static void
show_texture(struct eagl *e, struct image *image)
{
	GLint width = 0, height = 0, bound = 0;
	size_t row = IOSurfaceGetBytesPerRow(image->surface);
	uint8_t *pixels, *base;
	GLuint texture = shown_texture;

	if (shown_wanted[0] > 0 && (texture = texture_of_size(shown_wanted[0], shown_wanted[1])) == 0) {
		static unsigned said;

		if (said++ == 0) {
			say("redwood-gl: glshow: there is no texture of %d x %d\n", shown_wanted[0], shown_wanted[1]);
			say_sizes();
		}
		return;
	}
	if (shown_told != texture) {
		shown_told = texture;
		say("redwood-gl: glshow: the texture %u, %d of it by %d, every pixel a square of %d\n", texture,
		    shown_part[0] > 0 ? shown_part[0] : -1, shown_part[1] > 0 ? shown_part[1] : -1, shown_zoom);
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glBindTexture(GL_TEXTURE_2D, texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	if (shown_part[0] > 0) {
		width = shown_part[0] < width ? shown_part[0] : width;
		height = shown_part[1] < height ? shown_part[1] : height;
	}
	if (width <= 0 || height <= 0 || (pixels = malloc((size_t)width * (size_t)height * 4)) == NULL) {
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	if (glGetError() == 0 && IOSurfaceLock(image->surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(image->surface);
		for (GLint y = 0; y < height * shown_zoom && y < image->height; y++) {
			const uint8_t *from = pixels + (size_t)(y / shown_zoom) * (size_t)width * 4;
			uint8_t *to = base + row * (size_t)y;

			for (GLint x = 0; x < width * shown_zoom && x < image->width; x++) {
				memcpy(to + 4 * (size_t)x, from + 4 * (size_t)(x / shown_zoom), 4);
			}
		}
		IOSurfaceUnlock(image->surface, 0, NULL);
	}
	free(pixels);
}

/* Before the first drawing of a frame: the textures whose IOSurfaces somebody has written to since. */
static void
frame_begin(struct eagl *e)
{
	GLint bound = 0;
	bool rebound = false;

	if (e->in_frame) {
		return;
	}
	e->in_frame = true;
	e->began = now_us();
	eagl_tracing = e->frames + 1 >= trace_first && e->frames + 1 <= trace_last;
	if ((trace_at != 0 && e->began >= trace_at) || (trace_draws != 0 && e->last_draws == trace_draws) || trace_function_met) {
		trace_left = trace_frames;      /* (once) */
		trace_at = 0;
		trace_draws = 0;
		trace_function_met = 0;
		trace_function = -1;
	}
	trace_placing = 0;
	if (trace_left != 0) {
		trace_left--;
		eagl_tracing = !trace_placed;
		trace_placing = trace_placed;
	} else if (eagl_tracing && trace_placed) {
		/* (gltrace=FIRST-LAST with gltrace_place: a line a draw for those frames too) */
		eagl_tracing = 0;
		trace_placing = 1;
	}
	if (eagl_tracing) {
		eagl_trace("---- frame %u (framebuffer %u)\n", e->frames + 1, e->framebuffer);
	}
	for (unsigned i = 0; i < e->image_count; i++) {
		struct image *image = &e->images[i];

		if (image->target || image->shared || IOSurfaceGetSeed(image->surface) == image->seed) {
			continue;
		}
		if (!rebound) {
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			rebound = true;
		}
		glBindTexture(image->to, image->texture);
		surface_to_texture(e, image);
	}
	if (rebound) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	}
}

/*
 * glFlush, glFinish: what was drawn must be where the display takes it from. `finished`: the
 * library has been waited for already.
 */
static void frame_functions(struct eagl *e);
static void
frame_end(struct eagl *e, bool finished)
{
	static unsigned unfollowed;
	uint64_t before, waited, after;
	unsigned copied = 0, in_place = 0;

	clear_now(e);
	before = now_us();
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn && e->images[i].mirror != 0) {
			target_to_surface(e, &e->images[i]);    /* (the GPU's copy: before what follows sends it out) */
			in_place++;
			glFlush();                              /* (also after a glFinish: the copy is new since) */
		}
		if (e->images[i].target && e->images[i].drawn && e->images[i].shared) {
			in_place++;
		}
	}
	if (in_place != 0 && !finished && wait_for_frames) {
		glFinish();             /* the display's driver reads the surface as soon as it is told of it */
	} else if (in_place != 0 && !finished && sync_frames) {
		/*
		 * The display's driver waits for the last submission that named the surface -- and Mesa's
		 * freedreno may still hold a batch back after glFlush, to merge it with the next (submit
		 * merging, for small submissions). A fence that is asked to flush and waited for not at all
		 * sends everything held back to the kernel now; the GPU is not waited for. (Taken once for
		 * the cause of frames with parts missing; that was drawing into the surface itself, see
		 * blit_targets.)
		 */
		GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

		if (sync != NULL) {
			(void)glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
			glDeleteSync(sync);
		}
	}
	waited = now_us();
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn) {
			copied += !e->images[i].shared;
			target_to_surface(e, &e->images[i]);
			if (shown_texture != 0) {
				show_texture(e, &e->images[i]);
			}
		}
	}
	if (!e->in_frame) {
		return;
	}
	after = now_us();
	e->in_frame = false;
	e->frames++;
	if (latency_lines) {
		/* "tl: g FRAME began US ended US DRAWS": microseconds of mach_absolute_time (tools/latency.py) */
		say("tl: g %u began %llu ended %llu %u\n", e->frames, e->began, after, e->draws);
	}
	if (frame_lines) {
		/*
		 * One line a frame, for matching with a screen recording (the kernel's rec_ring, whose log
		 * says when it began in the same time): when the frame ended, in the kernel's time; what was
		 * drawn; into which of the display's surfaces and, of the first draw or clear into it, the
		 * scissor box -- CoreAnimation draws only what has changed, inside that box, and leaves the
		 * rest of the surface as it was --; and the draws of the blur whose radius varies.
		 */
		say("redwood-gl: f %u at %llu.%03llu s: %u draws, %u clears; display surface %u, %u draws into it, box %s %d,%d %dx%d; "
		    "vrd %u vrb %u; %u in place, %u copied; spans %u solid %u blended, ends %u %u; other %u %u\n", e->frames,
		    after / 1000000, after / 1000 % 1000, e->draws, e->clears, e->line.surface, e->line.into_display, e->line.boxed ? "on" : "off", e->line.box[0], e->line.box[1],
		    e->line.box[2], e->line.box[3], e->line.vrd, e->line.vrb, in_place, copied, e->line.solid_spans, e->line.blended_spans,
		    e->line.solid_ends, e->line.blended_ends, e->line.other_spans, e->line.other_ends);
		bzero(&e->line, sizeof(e->line));
		frame_functions(e);
	}
	if (in_place == 0 && copied == 0 && e->draws != 0 && unfollowed < 8) {
		/* (whatever it was drawn into is shown as it is, drawn or not: this is to be seen in the log) */
		unfollowed++;
		say("redwood-gl: frame %u: %u draws, and none into an IOSurface that is known here (framebuffer %u, texture %u)\n",
		    e->frames, e->draws, e->framebuffer, e->colour);
	}
	e->all_calls += e->calls;
	e->all_draws += e->draws;
	e->all_triangles += e->triangles;
	e->wait_us += waited - before;
	e->readback_us += after - waited;
	e->attach_us += e->attaching;
	e->flush_us += e->flushing;
	e->frame_us += after - e->began;
	if (e->calls > e->most_calls) {
		e->most_calls = e->calls;
	}
	if (e->triangles > e->most_triangles) {
		e->most_triangles = e->triangles;
	}
	if (e->frames <= 40 || e->frames % 20 == 0) {
		say("redwood-gl: frame %u: %u GL calls, %u draws, %u triangles, %u clears; %u textures refilled from IOSurfaces "
		    "(%llu KB); %u picture%s copied out, %u drawn in place; %llu us (%llu handing on, %llu waiting, %llu copying "
		    "out), %llu before it for the IOSurfaces\n", e->frames, e->calls, e->draws, e->triangles, e->clears, e->uploads,
		    e->upload_bytes >> 10, copied, copied == 1 ? "" : "s", in_place, after - e->began, e->flushing, waited - before,
		    after - waited, e->attaching);
	}
	if (e->frames == 30 || e->frames % 200 == 0) {
		summary(e);
	}
	census_report();
	if (e->frames == 600 && e->errors == 0) {
		check_errors = 0;
	}
	e->last_draws = e->draws;
	e->calls = e->draws = e->triangles = e->uploads = e->clears = 0;
	e->upload_bytes = e->attaching = e->flushing = 0;
}

#ifndef GL_BLEND
#define GL_BLEND                        0x0be2
#endif
/* (glframes=1) a draw of lines, into the display's picture or another: CoreAnimation fills glyphs span by span */
static void
count_spans(struct eagl *e, GLenum mode, GLsizei count)
{
	struct image *image;

	if (!frame_lines || mode != GL_LINES) {
		return;
	}
	if (e->framebuffer == 0 || e->colour == 0 || (image = image_of(e, e->colour)) == NULL || !image->target ||
	    image->width < 1000 || image->height < 2000) {
		e->line.other_spans++;
		e->line.other_ends += (unsigned)count;
		return;
	}
	if (glIsEnabled(GL_BLEND)) {
		e->line.blended_spans++;
		e->line.blended_ends += (unsigned)count;
	} else {
		e->line.solid_spans++;
		e->line.solid_ends += (unsigned)count;
	}
}

/* Something is about to be drawn into the framebuffer that is bound. */
static void
drawing(struct eagl *e)
{
	struct image *image;

	frame_begin(e);
	if (e->framebuffer != 0 && e->colour != 0 && (image = image_of(e, e->colour)) != NULL && image->target) {
		image->drawn = image->painted = true;
		if (frame_lines && image->width >= 1000 && image->height >= 2000) {
			/* the display's picture (a surface of its size): where this frame first draws into it */
			e->line.into_display++;
			if (!e->line.taken) {
				e->line.taken = true;
				e->line.boxed = glIsEnabled(GL_SCISSOR_TEST);
				glGetIntegerv(GL_SCISSOR_BOX, e->line.box);
				e->line.surface = IOSurfaceGetID(image->surface);
			}
		}
	}
}

/* ---- the entries written by hand ---------------------------------------------------------------- */

static void
my_glBindTexture(void *context, GLenum target, GLuint texture)
{
	struct eagl *e = context;
	struct image *image;

	EAGL_ENTER(e, 0x30);
	if (eagl_tracing) {
		eagl_trace("glBindTexture(%#x, %u)\n", target, texture);
	}
	glBindTexture(target, texture);
	if (target == GL_TEXTURE_2D && e->unit < 8) {
		e->bound[e->unit] = texture;
	}
	if (texture != 0 && e->image_count != 0 && (image = image_of(e, texture)) != NULL && !image->shared &&
	    (!image->target ? IOSurfaceGetSeed(image->surface) != image->seed || surface_changed(e, image) :
	    image->wrapped && !image->painted && surface_changed(e, image))) {
		surface_to_texture(e, image);
	}
	EAGL_LEAVE(e, 0x30);
}

static void
my_glClear(void *context, GLbitfield mask)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x58);
	drawing(e);
	e->clears++;
	if (eagl_tracing) {
		eagl_trace("glClear(%#x)\n", mask);
	}
	clear_now(e);
	if ((mask & GL_COLOR_BUFFER_BIT) && e->colour != 0) {
		struct group_texture *g = group_of(e, e->colour);

		if (g != NULL) {
			g->state = GROUP_FRESH;         /* (a group about to be drawn: see group_draw_begin()) */
		}
	}
	if (e->colour != 0) {
		/* what is about to be cleared may be what the next draw reads: see reads_what_it_writes() */
		e->unclear = mask;
		e->unclear_boxed = glIsEnabled(GL_SCISSOR_TEST);
		glGetIntegerv(GL_SCISSOR_BOX, e->unclear_box);
		glGetFloatv(GL_COLOR_CLEAR_VALUE, e->unclear_colour);
	} else {
		glClear(mask);
	}
	EAGL_LEAVE(e, 0x58);
}

/* While calls are written down: what a draw has left in the middle of what it drew into, and near its top. */
#define GL_VIEWPORT             0x0ba2

static void
trace_pixels(struct eagl *e)
{
	GLint view[4] = { 0, 0, 0, 0 };
	uint8_t middle[4] = { 0, 0, 0, 0 }, low[4] = { 0, 0, 0, 0 }, *all;
	unsigned lit = 0, left = ~0u, bottom = ~0u, right = 0, top = 0;

	glGetIntegerv(GL_VIEWPORT, view);
	glReadPixels(view[0] + view[2] / 2, view[1] + view[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, middle);
	glReadPixels(view[0] + view[2] / 2, view[1] + view[3] / 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, low);
	/* ... and how much of the picture is anything but black, and where */
	if (trace_lit && view[2] > 0 && view[3] > 0 && (all = malloc((size_t)view[2] * (size_t)view[3] * 4)) != NULL) {
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(view[0], view[1], view[2], view[3], GL_RGBA, GL_UNSIGNED_BYTE, all);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		for (unsigned y = 0; y < (unsigned)view[3]; y++) {
			const uint8_t *row = all + (size_t)y * (size_t)view[2] * 4;

			for (unsigned x = 0; x < (unsigned)view[2]; x++) {
				if (row[4 * x] | row[4 * x + 1] | row[4 * x + 2]) {
					lit++;
					left = x < left ? x : left;
					right = x > right ? x : right;
					bottom = y < bottom ? y : bottom;
					top = y > top ? y : top;
				}
			}
		}
		free(all);
	}
	eagl_trace("    framebuffer %u (%d x %d): middle %u %u %u %u, an eighth up %u %u %u %u (red green blue alpha); %u pixels "
	    "not black, within %d,%d to %d,%d%s\n", e->framebuffer, view[2], view[3], middle[0], middle[1], middle[2], middle[3],
	    low[0], low[1], low[2], low[3], lit, lit ? (int)left : 0, lit ? (int)bottom : 0, (int)right, (int)top,
	    glGetError() ? " (not readable)" : "");
}

#define GL_TEXTURE_INTERNAL_FORMAT      0x1003
#define GL_TEXTURE_WRAP_S               0x2802
#define GL_TEXTURE_WRAP_T               0x2803
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE0                     0x84c0
#define GL_UNSIGNED_SHORT               0x1403
#define GL_FLOAT                        0x1406
#define GL_HALF_FLOAT                   0x140b

static float half_float(uint16_t half);

#define GL_ACTIVE_UNIFORMS              0x8b86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH    0x8b87
#define GL_FLOAT_VEC2                   0x8b50
#define GL_FLOAT_VEC3                   0x8b51
#define GL_FLOAT_VEC4                   0x8b52
#define GL_FLOAT_MAT2                   0x8b5a
#define GL_FLOAT_MAT3                   0x8b5b
#define GL_FLOAT_MAT4                   0x8b5c

/* How many numbers a uniform of this type is made of (0: not one of floats). */
static unsigned
uniform_floats(GLenum type)
{
	switch (type) {
	case GL_FLOAT:          return 1;
	case GL_FLOAT_VEC2:     return 2;
	case GL_FLOAT_VEC3:     return 3;
	case GL_FLOAT_VEC4:     return 4;
	case GL_FLOAT_MAT2:     return 4;
	case GL_FLOAT_MAT3:     return 9;
	case GL_FLOAT_MAT4:     return 16;
	default:                return 0;
	}
}

/*
 * gltrace_uniforms=1: what the program of a traced draw was given, by the names its shaders use
 * (coord_arg, texmat0, ...). CoreAnimation sets them by number, and a number says nothing; this
 * asks the program itself what it has and what is in it.
 */
static void
trace_uniforms(struct eagl *e)
{
	GLint count = 0, longest = 0;

	glGetProgramiv(e->program, GL_ACTIVE_UNIFORMS, &count);
	glGetProgramiv(e->program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &longest);
	for (GLint i = 0; i < count; i++) {
		char name[128] = "", line[400] = "";
		GLint size = 0, place;
		GLenum type = 0;
		unsigned floats;
		size_t used = 0;

		if (longest >= (GLint)sizeof(name)) {
			break;
		}
		glGetActiveUniform(e->program, (GLuint)i, (GLsizei)sizeof(name), NULL, &size, &type, name);
		place = glGetUniformLocation(e->program, name);
		if (place < 0) {
			continue;
		}
		if ((floats = uniform_floats(type)) == 0) {
			GLint whole[4] = { 0 };

			glGetUniformiv(e->program, place, whole);
			eagl_trace("      %s (%d) = %d\n", name, place, whole[0]);
			continue;
		}
		for (GLint k = 0; k < size && k < 8 && used < sizeof(line) - 80; k++) {
			GLfloat values[16] = { 0 };

			glGetUniformfv(e->program, place + k, values);
			if (size > 1) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " [%d]", k);
			}
			for (unsigned c = 0; c < floats; c++) {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %.6g", (double)values[c]);
			}
		}
		eagl_trace("      %s (%d) =%s\n", name, place, line);
	}
}

/*
 * gltrace_shaders=1: the text of the shaders the program of a traced draw is made of, the first
 * time that program draws. A program's number says nothing by itself, and CoreAnimation's own
 * shaders are what tells what a draw means; the text is the one my_glShaderSource() kept.
 */
static void
trace_program(struct eagl *e)
{
	static GLuint told[64];
	static unsigned telling;
	GLuint shaders[8] = { 0 };
	GLsizei count = 0;

	for (unsigned i = 0; i < telling; i++) {
		if (told[i] == e->program) {
			return;
		}
	}
	if (telling < sizeof(told) / sizeof(told[0])) {
		told[telling++] = e->program;
	}
	glGetAttachedShaders(e->program, (GLsizei)(sizeof(shaders) / sizeof(shaders[0])), &count, shaders);
	for (GLsizei i = 0; i < count; i++) {
		const char *text = e->sources[shaders[i] % 256];
		size_t length = text != NULL ? strlen(text) : 0;

		eagl_trace("    program %u, shader %u, %zu characters:\n", e->program, shaders[i], length);
		for (size_t at = 0; at < length; at += 900) {
			write(2, text + at, length - at < 900 ? length - at : 900);
		}
		if (length > 0 && text[length - 1] != '\n') {
			write(2, "\n", 1);
		}
	}
}

/*
 * What a draw that is being written down is made of: the texture on the first unit, and the
 * first corners as CoreAnimation has them in its arrays (place, colour, place in the texture).
 */
static void
trace_draw(struct eagl *e, GLenum type, const void *indices, GLint first, GLsizei count)
{
	char textures[160] = "";
	size_t used = 0;

	if (trace_shaders) {
		trace_program(e);
	}
	if (trace_uniforms_on) {
		trace_uniforms(e);
	}

	for (unsigned unit = 0; unit < 4; unit++) {
		GLint width = 0, height = 0, format = 0, wrap_s = 0, wrap_t = 0, filter = 0;

		if (e->bound[unit] == 0) {
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		/* ... and how it is read past its edges and between its pixels, which is where a picture
		   taken from the edge of another goes wrong */
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filter);
		used += (size_t)snprintf(textures + used, sizeof(textures) - used, " [%u] %u (%d x %d, %#x, wrap %#x/%#x, mag %#x)",
		    unit, e->bound[unit], width, height, (unsigned)format, (unsigned)wrap_s, (unsigned)wrap_t, (unsigned)filter);
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	eagl_trace("    program %u, framebuffer %u (into texture %u), textures%s\n", e->program, e->framebuffer, e->colour, textures);
	for (GLsizei i = 0; i < count && i < trace_corners; i++) {
		unsigned vertex = indices == NULL ? (unsigned)(first + i) : type == GL_UNSIGNED_SHORT ?
		    ((const uint16_t *)indices)[i] : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] :
		    ((const uint32_t *)indices)[i];
		char line[200] = "";
		size_t used = 0;

		for (unsigned a = 0; a < 3 && used < sizeof(line) - 60; a++) {
			const uint8_t *at = e->arrays[a].pointer;
			GLint size = e->arrays[a].size > 4 ? 4 : e->arrays[a].size;

			if (at == NULL || size <= 0) {
				continue;
			}
			at += (size_t)vertex * (size_t)(e->arrays[a].stride ? e->arrays[a].stride :
			    size * (e->arrays[a].type == GL_FLOAT ? 4 : e->arrays[a].type == GL_HALF_FLOAT ? 2 : 1));
			used += (size_t)snprintf(line + used, sizeof(line) - used, " [%u]", a);
			for (GLint c = 0; c < size; c++) {
				if (e->arrays[a].type == GL_FLOAT) {
					float value;

					memcpy(&value, at + 4 * c, 4);
					used += (size_t)snprintf(line + used, sizeof(line) - used, " %.5g", (double)value);
				} else if (e->arrays[a].type == GL_HALF_FLOAT) {
					/* (a layer's colour comes this way: four half floats, not four bytes) */
					uint16_t half;

					memcpy(&half, at + 2 * c, 2);
					used += (size_t)snprintf(line + used, sizeof(line) - used, " %.4g",
					    (double)half_float(half));
				} else {
					used += (size_t)snprintf(line + used, sizeof(line) - used, " %u", at[c]);
				}
			}
		}
		eagl_trace("    corner %u:%s\n", vertex, line);
	}
}

#define GL_BLEND                        0x0be2
#define GL_BLEND_DST_RGB                0x80c8
#define GL_BLEND_SRC_RGB                0x80c9

static float
half_float(uint16_t half)
{
	int exponent = half >> 10 & 31;
	float value = exponent == 0 ? (float)(half & 1023) / 1024 / 16384 :
	    (1 + (float)(half & 1023) / 1024) * (exponent >= 15 ? (float)(1u << (exponent - 15)) : 1 / (float)(1u << (15 - exponent)));

	return half & 0x8000 ? -value : value;
}

/* gltrace_place: a line about a draw, if any of its first corners is inside the rectangle (or the rectangle inside them). */
static void
trace_placed_draw(struct eagl *e, GLenum mode, GLenum type, const void *indices, GLint first, GLsizei count)
{
	float low[2] = { 1e9f, 1e9f }, high[2] = { -1e9f, -1e9f }, colour[4] = { 0, 0, 0, 0 }, where[2] = { 0, 0 };
	GLint blend[2] = { 0, 0 }, scissor[4] = { 0, 0, 0, 0 };
	char textures[160] = "";
	size_t used = 0;

	if (trace_shaders) {
		trace_program(e);               /* (gltrace_shaders=1: the text of each program as it first draws here) */
	}
	if (trace_uniforms_on) {
		trace_uniforms(e);              /* (gltrace_uniforms=1: and what it was given) */
	}

	if (e->arrays[0].pointer == NULL || e->arrays[0].type != GL_FLOAT) {
		return;
	}
	for (GLsizei i = 0; i < count && i < 96; i++) {
		unsigned vertex = indices == NULL ? (unsigned)(first + i) : type == GL_UNSIGNED_SHORT ?
		    ((const uint16_t *)indices)[i] : type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] :
		    ((const uint32_t *)indices)[i];
		const uint8_t *at = (const uint8_t *)e->arrays[0].pointer + (size_t)vertex * (size_t)e->arrays[0].stride;
		float place[2];

		memcpy(place, at, sizeof(place));
		for (unsigned axis = 0; axis < 2; axis++) {
			low[axis] = place[axis] < low[axis] ? place[axis] : low[axis];
			high[axis] = place[axis] > high[axis] ? place[axis] : high[axis];
		}
		if (i == 0) {
			if (e->arrays[1].pointer != NULL && e->arrays[1].type == GL_HALF_FLOAT) {
				uint16_t halves[4];

				memcpy(halves, (const uint8_t *)e->arrays[1].pointer + (size_t)vertex * (size_t)e->arrays[1].stride, 8);
				for (unsigned part = 0; part < 4; part++) {
					colour[part] = half_float(halves[part]);
				}
			}
			if (e->arrays[2].pointer != NULL && e->arrays[2].type == GL_FLOAT) {
				memcpy(where, (const uint8_t *)e->arrays[2].pointer + (size_t)vertex * (size_t)e->arrays[2].stride, 8);
			}
		}
	}
	if (low[0] > trace_place[2] || high[0] < trace_place[0] || low[1] > trace_place[3] || high[1] < trace_place[1]) {
		return;
	}
	trace_listed = 1;
	trace_middle[0] = (low[0] + high[0]) / 2;
	trace_middle[1] = (low[1] + high[1]) / 2;
	for (unsigned unit = 0; unit < 4; unit++) {
		GLint width = 0, height = 0, format = 0;
		struct image *image;

		if (e->bound[unit] == 0) {
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		image = image_of(e, e->bound[unit]);
		used += (size_t)snprintf(textures + used, sizeof(textures) - used, " [%u] %u (%d x %d, %#x%s)", unit, e->bound[unit],
		    width, height, (unsigned)format, image == NULL ? "" : image->shared ? ", a surface's memory" :
		    image->wrapped ? ", a copy of wrapped memory" : ", a copy of a surface");
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
	glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
	glGetIntegerv(GL_SCISSOR_BOX, scissor);
	say("gl-at: frame %u draw %u into %u (texture %u): %s of %d from %g,%g to %g,%g; program %u; textures%s; colour %g %g %g "
	    "%g; in the texture at %g,%g; blend %s %#x %#x; scissor %s %d,%d %dx%d\n", e->frames + 1, e->draws, e->framebuffer,
	    e->colour, mode == GL_TRIANGLES ? "triangles" : mode == 1 ? "lines" : "strips", count, (double)low[0], (double)low[1],
	    (double)high[0], (double)high[1], e->program, textures, (double)colour[0], (double)colour[1], (double)colour[2],
	    (double)colour[3], (double)where[0], (double)where[1], glIsEnabled(GL_BLEND) ? "on" : "off", blend[0], blend[1],
	    glIsEnabled(GL_SCISSOR_TEST) ? "on" : "off", scissor[0], scissor[1], scissor[2], scissor[3]);
}

/* gltrace_look: the pixel a listed draw has left in the middle of itself (the matrix is CoreAnimation's, the last it gave). */
static void
trace_placed_look(struct eagl *e)
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	const GLfloat *m = e->matrix != NULL ? e->matrix : identity;
	float x = m[0] * trace_middle[0] + m[4] * trace_middle[1] + m[12];
	float y = m[1] * trace_middle[0] + m[5] * trace_middle[1] + m[13];
	float w = m[3] * trace_middle[0] + m[7] * trace_middle[1] + m[15];
	GLint view[4] = { 0, 0, 0, 0 };
	uint8_t pixel[4] = { 0, 0, 0, 0 };
	int px, py;

	trace_listed = 0;
	if (!trace_look || w == 0) {
		return;
	}
	glGetIntegerv(GL_VIEWPORT, view);
	px = view[0] + (int)((x / w + 1) / 2 * (float)view[2]);
	py = view[1] + (int)((y / w + 1) / 2 * (float)view[3]);
	glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	say("gl-at:     it left %u %u %u %u (red green blue alpha) at %d,%d of the framebuffer%s\n", pixel[0], pixel[1], pixel[2],
	    pixel[3], px, py, glGetError() ? " (not readable)" : "");
}

static unsigned
triangles_of(GLenum mode, GLsizei count)
{
	return mode == GL_TRIANGLES ? (unsigned)count / 3 : (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) &&
	       count > 2 ? (unsigned)count - 2 : 0;
}

/*
 * CoreAnimation's OpenGL ES renderer draws the last of its ninety "functions" (0x59, "cst" in the
 * table its Metal renderer names its shaders by; no textures of its own by that table) with no
 * program at all: glUseProgram(0), and then the draw. On iOS's library that may have meant
 * something; in OpenGL ES it draws nothing, and what goes that way is missing from the picture:
 * the lock screen's wallpaper, and the clock that is that wallpaper seen through its digits.
 * Here such a draw is made with a program of ours, the plainest there could be: CoreAnimation's
 * own vertex arrays (position 0, colour 1, texture coordinates 2, the latter in the texture's
 * pixels), the matrix it gives every program of its own (always from one place of its: that
 * place is read), and the colour times the texture on the first unit. With that the lock screen
 * has its wallpaper and its clock, and the home screen is as it was.
 * Only draws with a texture bound are treated so. CoreAnimation also makes draws with no program
 * and no texture, over the whole of a surface, several a frame: given the colour alone
 * (glplain=2) they paint the screen white, so whatever they are for, it is not that; they are
 * left to draw nothing, as before. What the function is on the Metal side was not found out.
 */
#define GL_VERTEX_SHADER                0x8b31
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED  0x8622

static const char plain_vertex[] =
    "#version 300 es\n"
    "uniform highp mat4 vertex_matrix;\n"
    "in highp vec4 vertex_position;\n"
    "in lowp vec4 vertex_color;\n"
    "in highp vec2 vertex_texcoord0;\n"
    "out lowp vec4 color;\n"
    "out highp vec2 texcoord;\n"
    "void main() {\n"
    "  gl_Position = vertex_matrix * vertex_position;\n"
    "  color = vertex_color;\n"
    "  texcoord = vertex_texcoord0;\n"
    "}\n";
static const char plain_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform lowp float textured;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  result = textured > 0.5 ? color * texture(texture0, texcoord / vec2(textureSize(texture0, 0))) : color;\n"
    "}\n";

/*
 * One kind of them has a second texture, on the second unit: a table, one row of pixels (seen:
 * 256 of two parts, brightness and opacity). What is drawn so is the home indicator, the bar at
 * the bottom of the screen that is light over dark pictures and dark over light ones. It is a
 * filter of CoreAnimation's own, "homeAffordanceBase", which its OpenGL ES renderer has no
 * program for: it binds what is behind the bar and the table, and draws with none; the filter's
 * three numbers (inputAddWhite, inputAmount, inputOverlayOpacity) it hands to no one. What the
 * filter does is CoreAnimation's software renderer's to say, and this is the same arithmetic:
 *   the table's entry for the brightness of what is behind, plus the white added, times the
 *   amount; that over what is behind; and the result laid over itself in "overlay" mode at the
 *   overlay's opacity.
 * The three numbers are the ones with which this gives the software renderer's bar on the
 * phone, 153 of 255 over black and 63 over white: 0.284, 1 and 0.31. (MaterialKit's defaults
 * for its "thin" bar would be 0.373, 0.31 and 0.4, and give neither.)
 */
static const char table_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform lowp sampler2D table;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "const mediump float white = .284, amount = 1., overlay = .31;\n"
    "void main() {\n"
    "  mediump vec4 behind = texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  mediump vec4 entry = texture(table, vec2(dot(behind.rgb, vec3(.2125, .7154, .0721)), .5));\n"
    "  mediump vec4 bar = clamp(entry + vec4(vec3(white), 1.), 0., 1.) * amount;\n"
    "  mediump vec4 under = behind * (1. - bar.a) + bar;\n"
    "  mediump vec4 over = under * overlay;\n"
    "  mediump vec3 dark = 2. * under.rgb * over.rgb;\n"
    "  mediump vec3 light = over.a * under.a - 2. * (under.a - under.rgb) * (over.a - over.rgb);\n"
    "  mediump vec3 both = mix(dark, light, step(under.a * .5, under.rgb));\n"
    "  mediump float opacity = over.a + under.a * (1. - over.a);\n"
    "  result = vec4(clamp(both + over.rgb * (1. - under.a) + under.rgb * (1. - over.a), 0., opacity), opacity);\n"
    "}\n";

/*
 * Two more are told apart by the "function" itself, which CoreAnimation's context has in a known
 * place while it draws (and which our library in the window server hands over with the context:
 * redwood_gl_function_values() below, called from inject.c). Both take a colour matrix, four
 * columns and what is added, the numbers function_values[] keeps.
 *
 *   "vcm" (45), the filter vibrantColorMatrix: the layer is not drawn, it says how much of the
 *   matrix is applied to what is behind it. The lock screen has two: a gradient over the whole
 *   screen, opaque red at the top and clear at the bottom, under which the wallpaper is toned
 *   down for the clock to be read; and the clock itself, through whose digits the wallpaper
 *   comes out nearly white. Drawn as a picture, the first was a red veil over the wallpaper.
 *   What the filter does is CoreAnimation's software renderer's to say (tex_vibrant_color_matrix:
 *   the matrix over what is there, tex_color_matrix_4x4_bias, and tex_mix by the layer's alpha);
 *   the matrix arithmetic is also in the OpenGL ES renderer's own shaders, colorMatrix4x4Bias.
 *   What is there is read with EXT_shader_framebuffer_fetch.
 *
 *   "c4p" (20), the filter colorMatrix told that its input is premultiplied
 *   (inputPremultipliedValues): the matrix over the layer's pixels as they are. The clock's
 *   digits pass through one that keeps nothing but red as alpha ("colorToAlphaMask") before they
 *   are that mask; drawn as they were, they stayed white.
 */
#define FUNCTION_MATRIX_PREMULTIPLIED   20
#define FUNCTION_DOWNSAMPLE_8           28
#define FUNCTION_VARIABLE_DOWNSAMPLE    38
#define FUNCTION_VARIABLE_BLUR          39
#define FUNCTION_SHADOW_BLUR            40
#define FUNCTION_VIBRANT_MATRIX         45

static const char vibrant_fragment[] =
    "#version 300 es\n"
    "#extension GL_EXT_shader_framebuffer_fetch : require\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform mediump vec4 colours[5];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "layout(location = 0) inout highp vec4 result;\n"
    "void main() {\n"
    "  mediump vec4 layer = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  mediump vec4 behind = result, s = behind;\n"
    "  s.rgb = s.rgb / max(s.a, .005);\n"
    "  s = s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4];\n"
    "  s.a = clamp(s.a, 0., 1.);\n"
    "  s.rgb = clamp(s.rgb * s.a, 0., 1.);\n"
    "  result = mix(behind, s, layer.a);\n"
    "}\n";
static const char masking_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform mediump vec4 colours[5];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  mediump vec4 s = color * texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  result = clamp(s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4], 0., 1.);\n"
    "}\n";

/*
 *   "ds8" (28): a picture made eight times smaller in one go, the first step of a blur of what is
 *   behind a layer (capture_backdrop()). Its numbers are four places, one and three pixels of the
 *   picture away each way: with the four of them mirrored in both directions, sixteen samples of
 *   four pixels each, they are the sixty-four pixels that become one. (CoreAnimation's own shaders
 *   for four and for six samples are of that kind; for this one the OpenGL ES renderer has none.)
 *   Drawn as it was given, one sample in the middle, four pixels of the sixty-four made the one:
 *   what is blurred after that shimmers when it moves.
 *   The colour of the corners is not the picture's to take: CoreAnimation's own shaders for four
 *   and for six samples do not touch it, and it is the layer's opacity, which it applies where the
 *   layer is laid on the screen. Multiplied in here, what was captured came out at the opacity of
 *   the layer that asked for it -- 0.45 for the translucent minus on a jiggling icon -- and
 *   everything made of it after, the blur, the curve of brightness and the colour matrix, worked
 *   on a picture too dark: the minus came out stained with what was behind it instead of the
 *   light grey it is on a phone.
 */
/*
 *   "sb3" (40): four samples around a point, averaged -- the same four places as "ds4" is given,
 *   one pixel out each way, with no weights of its own. It is asked for where a shadow is laid
 *   over what is behind it (the cards of the switcher of running applications) and in the home
 *   screen's edit mode. CoreAnimation's OpenGL ES renderer has no shader for it and, unlike the
 *   ones it never asks about, it asks and is told no -- and then what needs it is not drawn at
 *   all, so the shadow is missing. This waits for a way to hand CoreAnimation a program where it
 *   looks for one: answering that the renderer has the function (src/inject.c, glyes=40) only
 *   kills the window server, and no draw with no program ever arrives here (see there).
 */
static const char blur4_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec2 places[4];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  highp vec2 middle = texcoord / vec2(textureSize(texture0, 0));\n"
    "  result = .25 * (texture(texture0, middle + places[0]) + texture(texture0, middle + places[1]) +\n"
    "      texture(texture0, middle + places[2]) + texture(texture0, middle + places[3]));\n"
    "}\n";

static const char box_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec2 places[4];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  highp vec2 middle = texcoord / vec2(textureSize(texture0, 0));\n"
    "  highp vec4 sum = vec4(0.);\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    sum += texture(texture0, middle + places[i]) + texture(texture0, middle - places[i]);\n"
    "    sum += texture(texture0, middle + places[i] * vec2(1., -1.)) + texture(texture0, middle + places[i] * vec2(-1., 1.));\n"
    "  }\n"
    "  result = sum * .0625;\n"
    "}\n";

static int function_numbers;            /* boot-arg glnumbers=1 */

/* Four of a function's numbers at a time, for the line above (one of a few buffers in turn). */
static const char *
number_text(const float *values, size_t from, size_t count)
{
	static char texts[5][80];
	static unsigned which;
	char *text = texts[which++ % (sizeof(texts) / sizeof(texts[0]))];

	if (from >= count) {
		return "";
	}
	snprintf(text, sizeof(texts[0]), " [%g %g %g %g]", (double)values[from],
	    from + 1 < count ? (double)values[from + 1] : 0.0, from + 2 < count ? (double)values[from + 2] : 0.0,
	    from + 3 < count ? (double)values[from + 3] : 0.0);
	return text;
}

static void *ca_context;                /* CoreAnimation's GLContext */
static GLfloat function_values[90][20];
static uint8_t function_given[90];

/* From inject.c, at every GLContext::function_uniform(): the numbers a function is given, which no shader of CoreAnimation's takes. */
void
redwood_gl_function_values(void *context, unsigned function, unsigned index, size_t components, size_t count, const float *values)
{
	static uint8_t said[90];

	ca_context = context;
	if (function >= 90 || index != 0 || components * count > 20) {
		return;
	}
	/* (boot-arg glnumbers=1: every set of numbers a function is given that is not the one before,
	   up to a dozen of them -- the same function serves many a layer, with a matrix of its own) */
	if (!said[function] || (function_numbers && said[function] < 12 &&
	    memcmp(function_values[function], values, components * count * sizeof(values[0])) != 0)) {
		said[function]++;
		say("redwood-gl: CoreAnimation gives its function %u numbers: %zu of %zu:%s%s%s%s%s%s%s%s%s%s\n", function, count,
		    components, number_text(values, 0, components * count), number_text(values, 4, components * count),
		    number_text(values, 8, components * count), number_text(values, 12, components * count),
		    number_text(values, 16, components * count), "", "", "", "", "");
	}
	memcpy(function_values[function], values, components * count * sizeof(values[0]));
	function_given[function] = (uint8_t)(components * count);
}

/* boot-arg glplain=0: leave such draws as they are; 2: also those without a texture; 3: the two with a colour matrix drawn plainly too */
static int plain_draws = 1;

/*
 * The blur whose radius varies over the picture (the filter variableBlur with a mask: the strip
 * under the status bar in Search, through which what scrolls up fades into a blur) is made of two
 * functions, "vrd" (38) and "vrb" (39): the first, four times over, is to make of a picture its
 * smaller copies, each from the one before, and the second reads among them by the mask. In the
 * Metal renderer the copies are the levels of one texture. The OpenGL ES renderer has neither
 * shader, and nothing that says which level is drawn into: what it does, four times, is to clear
 * the texture and draw into it with that same texture as the source. What that leaves is the
 * driver's to decide (here now the picture, now nothing: the strip came out black after one
 * route to Search and right after another).
 * Here the levels are textures of our own, each half the size of the one before, one chain of
 * them for each picture blurred so: "vrd", whose one number is the level it reads, makes level
 * L + 1 of level L instead of drawing the picture into itself (vblur_level()), and "vrb" reads
 * the chain by the mask (vblur_draw_begin()). Boot-arg vblur=0 leaves it as it was before: no
 * level made, the picture not drawn from itself nor cleared for it, and "vrb" copying it out
 * plainly -- the strip unblurred. That stand-in had a fault of its own: it showed whatever the
 * picture last held, and once in a few dozen times in Search that was the wallpaper half dimmed
 * as it had been in the middle of Search's coming in, until something drew the strip again.
 *
 * Which function a draw is can be asked of CoreAnimation's context only while it draws (between
 * frames the place it is read from may be gone: the window server died of being asked at a
 * glClear). So every clear of a texture is put off, with the scissor and the colour it was to be
 * made with, until the next draw says what it is: that function, and the clear is dropped with
 * the draw; anything else, and it is made first (as it is before anything else that could tell
 * the difference: another framebuffer, another attachment, a texture deleted or filled, the end
 * of a frame). Only the clears of a texture that was also bound to be read used to be put off,
 * but CoreAnimation clears a surface as it makes it the target (GLESContext::set_surface: the
 * framebuffer, the viewport, the clear), before it binds the textures of the draw that follows:
 * the first of the four clears found the picture of the step before on the first unit, was
 * made, and left the strip black -- now in the first Search of a run, now from the second on,
 * by what had been drawn before.
 */
/*
 * Boot-arg vblur=0: the stand-in. vblur=2, for checking: the strip painted by its mask instead of
 * blurred, red where the radius is whole, blue where there is none, and the mask's pixels in the log.
 */
static int vblur_on = 1;

/*
 * Halving: a pixel of the level made is the average of the sixteen of the level it is made of
 * around it, by weight (four samples, each the GPU's own average of four pixels), which is what
 * makes a chain of such levels look blurred rather than coarse when it is read back enlarged.
 */
static const char vblur_vertex[] =
    "#version 300 es\n"
    "void main() {\n"
    "  vec2 corner = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "  gl_Position = vec4(corner, 0.0, 1.0);\n"
    "}\n";
static const char halve_fragment[] =
    "#version 300 es\n"
    "uniform highp sampler2D source;\n"
    "uniform highp vec2 texel;\n"
    "out highp vec4 result;\n"
    "void main() {\n"
    "  highp vec2 at = gl_FragCoord.xy * 2.0 * texel;\n"
    "  result = 0.25 * (texture(source, at + texel * vec2(-1.0, -1.0)) + texture(source, at + texel * vec2(1.0, -1.0)) +\n"
    "      texture(source, at + texel * vec2(-1.0, 1.0)) + texture(source, at + texel * vec2(1.0, 1.0)));\n"
    "}\n";
/*
 * Reading the chain: the mask (its opacity, on the second unit, at CoreAnimation's second texture
 * coordinates) says how much of the radius (the function's first number, in pixels of the
 * picture) is wanted at each pixel; a radius of r is read at level log2(r), between two levels
 * mixed. The mask's coordinates run past its ends (above the strip's top, below its bottom) and
 * the texture repeats: unheld, the last row of the strip read the mask's first, the whole radius,
 * and came out a line blurred darker than what it lay on. They are held to the mask's ends.
 * With the function's second number set (inputNormalizeEdges, it seems), what the blur drew in
 * from outside the picture -- nothing, transparent -- is made up for by dividing by the opacity,
 * as the edges of an ordinary blur are.
 * The colour's opacity is the blurring layer's (Spotlight's strip fades so while the gesture that
 * dismisses Search is under way), and CoreAnimation puts what this draws on the screen as it is,
 * without blending: so the opacity is how much of the blur is shown over the picture itself, not a
 * darkening. Multiplied in, the strip went dark with it -- to a quarter, in four frames, at each
 * start of that gesture -- and stayed dark whenever the gesture ended between two of the window
 * server's renderings of the strip: it reuses the last one until what is behind the strip changes
 * (the dark strip in Search, rc.jobs.strip).
 */
static const char vblur_draw_vertex[] =
    "#version 300 es\n"
    "uniform highp mat4 vertex_matrix;\n"
    "in highp vec4 vertex_position;\n"
    "in lowp vec4 vertex_color;\n"
    "in highp vec2 vertex_texcoord0;\n"
    "in highp vec2 vertex_texcoord1;\n"
    "out lowp vec4 color;\n"
    "out highp vec2 texcoord;\n"
    "out highp vec2 maskcoord;\n"
    "void main() {\n"
    "  gl_Position = vertex_matrix * vertex_position;\n"
    "  color = vertex_color;\n"
    "  texcoord = vertex_texcoord0;\n"
    "  maskcoord = vertex_texcoord1;\n"
    "}\n";
static const char vblur_fragment[] =
    "#version 300 es\n"
    "uniform highp sampler2D level0;\n"
    "uniform highp sampler2D mask;\n"
    "uniform highp sampler2D level1;\n"
    "uniform highp sampler2D level2;\n"
    "uniform highp sampler2D level3;\n"
    "uniform highp sampler2D level4;\n"
    "uniform highp float radius;\n"
    "uniform int levels;\n"
    "uniform highp float normalize_edges;\n"
    "uniform int paint;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "in highp vec2 maskcoord;\n"
    "out highp vec4 result;\n"
    "highp vec4 level(int k, highp vec2 at) {\n"
    "  if (k <= 0) return texture(level0, at);\n"
    "  if (k == 1) return texture(level1, at);\n"
    "  if (k == 2) return texture(level2, at);\n"
    "  if (k == 3) return texture(level3, at);\n"
    "  return texture(level4, at);\n"
    "}\n"
    "void main() {\n"
    "  highp vec2 at = texcoord / vec2(textureSize(level0, 0));\n"
    "  highp vec2 mask_size = vec2(textureSize(mask, 0));\n"
    "  highp vec2 m_at = maskcoord.x > 1.5 || maskcoord.y > 1.5 ? maskcoord / mask_size : maskcoord;\n"
    "  m_at = clamp(m_at, 0.5 / mask_size, 1.0 - 0.5 / mask_size);\n"
    "  highp float m = texture(mask, m_at).a;\n"
    "  highp float lod = clamp(log2(max(m * radius, 1.0)), 0.0, float(levels - 1));\n"
    "  int k = int(floor(lod));\n"
    "  highp vec4 c = mix(level(k, at), level(min(k + 1, levels - 1), at), lod - float(k));\n"
    "  if (normalize_edges > 0.5) {\n"
    "    c = c.a > 0.004 ? vec4(c.rgb / c.a, 1.0) : vec4(0.0);\n"
    "    highp vec4 sharp = texture(level0, at);\n"
    "    sharp = sharp.a > 0.004 ? vec4(sharp.rgb / sharp.a, 1.0) : c;\n"
    "    result = vec4(mix(sharp.rgb, c.rgb, color.a), 1.0);\n"
    "  } else {\n"
    "    result = color * c;\n"
    "  }\n"
    "  if (paint != 0) {\n"
    "    highp float t = m;\n"
    "    result = vec4(t, 0.25 * c.g, 1.0 - t, 1.0);\n"
    "  }\n"
    "}\n";

static GLuint
vblur_program(const char *vertex, const char *fragment, const char *what)
{
	GLuint program = glCreateProgram(), shader;
	const GLchar *text;
	GLint good = 0;
	char said[512] = "";

	shader = glCreateShader(GL_VERTEX_SHADER);
	text = vertex;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	shader = glCreateShader(GL_FRAGMENT_SHADER);
	text = fragment;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(shader, sizeof(said), NULL, said);
	}
	glAttachShader(program, shader);
	glBindAttribLocation(program, 0, "vertex_position");
	glBindAttribLocation(program, 1, "vertex_color");
	glBindAttribLocation(program, 2, "vertex_texcoord0");
	glBindAttribLocation(program, 3, "vertex_texcoord1");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	say("redwood-gl: a program of ours for the blur whose radius varies (%s): %s %s\n", what, good ? "made" : "NOT MADE", said);
	return good ? program : 0;
}

static struct vblur_chain *
vblur_chain_of(struct eagl *e, GLuint picture, bool make)
{
	struct vblur_chain *oldest = &e->chains[0];

	for (unsigned i = 0; i < VBLUR_CHAINS; i++) {
		if (e->chains[i].picture == picture && picture != 0) {
			return &e->chains[i];
		}
		if (e->chains[i].used < oldest->used) {
			oldest = &e->chains[i];
		}
	}
	if (!make) {
		return NULL;
	}
	/* (the one least lately used gives its textures up; they are made again at the sizes wanted) */
	oldest->picture = picture;
	oldest->made = 0;
	oldest->width = oldest->height = 0;
	return oldest;
}

static void
vblur_forget(struct eagl *e, GLuint picture)
{
	struct vblur_chain *chain = vblur_chain_of(e, picture, false);

	if (chain != NULL) {
		chain->picture = 0;
		chain->made = 0;
	}
}

/*
 * A draw of "vrd" (function 38) that reads level `from`: level from + 1 is made of it, in a
 * texture of ours. Every bit of state touched is put back as CoreAnimation had it.
 */
static void
vblur_level(struct eagl *e, int from)
{
	GLuint picture = e->bound[0];
	struct vblur_chain *chain;
	GLint view[4], box[4], width = 0, height = 0, to = from + 1;
	GLboolean scissored, blended;
	GLuint source;

	if (from < 0 || to >= VBLUR_LEVELS || picture == 0) {
		return;
	}
	if (e->halve == 0) {
		e->halve = vblur_program(vblur_vertex, halve_fragment, "halving");
		if (e->halve != 0) {
			glUseProgram(e->halve);
			glUniform1i(glGetUniformLocation(e->halve, "source"), 0);
			e->halve_texel = glGetUniformLocation(e->halve, "texel");
			glUseProgram(0);
		}
	}
	if (e->halve == 0) {
		return;
	}
	glActiveTexture(GL_TEXTURE0);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	chain = vblur_chain_of(e, picture, true);
	chain->used = e->frames + 1;
	if (chain->width != width || chain->height != height) {
		chain->width = width;
		chain->height = height;
		chain->made = 1;
		for (int k = 1; k < VBLUR_LEVELS; k++) {
			chain->level_width[k] = 0;
		}
	}
	if (from == 0) {
		chain->made = 1;                        /* (the picture is new: the levels made of the one before are not of it) */
	}
	if (from >= chain->made) {
		glActiveTexture(GL_TEXTURE0 + e->unit);
		return;                                 /* (the level it is to be made of is not there) */
	}
	if (chain->level_width[to] == 0) {
		GLsizei w = chain->width, h = chain->height;

		for (int k = 0; k < to; k++) {
			w = w / 2 > 1 ? w / 2 : 1;
			h = h / 2 > 1 ? h / 2 : 1;
		}
		if (chain->level[to] == 0) {
			glGenTextures(1, &chain->level[to]);
			glGenFramebuffers(1, &chain->framebuffer[to]);
		}
		glBindTexture(GL_TEXTURE_2D, chain->level[to]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, 0x2801, 0x2601);         /* min filter: linear */
		glTexParameteri(GL_TEXTURE_2D, 0x2800, 0x2601);         /* mag filter: linear */
		glTexParameteri(GL_TEXTURE_2D, 0x2802, 0x812f);         /* clamp to edge */
		glTexParameteri(GL_TEXTURE_2D, 0x2803, 0x812f);
		glBindFramebuffer(GL_FRAMEBUFFER, chain->framebuffer[to]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, chain->level[to], 0);
		chain->level_width[to] = w;
		chain->level_height[to] = h;
	}
	source = from == 0 ? picture : chain->level[from];
	glGetIntegerv(GL_VIEWPORT, view);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	scissored = glIsEnabled(GL_SCISSOR_TEST);
	blended = glIsEnabled(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, chain->framebuffer[to]);
	glViewport(0, 0, chain->level_width[to], chain->level_height[to]);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glUseProgram(e->halve);
	glBindTexture(GL_TEXTURE_2D, source);
	glUniform2f(e->halve_texel, 1.0f / (float)(from == 0 ? chain->width : chain->level_width[from]),
	    1.0f / (float)(from == 0 ? chain->height : chain->level_height[from]));
	glDrawArrays(GL_TRIANGLES, 0, 3);
	chain->made = to + 1;
	/* back as it was */
	glBindTexture(GL_TEXTURE_2D, picture);
	glUseProgram(0);
	(blended ? glEnable : glDisable)(GL_BLEND);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glScissor(box[0], box[1], box[2], box[3]);
	glViewport(view[0], view[1], view[2], view[3]);
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	glActiveTexture(GL_TEXTURE0 + e->unit);
}

static bool
reads_what_it_writes(struct eagl *e)
{
	return e->program == 0 && e->colour != 0 && e->colour == e->bound[0] && ca_context != NULL &&
	       *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10) == FUNCTION_VARIABLE_DOWNSAMPLE;
}

static void
clear_now(struct eagl *e)
{
	GLint box[4] = { 0, 0, 0, 0 };
	GLfloat colour[4] = { 0, 0, 0, 0 };
	GLboolean boxed;

	if (e->unclear == 0) {
		return;
	}
	boxed = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
	(e->unclear_boxed ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glScissor(e->unclear_box[0], e->unclear_box[1], e->unclear_box[2], e->unclear_box[3]);
	glClearColor(e->unclear_colour[0], e->unclear_colour[1], e->unclear_colour[2], e->unclear_colour[3]);
	glClear(e->unclear);
	e->unclear = 0;
	(boxed ? glEnable : glDisable)(GL_SCISSOR_TEST);
	glScissor(box[0], box[1], box[2], box[3]);
	glClearColor(colour[0], colour[1], colour[2], colour[3]);
}

/* One of our programs, made: CoreAnimation's own vertex arrays go in at the places it has them. */
static GLuint
plain_program(const char *fragment, const char *what)
{
	GLuint program = glCreateProgram(), shader;
	const GLchar *text;
	GLint good = 0;

	shader = glCreateShader(GL_VERTEX_SHADER);
	text = plain_vertex;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	shader = glCreateShader(GL_FRAGMENT_SHADER);
	text = fragment;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	glBindAttribLocation(program, 0, "vertex_position");
	glBindAttribLocation(program, 1, "vertex_color");
	glBindAttribLocation(program, 2, "vertex_texcoord0");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	say("redwood-gl: a program of ours for the draws CoreAnimation makes with none (%s): %s\n", what, good ? "made" : "NOT MADE");
	return program;
}

/*
 * A census (boot-arg glcensus=1) of what CoreAnimation's OpenGL ES renderer has no shader for and
 * meets all the same: every draw it makes with no program, by function, and every time it asks
 * for a function and is told there is none (it then draws the thing another way, or not at
 * all). Written down: each case the first time it is met, with what it is made of -- where on
 * which framebuffer, the textures, the blending, the numbers CoreAnimation has for the function
 * (a filter's inputs) and how the draw is made here; every ten seconds, what there was of each
 * in those seconds (which is what tells the screens of a scenario apart); and with that the
 * count of each since the start.
 */
enum { CENSUS_NOT_DRAWN, CENSUS_PLAIN, CENSUS_TABLE, CENSUS_VIBRANT, CENSUS_MASKING, CENSUS_BOX, CENSUS_BLUR4, CENSUS_LEVEL,
       CENSUS_VBLUR, CENSUS_KINDS };
static const char *const census_kinds[CENSUS_KINDS] = {
	"NOT DRAWN (no program, and no texture, or nothing of ours to draw it with, or it would read what it writes)", "drawn as the picture it is given (colour times texture)",
	"drawn through a table (ours, for the home indicator)", "drawn with our program for a colour matrix over what is behind",
	"drawn with our program for a colour matrix over premultiplied pixels",
	"drawn with our program that makes a picture eight times smaller",
	"drawn with our program that averages four samples around a point (a shadow's blur)",
	"made into a level of ours, half the size of the one before (a blur whose radius varies)",
	"drawn with our program that reads the levels by the mask (a blur whose radius varies)",
};
static int census_on;
static struct census {
	unsigned draws[CENSUS_KINDS], lately[CENSUS_KINDS];
	unsigned refused, refused_lately;
	uint8_t said;                   /* a bit for each kind met */
} census[90];
static uint64_t census_told;

static const char *
census_name(unsigned function)
{
	static const char names[] = REDWOOD_CA_FUNCTIONS;
	static char name[4];

	memcpy(name, function < 90 ? names + 4 * function : "?  ", 3);
	return name;
}

/* From inject.c: CoreAnimation asked whether the function it is about to use exists, and was told no. */
static uint16_t frame_refused[90], frame_noprog[90][CENSUS_KINDS];     /* (glframes=1) this frame's, by function */

void
redwood_gl_function_refused(unsigned function)
{
	if (frame_lines && function < 90) {
		frame_refused[function]++;
	}
	if (census_on && function < 90) {
		census[function].refused++;
		census[function].refused_lately++;
	}
}

static void
census_draw(struct eagl *e, unsigned function, unsigned kind)
{
	char textures[200] = "", numbers[200] = "";
	GLint source = 0, destination = 0, viewport[4] = { 0, 0, 0, 0 };
	float low[2] = { 0, 0 }, high[2] = { 0, 0 };
	size_t used = 0;

	if (frame_lines && function < 90) {
		frame_noprog[function][kind]++;
	}
	if (!census_on || function >= 90) {
		return;
	}
	census[function].draws[kind]++;
	census[function].lately[kind]++;
	if (census[function].said & (1u << kind)) {
		return;
	}
	census[function].said |= (uint8_t)(1u << kind);
	for (unsigned unit = 0; unit < 4; unit++) {
		GLint width = 0, height = 0, format = 0;

		if (e->bound[unit] == 0) {
			continue;
		}
		glActiveTexture(GL_TEXTURE0 + unit);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		used += (size_t)snprintf(textures + used, sizeof(textures) - used, " [%u] %d x %d (%#x)", unit, width, height,
		    (unsigned)format);
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	for (unsigned i = 0; i < 4 && e->arrays[0].pointer != NULL && e->arrays[0].type == GL_FLOAT && e->arrays[0].size >= 2; i++) {
		float place[2];

		memcpy(place, (const uint8_t *)e->arrays[0].pointer + i * (size_t)(e->arrays[0].stride ? e->arrays[0].stride :
		    4 * e->arrays[0].size), sizeof(place));
		for (unsigned c = 0; c < 2; c++) {
			low[c] = i == 0 || place[c] < low[c] ? place[c] : low[c];
			high[c] = i == 0 || place[c] > high[c] ? place[c] : high[c];
		}
	}
	used = 0;
	for (unsigned i = 0; i < function_given[function] && used < sizeof(numbers) - 16; i++) {
		used += (size_t)snprintf(numbers + used, sizeof(numbers) - used, " %.3g", (double)function_values[function][i]);
	}
	glGetIntegerv(GL_BLEND_SRC_RGB, &source);
	glGetIntegerv(GL_BLEND_DST_RGB, &destination);
	glGetIntegerv(GL_VIEWPORT, viewport);
	say("redwood-gl: census: function %u (\"%s\") with no program, %s: first at %g,%g to %g,%g of framebuffer %u (%d x %d); "
	    "textures%s; blending %s (%#x, %#x); its numbers:%s\n", function, census_name(function), census_kinds[kind], (double)low[0],
	    (double)low[1], (double)high[0], (double)high[1], e->framebuffer, viewport[2], viewport[3], textures[0] ? textures : " none",
	    glIsEnabled(GL_BLEND) ? "on" : "off", (unsigned)source, (unsigned)destination, numbers[0] ? numbers : " none");
}

/* Every ten seconds: what there was since the last time, and in all. */
/*
 * (glframes=1) after a frame's line, when it is not what the frame before had: the functions
 * CoreAnimation was refused in it, its draws with no program by function and kind (census_kinds[]),
 * and what was done with groups that hold a vibrant layer (group_draw_begin()).
 */
static void
frame_functions(struct eagl *e)
{
	static char said[600];
	char text[600] = "";
	size_t used = 0;

	for (unsigned f = 0; f < 90 && used < sizeof(text) - 40; f++) {
		if (frame_refused[f] != 0) {
			used += (size_t)snprintf(text + used, sizeof(text) - used, " refused %u %s x%u;", f, census_name(f), frame_refused[f]);
		}
		for (unsigned k = 0; k < CENSUS_KINDS; k++) {
			if (frame_noprog[f][k] != 0 && used < sizeof(text) - 40) {
				used += (size_t)snprintf(text + used, sizeof(text) - used, " %u %s kind %u x%u;", f, census_name(f), k,
				    frame_noprog[f][k]);
			}
		}
	}
	if (group_covered + group_laid + group_flattened != 0 && used < sizeof(text) - 60) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, " groups: %u covered, %u laid, %u flattened;", group_covered,
		    group_laid, group_flattened);
	}
	group_covered = group_laid = group_flattened = 0;
	if (used != 0 && strcmp(text, said) != 0) {
		say("redwood-gl: f %u functions:%s\n", e->frames, text);
	}
	memcpy(said, text, used + 1);
	bzero(frame_refused, sizeof(frame_refused));
	bzero(frame_noprog, sizeof(frame_noprog));
}

static void
census_report(void)
{
	char lately[900] = "", all[900] = "";
	size_t used_lately = 0, used_all = 0;
	uint64_t now = now_us();

	if (!census_on || now - census_told < 10000000) {
		return;
	}
	census_told = now;
	for (unsigned f = 0; f < 90; f++) {
		struct census *c = &census[f];
		unsigned drawn = 0, drawn_lately = 0;

		for (unsigned k = 0; k < CENSUS_KINDS; k++) {
			drawn += c->draws[k];
			drawn_lately += c->lately[k];
			c->lately[k] = 0;
		}
		if ((drawn_lately != 0 || c->refused_lately != 0) && used_lately < sizeof(lately) - 40) {
			used_lately += (size_t)snprintf(lately + used_lately, sizeof(lately) - used_lately, " %u %s x%u", f, census_name(f),
			    drawn_lately);
			if (c->refused_lately != 0) {
				used_lately += (size_t)snprintf(lately + used_lately, sizeof(lately) - used_lately, " (refused x%u)", c->refused_lately);
			}
		}
		if ((drawn != 0 || c->refused != 0) && used_all < sizeof(all) - 60) {
			used_all += (size_t)snprintf(all + used_all, sizeof(all) - used_all, " %u %s x%u", f, census_name(f), drawn);
			if (c->draws[CENSUS_NOT_DRAWN] != 0) {
				used_all += (size_t)snprintf(all + used_all, sizeof(all) - used_all, " (x%u not drawn)", c->draws[CENSUS_NOT_DRAWN]);
			}
			if (c->refused != 0) {
				used_all += (size_t)snprintf(all + used_all, sizeof(all) - used_all, " (refused x%u)", c->refused);
			}
		}
		c->refused_lately = 0;
	}
	if (lately[0] != 0) {
		say("redwood-gl: census: in the last ten seconds:%s\n", lately);
		say("redwood-gl: census: since the start:%s\n", all);
	}
}

/* A draw of "vrb" (function 39) made with our program over the chain: true if it can be. */
static bool
vblur_draw_begin(struct eagl *e, struct vblur_chain *chain, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static unsigned told;
	int levels = chain->made < 5 ? chain->made : 5;

	if (e->vblur == 0) {
		e->vblur = vblur_program(vblur_draw_vertex, vblur_fragment, "reading the levels by the mask");
		if (e->vblur == 0) {
			vblur_on = 0;                   /* (better the stand-in than nothing) */
			return false;
		}
		glUseProgram(e->vblur);
		glUniform1i(glGetUniformLocation(e->vblur, "level0"), 0);
		glUniform1i(glGetUniformLocation(e->vblur, "mask"), 1);
		for (int k = 1; k < 5; k++) {
			char name[8] = "levelN";

			name[5] = (char)('0' + k);
			glUniform1i(glGetUniformLocation(e->vblur, name), k + 1);
		}
		e->vblur_matrix = glGetUniformLocation(e->vblur, "vertex_matrix");
		e->vblur_radius = glGetUniformLocation(e->vblur, "radius");
		e->vblur_levels = glGetUniformLocation(e->vblur, "levels");
		e->vblur_normalize = glGetUniformLocation(e->vblur, "normalize_edges");
		e->vblur_paint = glGetUniformLocation(e->vblur, "paint");
	}
	glUseProgram(e->vblur);
	glUniformMatrix4fv(e->vblur_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
	glUniform1f(e->vblur_radius, function_values[FUNCTION_VARIABLE_BLUR][0]);
	glUniform1i(e->vblur_levels, levels);
	glUniform1f(e->vblur_normalize, function_values[FUNCTION_VARIABLE_BLUR][1]);
	glUniform1i(e->vblur_paint, vblur_on == 2);
	for (int k = 1; k < 5; k++) {
		glActiveTexture(GL_TEXTURE0 + k + 1);
		glBindTexture(GL_TEXTURE_2D, k < chain->made ? chain->level[k] : e->bound[0]);
	}
	glActiveTexture(GL_TEXTURE0 + e->unit);
	e->vblur_bound = true;
	for (GLuint i = 0; i < 4; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	if (told++ < 4) {
		const uint8_t *at = e->arrays[3].pointer;
		float first[2] = { 0, 0 };

		if (at != NULL && e->arrays[3].type == GL_FLOAT) {
			memcpy(first, at, sizeof(first));
		}
		say("redwood-gl: a blur whose radius varies, read by its mask: %d levels of texture %u (%d x %d), radius %g, numbers %g %g "
		    "%g; the mask's coordinates at the first corner %g %g\n", levels, chain->picture, chain->width, chain->height,
		    (double)function_values[FUNCTION_VARIABLE_BLUR][0], (double)function_values[FUNCTION_VARIABLE_BLUR][1],
		    (double)function_values[FUNCTION_VARIABLE_BLUR][2], (double)function_values[FUNCTION_VARIABLE_BLUR][3],
		    (double)first[0], (double)first[1]);
	}
	if (vblur_on == 2 && told <= 2) {
		/* the mask itself, for the log: a pixel in every twelve, blue green red opacity */
		uint8_t column[4 * 512];
		GLint height = 0;
		char said[1024];
		size_t used = 0;

		glActiveTexture(GL_TEXTURE0 + 1);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glActiveTexture(GL_TEXTURE0 + e->unit);
		if (height > 0 && height <= 512) {
			if (e->reader == 0) {
				glGenFramebuffers(1, &e->reader);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->bound[1], 0);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, 1, height, GL_BGRA, GL_UNSIGNED_BYTE, column);
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
			for (GLint y = 0; y < height && used < sizeof(said) - 32; y += 12) {
				used += (size_t)snprintf(said + used, sizeof(said) - used, " %d:%u,%u,%u,%u", y, column[4 * y], column[4 * y + 1],
				    column[4 * y + 2], column[4 * y + 3]);
			}
			say("redwood-gl: the mask of the blur whose radius varies (texture %u, 1 x %d), from its first row:%s (error %#x)\n",
			    e->bound[1], height, said, glGetError());
		}
	}
	return true;
}

/*
 * A vibrant layer inside a group. While a layer of Control Center fades in (its opacity below 1),
 * CoreAnimation draws it and what is in it into a picture of its own, a group, and lays that over
 * the screen at the layer's opacity; at 1 it draws them straight onto the screen. The round
 * buttons of the connectivity module and the "Not Playing" title are vibrant: function 45 ("vcm",
 * our program for a colour matrix over what is behind, which reads the picture it draws into),
 * and in a group what is behind them is the group's picture, cleared to nothing: the colour matrix
 * of nothing is nothing, and they were missing from the screen until the fade was over, the
 * buttons' discs and the title coming in at once some half a second after the rest.
 *
 * So a vibrant draw into a group that holds nothing yet keeps there what it covers (its layer's
 * opacity times its mask), and the matrix with it; when that group is laid over the screen (or
 * over another picture with something in it), our program puts the matrix over what is there
 * by that coverage times the group's opacity -- which is what the group would have given had it
 * been started from what is behind it, and what the draw at full opacity gives. A group that
 * holds a vibrant layer's coverage and is then drawn into, or read any other way, is first made
 * what it would have been (the colour matrix of nothing, by the coverage). A group laid into
 * another group that holds nothing yet passes its coverage on. Boot-arg glgroup=0: as before.
 */

static const char group_cover_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec4 texmat;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  result = color * texture(texture0, texcoord * texmat.xy + texmat.zw);\n"
    "}\n";
static const char group_over_fragment[] =
    "#version 300 es\n"
    "#extension GL_EXT_shader_framebuffer_fetch : require\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform highp vec4 texmat;\n"
    "uniform mediump vec4 colours[5];\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "layout(location = 0) inout highp vec4 result;\n"
    "void main() {\n"
    "  mediump vec4 layer = color * texture(texture0, texcoord * texmat.xy + texmat.zw);\n"
    "  mediump vec4 behind = result, s = behind;\n"
    "  s.rgb = s.rgb / max(s.a, .005);\n"
    "  s = s.r * colours[0] + s.g * colours[1] + s.b * colours[2] + s.a * colours[3] + colours[4];\n"
    "  s.a = clamp(s.a, 0., 1.);\n"
    "  s.rgb = clamp(s.rgb * s.a, 0., 1.);\n"
    "  result = mix(behind, s, layer.a);\n"
    "}\n";
static const char group_flat_fragment[] =
    "#version 300 es\n"
    "#extension GL_EXT_shader_framebuffer_fetch : require\n"
    "uniform mediump vec4 flat_colour;\n"
    "layout(location = 0) inout highp vec4 result;\n"
    "void main() {\n"
    "  result = flat_colour * result.a;\n"
    "}\n";

#ifndef GL_BLEND_SRC_ALPHA
#define GL_BLEND_SRC_ALPHA              0x80cb
#define GL_BLEND_DST_ALPHA              0x80ca
#endif
#define GL_ONE                          1
#define GL_ONE_MINUS_SRC_ALPHA          0x0303

static GLfloat
clamp01(GLfloat value)
{
	return value < 0 ? 0 : value > 1 ? 1 : value;
}

/* The state of a texture of CoreAnimation's own (not one that stands for an IOSurface), or NULL. */
static struct group_texture *
group_of(struct eagl *e, GLuint texture)
{
	if (!group_vibrant_on || texture == 0 || texture >= GROUP_TEXTURES || image_of(e, texture) != NULL) {
		return NULL;
	}
	if (e->groups == NULL && (e->groups = calloc(GROUP_TEXTURES, sizeof(*e->groups))) == NULL) {
		return NULL;
	}
	return &e->groups[texture];
}

/* Our programs for it, made the first time. */
static bool
group_programs(struct eagl *e)
{
	GLint good = 0, good2 = 0, good3 = 0;

	if (e->group_cover != 0 || e->group_failed) {
		return !e->group_failed;
	}
	e->group_cover = plain_program(group_cover_fragment, "a vibrant layer's coverage, in a group");
	e->group_over = plain_program(group_over_fragment, "a group's vibrant layer over what is behind it");
	e->group_flat = plain_program(group_flat_fragment, "a group's vibrant layer as it would have been");
	glGetProgramiv(e->group_cover, GL_LINK_STATUS, &good);
	glGetProgramiv(e->group_over, GL_LINK_STATUS, &good2);
	glGetProgramiv(e->group_flat, GL_LINK_STATUS, &good3);
	e->group_failed = !good || !good2 || !good3;
	e->group_cover_matrix = glGetUniformLocation(e->group_cover, "vertex_matrix");
	e->group_cover_texmat = glGetUniformLocation(e->group_cover, "texmat");
	e->group_over_matrix = glGetUniformLocation(e->group_over, "vertex_matrix");
	e->group_over_texmat = glGetUniformLocation(e->group_over, "texmat");
	e->group_over_colours = glGetUniformLocation(e->group_over, "colours");
	e->group_flat_matrix = glGetUniformLocation(e->group_flat, "vertex_matrix");
	e->group_flat_colour = glGetUniformLocation(e->group_flat, "flat_colour");
	return !e->group_failed;
}

/*
 * A group that holds a vibrant layer's coverage made what CoreAnimation drew into it: the colour
 * matrix of nothing (its numbers added, opacity and all), by the coverage. Drawn through a
 * framebuffer of ours, the whole texture; everything of CoreAnimation's put back after.
 */
static void
group_flatten(struct eagl *e, GLuint texture, struct group_texture *g)
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static const GLfloat corners[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLint view[4], box[4], width = 0, height = 0, enabled[3];
	GLboolean scissored, blending;
	GLfloat flat[4], alpha = clamp01(g->colours[19]);

	g->state = GROUP_NONE;
	group_flattened++;
	flat[3] = alpha;
	for (int i = 0; i < 3; i++) {
		flat[i] = clamp01(g->colours[16 + i] * alpha);
	}
	glGetIntegerv(GL_VIEWPORT, view);
	glGetIntegerv(GL_SCISSOR_BOX, box);
	scissored = glIsEnabled(GL_SCISSOR_TEST);
	blending = glIsEnabled(GL_BLEND);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glBindTexture(GL_TEXTURE_2D, e->bound[0]);
	glActiveTexture(GL_TEXTURE0 + e->unit);
	if (e->reader == 0) {
		glGenFramebuffers(1, &e->reader);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	if (flat[3] == 0) {
		GLfloat colour[4];

		/* (nothing by any coverage is nothing: the numbers add no opacity, as they do not in Control Center) */
		glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glClearColor(colour[0], colour[1], colour[2], colour[3]);
	} else if (group_programs(e)) {
		glDisable(GL_BLEND);
		glUseProgram(e->group_flat);
		glUniformMatrix4fv(e->group_flat_matrix, 1, 0, identity);
		glUniform4fv(e->group_flat_colour, 1, flat);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		}
		glDisableVertexAttribArray(1);
		glDisableVertexAttribArray(2);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, corners);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glVertexAttribPointer(0, e->arrays[0].size, e->arrays[0].type, 0, e->arrays[0].stride, e->arrays[0].pointer);
		for (GLuint i = 0; i < 3; i++) {
			(enabled[i] ? glEnableVertexAttribArray : glDisableVertexAttribArray)(i);
		}
		glUseProgram(e->program);
		(blending ? glEnable : glDisable)(GL_BLEND);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	glViewport(view[0], view[1], view[2], view[3]);
	glScissor(box[0], box[1], box[2], box[3]);
	(scissored ? glEnable : glDisable)(GL_SCISSOR_TEST);
}

/* A texture's contents are made anew (an upload, a copy into it): whatever it held is gone. */
static void
group_forget(struct eagl *e, GLuint texture)
{
	struct group_texture *g = e->groups != NULL && texture < GROUP_TEXTURES ? &e->groups[texture] : NULL;

	if (g != NULL) {
		g->state = GROUP_NONE;
	}
}

/* A texture is about to be read other than by a draw that lays a group over a picture: it is made what it would have been. */
static void
group_read(struct eagl *e, GLuint texture)
{
	struct group_texture *g = e->groups != NULL && texture != 0 && texture < GROUP_TEXTURES ? &e->groups[texture] : NULL;

	if (g != NULL && g->state == GROUP_VIBRANT) {
		group_flatten(e, texture, g);
	}
}

/* CoreAnimation's own uniform of its program in use, by name (a matrix of 16 numbers, or a vector of 4); false if it has none. */
static bool
group_uniform(struct eagl *e, const char *name, GLfloat *values)
{
	GLint location = glGetUniformLocation(e->program, name);

	if (location < 0) {
		return false;
	}
	glGetUniformfv(e->program, location, values);
	return true;
}

/*
 * Before a draw: is it one of the above, and if so, set up for it (the draw itself is CoreAnimation's,
 * with its arrays). Returns 0 for a draw left as it is, 1 for one of ours (group_draw_end() after it).
 */
static int
group_draw_begin(struct eagl *e, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static uint8_t said[90];
	unsigned function = ca_context != NULL ? *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10) : 90;
	struct group_texture *into = group_of(e, e->colour), *from = NULL;
	const GLfloat *colours = NULL;
	GLfloat matrix[16], texmat[4] = { 1, 1, 0, 0 };
	bool cover;

	if (!group_vibrant_on || e->colour == 0 || e->framebuffer == 0) {
		return 0;
	}
	if (e->program == 0 && function == FUNCTION_VIBRANT_MATRIX && fetch_offered && function_given[function] == 20 &&
	    e->bound[0] != 0 && e->bound[0] != e->colour && into != NULL && (into->state == GROUP_FRESH ||
	    (into->state == GROUP_VIBRANT && memcmp(into->colours, function_values[function], sizeof(into->colours)) == 0))) {
		/* a vibrant layer drawn into a group with nothing in it: its coverage */
		GLint width = 1, height = 1;

		glActiveTexture(GL_TEXTURE0);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glActiveTexture(GL_TEXTURE0 + e->unit);
		texmat[0] = 1.f / (GLfloat)(width > 0 ? width : 1);
		texmat[1] = 1.f / (GLfloat)(height > 0 ? height : 1);
		memcpy(matrix, e->matrix != NULL ? e->matrix : identity, sizeof(matrix));
		colours = function_values[function];
		cover = true;
	} else if (e->program != 0 && e->bound[0] != 0 && e->bound[0] != e->colour && (from = group_of(e, e->bound[0])) != NULL &&
	    from->state == GROUP_VIBRANT) {
		/* a group that holds a vibrant layer laid over something */
		bool laid = function == 1 || function == 3;    /* ("img", "cim": the picture as it is, by the colour) */

		if (function < 90 && !said[function]) {
			said[function] = 1;
			say("redwood-gl: a group that holds a vibrant layer is drawn with CoreAnimation's function %u (\"%s\"): %s\n",
			    function, census_name(function), laid ? "the layer over what is behind it" : "made what it would have been");
		}
		if (!laid || !group_uniform(e, "vertex_matrix", matrix)) {
			group_flatten(e, e->bound[0], from);
			from = NULL;
		} else {
			if (!group_uniform(e, "texmat0", texmat)) {
				texmat[0] = texmat[1] = 1;
			}
			colours = from->colours;
			cover = into != NULL && (into->state == GROUP_FRESH ||
			    (into->state == GROUP_VIBRANT && memcmp(into->colours, colours, sizeof(into->colours)) == 0));
		}
	}
	if (colours == NULL || !group_programs(e)) {
		/* not one of ours: what it reads is what CoreAnimation drew, and what it draws into is no longer a
		 * vibrant layer's coverage, nor empty */
		for (unsigned unit = 0; unit < 4 && e->groups != NULL; unit++) {
			group_read(e, e->bound[unit]);
		}
		if (into != NULL) {
			if (into->state == GROUP_VIBRANT) {
				group_flatten(e, e->colour, into);
			}
			into->state = GROUP_NONE;
		}
		return 0;
	}
	clear_now(e);
	if (cover) {
		glUseProgram(e->group_cover);
		glUniformMatrix4fv(e->group_cover_matrix, 1, 0, matrix);
		glUniform4fv(e->group_cover_texmat, 1, texmat);
		e->group_blend = glIsEnabled(GL_BLEND);
		glGetIntegerv(GL_BLEND_SRC_RGB, &e->group_blend_func[0]);
		glGetIntegerv(GL_BLEND_DST_RGB, &e->group_blend_func[1]);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &e->group_blend_func[2]);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &e->group_blend_func[3]);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		if (into->state != GROUP_VIBRANT) {
			memcpy(into->colours, colours, sizeof(into->colours));
		}
		into->state = GROUP_VIBRANT;
		group_covered++;
	} else {
		glUseProgram(e->group_over);
		glUniformMatrix4fv(e->group_over_matrix, 1, 0, matrix);
		glUniform4fv(e->group_over_texmat, 1, texmat);
		glUniform4fv(e->group_over_colours, 5, colours);
		e->group_blend = glIsEnabled(GL_BLEND);
		glGetIntegerv(GL_BLEND_SRC_RGB, &e->group_blend_func[0]);
		glGetIntegerv(GL_BLEND_DST_RGB, &e->group_blend_func[1]);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &e->group_blend_func[2]);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &e->group_blend_func[3]);
		glDisable(GL_BLEND);            /* (the program blends) */
		if (into != NULL) {
			if (into->state == GROUP_VIBRANT) {
				group_flatten(e, e->colour, into);
			}
			into->state = GROUP_NONE;
		}
		group_laid++;
	}
	for (GLuint i = 0; i < 3; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	return 1;
}

static void
group_draw_end(struct eagl *e, const GLint enabled[4])
{
	for (GLuint i = 0; i < 3; i++) {
		if (!enabled[i]) {
			glDisableVertexAttribArray(i);
		}
	}
	(e->group_blend ? glEnable : glDisable)(GL_BLEND);
	glBlendFuncSeparate((GLenum)e->group_blend_func[0], (GLenum)e->group_blend_func[1], (GLenum)e->group_blend_func[2],
	    (GLenum)e->group_blend_func[3]);
	glUseProgram(e->program);
}

static bool
plain_draw_begin(struct eagl *e, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	static uint8_t said[90];
	unsigned function = ca_context != NULL ? *(*(uint8_t **)((char *)ca_context + 0x10) + 0x10) : 90;      /* (90: not known yet) */
	GLint rows = 0;
	GLuint program;

	if (e->program == 0) {
		e->line.vrd += function == FUNCTION_VARIABLE_DOWNSAMPLE;
		e->line.vrb += function == FUNCTION_VARIABLE_BLUR;
	}

	if (e->unclear != 0 && reads_what_it_writes(e)) {
		e->unclear = 0;
	}
	clear_now(e);
	if (e->program != 0) {
		return false;
	}
	if (vblur_on && reads_what_it_writes(e) && function_given[FUNCTION_VARIABLE_DOWNSAMPLE] != 0) {
		static unsigned told;

		vblur_level(e, (int)function_values[FUNCTION_VARIABLE_DOWNSAMPLE][0]);
		census_draw(e, function, CENSUS_LEVEL);
		if (told++ < 8) {
			say("redwood-gl: a blur whose radius varies: level %d of texture %u made of level %d\n",
			    (int)function_values[FUNCTION_VARIABLE_DOWNSAMPLE][0] + 1, e->bound[0],
			    (int)function_values[FUNCTION_VARIABLE_DOWNSAMPLE][0]);
		}
		return false;
	}
	/*
	 * Nor is any other draw with no program that would read the very texture it draws into. The
	 * window server's snapshots end with one: function 61 ("edr", by its numbers the display's
	 * headroom, infinite: nothing to change) over the whole picture, with that picture as its
	 * texture. Drawn as the picture it is given, it read what the GPU had not yet written out of
	 * its own memory -- nothing -- and wrote that over everything: every launch image SpringBoard
	 * keeps came out black, and with it the zoom that opens an application the first time.
	 */
	if (!plain_draws || (plain_draws == 1 && e->bound[0] == 0) || reads_what_it_writes(e) ||
	    (e->colour != 0 && e->colour == e->bound[0])) {
		census_draw(e, function, CENSUS_NOT_DRAWN);
		return false;
	}
	if (function < 90 && !said[function]) {
		said[function] = 1;
		say("redwood-gl: a draw with no program is CoreAnimation's function %u (\"%s\")\n", function, census_name(function));
	}
	if ((int)function == trace_function && --trace_function_after == 0) {
		trace_function_met = 1;
	}
	e->blended = 0;
	if (e->bound[0] != 0 && plain_draws != 3 && function < 90 && ((function == FUNCTION_VIBRANT_MATRIX && fetch_offered) ||
	    function == FUNCTION_MATRIX_PREMULTIPLIED) && function_given[function] == 20) {
		bool vibrant = function == FUNCTION_VIBRANT_MATRIX;

		if (vibrant && e->vibrant == 0) {
			GLint good = 0;

			e->vibrant = plain_program(vibrant_fragment, "a colour matrix over what is behind");
			glGetProgramiv(e->vibrant, GL_LINK_STATUS, &good);
			e->no_vibrant = !good;                  /* (a library without the extension: Mesa's software one) */
			e->vibrant_matrix = glGetUniformLocation(e->vibrant, "vertex_matrix");
			e->vibrant_colours = glGetUniformLocation(e->vibrant, "colours");
		} else if (!vibrant && e->masking == 0) {
			e->masking = plain_program(masking_fragment, "a colour matrix over premultiplied pixels");
			e->masking_matrix = glGetUniformLocation(e->masking, "vertex_matrix");
			e->masking_colours = glGetUniformLocation(e->masking, "colours");
		}
		if (vibrant && e->no_vibrant) {
			census_draw(e, function, CENSUS_NOT_DRAWN);     /* better nothing than the mask as a picture */
			return false;
		}
		census_draw(e, function, vibrant ? CENSUS_VIBRANT : CENSUS_MASKING);
		program = vibrant ? e->vibrant : e->masking;
		glUseProgram(program);
		glUniformMatrix4fv(vibrant ? e->vibrant_matrix : e->masking_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
		glUniform4fv(vibrant ? e->vibrant_colours : e->masking_colours, 5, function_values[function]);
		if (vibrant && glIsEnabled(GL_BLEND)) {
			e->blended = 1;                 /* the program has blended already */
			glDisable(GL_BLEND);
		}
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
			if (!enabled[i]) {
				glEnableVertexAttribArray(i);
			}
		}
		return true;
	}
	if (vblur_on && e->bound[0] != 0 && e->bound[1] != 0 && function == FUNCTION_VARIABLE_BLUR && function_given[function] >= 2) {
		struct vblur_chain *chain = vblur_chain_of(e, e->bound[0], false);

		if (chain != NULL && chain->made > 0 && vblur_draw_begin(e, chain, enabled)) {
			census_draw(e, function, CENSUS_VBLUR);
			return true;
		}
	}
	if (e->bound[0] != 0 && plain_draws != 3 && function == FUNCTION_SHADOW_BLUR && function_given[function] == 8) {
		if (e->blur4 == 0) {
			e->blur4 = plain_program(blur4_fragment, "four samples around a point");
			e->blur4_matrix = glGetUniformLocation(e->blur4, "vertex_matrix");
			e->blur4_places = glGetUniformLocation(e->blur4, "places");
		}
		census_draw(e, function, CENSUS_BLUR4);
		glUseProgram(e->blur4);
		glUniformMatrix4fv(e->blur4_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
		glUniform2fv(e->blur4_places, 4, function_values[function]);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
			if (!enabled[i]) {
				glEnableVertexAttribArray(i);
			}
		}
		return true;
	}
	if (e->bound[0] != 0 && plain_draws != 3 && function == FUNCTION_DOWNSAMPLE_8 && function_given[function] == 8) {
		if (e->box == 0) {
			e->box = plain_program(box_fragment, "eight times smaller, sixteen samples");
			e->box_matrix = glGetUniformLocation(e->box, "vertex_matrix");
			e->box_places = glGetUniformLocation(e->box, "places");
		}
		census_draw(e, function, CENSUS_BOX);
		glUseProgram(e->box);
		glUniformMatrix4fv(e->box_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
		glUniform2fv(e->box_places, 4, function_values[function]);
		for (GLuint i = 0; i < 3; i++) {
			glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
			if (!enabled[i]) {
				glEnableVertexAttribArray(i);
			}
		}
		return true;
	}
	if (e->bound[0] != 0 && e->bound[1] != 0) {
		glActiveTexture(GL_TEXTURE0 + 1);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &rows);
		glActiveTexture(GL_TEXTURE0 + e->unit);
	}
	if (rows == 1) {
		if (e->lookup == 0) {
			e->lookup = plain_program(table_fragment, "through a table");
			e->lookup_matrix = glGetUniformLocation(e->lookup, "vertex_matrix");
			glUseProgram(e->lookup);
			glUniform1i(glGetUniformLocation(e->lookup, "table"), 1);
		}
		program = e->lookup;
	} else {
		if (e->plain == 0) {
			e->plain = plain_program(plain_fragment, "as they are");
			e->plain_matrix = glGetUniformLocation(e->plain, "vertex_matrix");
			e->plain_textured = glGetUniformLocation(e->plain, "textured");
		}
		program = e->plain;
	}
	census_draw(e, function, rows == 1 ? CENSUS_TABLE : CENSUS_PLAIN);
	glUseProgram(program);
	glUniformMatrix4fv(program == e->plain ? e->plain_matrix : e->lookup_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
	if (program == e->plain) {
		glUniform1f(e->plain_textured, e->bound[0] != 0 ? 1 : 0);
	}
	for (GLuint i = 0; i < 3; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	if (eagl_tracing || e->plain_said < 4) {
		const GLfloat *position = e->arrays[0].pointer, *where = e->arrays[2].pointer;

		e->plain_said++;
		say("redwood-gl: a draw with no program, into framebuffer %u: texture %u on the first unit%s; arrays %d %d %d; first "
		    "corner %g %g, texture coordinates %g %g\n", e->framebuffer, e->bound[0], rows == 1 ? ", a table on the second" : "",
		    enabled[0], enabled[1], enabled[2], position ? (double)position[0] : 0, position ? (double)position[1] : 0,
		    where ? (double)where[0] : 0, where ? (double)where[1] : 0);
	}
	return true;
}

static void
plain_draw_end(struct eagl *e, const GLint enabled[4])
{
	for (GLuint i = 0; i < 4; i++) {
		if (!enabled[i]) {
			glDisableVertexAttribArray(i);
		}
	}
	if (e->vblur_bound) {
		/* the textures of units 2 to 5 back as CoreAnimation had them */
		for (unsigned k = 2; k < 6; k++) {
			glActiveTexture(GL_TEXTURE0 + k);
			glBindTexture(GL_TEXTURE_2D, e->bound[k]);
		}
		glActiveTexture(GL_TEXTURE0 + e->unit);
		e->vblur_bound = false;
	}
	glUseProgram(0);
	if (e->blended) {
		glEnable(GL_BLEND);
		e->blended = 0;
	}
}

static void
my_glUseProgram(void *context, GLuint program)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x12c8);
	if (eagl_tracing) {
		eagl_trace("glUseProgram(%u)\n", program);
	}
	e->program = program;
	glUseProgram(program);
	EAGL_LEAVE(e, 0x12c8);
}

static void
my_glActiveTexture(void *context, GLenum unit)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0xab8);
	if (eagl_tracing) {
		eagl_trace("glActiveTexture(%#x)\n", unit);
	}
	e->unit = unit - 0x84c0;
	glActiveTexture(unit);
	EAGL_LEAVE(e, 0xab8);
}

static void
my_glUniformMatrix4fv(void *context, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1368);
	if (eagl_tracing) {
		eagl_trace("glUniformMatrix4fv(%d, %d, %d, %p)\n", location, count, transpose, (const void *)value);
		eagl_trace_floats(value, 16);
	}
	e->matrix = value;
	glUniformMatrix4fv(location, count, transpose, value);
	EAGL_LEAVE(e, 0x1368);
}

static void
my_glVertexAttribPointer(void *context, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
    const void *pointer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1000);
	if (eagl_tracing) {
		eagl_trace("glVertexAttribPointer(%u, %d, %#x, %d, %d, %p)\n", index, size, type, normalized, stride, pointer);
	}
	if (index < 8) {
		e->arrays[index].pointer = pointer;
		e->arrays[index].stride = stride;
		e->arrays[index].size = size;
		e->arrays[index].type = type;
	}
	glVertexAttribPointer(index, size, type, normalized, stride, pointer);
	EAGL_LEAVE(e, 0x1000);
}

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
	struct eagl *e = context;
	GLint enabled[4] = { 1, 1, 1, 1 };
	bool plain;

	EAGL_ENTER(e, 0x210);
	drawing(e);
	count_spans(e, mode, count);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawArrays(%#x, %d, %d)\n", mode, first, count);
		trace_draw(e, 0, NULL, first, count);
	}
	if (trace_placing) {
		trace_placed_draw(e, mode, 0, NULL, first, count);
	}
	if (skipping != 0 && skip_draw(e)) {
		/* glskip=N: nothing is drawn, so that what this program puts there can be seen missing */
	} else if (group_draw_begin(e, enabled)) {
		glDrawArrays(mode, first, count);
		group_draw_end(e, enabled);
	} else {
		plain = plain_draw_begin(e, enabled);
		glDrawArrays(mode, first, count);
		if (plain) {
			plain_draw_end(e, enabled);
		}
	}
	if (trace_listed) {
		trace_placed_look(e);
	}
	EAGL_LEAVE(e, 0x210);
	if (eagl_tracing) {
		trace_pixels(e);
	}
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	struct eagl *e = context;
	GLint enabled[4] = { 1, 1, 1, 1 };
	bool plain;

	EAGL_ENTER(e, 0x220);
	drawing(e);
	count_spans(e, mode, count);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawElements(%#x, %d, %#x, %p)\n", mode, count, type, indices);
		trace_draw(e, type, indices, 0, count);
	}
	if (trace_placing) {
		trace_placed_draw(e, mode, type, indices, 0, count);
	}
	if (skipping != 0 && skip_draw(e)) {
		/* glskip=N: see my_glDrawArrays */
	} else if (group_draw_begin(e, enabled)) {
		glDrawElements(mode, count, type, indices);
		group_draw_end(e, enabled);
	} else {
		plain = plain_draw_begin(e, enabled);
		glDrawElements(mode, count, type, indices);
		if (plain) {
			plain_draw_end(e, enabled);
		}
	}
	if (trace_listed) {
		trace_placed_look(e);
	}
	EAGL_LEAVE(e, 0x220);
	if (eagl_tracing) {
		trace_pixels(e);
	}
}

static void
my_glFinish(void *context)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2d0);
	if (eagl_tracing) {
		eagl_trace("glFinish()\n");
	}
	clear_now(e);
	glFinish();
	frame_end(e, true);
	EAGL_LEAVE(e, 0x2d0);
}

static void
my_glFlush(void *context)
{
	struct eagl *e = context;
	uint64_t began;

	EAGL_ENTER(e, 0x2d8);
	if (eagl_tracing) {
		eagl_trace("glFlush()\n");
	}
	clear_now(e);
	began = now_us();
	glFlush();
	e->flushing += now_us() - began;
	frame_end(e, false);
	EAGL_LEAVE(e, 0x2d8);
}

/* APPLE_fence: wait for one object to be done with. Everything is done with after glFinish. */
static void
my_glFinishObjectAPPLE(void *context, GLenum object, GLint name)
{
	struct eagl *e = context;

	(void)object;
	(void)name;
	EAGL_ENTER(e, 0xeb8);
	clear_now(e);
	glFinish();
	frame_end(e, true);
	EAGL_LEAVE(e, 0xeb8);
}

static void
my_glDeleteTextures(void *context, GLsizei n, const GLuint *textures)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1e0);
	clear_now(e);
	for (GLsizei i = 0; i < n; i++) {
		image_forget(e, textures[i]);
		vblur_forget(e, textures[i]);
		group_forget(e, textures[i]);
		if (textures[i] == e->colour) {
			e->colour = 0;          /* (a texture that is deleted leaves the framebuffer bound) */
		}
	}
	glDeleteTextures(n, textures);
	EAGL_LEAVE(e, 0x1e0);
}

static const GLubyte *
my_glGetString(void *context, GLenum name)
{
	struct eagl *e = context;
	const GLubyte *string;

	EAGL_ENTER(e, 0x3b0);
	/* 0x8a1e is no name of OpenGL ES's; iOS's library answers it (the constructor of CA::OGL::GLESContext asks) */
	string = name == 0x8a1e ? (const GLubyte *)"" : glGetString(name);
	if (name == GL_EXTENSIONS && string != NULL && !fetch_offered) {
		static char without[8192];      /* (the list less that extension: see fetch_offered) */
		size_t used = 0;

		for (const char *word = (const char *)string; *word != 0;) {
			size_t length = strcspn(word, " ");

			if (strncmp(word, "GL_EXT_shader_framebuffer_fetch", 31) != 0 && used + length + 2 < sizeof(without)) {
				memcpy(without + used, word, length);
				used += length;
				without[used++] = ' ';
			}
			word += length + (word[length] == ' ');
		}
		without[used] = 0;
		string = (const GLubyte *)without;
	}
	/* The list of extensions is longer than a line of the log, and it was being cut at 900
	   characters -- which is where one looks to see whether the driver takes a compressed
	   texture of iOS's kind (ASTC). In pieces, then, so that none of it is lost. */
	if (name == GL_EXTENSIONS && string != NULL) {
		size_t length = strlen((const char *)string);

		say("redwood-gl: asked for string %#x, %zu characters:\n", name, length);
		for (size_t at = 0; at < length; at += 700) {
			say("redwood-gl:  %.700s\n", (const char *)string + at);
		}
	} else {
		say("redwood-gl: asked for string %#x: %.900s\n", name, string ? (const char *)string : "(none)");
	}
	EAGL_LEAVE(e, 0x3b0);
	return string;
}

static void
my_glGetIntegerv(void *context, GLenum name, GLint *values)
{
	struct eagl *e = context;
	static unsigned said;

	EAGL_ENTER(e, 0x348);
	glGetIntegerv(name, values);
	if (said < 40) {
		said++;
		say("redwood-gl: asked for number %#x: %d\n", name, values[0]);
	}
	EAGL_LEAVE(e, 0x348);
}

static void
my_glTexImage2D(void *context, GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x970);
	if (level == 0) {
		note_format(pixels ? "filled" : "empty", (GLenum)internal, format, type, width, height);
	}
	if (eagl_tracing) {
		eagl_trace("glTexImage2D(%#x, %d, %#x, %d, %d, %d, %#x, %#x, %p)\n", target, level, internal, width, height, border,
		    format, type, pixels);
		if (pixels != NULL && type == 0x140b && format == GL_RGBA) {            /* half floats: the first and the last pixel */
			const uint16_t *half = pixels;

			eagl_trace("    half floats: %#x %#x %#x %#x ... %#x %#x %#x %#x\n", half[0], half[1], half[2], half[3],
			    half[4 * (width - 1)], half[4 * (width - 1) + 1], half[4 * (width - 1) + 2], half[4 * (width - 1) + 3]);
		}
	}
	clear_now(e);
	if (target == GL_TEXTURE_2D && e->unit < 8) {
		group_forget(e, e->bound[e->unit]);
	}
	uint64_t began = slow_calls_us != 0 ? now_us() : 0;
	glTexImage2D(target, level, (GLint)internal_format((GLenum)internal, format), width, height, border, format, type, pixels);
	if (began != 0 && now_us() - began >= slow_calls_us) {
		/* (with glslow=US: which pictures take long to hand over) */
		say("tl: slow-tex %dx%d level %d format %#x type %#x%s %llu at %llu\n", width, height, level, format, type,
		    pixels ? "" : " (empty)", now_us() - began, began);
	}
	EAGL_LEAVE(e, 0x970);
}

/* An upload into part of a texture: a group with a vibrant layer in it is made what it would have been first. */
static void
my_glTexSubImage2D(void *context, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
    GLenum type, const void *pixels)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x9a0);
	if (eagl_tracing) {
		eagl_trace("glTexSubImage2D(%#x, %d, %d, %d, %d, %d, %#x, %#x, %p)\n", target, level, x, y, width, height, format, type,
		    pixels);
	}
	clear_now(e);
	if (target == GL_TEXTURE_2D && e->unit < 8 && e->groups != NULL) {
		group_read(e, e->bound[e->unit]);
		group_forget(e, e->bound[e->unit]);
	}
	glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
	EAGL_LEAVE(e, 0x9a0);
}

/* A copy of what is drawn into a texture: both made what they would have been first (see group_draw_begin()). */
static void
my_glCopyTexSubImage2D(void *context, GLenum target, GLint level, GLint x0, GLint y0, GLint x, GLint y, GLsizei width,
    GLsizei height)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1c8);
	if (eagl_tracing) {
		eagl_trace("glCopyTexSubImage2D(%#x, %d, %d, %d, %d, %d, %d, %d)\n", target, level, x0, y0, x, y, width, height);
	}
	clear_now(e);
	if (e->groups != NULL) {
		group_read(e, e->colour);
		if (target == GL_TEXTURE_2D && e->unit < 8) {
			group_read(e, e->bound[e->unit]);
			group_forget(e, e->bound[e->unit]);
		}
	}
	glCopyTexSubImage2D(target, level, x0, y0, x, y, width, height);
	EAGL_LEAVE(e, 0x1c8);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

/* Where the shared cache was built to be, before it was moved (as src/inject.c has it). */
#define CACHE_BASE 0x180000000UL

/* The callers of this call, as they would be in the cache as built: "ipsw a2s" names them. */
static void
say_callers(const char *what)
{
	uintptr_t callers[5], slide;
	const void *cache;
	size_t length = 0;
	char line[5 * 20 + 1] = "";
	size_t used = 0;

	cache = _dyld_get_shared_cache_range(&length);
	if (cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	callers[0] = (uintptr_t)__builtin_return_address(0);
	callers[1] = (uintptr_t)__builtin_return_address(1);
	callers[2] = (uintptr_t)__builtin_return_address(2);
	callers[3] = (uintptr_t)__builtin_return_address(3);
	callers[4] = (uintptr_t)__builtin_return_address(4);
	for (unsigned i = 0; i < sizeof(callers) / sizeof(callers[0]); i++) {
		if (callers[i] < slide + CACHE_BASE || callers[i] >= slide + CACHE_BASE + length) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " (not the cache)");
			continue;
		}
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %#lx", callers[i] - slide);
	}
	say("redwood-gl: %s, asked for by%s\n", what, line);
}

/*
 * A coarse look at a picture that arrives as ASTC 4x4, without asking the GPU for anything: a block
 * of it that holds one colour for all its sixteen pixels ("a void extent") carries that colour in
 * its last eight bytes, two bytes a channel, and an encoder writes those wherever the picture is
 * flat. Walking a grid of blocks and printing the top byte of red, green and blue of each therefore
 * shows the picture where it is flat and "??" where it is not -- enough to tell what the picture is.
 */
static void
astc_thumbnail(GLenum internal, GLsizei width, GLsizei height, GLsizei size, const void *data)
{
	const uint8_t *bytes = data;
	unsigned across, down, flat = 0, seen = 0;

	if (internal != 0x93b0 || data == NULL || width <= 0 || height <= 0) {
		return;                         /* only the four-by-four kind, and only with its bytes */
	}
	across = ((unsigned)width + 3) / 4;
	down = ((unsigned)height + 3) / 4;
	if ((size_t)size < (size_t)across * down * 16) {
		return;
	}
	for (unsigned row = 0; row < 16; row++) {
		char line[24 * 7 + 1] = "";
		size_t used = 0;
		unsigned by = down * row / 16;

		for (unsigned column = 0; column < 24; column++) {
			unsigned bx = across * column / 24;
			const uint8_t *block = bytes + 16 * ((size_t)by * across + bx);
			unsigned mode = block[0] | ((unsigned)block[1] << 8);

			seen++;
			if ((mode & 0x1ff) == 0x1fc) {
				flat++;
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x%02x%02x",
				    block[9], block[11], block[13]);
			} else {
				used += (size_t)snprintf(line + used, sizeof(line) - used, " ......");
			}
		}
		say("redwood-gl: astc %u x %u, row %u:%s\n", across, down, by, line);
	}
	say("redwood-gl: astc %u x %u: %u of the %u blocks looked at hold one colour\n", across, down, flat, seen);
}

static void
my_glCompressedTexImage2D(void *context, GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
    GLint border, GLsizei size, const void *data)
{
	struct eagl *e = context;

	static unsigned told;

	EAGL_ENTER(e, 0xbe8);
	if (level == 0) {
		note_format("compressed", internal, 0, 0, width, height);
	}
	if (told < 40) {
		(void)glGetError();             /* so that what is left behind is this call's own */
	}
	glCompressedTexImage2D(target, level, internal, width, height, border, size, data);
	/*
	 * A format the driver has not got is refused here, and then the texture stays undefined and
	 * is read as whatever the driver leaves in it -- magenta, with some. The first of these are
	 * written down with what the call left behind, because the errors of every call are only
	 * asked for over the first six hundred frames (see eagl_leave) and the pictures of the
	 * switcher arrive long after that.
	 */
	if (told < 40) {
		GLenum error = glGetError();
		char head[3 * 32 + 1] = "";

		/*
		 * The first blocks, as bytes. A block of ASTC says in its first eleven bits how it is
		 * laid out, and whether it is one of the kinds that only a decoder with the HDR part of
		 * the extension can read -- the driver here has GL_KHR_texture_compression_astc_ldr and
		 * not the HDR one, and an LDR decoder hands back magenta for a block it cannot read.
		 */
		for (unsigned i = 0; data != NULL && i < 32 && (GLsizei)i < size; i++) {
			snprintf(head + 3 * i, 4, " %02x", ((const uint8_t *)data)[i]);
		}
		told++;
		say("redwood-gl: a compressed picture into texture %u: internal format %#x, %d x %d, "
		    "level %d, %d bytes%s, it begins%s\n", e->bound[e->unit], internal, width, height,
		    level, size, error == 0 ? "" : error == 0x500 ? " -- refused, no such format" : " -- refused",
		    head);
		astc_thumbnail(internal, width, height, size, data);
		say_callers("that compressed picture");
	}
	EAGL_LEAVE(e, 0xbe8);
}

static void
my_glBindFramebuffer(void *context, GLenum target, GLuint framebuffer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1508);
	if (eagl_tracing) {
		eagl_trace("glBindFramebuffer(%#x, %u)\n", target, framebuffer);
	}
	clear_now(e);                   /* (a clear put off is of what was drawn into until now) */
	glBindFramebuffer(target, framebuffer);
	if (target == GL_READ_FRAMEBUFFER) {
		EAGL_LEAVE(e, 0x1508);
		return;                 /* (not what is drawn into) */
	}
	/*
	 * What is drawn into from here on is asked of the library, not kept by the framebuffer's
	 * name: CoreAnimation makes a framebuffer for every frame and the names do not come back,
	 * so there is no end of them (a table of 4096 was past its end six minutes into a tour,
	 * and from then on no frame was known to have been drawn into anything, or waited for).
	 */
	e->framebuffer = framebuffer;
	e->colour = 0;
	if (framebuffer != 0) {
		GLint kind = 0, name = 0;

		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &kind);
		if (kind == GL_TEXTURE) {
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			e->colour = (GLuint)name;
		}
	}
	EAGL_LEAVE(e, 0x1508);
}

static void
my_glFramebufferTexture2D(void *context, GLenum target, GLenum attachment, GLenum to, GLuint texture, GLint level)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1530);
	clear_now(e);
	if (attachment == GL_COLOR_ATTACHMENT0 && target != GL_READ_FRAMEBUFFER) {
		e->colour = texture;
	}
	if (eagl_tracing) {
		eagl_trace("glFramebufferTexture2D(%#x, %#x, %#x, %u, %d)\n", target, attachment, to, texture, level);
	}
	glFramebufferTexture2D(target, attachment, to, texture, level);
	EAGL_LEAVE(e, 0x1530);
}

static void
my_glDeleteFramebuffers(void *context, GLsizei n, const GLuint *framebuffers)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1510);
	clear_now(e);
	for (GLsizei i = 0; i < n; i++) {
		if (framebuffers[i] == e->framebuffer) {
			e->framebuffer = e->colour = 0;
		}
	}
	glDeleteFramebuffers(n, framebuffers);
	EAGL_LEAVE(e, 0x1510);
}

/* A private entry of iOS's library (CA::OGL::GLESContext::set_surface calls it); nobody else has it. */
static void
my_glFramebufferParameteriAPPLE(void *context, GLenum target, GLenum name, GLint value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2008);
	if (slot_calls[0x2008 / 8] <= 4) {
		say("redwood-gl: glFramebufferParameteriAPPLE(%#x, %#x, %d): nothing done\n", target, name, value);
	}
}

/* EXT_debug_marker: names for a debugger's list of calls. */
static void
my_glInsertEventMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1988);
}

static void
my_glPushGroupMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1990);
}

static void
my_glPopGroupMarkerEXT(void *context)
{
	EAGL_ENTER(context, 0x1998);
}

/*
 * CoreAnimation's shaders are in the first version of the shading language, with one thing of
 * Apple's own in them: sampler2DRect and texture2DRect(), a texture read at coordinates counted in
 * its pixels (what it shows of IOSurfaces is read so). The language has no such thing, and in
 * its first version no way to make one, since a shader cannot ask how big a texture is. In
 * version 3.00 it can. So every shader is handed on as a 3.00 one: the words of the first
 * version that 3.00 dropped are defined back, and texture2DRect() divides by textureSize().
 * (All of a program's shaders must be of one version, so all are treated alike.)
 */
/*
 * CoreAnimation's shaders say "lowp" and "mediump" for nearly everything, as shaders written for
 * Apple's GPUs do. On an Adreno mediump is a float of sixteen bits, eleven of them the number: a
 * place in a texture of 1024 rows worked out so is right to half a row, and the table CoreAnimation
 * looks a gradient's colours up in (32 x 1024, a cube of colour laid out flat; its fragment shader
 * with "c_st") then gives sixty-odd shades where it has thousands. The Weather widget's sky was
 * drawn in horizontal bands by that. Everything is computed in full precision instead (the two
 * words are defined away, in vertex shaders as well: a uniform must be the same in both).
 * Boot-arg glprecision=0: as the shaders say.
 */
static int full_precision = 1;
static const char all_highp[] =
    "#define lowp highp\n"
    "#define mediump highp\n";

static const char fragment_start[] =
    "#define varying in\n"
    "#define texture2D texture\n"
    "#define texture2DProj textureProj\n"
    "#define texture2DLod textureLod\n"
    "#define textureCube texture\n"
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture(s, (c) / vec2(textureSize(s, 0)))\n"
    "highp vec4 redwood_data[1];\n"
    "#define gl_FragData redwood_data\n"
    "#define gl_FragColor redwood_data[0]\n";
/*
 * What a fragment shader of the first version writes to (gl_FragData), and with the extension a
 * GPU's driver offers and CoreAnimation then uses (EXT_shader_framebuffer_fetch) reads what is
 * there already from (gl_LastFragData), are in 3.00 one variable of the shader's own, declared
 * "out" or "inout". The shader's main() is given another name and called from one that reads
 * that variable before and writes it after.
 */
static const char fragment_plain[] =
    "layout(location = 0) out highp vec4 redwood_colour;\n"
    "#define main redwood_main\n"
    "#line 1\n";
static const char fragment_plain_end[] =
    "\n#undef main\n"
    "void main() { redwood_main(); redwood_colour = redwood_data[0]; }\n";
static const char fragment_fetch[] =
    "layout(location = 0) inout highp vec4 redwood_colour;\n"
    "highp vec4 redwood_last[1];\n"
    "#define gl_LastFragData redwood_last\n"
    "#define main redwood_main\n"
    "#line 1\n";
static const char fragment_fetch_end[] =
    "\n#undef main\n"
    "void main() { redwood_last[0] = redwood_colour; redwood_main(); redwood_colour = redwood_data[0]; }\n";
static const char vertex_start[] =
    "#define attribute in\n"
    "#define varying out\n"
    "#define texture2D texture\n"
    "#define texture2DLod textureLod\n"
    "#line 1\n";

static GLuint
my_glCreateShader(void *context, GLenum type)
{
	struct eagl *e = context;
	GLuint shader;

	EAGL_ENTER(e, 0x1298);
	shader = glCreateShader(type);
	e->fragment[shader % 256] = type == GL_FRAGMENT_SHADER;
	EAGL_LEAVE(e, 0x1298);
	return shader;
}

/*
 * CoreAnimation does not ask whether its shaders compiled. Here it is asked, and what the
 * compiler said is written down with the shader's text.
 */
static void
my_glShaderSource(void *context, GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	struct eagl *e = context;
	bool fragment = e->fragment[shader % 256], fetch;
	const GLchar *parts[6] = { "#version 300 es\n", fragment ? fragment_start : vertex_start, "", NULL, "",
		                   full_precision ? all_highp : "" };
	size_t size = 1, extensions = 0;
	char *text, *line, *head;

	EAGL_ENTER(e, 0x12a0);
	for (GLsizei i = 0; i < count; i++) {
		size += lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
	}
	if ((text = calloc(1, 2 * size + 64)) == NULL) {
		glShaderSource(shader, count, strings, lengths);
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		strncat(text, strings[i], lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]));
	}
	/* "#extension" lines must stay ahead of everything that is not the preprocessor's: they move to the top */
	head = text + size;
	strcpy(head, parts[0]);
	for (line = text; (line = strstr(line, "#extension")) != NULL; line += 2) {
		size_t length = strcspn(line, "\n");

		strncat(head, line, length);
		strcat(head, "\n");
		line[0] = line[1] = '/';
		extensions++;
	}
	parts[0] = head;
	parts[3] = text;
	if (fragment) {
		fetch = strstr(text, "gl_LastFragData") != NULL;
		parts[2] = fetch ? fragment_fetch : fragment_plain;
		parts[4] = fetch ? fragment_fetch_end : fragment_plain_end;
	}
	free((void *)e->sources[shader % 256]);
	e->sources[shader % 256] = text;
	glShaderSource(shader, 6, (const GLchar *const[6]){ parts[0], parts[5], parts[1], parts[2], parts[3], parts[4] }, NULL);
	EAGL_LEAVE(e, 0x12a0);
}

static void
say_long(const char *what, const char *text)
{
	say("redwood-gl: %s:\n", what);
	for (size_t at = 0, length = strlen(text); at < length; at += 900) {
		write(2, text + at, length - at < 900 ? length - at : 900);
	}
	say("\n");
}

static void
my_glCompileShader(void *context, GLuint shader)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12a8);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
	e->shaders++;
	if (!good) {
		glGetShaderInfoLog(shader, sizeof(said), NULL, said);
		say("redwood-gl: A SHADER DOES NOT COMPILE (shader %u): %s\n", shader, said);
		if (e->sources[shader % 256] != NULL) {
			say_long("its text", e->sources[shader % 256]);
		}
	} else if ((e->shaders <= 2 || all_shaders) && e->sources[shader % 256] != NULL) {
		say("redwood-gl: shader %u:\n", shader);
		say_long(all_shaders ? "a shader" : "the first shaders, for the record", e->sources[shader % 256]);
	}
	EAGL_LEAVE(e, 0x12a8);
}

static void
my_glLinkProgram(void *context, GLuint program)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12c0);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	e->programs++;
	if (!good) {
		glGetProgramInfoLog(program, sizeof(said), NULL, said);
		say("redwood-gl: A PROGRAM DOES NOT LINK (program %u): %s\n", program, said);
	}
	EAGL_LEAVE(e, 0x12c0);
}

/* iOS's library has one entry for deleting a program and deleting a shader. */
static void
my_glDeleteProgram(void *context, GLuint name)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1280);
	if (glIsProgram(name)) {
		glDeleteProgram(name);
	} else {
		glDeleteShader(name);
	}
	EAGL_LEAVE(e, 0x1280);
}

#define EAGL_GEN_TABLE
#include "eagl_gen.h"
#undef EAGL_GEN_TABLE

_Static_assert(EAGL_TABLE_SIZE <= SLOTS * 8, "the table has grown");

/* ---- EAGLContext ---------------------------------------------------------------------------------- */

static id
my_initWithAPI(id self, SEL selector, unsigned long api, id properties)
{
	struct eagl *e;
	unsigned place;

	(void)selector;
	(void)properties;
	for (place = 0; place < sizeof(contexts) / sizeof(contexts[0]) && contexts[place] != NULL; place++) {
	}
	if (api < 2 || api > 3 || place == sizeof(contexts) / sizeof(contexts[0]) || (e = calloc(1, sizeof(*e))) == NULL) {
		say("redwood-gl: no context of API %lu\n", api);
		return nil;
	}
	if ((e->gl = redwood_gl_create((int)api)) == NULL || !redwood_gl_make_current(e->gl)) {
		say("redwood-gl: %s gives no OpenGL ES %lu context\n", redwood_gl_library(), api);
		free(e);
		return nil;
	}
	current = e;
	e->object = self;
	e->api = api;
	e->table[0] = e;
	eagl_fill_table(e->table);
	contexts[place] = e;
	say("redwood-gl: an OpenGL ES %lu context for CoreAnimation, by %s: %s; %s; shaders %s\n", api, redwood_gl_library(),
	    (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
	return self;
}

static unsigned long
my_API(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->api : 0;
}

/*
 * -[EAGLContext sharegroup]. CoreAnimation never asks for it, but CoreImage does: a CIContext made
 * over an EAGLContext goes +[CIContext contextWithEAGLContext:] -> internalContextWithEAGLContext:
 * -> CI::GLContext::GLContext -> wrapEAGLContextCreate -> -[EAGLContext sharegroup], and iOS's own
 * accessor reads ivars of an EAGLContext that my_initWithAPI never filled in (it keeps its state in
 * `struct eagl` beside the object and does not call through to Apple's init). On the phone that
 * crashed in -[EAGLContext sharegroup]+4 touching address 0x8. Nothing here shares a context with
 * anybody, so the honest answer is none; what CoreImage then does is the next thing to find out.
 */
static id
my_sharegroup(id self, SEL selector)
{
	static int said;

	(void)self;
	(void)selector;
	if (!said) {
		said = 1;
		say("redwood-gl: -sharegroup asked for: none (nothing here shares a context)\n");
	}
	return nil;
}

static unsigned long
my_setParameter(id self, SEL selector, unsigned parameter, const int32_t *values)
{
	(void)self;
	(void)selector;
	say("redwood-gl: -setParameter:%#x to:%d: noted\n", parameter, values ? values[0] : 0);
	return 0;
}

static void *
my_getMacroContextPrivate(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->table : NULL;
}

/* (the types are the method's own: the last three arrive on the stack, where sizes matter) */
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface, unsigned long target, unsigned long internal,
    GLsizei width, GLsizei height, unsigned long format, unsigned long type, uint32_t plane, BOOL invert)
{
	static unsigned shown;
	struct eagl *e = context_of(self);
	struct image *image;
	GLint texture = 0;
	uint64_t began;

	(void)selector;
	if (e == NULL || surface == NULL) {
		return NO;
	}
	eagl_enter(e, 0);
	clear_now(e);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	if (target != GL_TEXTURE_2D || texture == 0 || (image = image_new(e, (GLuint)texture, surface)) == NULL) {
		say("redwood-gl: -texImageIOSurface: target %#lx, texture %d: not done\n", target, texture);
		return NO;
	}
	image->to = (GLenum)target;
	image->internal = (GLenum)internal;
	image->format = (GLenum)format;
	image->type = (GLenum)type;
	image->plane = plane;
	image->invert = invert;
	image->width = width;
	image->height = height;
	note_format(invert ? "IOSurface shown, inverted" : "IOSurface shown", image->internal, image->format, image->type, width,
	    height);
	if (shown++ < 24) {
		say("redwood-gl: -texImageIOSurface: surface %u ('%.4s' %zu x %zu, %zu planes) as texture %d: %d x %d, plane %u%s\n",
		    IOSurfaceGetID(surface), (const char *)&(uint32_t){ __builtin_bswap32(IOSurfaceGetPixelFormat(surface)) },
		    IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), IOSurfaceGetPlaneCount(surface), texture, width,
		    height, plane, invert ? ", inverted" : "");
	}
	image->wrapped = IOSurfaceGetPixelFormat(surface) == 0;
	began = now_us();
	if (!surface_shared(e, image)) {
		surface_changed(e, image);              /* (what it adds up to now) */
		surface_to_texture(e, image);
	}
	e->attaching += now_us() - began;
	eagl_leave(e, 0x970);
	return YES;
}

/* (the "image" is the target, GL_TEXTURE_2D, and means the texture bound to it) */
static BOOL
my_attachImage(id self, SEL selector, unsigned long target, IOSurfaceRef surface, BOOL inverted)
{
	struct eagl *e = context_of(self);
	static unsigned attached;
	struct image *image;
	GLint texture = 0;
	uint32_t pixels;
	uint64_t began;

	(void)selector;
	if (e == NULL || surface == NULL) {
		return NO;
	}
	eagl_enter(e, 0);
	clear_now(e);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	pixels = IOSurfaceGetPixelFormat(surface);
	if (target != GL_TEXTURE_2D || texture == 0 || (pixels != 'BGRA' && pixels != 0) ||
	    IOSurfaceGetBytesPerElement(surface) != 4 || IOSurfaceGetPlaneCount(surface) != 0 ||
	    (image = image_new(e, (GLuint)texture, surface)) == NULL) {
		say("redwood-gl: -attachImage: target %#lx, texture %d, an IOSurface of '%.4s': not done\n", target, texture,
		    (const char *)&(uint32_t){ __builtin_bswap32(pixels) });
		return NO;
	}
	image->target = true;
	image->to = GL_TEXTURE_2D;
	image->invert = inverted;
	image->width = (GLsizei)IOSurfaceGetWidth(surface);
	image->height = (GLsizei)IOSurfaceGetHeight(surface);
	image->internal = image->format = GL_BGRA;
	image->type = GL_UNSIGNED_BYTE;
	/*
	 * A surface with no pixel format is memory CoreAnimation has made a surface around: a
	 * picture to be shown, as an application has drawn it. Those that come this way are mostly
	 * the signs applications draw to be tinted -- black, so nothing but opacity -- and their
	 * memory is opacity first, then blue, green, red (seen: the first byte of every four is all
	 * there is in the black ones, and the one sign with colours in it, blue in the software
	 * renderer's picture, comes out orange if red is taken to follow the opacity). Read
	 * blue-first, like the surfaces that are drawn into, the black ones have no opacity at all
	 * and are not seen. iOS's library has the order from the surface; here it is taken from
	 * there being no pixel format.
	 */
	image->wrapped = image->alpha_first = pixels == 0;
	note_format("IOSurface attached", GL_BGRA, GL_BGRA, GL_UNSIGNED_BYTE, image->width, image->height);
	/*
	 * What is attached is as often read as drawn into (a picture CoreAnimation has made a surface
	 * around, to be shown): a texture that is not the surface's memory starts with what the surface holds.
	 */
	began = now_us();
	if (surface_mirrored(e, image) || !surface_shared(e, image)) {
		surface_changed(e, image);              /* (what it adds up to now) */
		surface_to_texture(e, image);
	}
	e->attaching += now_us() - began;
	if (attached++ < 24) {
		say("redwood-gl: -attachImage: texture %d is IOSurface %u ('%.4s' %d x %d, %zu bytes a row)%s%s\n", texture,
		    IOSurfaceGetID(surface), (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, image->width, image->height,
		    IOSurfaceGetBytesPerRow(surface), inverted ? ", drawn inverted" : "", image->shared ? ": its memory" :
		    image->mirror != 0 ? ": a texture, copied into its memory by the GPU" : ": a copy");
	}
	eagl_leave(e, 0x970);
	return YES;
}

static void
my_dealloc(id self, SEL selector)
{
	struct objc_super super = { self, class_getSuperclass(objc_getClass("EAGLContext")) };
	struct eagl *e = context_of(self);

	if (e != NULL) {
		say("redwood-gl: the context goes\n");
		summary(e);
		for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
			if (contexts[i] == e) {
				contexts[i] = NULL;
			}
		}
		if (current == e) {
			redwood_gl_make_current(NULL);
			current = NULL;
		}
		redwood_gl_destroy(e->gl);
		free(e);
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&super, selector);
}

/* Does the display's driver wait for the GPU before it shows a surface (iokit/Kernel/MiniIOMobileFramebuffer.cpp)? */
static bool
display_waits(void)
{
	io_service_t display = IOServiceGetMatchingService(0, IOServiceMatching("IOMobileFramebuffer"));
	CFTypeRef says;
	bool waits;

	if (display == 0) {
		return false;
	}
	says = IORegistryEntryCreateCFProperty(display, CFSTR("RedwoodWaitsForGPU"), kCFAllocatorDefault, 0);
	waits = says == kCFBooleanTrue;
	if (says != NULL) {
		CFRelease(says);
	}
	IOObjectRelease(display);
	return waits;
}

__attribute__((constructor)) static void
install(void)
{
	static const struct {
		const char *name;
		IMP ours;
	} methods[] = {
		{ "initWithAPI:properties:", (IMP)my_initWithAPI },
		{ "API", (IMP)my_API },
		{ "sharegroup", (IMP)my_sharegroup },
		{ "setParameter:to:", (IMP)my_setParameter },
		{ "getMacroContextPrivate", (IMP)my_getMacroContextPrivate },
		{ "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:", (IMP)my_texImageIOSurface },
		{ "attachImage:toCoreSurface:invertedRender:", (IMP)my_attachImage },
		{ "dealloc", (IMP)my_dealloc },
	};
	Class class = objc_getClass("EAGLContext");
	unsigned replaced = 0;
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;

	/* the kernel's boot-args are where a run is told things */
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		if ((argument = strstr(arguments, "gltrace=")) != NULL) {
			trace_last = trace_first = (unsigned)strtoul(argument + 8, (char **)&argument, 10);
			if (*argument == '-') {
				trace_last = (unsigned)strtoul(argument + 1, NULL, 10);
			}
		}
		if ((argument = strstr(arguments, "gltrace_at=")) != NULL) {
			trace_at = now_us() + strtoull(argument + 11, NULL, 10) * 1000000;
		}
		if ((argument = strstr(arguments, "gltrace_draws=")) != NULL) {
			trace_draws = (unsigned)strtoul(argument + 14, NULL, 10);
		}
		if ((argument = strstr(arguments, "gltrace_function=")) != NULL) {
			trace_function = (int)strtoul(argument + 17, (char **)&argument, 10);
			trace_function_after = *argument == ',' ? (int)strtoul(argument + 1, NULL, 10) : 1;
		}
		if ((argument = strstr(arguments, "gltrace_frames=")) != NULL) {
			trace_frames = (unsigned)strtoul(argument + 15, NULL, 10);
		}
		trace_lit = strstr(arguments, "gltrace_lit=1") != NULL;
		trace_look = strstr(arguments, "gltrace_look=1") != NULL;
		trace_shaders = strstr(arguments, "gltrace_shaders=1") != NULL;
		function_numbers = strstr(arguments, "glnumbers=1") != NULL;
		trace_uniforms_on = strstr(arguments, "gltrace_uniforms=1") != NULL;
		if ((argument = strstr(arguments, "gltrace_corners=")) != NULL) {
			trace_corners = (GLsizei)strtol(argument + 16, NULL, 10);
		}
		fetch_offered = strstr(arguments, "glfetch=0") == NULL;
		group_vibrant_on = strstr(arguments, "glgroup=0") == NULL;
		if ((argument = strstr(arguments, "gltrace_place=")) != NULL) {
			char *end = (char *)argument + 14;

			for (unsigned i = 0; i < 4; i++) {
				trace_place[i] = strtof(end, &end);
				end += *end == ',';
			}
			trace_placed = 1;
		}
		all_shaders = strstr(arguments, "glshaders=1") != NULL;
		census_on = strstr(arguments, "glcensus=1") != NULL;
		frame_lines = strstr(arguments, "glframes=1") != NULL;
		latency_lines = strstr(arguments, "touch_lat=1") != NULL;
		if ((argument = strstr(arguments, "glslow=")) != NULL) {
			slow_calls_us = strtoull(argument + 7, NULL, 10);
		}
		full_precision = strstr(arguments, "glprecision=0") == NULL;
		vblur_on = strstr(arguments, "vblur=0") != NULL ? 0 : strstr(arguments, "vblur=2") != NULL ? 2 : 1;
		if ((argument = strstr(arguments, "glplain=")) != NULL) {
			plain_draws = (int)strtoul(argument + 8, NULL, 10);
		}
		if ((argument = strstr(arguments, "glskip=")) != NULL) {
			const char *at = argument + 7;

			while (skipping < sizeof(skipped) / sizeof(skipped[0])) {
				skipped[skipping++] = (GLuint)strtoul(at, NULL, 10);
				if ((at = strchr(at, ',')) == NULL) {
					break;
				}
				at++;
			}
		}
		if ((argument = strstr(arguments, "glshow=")) != NULL) {
			if (strchr(argument + 7, 'x') != NULL && strchr(argument + 7, 'x') < argument + 7 + strcspn(argument + 7, " ")) {
				shown_wanted[0] = (GLint)strtol(argument + 7, NULL, 10);
				shown_wanted[1] = (GLint)strtol(strchr(argument + 7, 'x') + 1, NULL, 10);
				shown_texture = 1;              /* (that there is one to show at all) */
			} else {
				shown_texture = (GLuint)strtoul(argument + 7, NULL, 10);
			}
			if ((argument = strstr(arguments, "glshow_part=")) != NULL) {
				shown_part[0] = (GLint)strtol(argument + 12, NULL, 10);
				shown_part[1] = strchr(argument + 12, ',') != NULL ?
				    (GLint)strtol(strchr(argument + 12, ',') + 1, NULL, 10) : shown_part[0];
			}
			if ((argument = strstr(arguments, "glshow_zoom=")) != NULL) {
				shown_zoom = (GLint)strtol(argument + 12, NULL, 10);
				shown_zoom = shown_zoom < 1 ? 1 : shown_zoom;
			}
		}
		share_surfaces = strstr(arguments, "glcopy=1") == NULL;
		blit_targets = strstr(arguments, "glblit=0") == NULL;
		sync_frames = strstr(arguments, "glsync=0") == NULL;
		if (strstr(arguments, "glwait=0") != NULL || (strstr(arguments, "glwait=1") == NULL && display_waits())) {
			wait_for_frames = 0;
		}
		say("redwood-gl: a frame is waited for %s\n", wait_for_frames ? "here, before it is handed to the display" :
		    "by the display's driver, not here");
		for (argument = arguments; (argument = strstr(argument, "glenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *value;

			if (length < sizeof(setting)) {
				memcpy(setting, argument + 6, length);
				setting[length] = 0;
				if ((value = strchr(setting, '=')) != NULL) {
					*value = 0;
					setenv(setting, value + 1, 1);
					say("redwood-gl: in the environment: %s=%s\n", setting, value + 1);
				}
			}
		}
	}

	if (class == Nil) {
		say("redwood-gl: this program has no EAGLContext: nothing to do\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		SEL selector = sel_registerName(methods[i].name);
		Method method = class_getInstanceMethod(class, selector);

		if (method == NULL) {
			say("redwood-gl: EAGLContext has no -%s\n", methods[i].name);
			continue;
		}
		say("redwood-gl: -[EAGLContext %s] (%s) replaced\n", methods[i].name, method_getTypeEncoding(method));
		if (method == class_getInstanceMethod(class_getSuperclass(class), selector)) {
			/* inherited: the class gets one of its own, the one it inherits is everybody's */
			class_addMethod(class, selector, methods[i].ours, method_getTypeEncoding(method));
		} else {
			method_setImplementation(method, methods[i].ours);
		}
		replaced++;
	}
	say("redwood-gl: %u of %zu methods of EAGLContext are ours; OpenGL ES by %s\n", replaced,
	    sizeof(methods) / sizeof(methods[0]), redwood_gl_library());
}
