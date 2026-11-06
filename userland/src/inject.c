/*
 * libredwood.dylib: code of ours inside stock iOS programs.
 *
 * launchd starts the jobs named in INJECT (see build.sh / mkjobs.py) with
 * DYLD_INSERT_LIBRARIES pointing here; dyld honours that because the kernel's AMFI stand-in
 * says so. What is done depends on the program:
 *
 *   backboardd   the window server (CoreAnimation in QuartzCore). There is no GPU, so the
 *                render server must use CoreAnimation's own software rasterizer, and we want to
 *                see why a frame is or is not drawn. The C++ objects are reached by replacing
 *                entries of their virtual tables.
 *                It is also where input enters the system: backboardd is the HID event server,
 *                and without Apple's multitouch driver it has no touch screen. We give it one:
 *                a "virtual" HID service that turns the samples of /dev/redwood-touch into the
 *                events that driver would have sent.
 *
 *                And it does not wait five seconds for an ambient light sensor.
 *
 *   containermanagerd (both of them)
 *                They ask the user manager for the "personas" of the user, of which this
 *                system has none, twenty times a second apart before they give up -- and
 *                everything that needs a container (the window server, for one) waits with
 *                them. Told that personas are not supported, they do not ask.
 *
 * Addresses of things inside the dyld shared cache are those of iOS 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <Block.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>

static void *prof_main(void *unused);  /* bbd_prof=1 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "redwood_gl.h"
#include "redwood_input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define REDWOOD_GL "/redwood/lib/libredwood-gl.dylib"

static uintptr_t slide;

static void
say(const char *format, ...)
{
	char line[256];
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

/* Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`. */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("redwood: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("redwood: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- backboardd: CA::WindowServer ------------------------------------------------------------ */

#define VT_APPLE_CLCD_DISPLAY   0x1d5b44520UL   /* vtable for CA::WindowServer::AppleCLCDDisplay */
#define VT_EAGL_SERVER          0x1d5b45c58UL   /* vtable for CA::WindowServer::EAGLServer */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

#define DISPLAY_FLAGS(display)  (*(uint64_t *)((char *)(display) + 0x9f2))
#define DISPLAY_B233(display)   (*(uint8_t *)((char *)(display) + 0x233))

static int (*orig_is_ready)(void *display);
static int (*orig_ignore_update_p)(void *display, void *update);
static uintptr_t (*orig_begin_update)(void *display, void *update);
static uintptr_t (*orig_finish_update)(void *display, void *update, unsigned flags);
static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);

static unsigned calls_ready, calls_ignore, calls_begin, calls_finish, calls_render;
static int last_ready = -1, last_ignore = -1;

static int
my_is_ready(void *display)
{
	int ready = orig_is_ready(display);

	if (ready != last_ready || calls_ready < 3) {
		say("redwood: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
		    (unsigned long long)DISPLAY_FLAGS(display), DISPLAY_B233(display));
		last_ready = ready;
	}
	calls_ready++;
	return ready;
}

static int
my_ignore_update_p(void *display, void *update)
{
	int ignore = orig_ignore_update_p(display, update) & 1;

	if (ignore != last_ignore || calls_ignore < 3) {
		uint64_t *q = (uint64_t *)((char *)display + 0x220);

		say("redwood: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("redwood:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
		    (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4],
		    (unsigned long long)q[5]);
		last_ignore = ignore;
	}
	calls_ignore++;
	return ignore;
}

static uintptr_t
my_begin_update(void *display, void *update)
{
	uintptr_t r = orig_begin_update(display, update);

	if (calls_begin++ < 6) {
		say("redwood: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("redwood: finish_update(%#x) -> %#lx\n", flags, r);
	}
	return r;
}

/*
 * Every frame the server asks for its renderer, and while there is none it tries again to make a
 * Metal and then an OpenGL ES context, which costs a lot and cannot succeed here. One try is enough:
 * without a renderer the server draws with CA::WindowServer::Server::sw_renderer().
 */
static void *
my_renderer(void *server)
{
	static int failed, said;
	void *renderer;

	if (failed) {
		return NULL;
	}
	renderer = orig_renderer(server);
	if (renderer == NULL) {
		failed = 1;
		say("redwood: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("redwood: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("redwood: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
	}
	return r;
}

/*
 * CoreAnimation draws a layer's pixels with one of ninety "functions" (a fragment shader each: an
 * image as it is, an image through a colour matrix, a blur of so many samples, ...), and asks
 * its renderer which of them it has. Its OpenGL ES renderer has not half of them (the Metal one
 * has all). Which ones are asked for and missing is worth knowing: said once each.
 * (The last of the ninety, "cst", is not asked about: the OpenGL ES renderer draws it with no
 * program at all. The lock screen's wallpaper goes that way; see plain_draw_begin() in eagl.c.)
 */
#define VT_GLES_CONTEXT         0x1d5b3b540UL   /* vtable for CA::OGL::GLESContext */

static int gl_loaded;                           /* CoreAnimation draws with OpenGL ES (opengl_es() below) */
static void (*gl_function_refused)(unsigned function);  /* our OpenGL ES library keeps count (eagl.c, the census) */
static int (*orig_function_supported)(void *context);

/*
 * Boot-arg glyes=N[,N...]: the renderer is made to answer that it has those functions after all.
 * It has no shader for them, so CoreAnimation draws them with no program -- which is the way the
 * functions it never asks about are drawn, and which our OpenGL ES library stands in for
 * (eagl.c, plain_draw_begin()); without this, what needs such a function is not drawn at all.
 * For finding out what a missing function is for: with it the draw is made, its numbers arrive
 * (function_uniform below), and the picture says whether the thing was that function's.
 */
/*
 * Saying yes to a function the renderer has no shader for is not enough and not safe. Told yes for
 * "sb3" (40) -- the four samples of a shadow's blur, which our OpenGL ES library has a stand-in for
 * (eagl.c, blur4_fragment) -- CoreAnimation takes the way of one it has a shader for, finds none,
 * and the window server dies; it came back every half a minute, with SpringBoard after it, and the
 * stand-in was never reached: a function it asks about and is told yes to is not one it draws with
 * no program. What is needed is to hand it a program where it looks for one, which is another
 * entry of the same table. Until then it is told the truth, and what needs that function is not
 * drawn -- the shadow under a card of the switcher of running applications.
 */
/*
 * Where the window server died. Its crash reports never arrive here -- the kernel says "Failed to
 * send exception EXC_CORPSE_NOTIFY" and that is the end of it -- so a SIGSEGV in CoreAnimation has
 * been a silent restart every half a minute with nothing to go on. This turns one into a line: the
 * address it touched and where it was, both as they would be in the shared cache as built, which
 * "ipsw dyld a2s" turns into a name. Then the signal is let through to kill the process as before.
 */
static void
say_the_fault(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	uintptr_t pc = 0, lr = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)context->uc_mcontext->__ss.__fp : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("redwood: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
	    info != NULL ? info->si_addr : NULL, pc - slide, lr - slide, frames);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

static void
watch_for_faults(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = say_the_fault;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
}

/*
 * CoreAnimation's OpenGL ES renderer keeps the source of one fragment shader per function in a
 * table of its own -- unslid 0x1d5b54790, indexed by the function's number -- and the functions it
 * has no shader for are null entries there. That is the whole of "the renderer has no function
 * 40": GLContext::create_shader takes the source from that table, and function_supported answers
 * from the same place. Made to answer yes without the entry, create_shader hands the null to
 * strdup and the window server dies in strlen, which is what glyes=40 did for a whole afternoon.
 *
 * So the way to give the renderer a function is to give it a source. "sb3" (40) is the single
 * pass of a blur -- GaussianBlurFilter::render -> Context::blur_surface ->
 * BlurState::single_pass_blur -- the one that lays the shadow under a card of the switcher of
 * running applications. It is given the same four places as "ds4" (26) and has no weights of its
 * own, so this is ds4's own shader with the weights taken out, in the dialect the others are
 * written in (sampler2DRect, varying, gl_FragData -- the preamble is added by the renderer).
 *
 * Where this stands. With the entry filled the window server no longer dies: thirty-odd crashes a
 * run become none, with glyes=40 as well, and the function really is drawn -- the renderer rewrites
 * our source into its own dialect (sampler2DRect becomes sampler2D) and compiles it, and the
 * picture changes by a level or two across the whole screen where before it did not change at all.
 *
 * What it does not do is bring back a shadow, and the reason is that none was missing. This
 * function was taken up because a note here said the shadow under a card of the switcher was not
 * drawn; that note was reasoning from "the function is refused", not an observation. Against the
 * same frame drawn by CoreAnimation's software renderer -- which draws everything correctly, only
 * slowly -- the edge of a card is the same to the level: 18, 17, 17, then the card. There is no
 * soft falloff there in either, so there is nothing of that kind to restore.
 *
 * Its effect is nil for a second reason worth writing down: the four places reach the shader as
 * varyings that a vertex shader computes from "uniform highp vec2 offset[4]", and the two groups
 * of four numbers function_uniform delivers for it are of the order of 5e-19 -- zero, for a
 * blur. CoreAnimation asks for the single-pass blur exactly when the radius is too small to need
 * the multi-pass one, so the four samples coincide and the pass copies what it was given.
 */
#define CA_SHADER_TABLE         0x1d5b54790UL

static const char shadow_blur_source[] =
    "uniform lowp sampler2DRect texture0;\n"
    "varying highp vec2 texcoord0, texcoord1, texcoord2, texcoord3;\n"
    "void main () {\n"
    "  lowp vec4 q0, q1, q2, q3;\n"
    "  q0 = texture2DRect(texture0, texcoord0);\n"
    "  q1 = texture2DRect(texture0, texcoord1);\n"
    "  q2 = texture2DRect(texture0, texcoord2);\n"
    "  q3 = texture2DRect(texture0, texcoord3);\n"
    "  gl_FragData[0] = (q0 + q1 + q2 + q3) * 0.25;\n"
    "}\n";

static void
give_the_renderer_a_shader(unsigned function, const char *source)
{
	uintptr_t *entry = (uintptr_t *)(CA_SHADER_TABLE + slide + 8 * (uintptr_t)function);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);

	if (*entry != 0) {
		say("redwood: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("redwood: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("redwood: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = REDWOOD_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("redwood: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("redwood: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
}

/*
 * CA::OGL::Context::compile_shader(const char *) -- slot 0x060 of the same table. The OpenGL ES
 * renderer does not carry its shaders ready-made: it assembles the text of each from a preamble
 * (GLESContext::shader_preamble, slot 0x400) and pieces chosen by what the draw needs, and
 * compiles it here. That makes this the place where it looks for a program, and therefore the
 * place where one can be handed to it for a function it has no pieces for ("sb3", 40: the four
 * samples of a shadow's blur, missing under the cards of the switcher). This writes down what it
 * is asked to compile, so that a run with glyes=40 says whether a source is assembled at all
 * before the window server dies, and what it looks like if so.
 */
static uintptr_t (*orig_compile_shader)(void *context, const char *source);

static uintptr_t
my_compile_shader(void *context, const char *source)
{
	static unsigned told, seen;

	/* that it is called at all is worth one line, because a renderer that compiles nothing here
	   would mean the text is assembled and compiled somewhere this table does not reach */
	if (seen++ == 0) {
		say("redwood: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("redwood: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("redwood:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = REDWOOD_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("redwood: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("redwood: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
		    function, function < 90 ? names + 4 * function : "?");
	}
	return supported;
}

/*
 * A function that takes numbers besides its textures (a colour matrix, say) is given them through
 * GLContext::function_uniform(), which hands them to the function's shader. The functions the
 * OpenGL ES renderer has no shader for are drawn all the same, with no program, and their numbers
 * go nowhere. Our OpenGL ES library draws some of those with programs of its own (eagl.c,
 * plain_draw_begin()): it is told the numbers here, and with them the context, in which it
 * finds which function a draw is.
 */
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	/*
	 * Who is calling. Told yes for a function it has no shader for (glyes=40), CoreAnimation gets
	 * this far -- it hands the function its numbers -- and then dies in whatever comes next, which
	 * is where it takes the program for the function out of a table of its own. These addresses,
	 * unslid, name that caller for "ipsw dyld a2s", and it is the only thread to pull: nothing is
	 * ever compiled for the function (no shader arrives at our library), and the compiler is not
	 * reached through the renderer's table either.
	 */
	if (say_yes[function < 90 ? function : 0]) {
		static uint8_t told[90];

		if (function < 90 && !told[function]) {
			told[function] = 1;
			say("redwood: function %u takes its numbers from %#lx %#lx %#lx\n", function,
			    (uintptr_t)__builtin_return_address(0) - slide,
			    (uintptr_t)__builtin_return_address(1) - slide,
			    (uintptr_t)__builtin_return_address(2) - slide);
		}
	}
	if (gl_function_values != NULL) {
		gl_function_values(context, function, index, components, count, values);
	}
	orig_function_uniform(context, index, components, count, values);
}

/* ---- backboardd: the window server stays on OpenGL ES ------------------------------------------ */

/*
 * With the Metal properties on the accelerator service (boot-arg metal=1) MTLCreateSystemDefaultDevice()
 * answers in every process, and the window server is the first to ask. Its road to Metal has no fork
 * in it: CA::WindowServer::EAGLServer's constructor (0x1883d1544) calls renderer() (0x1882e2548) with a
 * direct branch -- past the virtual table, so my_renderer() above never sees that first call -- and
 * renderer() calls CAMetalContextCreate() (0x18837b718), which is nothing but a dispatch_once around
 * MTLCreateSystemDefaultDevice(). A device that is not nil goes straight into
 * CA::OGL::new_metal_context() (0x18847efcc), where the renderer asks for its shaders:
 *
 *	quartzcore_bundle = [NSBundle bundleWithIdentifier:@"com.apple.QuartzCore"];
 *	assert(quartzcore_bundle != nil);				ogl-metal.mm:3305
 *	metallib_url = [quartzcore_bundle URLForResource:@"default" withExtension:@"metallib"];
 *	assert(metallib_url != nil);					ogl-metal.mm:3308 -- here
 *
 * The bundle is found; the resource is not, because QuartzCore.framework/default.metallib is not in
 * this image (ios165/select-system.py drops every .metallib under the framework directories). That is
 * fourteen aborts in one boot with launchd restarting the server after each -- logs/135751-run.log --
 * and the window server is the one process that must not crash-loop: nothing is drawn at all while it
 * is down. The road that works here is the other one: renderer() falls back to
 * [EAGLContext initWithAPI:3] and CA::OGL::GLESContext when the device is nil, and behind EAGLContext
 * is our own OpenGL ES (userland/src/eagl.c, Mesa off-screen).
 *
 * So the device is taken out of this process before CoreAnimation can ask for it. Metal keeps every
 * device it has made in one static NSMutableArray, and MTLCreateSystemDefaultDevice() is no more than
 * "if that array holds exactly one object, return it" (its block, 0x181a3442c): an empty array answers
 * nil without a single instruction of Metal's being touched. The array is built on first use, so it
 * has to be asked for first -- and that creates the device object, which is exactly what
 * CoreAnimation's next instruction would have done anyway, so it costs this process nothing it was not
 * already paying. The device is retained and never released on purpose: what our driver's -dealloc
 * would do to a GPU this same process is drawing on through Mesa is not measured.
 *
 * Boot-arg camtl=1 leaves the device where it is, for the boot that means to put CoreAnimation on the
 * Metal road deliberately -- which needs QuartzCore's default.metallib in the image first, or this
 * same assertion comes back.
 */
#define MTL_DEVICE_ARRAY        0x1e1efe9a0UL   /* Metal's static NSMutableArray *_deviceArray */
#define CA_DETECT_DISPLAYS      0x1884da264UL   /* -[CAWindowServer _detectDisplays] */

extern id objc_retain(id object);

static void (*orig_detect_displays)(id server, SEL selector);

static void
keep_metal_from_core_animation(void)
{
	unsigned long (*ask)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	void (*tell)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
	id (*create)(void) = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
	id *slot = (id *)(MTL_DEVICE_ARRAY + slide);
	id device, array;
	unsigned long held;

	if (create == NULL) {
		say("redwood: metal: this process has no MTLCreateSystemDefaultDevice; nothing to keep\n");
		return;
	}
	if ((device = create()) == NULL) {
		say("redwood: metal: no device in this process; CoreAnimation keeps OpenGL ES by itself\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("redwood: metal: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("redwood: metal: %#lx holds a %s, not Metal's device array; left alone\n", MTL_DEVICE_ARRAY,
		    object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("redwood: metal: %s taken out of the window server (%lu in the list, %lu now); "
	    "MTLCreateSystemDefaultDevice() = %s\n", object_getClassName(device), held,
	    ask(array, sel_registerName("count")), create() != NULL ? "A DEVICE STILL" : "nil");
}

/*
 * -[CAWindowServer _detectDisplays] is the caller of CA::WindowServer::AppleDisplay::new_server(),
 * which builds the EAGLServer whose constructor asks for the device: the last Objective-C method on
 * the way in, and the only hook on this path that does not mean writing to Apple's code.
 */
static void
my_detect_displays(id server, SEL selector)
{
	static int once;

	if (!once) {
		once = 1;
		keep_metal_from_core_animation();
	}
	orig_detect_displays(server, selector);
}

/*
 * camtl=1: CoreAnimation's Metal renderer, and the shaders it asks for. CA::OGL::new_metal_context
 * asserts that [QuartzCore's bundle URLForResource:@"default" withExtension:@"metallib"] is not nil
 * (ogl-metal.mm:3308), and this image has no such file: the iPhone X's firmware carries QuartzCore's
 * shaders only as default.pipelinelib, compiled for the A11. The same library in AIR -- which the
 * driver compiles for the Mali -- comes from the macOS QuartzCore (its first slice, cputype
 * 0x1000017), where the functions iOS 16.5 asks for (fixed_frag_lph_cph, std_vert0, narrow_blur_7_frag
 * and so on) carry the same names; build.sh puts it at QC_METALLIB. That one question about that one
 * resource is answered here, and every other is left to NSBundle.
 */
#define QC_METALLIB "/redwood/share/QuartzCore-default.metallib"

static id (*orig_url_for_resource)(id bundle, SEL selector, id name, id extension);

static id
my_url_for_resource(id bundle, SEL selector, id name, id extension)
{
	BOOL (*same)(id, SEL, id) = (BOOL (*)(id, SEL, id))objc_msgSend;
	SEL equal = sel_registerName("isEqualToString:");
	id identifier = ((id (*)(id, SEL))objc_msgSend)(bundle, sel_registerName("bundleIdentifier"));

	if (name != nil && extension != nil && identifier != nil &&
	    same(identifier, equal, (id)CFSTR("com.apple.QuartzCore")) &&
	    same(name, equal, (id)CFSTR("default")) && same(extension, equal, (id)CFSTR("metallib")) &&
	    access(QC_METALLIB, R_OK) == 0) {
		static int told;

		if (!told) {
			told = 1;
			say("redwood: QuartzCore's default.metallib is " QC_METALLIB "\n");
		}
		return (id)CFAutorelease(CFURLCreateWithFileSystemPath(NULL, CFSTR(QC_METALLIB), kCFURLPOSIXPathStyle, false));
	}
	return orig_url_for_resource(bundle, selector, name, extension);
}

/*
 * The shaders are macOS 26's and the renderer is iOS 16.5's, and Apple's compiler service here
 * cannot specialize them: MTLCompilerService of 16.5 reads AIR 2.5 and dies on the library's 2.8
 * (45 crashes in one boot). So nothing is specialized by Apple at all. -newFunctionWithName:
 * constantValues: answers the plain function, and the values CoreAnimation set are kept on it
 * (associated, under the selector redwoodFunctionConstants, which src/redwoodmetal.m reads when
 * it builds a pipeline and hands to the compiler library: rwa_render_constants). A uchar3 is
 * kept as four bytes, the fourth 0: fc_color_opcodes0 is a uchar4 in the newer shader.
 */
typedef struct { unsigned long location, length; } NSRangeLike;

static void (*orig_set_constant_index)(id, SEL, const void *, unsigned long, unsigned long);
static void (*orig_set_constant_name)(id, SEL, const void *, unsigned long, id);
static void (*orig_set_constant_range)(id, SEL, const void *, unsigned long, NSRangeLike);

static size_t
constant_size(unsigned long type)
{
	struct { unsigned long first, elem; } groups[] = {
		{ 3, 4 } /* Float */, { 16, 2 } /* Half */, { 29, 4 } /* Int */, { 33, 4 } /* UInt */,
		{ 37, 2 } /* Short */, { 41, 2 } /* UShort */, { 45, 1 } /* Char */, { 49, 1 } /* UChar */,
		{ 53, 1 } /* Bool */,
	};

	for (unsigned i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
		if (type >= groups[i].first && type < groups[i].first + 4) {
			return groups[i].elem * (type - groups[i].first + 1);
		}
	}
	return type == 81 || type == 85 ? 8 : 0;        /* Long, ULong */
}

static void
keep_constant(id values, id key, const void *value, unsigned long type)
{
	SEL slot = sel_registerName("redwoodConstantValues");
	id kept = objc_getAssociatedObject(values, slot);
	uint64_t bits = 0;
	size_t size = constant_size(type);

	if (value == NULL || size == 0) {
		return;
	}
	memcpy(&bits, value, size < 8 ? size : 8);
	if (kept == nil) {
		kept = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableDictionary"), sel_registerName("new"));
		objc_setAssociatedObject(values, slot, kept, OBJC_ASSOCIATION_RETAIN);
		((void (*)(id, SEL))objc_msgSend)(kept, sel_registerName("release"));
	}
	((void (*)(id, SEL, id, id))objc_msgSend)(kept, sel_registerName("setObject:forKey:"),
	    ((id (*)(id, SEL, unsigned long long))objc_msgSend)((id)objc_getClass("NSNumber"),
	    sel_registerName("numberWithUnsignedLongLong:"), bits), key);
}

static id
constant_number(unsigned long n)
{
	return ((id (*)(id, SEL, unsigned long))objc_msgSend)((id)objc_getClass("NSNumber"),
	    sel_registerName("numberWithUnsignedLong:"), n);
}

static void
my_set_constant_index(id values, SEL selector, const void *value, unsigned long type, unsigned long index)
{
	keep_constant(values, constant_number(index), value, type);
	if (type == 51 && value != NULL) {
		uint8_t four[4] = { ((const uint8_t *)value)[0], ((const uint8_t *)value)[1],
		    ((const uint8_t *)value)[2], 0 };

		orig_set_constant_index(values, selector, four, 52, index);
		return;
	}
	orig_set_constant_index(values, selector, value, type, index);
}

static void
my_set_constant_name(id values, SEL selector, const void *value, unsigned long type, id name)
{
	keep_constant(values, name, value, type);
	if (type == 51 && value != NULL) {
		uint8_t four[4] = { ((const uint8_t *)value)[0], ((const uint8_t *)value)[1],
		    ((const uint8_t *)value)[2], 0 };

		orig_set_constant_name(values, selector, four, 52, name);
		return;
	}
	orig_set_constant_name(values, selector, value, type, name);
}

static void
my_set_constant_range(id values, SEL selector, const void *value, unsigned long type, NSRangeLike range)
{
	size_t size = constant_size(type);

	for (unsigned long i = 0; value != NULL && size != 0 && i < range.length; i++) {
		keep_constant(values, constant_number(range.location + i), (const uint8_t *)value + i * size, type);
	}
	orig_set_constant_range(values, selector, value, type, range);
}

/* the plain function, with the values CoreAnimation set kept on it by Metal's index */
static id
plain_function(id library, id name, id values)
{
	id function = ((id (*)(id, SEL, id))objc_msgSend)(library, sel_registerName("newFunctionWithName:"), name);
	id kept = values ? objc_getAssociatedObject(values, sel_registerName("redwoodConstantValues")) : nil;

	if (function == nil || kept == nil) {
		return function;
	}
	id by_index = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableDictionary"), sel_registerName("new"));
	id constants = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("functionConstantsDictionary"));
	id keys = ((id (*)(id, SEL))objc_msgSend)(kept, sel_registerName("allKeys"));
	unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(keys, sel_registerName("count"));

	for (unsigned long i = 0; i < n; i++) {
		id key = ((id (*)(id, SEL, unsigned long))objc_msgSend)(keys, sel_registerName("objectAtIndex:"), i);
		id value = ((id (*)(id, SEL, id))objc_msgSend)(kept, sel_registerName("objectForKey:"), key);

		if (((BOOL (*)(id, SEL, Class))objc_msgSend)(key, sel_registerName("isKindOfClass:"), objc_getClass("NSString"))) {
			id constant = constants ? ((id (*)(id, SEL, id))objc_msgSend)(constants, sel_registerName("objectForKey:"), key) : nil;

			if (constant == nil) {
				continue;
			}
			key = constant_number(((unsigned long (*)(id, SEL))objc_msgSend)(constant, sel_registerName("index")));
		}
		((void (*)(id, SEL, id, id))objc_msgSend)(by_index, sel_registerName("setObject:forKey:"), value, key);
	}
	objc_setAssociatedObject(function, sel_registerName("redwoodFunctionConstants"), by_index, OBJC_ASSOCIATION_RETAIN);
	((void (*)(id, SEL))objc_msgSend)(by_index, sel_registerName("release"));
	return function;
}

static id
my_new_function_values_error(id library, SEL selector, id name, id values, id *error)
{
	(void)selector;
	id function = plain_function(library, name, values);

	if (function == nil && error != NULL) {
		*error = nil;
	}
	return function;
}

static void
my_new_function_values_handler(id library, SEL selector, id name, id values, void (^handler)(id, id))
{
	(void)selector;
	id function = plain_function(library, name, values);

	handler(function, nil);
	if (function != nil) {
		((void (*)(id, SEL))objc_msgSend)(function, sel_registerName("release"));
	}
}

/* -newFunctionWithDescriptor:error:, which is what QuartzCore 16.5 sends: the descriptor carries
   the name and the constant values. Every class that implements it is hooked, its own
   implementation kept beside it. */
static struct { Class cls; IMP imp; } descriptor_originals[16];
static unsigned descriptor_original_count;

static id
my_new_function_descriptor(id library, SEL selector, id descriptor, id *error)
{
	id name = ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("name"));
	id values = ((id (*)(id, SEL))objc_msgSend)(descriptor, sel_registerName("constantValues"));
	Class cls = object_getClass(library);

	if (name != nil && values != nil) {
		id function = plain_function(library, name, values);

		if (function != nil || error == NULL) {
			return function;
		}
	}
	for (; cls != Nil; cls = class_getSuperclass(cls)) {
		for (unsigned i = 0; i < descriptor_original_count; i++) {
			if (descriptor_originals[i].cls == cls) {
				return ((id (*)(id, SEL, id, id *))descriptor_originals[i].imp)(library, selector, descriptor, error);
			}
		}
	}
	return nil;
}

static int
hook_function_descriptors(void)
{
	SEL selector = sel_registerName("newFunctionWithDescriptor:error:");
	Protocol *protocol = objc_getProtocol("MTLLibrary");
	unsigned count = 0;
	Class *classes = objc_copyClassList(&count);
	int hooked = 0;

	for (unsigned i = 0; classes != NULL && i < count && descriptor_original_count < 16; i++) {
		unsigned n = 0;
		Method *methods;

		if (protocol != NULL && !class_conformsToProtocol(classes[i], protocol)) {
			continue;
		}
		methods = class_copyMethodList(classes[i], &n);
		for (unsigned k = 0; k < n; k++) {
			if (method_getName(methods[k]) == selector) {
				descriptor_originals[descriptor_original_count].cls = classes[i];
				descriptor_originals[descriptor_original_count++].imp =
				    method_setImplementation(methods[k], (IMP)my_new_function_descriptor);
				say("redwood: camtl=1: -[%s newFunctionWithDescriptor:error:] specializes without Apple\n",
				    class_getName(classes[i]));
				hooked++;
			}
		}
		free(methods);
	}
	free(classes);
	return hooked;
}

static void
specialize_without_apple(void)
{
	Class values = objc_getClass("MTLFunctionConstantValuesInternal");
	Class library = objc_getClass("_MTLLibrary");
	Method m;
	int done = 0;

	if (values == Nil) {
		values = objc_getClass("MTLFunctionConstantValues");
	}
	if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValue:type:atIndex:"))) != NULL) {
		orig_set_constant_index = (void (*)(id, SEL, const void *, unsigned long, unsigned long))method_getImplementation(m);
		method_setImplementation(m, (IMP)my_set_constant_index);
		done++;
	}
	if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValue:type:withName:"))) != NULL) {
		orig_set_constant_name = (void (*)(id, SEL, const void *, unsigned long, id))method_getImplementation(m);
		method_setImplementation(m, (IMP)my_set_constant_name);
		done++;
	}
	if ((m = class_getInstanceMethod(values, sel_registerName("setConstantValues:type:withRange:"))) != NULL) {
		orig_set_constant_range = (void (*)(id, SEL, const void *, unsigned long, NSRangeLike))method_getImplementation(m);
		method_setImplementation(m, (IMP)my_set_constant_range);
		done++;
	}
	if (library != Nil &&
	    (m = class_getInstanceMethod(library, sel_registerName("newFunctionWithName:constantValues:error:"))) != NULL) {
		method_setImplementation(m, (IMP)my_new_function_values_error);
		done++;
	}
	if (library != Nil &&
	    (m = class_getInstanceMethod(library, sel_registerName("newFunctionWithName:constantValues:completionHandler:"))) != NULL) {
		method_setImplementation(m, (IMP)my_new_function_values_handler);
		done++;
	}
	done += hook_function_descriptors();
	say("redwood: camtl=1: functions are specialized by the compiler library, not by Apple (%d hooks)\n", done);
}

/*
 * camtl=1: where backboardd dies when no signal handler can say so -- a fault on a stack guard
 * page leaves the faulting thread no stack to run one on. A Mach exception port for the task
 * is served on a thread of its own instead: it reads the faulting thread's registers, names
 * pc, lr and the frame-pointer chain with dladdr (image and nearest symbol), keeps the lines
 * in the kernel's log (kern.redwood.say, "!" first, which the kernel says again every minute)
 * and answers KERN_FAILURE, so that the crash goes on exactly as before.
 */
#include <mach/exception_types.h>
#include <mach/thread_status.h>

static void
keep_line(const char *format, ...)
{
	char line[200] = "!";
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line + 1, sizeof(line) - 1, format, arguments);
	va_end(arguments);
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	say("%s\n", line + 1);
}

static void
name_address(const char *what, uintptr_t address)
{
	Dl_info info;

	if (address != 0 && dladdr((void *)address, &info) && info.dli_fname != NULL) {
		const char *image = strrchr(info.dli_fname, '/');

		keep_line("bbd fault %s %#lx %s+%#lx %s+%#lx", what, address, image ? image + 1 : info.dli_fname,
		    address - (uintptr_t)info.dli_fbase, info.dli_sname ? info.dli_sname : "?",
		    info.dli_saddr ? address - (uintptr_t)info.dli_saddr : 0);
	} else {
		keep_line("bbd fault %s %#lx (no image)", what, address);
	}
}

#pragma pack(4)
struct exception_request {
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t thread;
	mach_msg_port_descriptor_t task;
	NDR_record_t ndr;
	exception_type_t exception;
	mach_msg_type_number_t code_count;
	int64_t code[2];
	char trailer[64];
};
struct exception_reply {
	mach_msg_header_t header;
	NDR_record_t ndr;
	kern_return_t result;
};
#pragma pack()

static void *
exception_thread(void *argument)
{
	mach_port_t port = (mach_port_t)(uintptr_t)argument;

	for (;;) {
		struct exception_request request;
		struct exception_reply reply;

		memset(&request, 0, sizeof(request));
		if (mach_msg(&request.header, MACH_RCV_MSG, 0, sizeof(request), port, MACH_MSG_TIMEOUT_NONE,
		    MACH_PORT_NULL) != KERN_SUCCESS) {
			continue;
		}
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		mach_port_t thread = request.thread.name;

		keep_line("bbd fault: exception %d code %#llx %#llx", request.exception,
		    (unsigned long long)request.code[0], (unsigned long long)request.code[1]);
		{
			/* the memory around the address that faulted: which mapping it was in, and how much
			 * of it there is -- a write past the end of a GPU buffer's mapping (pan_preload_fb_part)
			 * says here how long the mapping really was */
			vm_address_t at = (vm_address_t)request.code[1] - 0x200;
			for (int k = 0; k < 2; k++) {
				vm_address_t start = at;
				vm_size_t size = 0;
				vm_region_basic_info_data_64_t info;
				mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
				mach_port_t object = MACH_PORT_NULL;

				if (vm_region_64(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
				    (vm_region_info_t)&info, &n, &object) == KERN_SUCCESS) {
					keep_line("bbd fault region from %#llx: %#llx..%#llx prot %d/%d shared %d", (unsigned long long)at,
					    (unsigned long long)start, (unsigned long long)(start + size), info.protection,
					    info.max_protection, info.shared);
				} else {
					keep_line("bbd fault region from %#llx: none", (unsigned long long)at);
				}
				at = (vm_address_t)request.code[1];
			}
		}
		if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
			uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
			uintptr_t lr = (uintptr_t)arm_thread_state64_get_lr(state);
			uintptr_t sp = (uintptr_t)arm_thread_state64_get_sp(state);
			uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

			keep_line("bbd fault sp %#lx fp %#lx", sp, fp);
			name_address("pc", pc);
			name_address("lr", lr);
			for (unsigned i = 0; i < 10 && fp != 0 && (fp & 7) == 0; i++) {
				uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

				if (ret == 0) {
					break;
				}
				name_address("frame", ret & 0x0000000fffffffffUL);
				fp = next > fp ? next : 0;
			}
		}
		memset(&reply, 0, sizeof(reply));
		reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.header.msgh_bits), 0);
		reply.header.msgh_remote_port = request.header.msgh_remote_port;
		reply.header.msgh_size = sizeof(reply);
		reply.header.msgh_id = request.header.msgh_id + 100;
		reply.ndr = NDR_record;
		reply.result = KERN_FAILURE;
		mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
		    MACH_PORT_NULL);
	}
	return NULL;
}

static void
watch_for_exceptions(void)
{
	mach_port_t port = MACH_PORT_NULL;
	pthread_t thread;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS ||
	    task_set_exception_ports(mach_task_self(),
	    EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT |
	    EXC_MASK_GUARD | EXC_MASK_RESOURCE, port, (exception_behavior_t)(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES),
	    THREAD_STATE_NONE) != KERN_SUCCESS) {
		say("redwood: camtl=1: no exception port\n");
		return;
	}
	pthread_create(&thread, NULL, exception_thread, (void *)(uintptr_t)port);
	say("redwood: camtl=1: exceptions are reported (kept lines \"bbd fault\")\n");
}

static void
give_quartzcore_its_shaders(void)
{
	Method method = class_getInstanceMethod(objc_getClass("NSBundle"), sel_registerName("URLForResource:withExtension:"));

	if (method == NULL) {
		say("redwood: camtl=1: no -[NSBundle URLForResource:withExtension:]; QuartzCore finds no shaders\n");
		return;
	}
	watch_for_faults();     /* where the Metal renderer dies, if it does */
	watch_for_exceptions();
	specialize_without_apple();
	/* The iPhone X's OLED panel has CoreAnimation correct for its overdrive with a look-up table
	   drawn by display_overdrive_lut_gen, a shader the macOS library has no copy of ("Metal failed
	   to load fragment function: function=display_overdrive_lut_gen_lph"). The panel here is an
	   LCD; CoreAnimation's own switch turns the correction off, read from the environment. */
	setenv("CA_DISABLE_DISPLAY_OVERDRIVE_FIX", "1", 1);
	{
		/* boot-arg ca_dirty=0: every frame drawn whole, not only its changed regions -- for telling
		 * whether the rectangles of older frames in the user's video (run 214) come from a surface
		 * that did not keep what CoreAnimation last drew into it */
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "ca_dirty=0") != NULL) {
			setenv("CA_DISABLE_DIRTY_REGIONS", "1", 1);
			keep_line("redwood: camtl=1: CA_DISABLE_DIRTY_REGIONS, every frame drawn whole (ca_dirty=0)");
		}
	}
	{
		/* and what the driver says on its way (src/redwoodmetal.m, rw_print, writes to standard
		   output, which launchd gives this job nowhere to go) */
		int console = open("/dev/console", O_WRONLY | O_NOCTTY);

		if (console >= 0) {
			dup2(console, 1);
			close(console);
		}
	}
	orig_url_for_resource = (id (*)(id, SEL, id, id))method_getImplementation(method);
	method_setImplementation(method, (IMP)my_url_for_resource);
	say("redwood: camtl=1: QuartzCore's shaders come from %s (%s)\n", QC_METALLIB,
	    access(QC_METALLIB, R_OK) == 0 ? "there" : "MISSING");
}

static void
window_server_keeps_opengl_es(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	Class cls;
	Method method;
	IMP original;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "camtl=1") != NULL) {
		say("redwood: camtl=1: CoreAnimation may have the Metal device\n");
		give_quartzcore_its_shaders();
		if (strstr(arguments, "bbd_prof=1") != NULL) {
			pthread_t profiler;

			pthread_create(&profiler, NULL, prof_main, NULL);
		}
		return;
	}
	cls = objc_getClass("CAWindowServer");
	method = cls ? class_getInstanceMethod(cls, sel_registerName("_detectDisplays")) : NULL;
	original = method ? method_getImplementation(method) : NULL;
	if (original == NULL || (uintptr_t)original != CA_DETECT_DISPLAYS + slide) {
		say("redwood: -[CAWindowServer _detectDisplays]: found %#lx, not what was expected; left alone, "
		    "and CoreAnimation may take the Metal road\n", original ? (uintptr_t)original - slide : 0);
		return;
	}
	orig_detect_displays = (void (*)(id, SEL))original;
	method_setImplementation(method, (IMP)my_detect_displays);
}

/*
 * Boot-arg bbd_prof=1: a sampling profiler of the window server's own -- where its threads are, a
 * thousand times a second, for the parts of a frame no counter says (inside panfrost's draws). Every
 * ten seconds the most sampled addresses in libOSMesa and RedwoodMetal go to the log ("bbdprof:"),
 * as offsets into the image for atos on the host, with how many samples each image had.
 */
#include <mach/thread_act.h>

#define PROF_SLOTS 4096
static struct { uint64_t key; uint32_t count; } prof_slots[PROF_SLOTS];
static uintptr_t prof_mesa_base, prof_metal_base;
static uint64_t prof_images[4];        /* libOSMesa, RedwoodMetal, elsewhere, idle (in the kernel's traps) */
static const char *prof_names[64];      /* the images of "elsewhere" */
static unsigned prof_nnames;

static void
prof_add(uint64_t key)
{
	for (unsigned i = (unsigned)(key * 2654435761u) % PROF_SLOTS, n = 0; n < PROF_SLOTS; n++, i = (i + 1) % PROF_SLOTS) {
		if (prof_slots[i].count == 0 || prof_slots[i].key == key) {
			prof_slots[i].key = key;
			prof_slots[i].count++;
			return;
		}
	}
}

static void *
prof_main(void *unused)
{
	Dl_info info;
	uint64_t round = 0;
	thread_t self = mach_thread_self();

	(void)unused;
	void *mesa = dlsym(RTLD_DEFAULT, "rwa_render_draw");
	if (mesa != NULL && dladdr(mesa, &info)) {
		prof_mesa_base = (uintptr_t)info.dli_fbase;
	}
	Class metal = objc_getClass("RedwoodMetalCommandBuffer");
	IMP commit = metal ? class_getMethodImplementation(metal, sel_registerName("commit")) : NULL;
	if (commit != NULL && dladdr((void *)commit, &info)) {
		prof_metal_base = (uintptr_t)info.dli_fbase;
	}
	for (;;) {
		thread_act_array_t threads;
		mach_msg_type_number_t count;

		usleep(1000);
		if (prof_metal_base == 0 && (metal = objc_getClass("RedwoodMetalCommandBuffer")) != NULL &&
		    (commit = class_getMethodImplementation(metal, sel_registerName("commit"))) != NULL &&
		    dladdr((void *)commit, &info)) {
			prof_metal_base = (uintptr_t)info.dli_fbase;       /* loaded after the profiler started */
		}
		if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
			continue;
		}
		for (unsigned i = 0; i < count; i++) {
			if (threads[i] != self && thread_suspend(threads[i]) == KERN_SUCCESS) {
				arm_thread_state64_t state;
				mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;

				if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&state, &n) == KERN_SUCCESS) {
					uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(state);
					uint32_t insn = 0;

					if (prof_mesa_base && pc >= prof_mesa_base && pc < prof_mesa_base + (16u << 20)) {
						prof_images[0]++;
						prof_add(((uint64_t)0 << 60) | ((pc - prof_mesa_base) & ~0xfull));
					} else if (prof_metal_base && pc >= prof_metal_base && pc < prof_metal_base + (1u << 20)) {
						prof_images[1]++;
						prof_add(((uint64_t)1 << 60) | ((pc - prof_metal_base) & ~0x3full));
					} else if (pc != 0 && (memcpy(&insn, (void *)(pc - 4), 4), insn == 0xd4001001)) {
						prof_images[3]++;               /* just after an svc: waiting in the kernel */
					} else {
						prof_images[2]++;
						if (dladdr((void *)pc, &info) && info.dli_fname != NULL) {
							/* the others by image only */
							unsigned k;
							for (k = 0; k < prof_nnames && prof_names[k] != info.dli_fname; k++) {
							}
							if (k == prof_nnames && k < 64) {
								prof_names[prof_nnames++] = info.dli_fname;
							}
							prof_add(((uint64_t)2 << 60) | k);
						} else {
							prof_add(((uint64_t)3 << 60) | (pc >> 12));    /* in no image: by page */
						}
					}
				}
				thread_resume(threads[i]);
			}
			mach_port_deallocate(mach_task_self(), threads[i]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(thread_t));
		if (++round % 10000 == 0) {
			keep_line("bbdprof: libOSMesa %llu, RedwoodMetal %llu, elsewhere %llu, in the kernel %llu",
			    prof_images[0], prof_images[1], prof_images[2], prof_images[3]);
			/* the whole run's so far, kept: the busy minute is gone from the log by the end of it */
			static bool picked[PROF_SLOTS];

			memset(picked, 0, sizeof(picked));
			for (int top = 0; top < 42 && prof_images[0] > 300; top++) {
				unsigned best = PROF_SLOTS;
				uint64_t want = top < 24 ? 0 : top < 36 ? 2 : 3;  /* 24 places in libOSMesa, the other images, pages of none */
				for (unsigned i = 0; i < PROF_SLOTS; i++) {
					if (prof_slots[i].count != 0 && prof_slots[i].key >> 60 == want && !picked[i] &&
					    (best == PROF_SLOTS || prof_slots[i].count > prof_slots[best].count)) {
						best = i;
					}
				}
				if (best == PROF_SLOTS) {
					if (top < 24) {
						top = 23;
					} else if (top < 36) {
						top = 35;
					}
					continue;
				}
				uint64_t key = prof_slots[best].key;
				char line[160];

				if (key >> 60 == 3) {
					uint64_t page = (key & 0xfffffffffffffffull) << 12;
					vm_address_t region = (vm_address_t)page;
					vm_size_t size = 0;
					vm_region_basic_info_data_64_t info;
					mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
					mach_port_t object = MACH_PORT_NULL;

					(void)vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
					    (vm_region_info_t)&info, &n, &object);
					snprintf(line, sizeof(line), "!bbdprof: page %#llx (region %#lx+%#lx prot %d) %u",
					    (unsigned long long)page, (unsigned long)region, (unsigned long)size, info.protection,
					    prof_slots[best].count);
				} else if (key >> 60 == 2) {
					unsigned k = (unsigned)(key & 0xff);
					const char *name = k < prof_nnames ? strrchr(prof_names[k], '/') : NULL;

					snprintf(line, sizeof(line), "!bbdprof: %s %u", name ? name + 1 : "?", prof_slots[best].count);
				} else {
					snprintf(line, sizeof(line), "!bbdprof: %s+%#llx %u", key >> 60 == 0 ? "OSMesa" : "RedwoodMetal",
					    (unsigned long long)(key & 0xfffffffffffffffull), prof_slots[best].count);
				}
				sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
				picked[best] = true;
			}
		}
	}
	return NULL;
}

static void
window_server_fixes(void)
{
	window_server_keeps_opengl_es();
	orig_is_ready = replace_virtual("is_ready", VSLOT(VT_APPLE_CLCD_DISPLAY, 0xb0), 0x1882f0908UL, my_is_ready);
	orig_ignore_update_p = replace_virtual("ignore_update_p", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x138), 0x1882e0c90UL,
	    my_ignore_update_p);
	orig_begin_update = replace_virtual("begin_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x170), 0x1882ddda4UL,
	    my_begin_update);
	orig_finish_update = replace_virtual("finish_update", VSLOT(VT_APPLE_CLCD_DISPLAY, 0x178), 0x1882c8568UL,
	    my_finish_update);
	orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, 0x1a0), 0x1882e2548UL, my_renderer);
	orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, 0x178), 0x18835770cUL,
	    my_render_update);
	if (gl_loaded) {
		orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), 0x1884a9308UL,
		    my_function_supported);
		if (gl_function_values != NULL) {
			orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), 0x1884a9350UL,
			    my_function_uniform);
		}
		/* always, because the boot-args are read in another place than this and the order of the
		   two is not ours to rely on; the hook writes nothing unless glcompile=1 */
		orig_compile_shader = (uintptr_t (*)(void *, const char *))replace_virtual("compile_shader",
		    VSLOT(VT_GLES_CONTEXT, 0x060), 0x1883fc808UL, my_compile_shader);
		say("redwood: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("redwood: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
	    orig_finish_update && orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
}

/* ---- backboardd: no ambient light sensor ------------------------------------------------------------ */

/*
 * CoreBrightness (-[BLControl waitForALSArrival], called while the window server starts) waits
 * five seconds for the ambient light sensor to turn up among the HID services. There is none
 * and none will come: do not wait for that.
 *
 * Wait for something else instead, at this convenient point: our own touch screen service.
 * backboardd takes for its touch screen a service that is there when its touch processing
 * starts, a moment later; one that turns up afterwards it does not notice. (For as long as
 * the five seconds were waited here, ours was always in time by accident.)
 */
static dispatch_semaphore_t input_services_made;

static void
wait_for_als_arrival(__unused id self, __unused SEL selector)
{
	if (input_services_made != NULL &&
	    dispatch_semaphore_wait(input_services_made, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
		say("redwood: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("redwood: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("redwood: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("redwood: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("redwood: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("redwood: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct redwood_touch *t)
{
	uint32_t change = t->type == REDWOOD_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != REDWOOD_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != REDWOOD_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == REDWOOD_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.redwood.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static void
send_button(IOHIDServiceClientRef buttons, const struct redwood_touch *t)
{
	uint32_t usage = t->finger == REDWOOD_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == REDWOOD_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == REDWOOD_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == REDWOOD_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == REDWOOD_BUTTON_LOCK) {
		publish_lock_button(t->type == REDWOOD_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == REDWOOD_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct redwood_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("redwood input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open(REDWOOD_TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		say("redwood: input: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("redwood: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
		screen = make_service(client, "Redwood touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("redwood: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "Redwood buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	say("redwood: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (count++ < 4) {
			say("redwood: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > REDWOOD_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == REDWOOD_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct redwood_touch press = { .time = t.time, .type = REDWOOD_BUTTON_DOWN, .finger = REDWOOD_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = REDWOOD_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == REDWOOD_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("redwood: input: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
	return NULL;
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

/* ---- containermanagerd ---------------------------------------------------------------------------- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1c3ea6298UL + slide) {
		say("redwood: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("redwood: container manager: no personas\n");
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1; on the Infinix mali=1), which the service RedwoodGPU
 * (RedwoodMali) says (it is there
 * before the first program runs, or not at all). Everywhere else -- QEMU, a run without the
 * boot-arg, a phone without the GPU's firmware in the image, a GPU that did not start --
 * nothing is loaded, and CoreAnimation draws with its own software renderer as in an image
 * without the library.
 * Boot-arg gl=0: not even with a GPU; gl=1: also without one, by Mesa's software driver (slow;
 * for working on eagl.c in QEMU).
 */
static void
opengl_es(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;
	void *library;
	int wanted = -1, told = 0;

	if (access(REDWOOD_GL, R_OK) != 0) {
		return;                                 /* (an image built without it) */
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		functions_said_yes(arguments);
		compile_shaders_said = strstr(arguments, "glcompile=1") != NULL;
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				wanted = argument[3] != '0';
				told = 1;
			}
		}
	}
	if (wanted < 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
		if (gpu == 0) {
			/* the Infinix's Mali, kept by the kernel with boot-arg mali=1 (iokit/Kernel/RedwoodMali.cpp) */
			gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodMali"));
		}
		wanted = gpu != 0;
		if (gpu != 0) {
			IOObjectRelease(gpu);
		}
	}
	if (!wanted) {
		say("redwood: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	if ((library = dlopen(REDWOOD_GL, RTLD_NOW)) == NULL) {
		say("redwood: %s: %s\n", REDWOOD_GL, dlerror());
		return;
	}
	gl_loaded = 1;
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
	    "redwood_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(library, "redwood_gl_function_refused");
}

/* ---- backboardd: the device's orientation from the accelerometer --------------------------------------- */
/*
 * backboardd learns which way up the phone is from CoreMotion (-[BKOrientationManager
 * _queue_updateDeviceOrientationFromCoreMotion:], "Updating device orientation from CoreMotion to:"),
 * which on an iPhone has it from the motion co-processor. Here the kernel has the accelerometer
 * (bsd/dev/redwood_scp.c, scp_step=7: kern.redwood.accel, "x y z count" in 1/1000 m/s^2), and this
 * reads it five times a second and hands the manager the orientation it makes: the axis gravity is
 * on, when it is clearly on one (more than 6.5 m/s^2 of 9.8), the same two readings running --
 * CMDeviceOrientation: 1 portrait, 2 upside down, 3 landscape left (the top to the left), 4
 * landscape right, 5 face up, 6 face down. Android's axes are taken for the hub's: x to the right,
 * y to the top, z out of the screen (face up: z = +9.8, run 292) -- x and y not checked yet.
 * The manager is the one whose methods are seen called (the first three below at least run at
 * boot); its own queue (ivar _queue) is where its _queue_ methods run. The method takes CoreMotion's
 * orientation manager (v24@0:8@16, checked before every call); it is given a stand-in (below). Boot-arg orientation=0: none of this.
 */
static id orientation_manager;
static IMP orientation_events_original, orientation_interface_original, orientation_sync_original;

static void
orientation_events_enabled(id self, SEL selector, BOOL enabled)
{
	orientation_manager = self;
	((void (*)(id, SEL, BOOL))orientation_events_original)(self, selector, enabled);
}

static void
orientation_interface(id self, SEL selector, long long orientation)
{
	orientation_manager = self;
	((void (*)(id, SEL, long long))orientation_interface_original)(self, selector, orientation);
}

static void
orientation_sync(id self, SEL selector)
{
	orientation_manager = self;
	((void (*)(id, SEL))orientation_sync_original)(self, selector);
}

static long long
orientation_from(int x, int y, int z)
{
	const int clear = 6500;

	if (z > clear) {
		return 5;
	}
	if (z < -clear) {
		return 6;
	}
	if (abs(y) >= abs(x)) {
		return y > clear ? 1 : y < -clear ? 2 : 0;
	}
	return x > clear ? 3 : x < -clear ? 4 : 0;
}

static Class orientation_class;
static int orientation_state;
static long long orientation_now;       /* what the stand-in for CoreMotion says */
static unsigned orientation_asked;

/*
 * -[BKOrientationManager _queue_updateDeviceOrientationFromCoreMotion:] takes an object (v24@0:8@16,
 * run 301) -- CoreMotion's, of which it asks its orientation (run 302). This is a
 * stand-in: orientation (and deviceOrientation) is the accelerometer's; any other selector sent to it answers 0 (a
 * method made for it on the spot) and is said, the first ten, so that what backboardd asks is seen.
 */
static long long
stand_in_orientation(__unused id self, __unused SEL selector)
{
	return orientation_now;
}

static long long
stand_in_zero(__unused id self, __unused SEL selector)
{
	return 0;
}

static BOOL
stand_in_resolve(Class self, SEL selector, SEL wanted)
{
	if (orientation_asked++ < 10) {
		keep_line("orientation: the stand-in for CoreMotion asked for %s: 0", sel_getName(wanted));
	}
	class_addMethod(self, wanted, (IMP)stand_in_zero, "q16@0:8");
	return YES;
}

static id
orientation_stand_in(void)
{
	static id made;

	if (made == nil) {
		Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "RedwoodDeviceOrientationSource", 0);

		class_addMethod(cls, sel_registerName("deviceOrientation"), (IMP)stand_in_orientation, "q16@0:8");
		/* what backboardd asks for (run 302): a CMDeviceOrientation's own property */
		class_addMethod(cls, sel_registerName("orientation"), (IMP)stand_in_orientation, "q16@0:8");
		class_addMethod(object_getClass((id)cls), sel_registerName("resolveInstanceMethod:"), (IMP)stand_in_resolve, "B24@0:8:16");
		objc_registerClassPair(cls);
		made = ((id (*)(id, SEL))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc")),
		    sel_registerName("init"));
	}
	return made;
}           /* kern.redwood.orientation_state (bsd/dev/redwood_scp.c) */

static void
orientation_say_state(int set, int clear)
{
	orientation_state = (orientation_state & ~clear) | set;
	sysctlbyname("kern.redwood.orientation_state", NULL, NULL, &orientation_state, sizeof(orientation_state));
}

/* the class found and the three methods hooked, once backboardd is far enough for it to be there */
static void
orientation_hook(void)
{
	static const struct { const char *selector; IMP replacement; IMP *original; } hooks[] = {
		{ "_queue_setOrientationEventsEnabled:", (IMP)orientation_events_enabled, &orientation_events_original },
		{ "_queue_setCurrentUserInterfaceOrientation:", (IMP)orientation_interface, &orientation_interface_original },
		{ "_queue_updateDeviceOrientationSynchronously", (IMP)orientation_sync, &orientation_sync_original },
	};
	Class cls = objc_getClass("BKOrientationManager");

	if (cls == NULL || orientation_class != NULL) {
		return;
	}
	for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		Method method = class_getInstanceMethod(cls, sel_registerName(hooks[i].selector));

		if (method != NULL) {
			*hooks[i].original = method_setImplementation(method, hooks[i].replacement);
		}
	}
	orientation_class = cls;
	orientation_say_state(1 << 8, 0);
	keep_line("orientation: BKOrientationManager found and hooked");
}

static void (^orientation_tick)(void);
static volatile bool orientation_stop;

static void *
orientation_thread(__unused void *argument)
{
	sleep(2);
	while (!orientation_stop) {
		orientation_tick();
		usleep(200000);
	}
	return NULL;
}

static void
orientation_start(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t thread;
	static long long sent, seen;
	static unsigned changes;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "orientation=0") != NULL) {
		return;
	}
	orientation_say_state(1 << 12, 0);      /* (bit 12: started) */
	orientation_hook();             /* (at this point the class may not be there yet: then from the timer) */
	/* a thread of its own, as the touch screen's: a dispatch timer made here never fired (run 298: state 0x1100) */
	orientation_tick = ^{
		Class cls = orientation_class;
		char value[64] = "";
		size_t length = sizeof(value) - 1;
		int x, y, z;
		long long now;
		SEL update = sel_registerName("_queue_updateDeviceOrientationFromCoreMotion:");
		Method method = class_getInstanceMethod(cls, update);
		Ivar queue_ivar = class_getInstanceVariable(cls, "_queue");
		static int told;

		static unsigned ticks;

		if (++ticks % 5 == 0) {
			orientation_say_state((int)((ticks / 5) << 16) | (1 << 11) | (orientation_manager ? 1 << 9 : 0) |
			    (method && strcmp(method_getTypeEncoding(method), "v24@0:8@16") == 0 ? 1 << 10 : 0) | (int)(sent & 0xff),
			    (int)0xffff0000 | 0xeff);
		}
		if (ticks % 300 == 75) {      /* 15 s in, then a line a minute, so that one is near the end of the log */
			keep_line("orientation: a minute on: class %s, last sent %lld, seen %lld, %u changes; manager %p, update method %s",
			    cls ? "found" : "NOT FOUND", sent, seen, changes, (void *)orientation_manager,
			    method ? method_getTypeEncoding(method) : "none");
		}
		if (cls == NULL) {
			orientation_hook();
			return;
		}
		if (sysctlbyname("kern.redwood.accel", value, &length, NULL, 0) != 0 || sscanf(value, "%d %d %d", &x, &y, &z) != 3) {
			/* not yet: the kernel brings the sensor hub up in a thread of its own, ~20 s, often after
			 * backboardd is here (run 300: the feed gave up on the first "none" and was gone) */
			if (!told++) {
				keep_line("orientation: kern.redwood.accel says \"%s\": waiting for it (scp_step=7?)", value);
			}
			return;
		}
		now = orientation_from(x, y, z);
		if (now == 0 || now != seen) {
			seen = now;                     /* the same twice before it counts */
			return;
		}
		if (now == sent || orientation_manager == nil || method == NULL || queue_ivar == NULL) {
			if (told++ == 0 && (orientation_manager == nil || method == NULL || queue_ivar == NULL)) {
				keep_line("orientation: the manager %p, its method %p, its queue ivar %p: not yet", (void *)orientation_manager,
				    (void *)method, (void *)queue_ivar);
			}
			return;
		}
		if (strcmp(method_getTypeEncoding(method), "v24@0:8@16") != 0) {
			if (told++ < 2) {
				keep_line("orientation: _queue_updateDeviceOrientationFromCoreMotion: is %s, not v24@0:8@16: left alone",
				    method_getTypeEncoding(method));
			}
			return;
		}
		sent = now;
		if (changes++ < 30) {
			keep_line("orientation: %lld from x %d y %d z %d", now, x, y, z);
		}
		dispatch_async((dispatch_queue_t)object_getIvar(orientation_manager, queue_ivar), ^{
			orientation_now = now;
			((void (*)(id, SEL, id))objc_msgSend)(orientation_manager, update, orientation_stand_in());
		});
	};
	orientation_tick = Block_copy(orientation_tick);
	if (pthread_create(&thread, NULL, orientation_thread, NULL) == 0) {
		pthread_detach(thread);
	}
	say("redwood: orientation: from kern.redwood.accel, five times a second (orientation=0 to leave out)\n");
}

__attribute__((constructor))
static void
redwood_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "backboardd") == 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		/*
		 * Boot-arg ca_vbl=1: CoreAnimation's frame clock from the display driver's refresh
		 * notifications. QuartzCore takes them (IOMobileFramebufferEnableVSyncNotifications) only on
		 * the phones with an A7 to A10, or when CA_FORCE_TIMER is in the environment
		 * (CADeviceUseVBL()); on the iPhone X it runs a timer of its own instead (CADisplayTimer),
		 * whose phase it would set from the frame timings of a display driver this one is not.
		 */
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "ca_vbl=1") != NULL) {
			setenv("CA_FORCE_TIMER", "1", 1);
			say("redwood: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		/*
		 * Boot-arg caenv=NAME=VALUE (as many as wanted): into the window server's environment, for
		 * CoreAnimation's own switches (CA_LOG_TRIPLE_BUFFERING=1, CA_FORCE_DOUBLE_BUFFERING=1 ...).
		 */
		for (const char *argument = arguments; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
			char setting[128];
			size_t length = strcspn(argument + 6, " ");
			char *equals;

			if ((argument != arguments && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
				continue;
			}
			memcpy(setting, argument + 6, length);
			setting[length] = 0;
			if ((equals = strchr(setting, '=')) != NULL) {
				*equals = 0;
				setenv(setting, equals + 1, 1);
				say("redwood: caenv: %s=%s for the window server\n", setting, equals + 1);
			}
		}
		opengl_es();
		window_server_fixes();
		/*
		 * brightness_fixes() is not used for now: with it backboardd no longer took our touch
		 * screen service for its own ("still waiting for a main screen digitizer"), whether the
		 * service was made before or after the touch processing starts. (Most likely it only
		 * moved the start of the event server onto the moment make_service() now knows how to
		 * survive; not tried again since, and until it is, the five seconds stay.)
		 */
		input_start();
		orientation_start();
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	} else if (strcmp(name, "com.apple.WebKit.GPU") == 0) {
		/*
		 * WebKit's GPU process draws pages with CoreAnimation's Metal renderer too, and dies on the
		 * same assert as backboardd would without us: ogl-metal.mm:3308 "metallib_url != nil",
		 * the moment it opens the GPU (run 460: Safari's pages came in over the Wi-Fi, 3 MB, and
		 * were never drawn). So with camtl=1 it gets the window server's shaders and their hooks.
		 */
		window_server_keeps_opengl_es();
	} else {
		/*
		 * Any other app we are injected into (Preferences for the Apple ID 2FA sheet, the App Store):
		 * with camtl=1 its QuartzCore takes the Metal road too and asserts metallib_url != nil in
		 * CA::OGL::new_metal_context (ogl-metal.mm:3308), because QuartzCore.framework/default.metallib
		 * is not in the image -- which is why Settings aborted on the 2FA code sheet (run 473,
		 * userabort Preferences: _abort <- _err <- QuartzCore). Give the app the same shaders and hooks
		 * the window server gets, so it does not abort. give_quartzcore_its_shaders() is process-agnostic.
		 */
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "camtl=1") != NULL) {
			say("redwood: camtl=1: %s gets QuartzCore's Metal shaders too\n", name);
			give_quartzcore_its_shaders();
		}
	}
}
