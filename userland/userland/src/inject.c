/*
 * libhakios.dylib: code of ours inside stock Darwin programs.
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
 *                a "virtual" HID service that turns the samples of /dev/hakios-touch into the
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
 * Addresses of things inside the dyld shared cache are those of Darwin 16.5 (20F66) for iPhone10,3;
 * every one is checked against what is expected to be there before it is touched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "board/gl.h"
#include "board/input.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */
#define HAKIOS_GL "/hakios/lib/libhakios-gl.dylib"

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
		say("hakios: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	/* the tables are in read-only memory shared by every process: get a private writable copy of the page */
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hakios: %s: cannot make the table writable (%d)\n", what, kr);
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
		say("hakios: is_ready -> %d (call %u, flags %#llx, b233 %#x)\n", ready, calls_ready,
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

		say("hakios: ignore_update_p -> %d (call %u, flags %#llx, update flags %#x)\n", ignore, calls_ignore,
		    (unsigned long long)DISPLAY_FLAGS(display), *(uint32_t *)((char *)update + 0xf0));
		say("hakios:   display +0x220: %#llx %#llx %#llx %#llx %#llx %#llx\n", (unsigned long long)q[0],
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
		say("hakios: begin_update -> %#lx (flags %#llx)\n", r, (unsigned long long)DISPLAY_FLAGS(display));
	}
	return r;
}

static uintptr_t
my_finish_update(void *display, void *update, unsigned flags)
{
	uintptr_t r = orig_finish_update(display, update, flags);

	if (calls_finish++ < 6) {
		say("hakios: finish_update(%#x) -> %#lx\n", flags, r);
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
		say("hakios: no accelerated renderer; the software one will draw\n");
	} else if (!said) {
		said = 1;
		say("hakios: the server has an accelerated renderer (%p)\n", renderer);
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hakios: render_update -> %#lx (flags %#x)\n", r, flags ? *flags : 0);
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
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss);
	}
	/*
	 * And who called, by walking the frame pointers: on arm64 each frame holds the previous one
	 * at [fp] and its return address at [fp + 8]. The crash itself is usually in a library
	 * function -- strdup of a null name, say -- and the frame that matters is two or three up.
	 */
	char frames[8 * 20 + 1] = "";
	size_t used = 0;
	uintptr_t fp = context != NULL && context->uc_mcontext != NULL ? (uintptr_t)__darwin_arm_thread_state64_get_fp(context->uc_mcontext->__ss) : 0;

	for (unsigned i = 0; i < 8 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %#lx", ret - slide);
		fp = next > fp ? next : 0;      /* the chain only ever goes up the stack */
	}
	say("hakios: signal %d at %p, pc %#lx, lr %#lx, called from%s (unslid)\n", signal_number,
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
	/* An Objective-C exception that nobody catches ends in abort(), not in a fault, and a program
	   that dies that way says nothing at all here -- a run that restarted a daemon 31 times was
	   silent until these three were watched as well. */
	sigaction(SIGABRT, &action, NULL);
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
		say("hakios: CoreAnimation's function %u has a shader of its own; left alone\n", function);
		return;
	}
	/* the table is read-only memory shared by every process: a private writable copy of the page */
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("hakios: CoreAnimation's function %u: cannot make the table of shaders writable\n", function);
		return;
	}
	*entry = (uintptr_t)source;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	say("hakios: CoreAnimation's function %u (\"sb3\") is given a shader of ours\n", function);
}

static uint8_t say_yes[90];
static int compile_shaders_said;                /* boot-arg glcompile=1: write down every shader source */

static void
functions_said_yes(const char *arguments)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	const char *argument = strstr(arguments, "glno=");

	/* glno=N[,N...]: and the other way about, for seeing what a function of ours stands in for */
	for (argument = argument != NULL ? argument + 5 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 0;
			say("hakios: the renderer will not answer that it has CoreAnimation's function %u (\"%.3s\")\n",
			    function, names + 4 * function);
		}
		argument = *argument == ',' ? argument + 1 : NULL;
	}
	argument = strstr(arguments, "glyes=");
	for (argument = argument != NULL ? argument + 6 : NULL; argument != NULL && *argument >= '0' && *argument <= '9';) {
		unsigned function = (unsigned)strtoul(argument, (char **)&argument, 10);

		if (function < 90) {
			say_yes[function] = 1;
			say("hakios: the renderer will answer that it has CoreAnimation's function %u (\"%.3s\")\n",
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
		say("hakios: CoreAnimation compiles its shaders through the table (the first of them, %zu characters)\n",
		    source != NULL ? strlen(source) : 0);
	}
	if (compile_shaders_said && told < 24) {
		told++;
		say("hakios: CoreAnimation compiles a shader, %zu characters:\n", source != NULL ? strlen(source) : 0);
		for (size_t at = 0, length = source != NULL ? strlen(source) : 0; at < length; at += 700) {
			say("hakios:  %.700s\n", source + at);
		}
	}
	return orig_compile_shader(context, source);
}

static int
my_function_supported(void *context)
{
	static const char names[] = HAKIOS_CA_FUNCTIONS;
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	uint8_t function = *(*(uint8_t **)((char *)context + 0x10) + 0x10);

	if (!supported && function < 90 && say_yes[function] && gl_loaded) {
		if (!said[function]) {
			said[function] = 1;
			say("hakios: CoreAnimation's function %u (\"%.3s\"): the renderer is made to say it has it\n",
			    function, names + 4 * function);
		}
		return 1;
	}
	if (!supported && gl_function_refused != NULL) {
		gl_function_refused(function);
	}
	if (!supported && !said[function]) {
		said[function] = 1;
		say("hakios: CoreAnimation's OpenGL ES renderer has no function %u (\"%.3s\"): what needs it is not drawn\n",
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
			say("hakios: function %u takes its numbers from %#lx %#lx %#lx\n", function,
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
 * this image (darwin165/select-system.py drops every .metallib under the framework directories). That is
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
		say("hakios: metal: this process has no MTLCreateSystemDefaultDevice; nothing to keep\n");
		return;
	}
	if ((device = create()) == NULL) {
		say("hakios: metal: no device in this process; CoreAnimation keeps OpenGL ES by itself\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("hakios: metal: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("hakios: metal: %#lx holds a %s, not Metal's device array; left alone\n", MTL_DEVICE_ARRAY,
		    object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("hakios: metal: %s taken out of the window server (%lu in the list, %lu now); "
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

/* (method is -[CAWindowServer _detectDisplays], already found where 16.5 has it: window_server_fixes) */
static void
window_server_keeps_opengl_es(Method method)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "camtl=1") != NULL) {
		say("hakios: camtl=1: CoreAnimation may have the Metal device (it needs "
		    "QuartzCore.framework/default.metallib in the image)\n");
		return;
	}
	orig_detect_displays = (void (*)(id, SEL))method_getImplementation(method);
	method_setImplementation(method, (IMP)my_detect_displays);
}

static void
window_server_fixes(void)
{
	Class cls = objc_getClass("CAWindowServer");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("_detectDisplays")) : NULL;
	IMP detect = method ? method_getImplementation(method) : NULL;

	/*
	 * Every address of QuartzCore's in this part is 16.5's (20F66), the tables' as well as the code's,
	 * and this library also goes unchanged into the backboardd of Darwin 26 and 27, where they are
	 * something else or not mapped at all. Even reading a table to compare its entry is not safe
	 * there, so one method is looked at first, and nothing of QuartzCore's is read or written
	 * unless that method is where 16.5 has it.
	 */
	if (detect == NULL || (uintptr_t)detect != CA_DETECT_DISPLAYS + slide) {
		say("hakios: -[CAWindowServer _detectDisplays]: found %#lx, not 16.5's; the window server is left alone, "
		    "and CoreAnimation may take the Metal road\n", detect ? (uintptr_t)detect - slide : 0);
		return;
	}
	window_server_keeps_opengl_es(method);
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
		say("hakios: the shader compiler of CoreAnimation is %s\n", orig_compile_shader ? "watched" : "NOT watched");
		watch_for_faults();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			/* (only into the table of the renderer whose function_supported was found where 16.5 has it) */
			if (orig_function_supported != NULL && sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
			    strstr(arguments, "glsb3=1") != NULL) {
				give_the_renderer_a_shader(40, shadow_blur_source);
			}
		}
	}
	say("hakios: window server hooks %s\n", orig_is_ready && orig_ignore_update_p && orig_begin_update &&
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
		say("hakios: input: no touch screen service after five seconds; going on without\n");
	}
}

__attribute__((unused)) static void
brightness_fixes(void)
{
	Class cls = objc_getClass("BLControl");
	Method method = cls ? class_getInstanceMethod(cls, sel_registerName("waitForALSArrival")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != 0x1af043734UL + slide) {
		say("hakios: -[BLControl waitForALSArrival]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)wait_for_als_arrival);
}

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on Darwin (prototypes checked against 20F66) */
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
	volatile long long report_interval;     /* the "ReportInterval" a client last set (microseconds; -1: none yet) */
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
		say("hakios: input: the event server asks for property \"%s\", which we do not have\n", name);
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
			say("hakios: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	/* how often a client (locationd) wants a sensor's samples: hid_motion.h */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("ReportInterval")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		long long interval = -1;

		CFNumberGetValue(value, kCFNumberSInt64Type, &interval);
		((struct service *)refcon)->report_interval = interval;
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
		say("hakios: input: the event server sets \"%s\" = %s\n", name, text);
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
 *
 * KEPT, if not NULL, is given the service's own state, for a caller that reads what clients set on it.
 */
static IOHIDServiceClientRef
make_service_kept(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display,
    struct service **kept)
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
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("HakiOS"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* (the dictionary holds what it was given: these references are let go) */
	CFRelease(name);
	CFRelease(page_number);
	CFRelease(usage_number);
	CFRelease(zero);
	CFRelease(pairs);
	CFRelease(pair);
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	made->report_interval = -1;
	if (kept != NULL) {
		*kept = made;
	}
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
		say("hakios: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	return make_service_kept(client, product, page, usage, display, NULL);
}

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch(IOHIDServiceClientRef screen, const struct hakios_touch *t)
{
	uint32_t change = t->type == HAKIOS_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != HAKIOS_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != HAKIOS_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == HAKIOS_TOUCH_DOWN) {
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

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}

static uint32_t buttons_held;           /* one bit for each button (HAKIOS_BUTTON_*) whose press was sent and not its release */

static void
send_button(IOHIDServiceClientRef buttons, const struct hakios_touch *t)
{
	uint32_t usage = t->finger == HAKIOS_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == HAKIOS_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == HAKIOS_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == HAKIOS_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == HAKIOS_BUTTON_LOCK) {
		publish_lock_button(t->type == HAKIOS_BUTTON_DOWN);
	}
	if (t->type == HAKIOS_BUTTON_DOWN) {
		buttons_held |= 1u << t->finger;
	} else {
		buttons_held &= ~(1u << t->finger);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == HAKIOS_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Where the samples that would have lifted them may be lost (the device given up on and opened again):
 * every finger that is down lifted where it was last, and every button that is held let go -- or the
 * window server keeps a finger on the screen, or the side button held, for good. (Only the input thread
 * sends touches and buttons in this library, so fingers[] and buttons_held are its own.)
 */
static void
release_all(IOHIDServiceClientRef screen, IOHIDServiceClientRef buttons)
{
	struct hakios_touch t = { .time = mach_absolute_time() };
	uint32_t held = buttons_held;

	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (fingers[i].touching) {
			t.type = HAKIOS_TOUCH_UP;
			t.finger = i;
			t.x = (uint32_t)(fingers[i].x * 65535 + 0.5);
			t.y = (uint32_t)(fingers[i].y * 65535 + 0.5);
			send_touch(screen, &t);
		}
	}
	t.type = HAKIOS_BUTTON_UP;
	t.x = t.y = 0;
	for (uint32_t b = 0; buttons != NULL && b < 32; b++) {
		if (held & (1u << b)) {
			t.finger = b;
			send_button(buttons, &t);
		}
	}
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

/* the phone's orientation, as a service of its own beside the touch screen and the buttons */
#include "hid_orientation.h"
/* ... and the accelerometer's samples, for Darwin's motion stack, as another */
#include "hid_motion.h"
/* ... and the gyroscope's rates, for the same, as a third */
#include "hid_gyro.h"

/*
 * /dev/hakios-touch, opened for reading. One program may read it (board/input.c): while another does -- a
 * backboardd of before that is still going away -- the open is EBUSY, and is tried again each second
 * rather than leaving this one without touches for good. -1: it cannot be read at all (said why).
 */
static int
open_touch_device(void)
{
	int device;
	bool said = false;

	while ((device = open(HAKIOS_TOUCH_DEVICE, O_RDONLY)) < 0 && (errno == EBUSY || errno == EINTR)) {
		if (errno == EBUSY && !said) {
			say("hakios: input: %s: %s: waiting for the other reader to let it go\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
			said = true;
		}
		sleep(1);
	}
	if (device < 0) {
		say("hakios: input: %s: %s\n", HAKIOS_TOUCH_DEVICE, strerror(errno));
	} else if (said) {
		say("hakios: input: %s: ours now\n", HAKIOS_TOUCH_DEVICE);
	}
	return device;
}

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct hakios_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("hakios input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open_touch_device();
	if (device < 0) {
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("hakios: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("hakios.input", NULL));
		screen = make_service(client, "HakiOS touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("hakios: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "HakiOS buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
		orientation_start(client);      /* (the phone's orientation, when the kernel follows it: hid_orientation.h) */
		motion_start(client);           /* (the accelerometer's samples, when the kernel gives them: hid_motion.h) */
		gyro_start(client);             /* (the gyroscope's rates, when the kernel gives them: hid_gyro.h) */
	}
	say("hakios: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}

	for (;;) {
		ssize_t got = read(device, &t, sizeof(t));
		uint64_t read_at = mach_absolute_time();

		if (got != (ssize_t)sizeof(t)) {
			/*
			 * A signal (EINTR) took nothing: read again. Anything else may have taken samples with it: what is
			 * held is let go, and the device opened again -- the thread that ended here left the phone with no
			 * touch screen and no buttons until backboardd was started again, and with a finger held, if one was.
			 */
			if (got < 0 && errno == EINTR) {
				continue;
			}
			say("hakios: input: %s: %s: opening it again\n", HAKIOS_TOUCH_DEVICE, got < 0 ? strerror(errno) :
			    "not a whole sample");
			release_all(screen, buttons);
			waking = 0;
			close(device);
			sleep(1);
			if ((device = open_touch_device()) < 0) {
				return NULL;
			}
			continue;
		}

		if (count++ < 4) {
			say("hakios: input: sample type %u finger %u at %u %u\n", t.type, t.finger, t.x, t.y);
		}
		if (t.type > HAKIOS_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == HAKIOS_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct hakios_touch press = { .time = t.time, .type = HAKIOS_BUTTON_DOWN, .finger = HAKIOS_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = HAKIOS_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == HAKIOS_TOUCH_UP) {
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
		say("hakios: +[MCMUserIdentityCache personasAreSupported]: found %#lx, not what was expected; left alone\n",
		    original ? (uintptr_t)original - slide : 0);
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	say("hakios: container manager: no personas\n");
}

/* ---------------------------------------------------------------------------------------------- */

/*
 * An OpenGL ES implementation for CoreAnimation (src/eagl.c, Mesa behind it): loaded, it puts
 * itself behind EAGLContext, and the renderer the server then gets is the OpenGL ES one. That
 * is worth it where a GPU does the drawing, and only there: on the phone, when the kernel has
 * started the GPU and keeps it (boot-arg gpu=1), which the service HakiosGPU says (it is there
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

	if (access(HAKIOS_GL, R_OK) != 0) {
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
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
		wanted = gpu != 0;
		if (gpu != 0) {
			IOObjectRelease(gpu);
		}
	}
	if (!wanted) {
		say("hakios: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	if ((library = dlopen(HAKIOS_GL, RTLD_NOW)) == NULL) {
		say("hakios: %s: %s\n", HAKIOS_GL, dlerror());
		return;
	}
	gl_loaded = 1;
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(library,
	    "hakios_gl_function_values");
	gl_function_refused = (void (*)(unsigned))dlsym(library, "hakios_gl_function_refused");
}

/* ---- boot-arg dyldcount=1: who asks dyld which image an address is in -------------------------------------------
 *
 * The window server spent about half of its processor time in dyld's search for the image that holds an address
 * (APIs::findImageMappedAt: forEachImageTextSegment, forEachRegion and strncmp, a walk over the shared cache's images
 * for every question; hakios_prof=backboardd, logs/203533-run.log), and none of it inside the Metal plug-in's draw
 * calls (metal_fps=sample, logs/204443-run.log). These four are the ways in; each call is counted by the address it
 * returns to, and every five seconds the call sites that asked most are said, unslid for `ipsw dyld a2s` when they are
 * in the shared cache. Interposed for the life of the process (dyld applies an inserted library's __interpose to every
 * image); counted only with the boot-arg, a load and a branch otherwise.
 */
#include <ptrauth.h>
#include <stdatomic.h>
#include <stdbool.h>

extern bool _dyld_is_memory_immutable(const void *addr, size_t length);
extern const struct mach_header *dyld_image_header_containing_address(const void *addr);
extern const char *dyld_image_path_containing_address(const void *addr);

#define DC_SLOTS 512
static const char *const dc_api[] = { "dladdr", "_dyld_is_memory_immutable", "dyld_image_header_containing_address",
	"dyld_image_path_containing_address", "syslog", "vsyslog" };
static struct {
	_Atomic uintptr_t caller;
	_Atomic uint32_t count;
	uint32_t api;
} dc_seen[DC_SLOTS];
static int dc_on;
static _Atomic uint32_t dc_lost;

static void
dc_count(void *returns_to, uint32_t api)
{
	uintptr_t caller = (uintptr_t)ptrauth_strip(returns_to, ptrauth_key_return_address);

	if (!dc_on || caller == 0) {
		return;
	}
	for (unsigned i = (unsigned)(((caller >> 2) * 2654435761u) % DC_SLOTS), n = 0; n < DC_SLOTS; i = (i + 1) % DC_SLOTS, n++) {
		uintptr_t held = atomic_load_explicit(&dc_seen[i].caller, memory_order_relaxed);

		if (held == 0) {
			uintptr_t empty = 0;

			if (atomic_compare_exchange_strong(&dc_seen[i].caller, &empty, caller)) {
				dc_seen[i].api = api;
				held = caller;
			} else {
				held = empty;
			}
		}
		if (held == caller) {
			atomic_fetch_add_explicit(&dc_seen[i].count, 1, memory_order_relaxed);
			return;
		}
	}
	atomic_fetch_add_explicit(&dc_lost, 1, memory_order_relaxed);
}

static int
dc_dladdr(const void *addr, Dl_info *info)
{
	dc_count(__builtin_return_address(0), 0);
	return dladdr(addr, info);
}
static bool
dc_immutable(const void *addr, size_t length)
{
	dc_count(__builtin_return_address(0), 1);
	return _dyld_is_memory_immutable(addr, length);
}
static const struct mach_header *
dc_header(const void *addr)
{
	dc_count(__builtin_return_address(0), 2);
	return dyld_image_header_containing_address(addr);
}
static const char *
dc_path(const void *addr)
{
	dc_count(__builtin_return_address(0), 3);
	return dyld_image_path_containing_address(addr);
}

/* (and syslog: the sampler below found dyld working for syslog's os_log shim, called by something deeper) */
#include <syslog.h>
static void
dc_syslog(int priority, const char *format, ...)
{
	va_list ap;

	dc_count(__builtin_return_address(0), 4);
	va_start(ap, format);
	vsyslog(priority, format, ap);
	va_end(ap);
}
static void
dc_vsyslog(int priority, const char *format, va_list ap)
{
	dc_count(__builtin_return_address(0), 5);
	vsyslog(priority, format, ap);
}

#define DC_INTERPOSE(replacement, replacee)                                                                         \
	__attribute__((used)) static const struct {                                                                   \
		const void *with;                                                                                       \
		const void *what;                                                                                       \
	} dc_interpose_##replacee __attribute__((section("__DATA,__interpose"))) = { (const void *)&replacement,      \
		(const void *)&replacee }
DC_INTERPOSE(dc_dladdr, dladdr);
DC_INTERPOSE(dc_immutable, _dyld_is_memory_immutable);
DC_INTERPOSE(dc_header, dyld_image_header_containing_address);
DC_INTERPOSE(dc_path, dyld_image_path_containing_address);
DC_INTERPOSE(dc_syslog, syslog);
DC_INTERPOSE(dc_vsyslog, vsyslog);

static void *
dc_report(void *unused)
{
	size_t cache_length = 0;
	const uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&cache_length);

	(void)unused;
	for (;;) {
		uint32_t total = 0, lost;

		sleep(5);
		lost = atomic_exchange(&dc_lost, 0);
		/* the eight call sites asked most, each taken out of the running as it is said */
		uint32_t counts[DC_SLOTS];

		for (unsigned i = 0; i < DC_SLOTS; i++) {
			counts[i] = atomic_exchange(&dc_seen[i].count, 0);
			total += counts[i];
		}
		say("hakios: dyldcount: %u questions in 5 s (%u from call sites past the %u counted)\n", total + lost, lost,
		    DC_SLOTS);
		for (unsigned k = 0; k < 8 && total != 0; k++) {
			unsigned best = DC_SLOTS;

			for (unsigned i = 0; i < DC_SLOTS; i++) {
				if (counts[i] != 0 && (best == DC_SLOTS || counts[i] > counts[best])) {
					best = i;
				}
			}
			if (best == DC_SLOTS) {
				break;
			}
			uintptr_t caller = atomic_load(&dc_seen[best].caller);
			Dl_info info = { 0 };
			const char *image = dladdr((const void *)caller, &info) && info.dli_fname != NULL ? info.dli_fname : "?";
			const char *base = strrchr(image, '/');

			say("hakios: dyldcount: %u %s from %#lx (%s %#lx)\n", counts[best], dc_api[dc_seen[best].api % 6],
			    (unsigned long)caller, base != NULL ? base + 1 : image,
			    (unsigned long)(caller >= cache && caller < cache + cache_length ? caller - slide
			    : caller - (uintptr_t)info.dli_fbase));
			counts[best] = 0;
		}
	}
	return NULL;
}

/*
 * The same question from the other end, because calls from the shared cache's own images to those four are not
 * interposed (the first boot with dyldcount=1 counted only the Metal plug-in's few): every millisecond each other
 * thread of the process is stopped, and when it stands inside dyld its frame chain is walked to the first return
 * address outside dyld and libdyld -- the code that asked. The walk reads through vm_read_overwrite, which a
 * frame pointer gone wrong cannot fault.
 */
#include <mach-o/dyld_images.h>
#include <os/log.h>
#include <mach-o/loader.h>

#define DS_SLOTS 256
static struct {
	uintptr_t caller, caller2;
	uint32_t count;
} ds_seen[DS_SLOTS];
static uint32_t ds_samples, ds_in_dyld, ds_lost;

/* where an image's __TEXT lies, from its mach header in memory */
static void
text_range(const struct mach_header_64 *mh, uintptr_t *lo, uintptr_t *hi)
{
	const struct load_command *lc = (const struct load_command *)(mh + 1);

	*lo = *hi = 0;
	for (uint32_t i = 0; mh != NULL && i < mh->ncmds; i++) {
		if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT") == 0) {
			*lo = (uintptr_t)mh;
			*hi = *lo + (uintptr_t)((const struct segment_command_64 *)lc)->vmsize;
			return;
		}
		lc = (const struct load_command *)((const uint8_t *)lc + lc->cmdsize);
	}
}

static void *
ds_sampler(void *unused)
{
	struct task_dyld_info di;
	mach_msg_type_number_t n = TASK_DYLD_INFO_COUNT;
	uintptr_t dlo = 0, dhi = 0, llo = 0, lhi = 0, tlo = 0, thi = 0, alo = 0, ahi = 0;
	Dl_info me = { 0 }, trace = { 0 }, asl = { 0 };
	unsigned ticks = 0;

	(void)unused;
	if (task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&di, &n) == KERN_SUCCESS) {
		const struct dyld_all_image_infos *all = (const struct dyld_all_image_infos *)(uintptr_t)di.all_image_info_addr;

		text_range((const struct mach_header_64 *)all->dyldImageLoadAddress, &dlo, &dhi);
	}
	if (dladdr((const void *)(uintptr_t)ptrauth_strip((void *)&dladdr, ptrauth_key_function_pointer), &me) && me.dli_fbase != NULL) {
		text_range((const struct mach_header_64 *)me.dli_fbase, &llo, &lhi);
	}
	/* (libsystem_trace and libsystem_asl as well: dyld was found working for syslog's os_log shim, and the code that
	 * asks is the caller of syslog, two libraries further out) */
	if (dladdr((const void *)(uintptr_t)ptrauth_strip((void *)&os_log_type_enabled, ptrauth_key_function_pointer), &trace) &&
	    trace.dli_fbase != NULL) {
		text_range((const struct mach_header_64 *)trace.dli_fbase, &tlo, &thi);
	}
	if (dladdr((const void *)(uintptr_t)ptrauth_strip((void *)&vsyslog, ptrauth_key_function_pointer), &asl) && asl.dli_fbase != NULL) {
		text_range((const struct mach_header_64 *)asl.dli_fbase, &alo, &ahi);
	}
	say("hakios: dyldcount: dyld's text %#lx-%#lx, libdyld's %#lx-%#lx, %s's %#lx-%#lx, %s's %#lx-%#lx\n", (unsigned long)dlo,
	    (unsigned long)dhi, (unsigned long)llo, (unsigned long)lhi, trace.dli_fname ? trace.dli_fname : "?", (unsigned long)tlo,
	    (unsigned long)thi, asl.dli_fname ? asl.dli_fname : "?", (unsigned long)alo, (unsigned long)ahi);
	if (dlo == 0) {
		return NULL;
	}
	for (;;) {
		thread_act_array_t threads;
		mach_msg_type_number_t count = 0;
		const thread_t self = mach_thread_self();

		usleep(1000);
		if (task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS) {
			for (mach_msg_type_number_t t = 0; t < count; t++) {
				arm_thread_state64_t st;
				mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;

				if (threads[t] == self || thread_suspend(threads[t]) != KERN_SUCCESS) {
					mach_port_deallocate(mach_task_self(), threads[t]);
					continue;
				}
				if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS) {
					const uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(st);

					ds_samples++;
					if (pc >= dlo && pc < dhi) {
						uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(st), found[2] = { 0, 0 };
						unsigned got = 0;
						uintptr_t lr = (uintptr_t)ptrauth_strip((void *)__darwin_arm_thread_state64_get_lr(st),
						    ptrauth_key_return_address);

						ds_in_dyld++;
						if (!((lr >= dlo && lr < dhi) || (lr >= llo && lr < lhi) || (lr >= tlo && lr < thi) || (lr >= alo && lr < ahi))) {
							found[got++] = lr;
						}
						for (int d = 0; d < 40 && got < 2 && fp != 0 && (fp & 7) == 0; d++) {
							uint64_t frame[2];
							vm_size_t read = 0;

							if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(frame), (vm_address_t)(uintptr_t)frame,
							    &read) != KERN_SUCCESS || read != sizeof(frame)) {
								break;
							}
							const uintptr_t ret = (uintptr_t)ptrauth_strip((void *)(uintptr_t)frame[1],
							    ptrauth_key_return_address);

							if (ret != 0 && !((ret >= dlo && ret < dhi) || (ret >= llo && ret < lhi) || (ret >= tlo && ret < thi) ||
							    (ret >= alo && ret < ahi))) {
								found[got++] = ret;
							}
							fp = (uintptr_t)frame[0];
						}
						if (got != 0) {
							unsigned i = (unsigned)(((found[0] >> 2) * 2654435761u) % DS_SLOTS), k = 0;

							while (k < DS_SLOTS && ds_seen[i].caller != 0 && ds_seen[i].caller != found[0]) {
								i = (i + 1) % DS_SLOTS;
								k++;
							}
							if (k == DS_SLOTS) {
								ds_lost++;
							} else {
								ds_seen[i].caller = found[0];
								ds_seen[i].caller2 = found[1];
								ds_seen[i].count++;
							}
						}
					}
				}
				thread_resume(threads[t]);
				mach_port_deallocate(mach_task_self(), threads[t]);
			}
			vm_deallocate(mach_task_self(), (vm_address_t)(uintptr_t)threads, count * sizeof(thread_t));
		}
		mach_port_deallocate(mach_task_self(), self);
		if (++ticks % 5000 != 0) {
			continue;
		}
		/* every five thousand looks (some five seconds and more): the callers dyld was found working for */
		say("hakios: dyldcount: %u threads seen running or waiting, %u of them inside dyld (%u callers past %u)\n",
		    ds_samples, ds_in_dyld, ds_lost, DS_SLOTS);
		for (unsigned k = 0; k < 10; k++) {
			unsigned best = DS_SLOTS;

			for (unsigned i = 0; i < DS_SLOTS; i++) {
				if (ds_seen[i].count != 0 && (best == DS_SLOTS || ds_seen[i].count > ds_seen[best].count)) {
					best = i;
				}
			}
			if (best == DS_SLOTS) {
				break;
			}
			Dl_info a = { 0 }, b = { 0 };
			const uintptr_t c1 = ds_seen[best].caller, c2 = ds_seen[best].caller2;
			const bool n1 = dladdr((const void *)c1, &a) != 0, n2 = c2 != 0 && dladdr((const void *)c2, &b) != 0;
			const char *i1 = n1 && a.dli_fname != NULL && strrchr(a.dli_fname, '/') ? strrchr(a.dli_fname, '/') + 1 : "?";
			const char *i2 = n2 && b.dli_fname != NULL && strrchr(b.dli_fname, '/') ? strrchr(b.dli_fname, '/') + 1 : "?";

			say("hakios: dyldcount: %u from %s %#lx (%s) <- %s %#lx (%s)\n", ds_seen[best].count, i1,
			    (unsigned long)(c1 - slide), n1 && a.dli_sname ? a.dli_sname : "?", i2, (unsigned long)(c2 - slide),
			    n2 && b.dli_sname ? b.dli_sname : "?");
			ds_seen[best].count = 0;
		}
		memset(ds_seen, 0, sizeof(ds_seen));
		ds_samples = ds_in_dyld = ds_lost = 0;
	}
	return NULL;
}

static void
dyld_count_start(const char *arguments)
{
	pthread_t thread;

	if (strstr(arguments, "dyldcount=1") == NULL) {
		return;
	}
	dc_on = 1;
	if (pthread_create(&thread, NULL, dc_report, NULL) == 0) {
		pthread_detach(thread);
	}
	if (pthread_create(&thread, NULL, ds_sampler, NULL) == 0) {
		pthread_detach(thread);
	}
	say("hakios: dyldcount=1: dyld's address-to-image questions counted by call site, and dyld's callers sampled\n");
}

/*
 * Boot-arg waitstack=1: where the window server's busy threads stand, running or waiting. Every two milliseconds each
 * other thread that has had processor time in the last 50 ms (one waiting for ever is left out) is stopped and its
 * program counter and first frames taken; every five seconds the stacks seen most often are said, each frame an
 * unslid address of the shared cache (ipsw dyld a2s on the host names it) or IMAGE+OFFSET for one outside it. What a
 * thread that draws a frame waits on between frames is then a stack among the first. CoreAnimation's own threads
 * (com.apple.coreanimation.*), whose stacks run deeper than the frames kept: waiting, the ten innermost frames, "|",
 * then the outermost; running, "run |" and the outermost alone -- which of the run loop's callouts it is in. (iOS 27.2:
 * the frame is drawn by IOMFBServer::thread_body's run loop on display_timer_callback; a frame_info thread beside it;
 * the render thread found waiting on the Metal plug-in's surface lock inside MetalContext::get_surface.)
 */
#define WS_SLOTS 512
#define WS_DEPTH 20
#define WS_THREADS 96
static struct {
	uint64_t key;
	uint32_t count;
	char name[20];
	uintptr_t frames[WS_DEPTH];
} ws_seen[WS_SLOTS];
static struct {
	mach_port_name_t port;
	uint64_t cpu;
	unsigned changed, seen;         /* (seen: the last look that found it; a thread gone is let go, its slot reused) */
} ws_threads[WS_THREADS];

static void
ws_frame(char *out, size_t size, uintptr_t address, uintptr_t cache, size_t cache_length)
{
	Dl_info info = { 0 };

	if (address >= cache && address < cache + cache_length) {
		snprintf(out, size, " %lx", (unsigned long)(address - slide));
	} else if (dladdr((const void *)address, &info) != 0 && info.dli_fname != NULL) {
		const char *base = strrchr(info.dli_fname, '/');

		snprintf(out, size, " %s+%lx", base != NULL ? base + 1 : info.dli_fname,
		    (unsigned long)(address - (uintptr_t)info.dli_fbase));
	} else {
		snprintf(out, size, " ?%lx", (unsigned long)address);
	}
}

static mach_port_t ws_main;            /* the process's main thread: its outer frames kept, as CoreAnimation's are */

/*
 * waitstack=sb: SpringBoard's main thread through each turn of its run loop that lasts over WS_TURN_MS -- what a tap
 * on an icon is followed by before the animation's first frame (some 200 ms on the phone). Every sample's whole stack
 * is counted, each return address once a sample; when the turn ends, "hakios: sbturn:" lines say how many samples it
 * had, the path the samples share (every address in half of them or more, from the outermost in: DEPTH:ADDRESS:COUNT)
 * and the addresses most often below it and the innermost frames -- where the time goes, as a call tree's hot path.
 */
#define WS_TURN_MS 30
#define WS_TURN_SLOTS 2048
static bool ws_turn_on;                         /* waitstack=sb */
static uintptr_t ws_main_lo, ws_main_hi;        /* the main thread's stack */
static volatile uint64_t ws_turn_woke;          /* mach time the main run loop woke; 0 while it waits */
static volatile uint32_t ws_turns;              /* turns ended */
static struct {
	uintptr_t pc;
	uint32_t count, depth;
} ws_turn_pc[WS_TURN_SLOTS], ws_turn_leaf[WS_TURN_SLOTS];
static uint32_t ws_turn_samples, ws_turn_of;    /* samples of the turn being profiled; ws_turns + 1 at its start */
static uint64_t ws_turn_from;
/* the main thread's processor time (us) and the time (mach) at the turn's first and last samples: busy, or waiting
 * for a core, or for something else */
static uint64_t ws_turn_cpu[2], ws_turn_at[2];
/* waitstack=sbfull: every sample's whole stack of a long turn kept as well, and said after the aggregates
 * ("hakios: sbstack TURN SAMPLE PART: innermost first ..."), for a call tree to be made of it on the host */
#define WS_FULL_SAMPLES 96
#define WS_FULL_DEPTH 160
static bool ws_full_on;
static uintptr_t ws_full[WS_FULL_SAMPLES][WS_FULL_DEPTH];
static uint8_t ws_full_n[WS_FULL_SAMPLES];
static unsigned ws_full_count, ws_full_turn;

static void
ws_turn_observer(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, __unused void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		ws_turn_woke = mach_absolute_time();
	} else if (activity == kCFRunLoopBeforeWaiting) {
		ws_turn_woke = 0;
		ws_turns++;
	}
}

static void
ws_turn_count(uintptr_t pc, uint32_t depth, bool leaf)
{
	__typeof__(ws_turn_pc[0]) *table = leaf ? ws_turn_leaf : ws_turn_pc;
	unsigned i = (unsigned)((pc >> 2) * 2654435761u % WS_TURN_SLOTS);

	for (unsigned k = 0; k < WS_TURN_SLOTS; k++, i = (i + 1) % WS_TURN_SLOTS) {
		if (table[i].count == 0 || table[i].pc == pc) {
			table[i].pc = pc;
			table[i].count++;
			if (table[i].count == 1 || depth < table[i].depth) table[i].depth = depth;
			return;
		}
	}
}

/*
 * snprintf answers what it would have written, not what it did: a frame named by a long library name can take a
 * line past its end, and the next append's size (sizeof(line) - used) would then wrap around to a huge one.
 */
static size_t
ws_clamp(size_t used, size_t size)
{
	return used < size - 1 ? used : size - 1;
}

static void
ws_turn_say(uintptr_t cache, size_t cache_length)
{
	static mach_timebase_info_data_t tb;
	char line[960];
	size_t used;
	unsigned said = 0;

	if (tb.denom == 0) mach_timebase_info(&tb);
	say("hakios: sbturn: %u samples of the main thread, the turn %llu ms or more; from its first sample to its last %llu ms, "
	    "the thread on a core %llu ms of them\n", ws_turn_samples,
	    (unsigned long long)((mach_absolute_time() - ws_turn_from) * tb.numer / tb.denom / 1000000),
	    (unsigned long long)((ws_turn_at[1] - ws_turn_at[0]) * tb.numer / tb.denom / 1000000),
	    (unsigned long long)((ws_turn_cpu[1] - ws_turn_cpu[0]) / 1000));
	/* the shared path, from the outermost in */
	used = (size_t)snprintf(line, sizeof(line), "hakios: sbturn path:");
	for (uint32_t d = 0; d < 64; d++) {
		for (unsigned i = 0; i < WS_TURN_SLOTS; i++) {
			if (ws_turn_pc[i].count == 0 || ws_turn_pc[i].depth != d || ws_turn_pc[i].count * 2 < ws_turn_samples) continue;
			if (used > sizeof(line) - 48) {
				snprintf(line + used, sizeof(line) - used, "\n");
				write(2, line, strlen(line));
				used = (size_t)snprintf(line, sizeof(line), "hakios: sbturn path:");
			}
			used = ws_clamp(used + (size_t)snprintf(line + used, sizeof(line) - used, " %u:", d), sizeof(line));
			ws_frame(line + used, sizeof(line) - used, ws_turn_pc[i].pc, cache, cache_length);
			used += strlen(line + used);
			used = ws_clamp(used + (size_t)snprintf(line + used, sizeof(line) - used, ":%u", ws_turn_pc[i].count),
			    sizeof(line));
			ws_turn_pc[i].count = 0;   /* (said) */
		}
	}
	snprintf(line + used, sizeof(line) - used, "\n");
	write(2, line, strlen(line));
	/* below it: the addresses seen most often, then the innermost frames */
	for (int leaf = 0; leaf < 2; leaf++) {
		__typeof__(ws_turn_pc[0]) *table = leaf ? ws_turn_leaf : ws_turn_pc;

		used = (size_t)snprintf(line, sizeof(line), "hakios: sbturn %s:", leaf ? "innermost" : "below");
		for (said = 0; said < 24; said++) {
			unsigned best = WS_TURN_SLOTS;

			for (unsigned i = 0; i < WS_TURN_SLOTS; i++) {
				if (table[i].count != 0 && (best == WS_TURN_SLOTS || table[i].count > table[best].count)) best = i;
			}
			if (best == WS_TURN_SLOTS || table[best].count * 20 < ws_turn_samples || used > sizeof(line) - 48) break;
			used = ws_clamp(used + (size_t)snprintf(line + used, sizeof(line) - used, " %u:", table[best].depth),
			    sizeof(line));
			ws_frame(line + used, sizeof(line) - used, table[best].pc, cache, cache_length);
			used += strlen(line + used);
			used = ws_clamp(used + (size_t)snprintf(line + used, sizeof(line) - used, ":%u", table[best].count),
			    sizeof(line));
			table[best].count = 0;
		}
		snprintf(line + used, sizeof(line) - used, "\n");
		write(2, line, strlen(line));
	}
	/* (waitstack=sbfull: each sample's stack, in parts of 60 frames: the console cuts a line at 1022 characters) */
	ws_full_turn++;
	for (unsigned k = 0; k < ws_full_count; k++) {
		for (unsigned from = 0, part = 0; from < ws_full_n[k]; from += 60, part++) {
			used = (size_t)snprintf(line, sizeof(line), "hakios: sbstack %u %u %u:", ws_full_turn, k, part);
			for (unsigned i = from; i < ws_full_n[k] && i < from + 60 && used < sizeof(line) - 40; i++) {
				ws_frame(line + used, sizeof(line) - used, ws_full[k][i], cache, cache_length);
				used += strlen(line + used);
			}
			snprintf(line + used, sizeof(line) - used, "\n");
			write(2, line, strlen(line));
		}
	}
	ws_full_count = 0;
	memset(ws_turn_pc, 0, sizeof(ws_turn_pc));
	memset(ws_turn_leaf, 0, sizeof(ws_turn_leaf));
	ws_turn_samples = 0;
}

static void *
ws_sampler(void *unused)
{
	size_t cache_length = 0;
	const uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&cache_length);
	unsigned ticks = 0, samples = 0, lost = 0;
	bool say_turn_after = false;

	(void)unused;
	pthread_setname_np("hakios.waitstack");
	for (;;) {
		thread_act_array_t threads;
		mach_msg_type_number_t count = 0;
		const thread_t self = mach_thread_self();

		usleep(2000);
		ticks++;
		if (task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS) {
			for (mach_msg_type_number_t t = 0; t < count; t++) {
				thread_basic_info_data_t basic;
				thread_extended_info_data_t ext;
				mach_msg_type_number_t bn = THREAD_BASIC_INFO_COUNT, en = THREAD_EXTENDED_INFO_COUNT;
				arm_thread_state64_t st;
				mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
				uint64_t cpu;
				unsigned slot;
				bool active;

				if (threads[t] == self || thread_info(threads[t], THREAD_BASIC_INFO, (thread_info_t)&basic, &bn) != KERN_SUCCESS) {
					mach_port_deallocate(mach_task_self(), threads[t]);
					continue;
				}
				cpu = (uint64_t)basic.user_time.seconds * 1000000 + basic.user_time.microseconds +
				    (uint64_t)basic.system_time.seconds * 1000000 + basic.system_time.microseconds;
				for (slot = 0; slot < WS_THREADS && ws_threads[slot].port != threads[t]; slot++) {
				}
				if (slot == WS_THREADS) {
					for (slot = 0; slot < WS_THREADS && ws_threads[slot].port != 0 && ws_threads[slot].seen + 1 >= ticks; slot++) {
					}
					if (slot == WS_THREADS) {
						mach_port_deallocate(mach_task_self(), threads[t]);
						continue;
					}
					ws_threads[slot].port = 0;
				}
				ws_threads[slot].seen = ticks;
				if (ws_threads[slot].port != threads[t]) {
					ws_threads[slot].port = threads[t];
					ws_threads[slot].cpu = cpu;
					ws_threads[slot].changed = 0;
				} else if (ws_threads[slot].cpu != cpu) {
					ws_threads[slot].cpu = cpu;
					ws_threads[slot].changed = ticks;
				}
				active = ws_threads[slot].changed != 0 && ticks - ws_threads[slot].changed <= 25;
				if (!active || thread_suspend(threads[t]) != KERN_SUCCESS) {
					mach_port_deallocate(mach_task_self(), threads[t]);
					continue;
				}
				if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS) {
					uintptr_t frames[WS_DEPTH] = { 0 }, all[160], fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(st);
					unsigned got = 0, n = 0;
					uint64_t key = 1469598103934665603ull;
					/* (the main thread's stack in waitstack=sb read where it lies, inside its bounds, and deeper: SpringBoard's
					 * run past 64 frames, and a read through the kernel for each frame kept it stopped the longer) */
					const bool direct = threads[t] == ws_main && ws_turn_on && ws_main_hi > ws_main_lo;
					const unsigned most = direct ? 160 : 64;

					all[n++] = (uintptr_t)ptrauth_strip((void *)__darwin_arm_thread_state64_get_pc(st), ptrauth_key_function_pointer);
					all[n++] = (uintptr_t)ptrauth_strip((void *)__darwin_arm_thread_state64_get_lr(st), ptrauth_key_return_address);
					for (int d = 0; d < 200 && n < most && fp != 0 && (fp & 7) == 0; d++) {
						uint64_t frame[2];
						vm_size_t read = 0;

						if (direct) {
							if (fp < ws_main_lo || fp + sizeof(frame) > ws_main_hi) break;
							memcpy(frame, (const void *)fp, sizeof(frame));
						} else if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof(frame), (vm_address_t)(uintptr_t)frame,
						    &read) != KERN_SUCCESS || read != sizeof(frame)) {
							break;
						}
						const uintptr_t ret = (uintptr_t)ptrauth_strip((void *)(uintptr_t)frame[1], ptrauth_key_return_address);

						if (ret != 0 && ret != all[n - 1]) {
							all[n++] = ret;
						}
						fp = (uintptr_t)frame[0];
					}
					/* (waitstack=sb: the main thread's whole stack in a long turn, ws_turn_say) */
					if (threads[t] == ws_main && ws_turn_on) {
						static mach_timebase_info_data_t tb;
						const uint64_t woke = ws_turn_woke, now = mach_absolute_time();

						if (tb.denom == 0) mach_timebase_info(&tb);
						bool long_turn = woke != 0 && (now - woke) * tb.numer / tb.denom > WS_TURN_MS * 1000000ull;

						if (long_turn && ws_turn_of != ws_turns + 1 && ws_turn_samples != 0) {
							/*
							 * (the last turn is said once the main thread runs again, not while it is stopped: it may be
							 * stopped holding dyld's lock or malloc's, which saying it takes -- and this sample waits
							 * for the next look)
							 */
							say_turn_after = true;
							long_turn = false;
						}
						if (long_turn) {
							if (ws_turn_of != ws_turns + 1) {
								ws_turn_of = ws_turns + 1;
								ws_turn_from = woke;
								ws_turn_cpu[0] = cpu;
								ws_turn_at[0] = now;
							}
							ws_turn_cpu[1] = cpu;
							ws_turn_at[1] = now;
							ws_turn_samples++;
							for (unsigned i = 0; i < n; i++) {
								bool again = false;

								for (unsigned j = 0; j < i && !again; j++) again = all[j] == all[i];
								if (!again) ws_turn_count(all[i], n - 1 - i, false);
							}
							ws_turn_count(all[0], 0, true);
							if (ws_full_on && ws_full_count < WS_FULL_SAMPLES) {
								memcpy(ws_full[ws_full_count], all, n * sizeof(all[0]));
								ws_full_n[ws_full_count++] = (uint8_t)n;
							}
						}
					}
					ext.pth_name[0] = 0;
					thread_info(threads[t], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &en);
					/* CoreAnimation's threads (waitstack=1): the two innermost frames, a 0, and the outermost ones -- which
					 * of the run loop's callouts it is in; every other thread its innermost frames */
					if ((strncmp(ext.pth_name, "com.apple.coreanim", 18) == 0 || threads[t] == ws_main) && n > WS_DEPTH) {
						uint32_t before = 0;
						vm_size_t read = 0;
						/* in a system call (the instruction before the pc an svc): where it waits; running: anywhere, 2 */
						const bool waiting = vm_read_overwrite(mach_task_self(), (vm_address_t)(all[0] - 4), 4,
						    (vm_address_t)(uintptr_t)&before, &read) == KERN_SUCCESS && read == 4 && before == 0xd4001001u;

						/* waiting: its ten innermost frames (who asked for the lock or the message), then the
						 * outermost nine; running: only where in the run loop */
						const unsigned inner = waiting ? 10 : 0;

						if (!waiting) {
							frames[got++] = 2;
						}
						for (unsigned i = 0; i < inner && i < n; i++) {
							frames[got++] = all[i];
						}
						frames[got++] = 1;
						for (unsigned i = n > WS_DEPTH - got ? n - (WS_DEPTH - got) : 0; i < n && got < WS_DEPTH; i++) {
							frames[got++] = all[i];
						}
					} else {
						for (unsigned i = 0; i < n && got < WS_DEPTH; i++) {
							frames[got++] = all[i];
						}
					}
					for (unsigned i = 0; i < WS_DEPTH; i++) {
						key = (key ^ frames[i]) * 1099511628211ull;
					}
					for (const char *c = ext.pth_name; *c != 0 && c < ext.pth_name + 19; c++) {
						key = (key ^ (uint8_t)*c) * 1099511628211ull;
					}
					samples++;
					{
						unsigned i = (unsigned)(key % WS_SLOTS), k = 0;

						while (k < WS_SLOTS && ws_seen[i].count != 0 && ws_seen[i].key != key) {
							i = (i + 1) % WS_SLOTS;
							k++;
						}
						if (k == WS_SLOTS) {
							lost++;
						} else {
							if (ws_seen[i].count == 0) {
								ws_seen[i].key = key;
								memcpy(ws_seen[i].frames, frames, sizeof(frames));
								strlcpy(ws_seen[i].name, ext.pth_name, sizeof(ws_seen[i].name));
							}
							ws_seen[i].count++;
						}
					}
				}
				thread_resume(threads[t]);
				if (say_turn_after) {
					say_turn_after = false;
					ws_turn_say(cache, cache_length);       /* (the main thread running again: see above) */
				}
				mach_port_deallocate(mach_task_self(), threads[t]);
			}
			vm_deallocate(mach_task_self(), (vm_address_t)(uintptr_t)threads, count * sizeof(thread_t));
		}
		mach_port_deallocate(mach_task_self(), self);
		if (ws_turn_samples != 0 && ws_turn_of != ws_turns + 1) {
			ws_turn_say(cache, cache_length);   /* (the long turn is over) */
		}
		if (ticks % 2500 != 0) {
			continue;
		}
		say("hakios: waitstack: %u samples of busy threads in 5 s (%u stacks past %u)\n", samples, lost, WS_SLOTS);
		for (unsigned k = 0; k < 24; k++) {
			unsigned best = WS_SLOTS;
			char line[900];
			size_t used;

			for (unsigned i = 0; i < WS_SLOTS; i++) {
				if (ws_seen[i].count != 0 && (best == WS_SLOTS || ws_seen[i].count > ws_seen[best].count)) {
					best = i;
				}
			}
			if (best == WS_SLOTS || ws_seen[best].count < 5) {
				break;
			}
			used = (size_t)snprintf(line, sizeof(line), "hakios: waitstack: %u [%s]", ws_seen[best].count,
			    ws_seen[best].name[0] != 0 ? ws_seen[best].name : "-");
			for (unsigned i = 0; i < WS_DEPTH && ws_seen[best].frames[i] != 0 && used < sizeof(line) - 40; i++) {
				if (ws_seen[best].frames[i] == 1 || ws_seen[best].frames[i] == 2) {
					used += (size_t)snprintf(line + used, sizeof(line) - used, ws_seen[best].frames[i] == 1 ? " |" : " run");
					continue;
				}
				ws_frame(line + used, sizeof(line) - used, ws_seen[best].frames[i], cache, cache_length);
				used += strlen(line + used);
			}
			snprintf(line + used, sizeof(line) - used, "\n");
			write(2, line, strlen(line));
			ws_seen[best].count = 0;
		}
		memset(ws_seen, 0, sizeof(ws_seen));
		samples = lost = 0;
	}
	return NULL;
}

static void
wait_stack_start(const char *arguments)
{
	pthread_t thread;

	if (strstr(arguments, "waitstack=1") == NULL) {
		return;
	}
	ws_main = mach_thread_self();   /* (called from the constructor, on the main thread) */
	if (pthread_create(&thread, NULL, ws_sampler, NULL) == 0) {
		pthread_detach(thread);
	}
	say("hakios: waitstack=1: the stacks of the busy threads sampled every 2 ms, said every 5 s\n");
}

__attribute__((constructor))
static void
hakios_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (name == NULL || cache == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	if (strcmp(name, "SpringBoard") == 0) {
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		/* boot-arg waitstack=sb: the sampler in SpringBoard instead (its image made with WAITSTACK_SB=1, root26-darwin.sh),
		 * and its main thread's long run loop turns profiled (ws_turn_say) */
		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 && strstr(arguments, "waitstack=sb") != NULL) {
			const pthread_t me = pthread_self();   /* (the constructor runs on the main thread) */

			ws_main_hi = (uintptr_t)pthread_get_stackaddr_np(me);
			ws_main_lo = ws_main_hi - pthread_get_stacksize_np(me);
			ws_turn_on = true;
			ws_full_on = strstr(arguments, "waitstack=sbfull") != NULL;
			CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
			    ws_turn_observer, NULL), kCFRunLoopCommonModes);
			CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, LONG_MAX,
			    ws_turn_observer, NULL), kCFRunLoopCommonModes);
			wait_stack_start("waitstack=1");
		}
		return;
	}
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
			say("hakios: ca_vbl=1: CoreAnimation is to take the display driver's refresh notifications\n");
		}
		dyld_count_start(arguments);
		wait_stack_start(arguments);
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
				say("hakios: caenv: %s=%s for the window server\n", setting, equals + 1);
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
	} else if (strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
	}
}
