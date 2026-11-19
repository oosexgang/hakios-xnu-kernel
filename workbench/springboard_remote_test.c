/*
 * Host tests of what SpringBoard's library (userland/userland/src/inject_sb.c) does on two of the notifications of its
 * remote control, which any program can post: com.hakios.layers (layers(), whose layer_line() walks the layer tree)
 * and com.hakios.switcher (switcher()). workbench/run.sh cuts the part of the file from its SEND macros to Control
 * Center's fades out unchanged but for those two macros: here they look a method up in the test's own Objective-C
 * runtime and call it with the types the call site gives, as objc_msgSend would; a selector a class has not got
 * ends the test as it ends SpringBoard.
 *
 *   springboard_remote_test               the scenarios' names
 *   springboard_remote_test SCENARIO      one scenario a process
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- the Objective-C runtime: classes with a few methods each, objects that are layers, arrays and strings ---- */

typedef struct test_object *id;
typedef struct test_class *Class;
typedef struct objc_selector *SEL;
typedef void (*IMP)(void);
typedef struct test_method *Method;
typedef bool BOOL;
#define YES true
#define NO false
#define nil NULL
#define __unused __attribute__((unused))

struct test_method {
	const char *name, *types;
	IMP imp;
};
struct test_class {
	struct test_class *isa;         /* a class's metaclass, whose methods are the class's own */
	const char *name;
	struct test_class *super;
	struct test_method methods[16];
};
struct test_object {
	struct test_class *isa;
	const char *text;               /* a string's */
	id name, mask, sublayers;       /* a layer's */
	id *items;                      /* an array's */
	long count;
};

static SEL sel_registerName(const char *name) { return (SEL)(void *)name; }
static const char *sel_getName(SEL sel) { return (const char *)(void *)sel; }
static Class object_getClass(id object) { return object->isa; }
static Class class_getSuperclass(Class cls) { return cls->super; }
static const char *method_getTypeEncoding(Method m) { return m->types; }

static Method
class_getInstanceMethod(Class cls, SEL sel)
{
	for (; cls != NULL; cls = cls->super) {
		for (unsigned i = 0; i < sizeof(cls->methods) / sizeof(cls->methods[0]) && cls->methods[i].name != NULL; i++) {
			if (strcmp(cls->methods[i].name, sel_getName(sel)) == 0) {
				return &cls->methods[i];
			}
		}
	}
	return NULL;
}

static Method class_getClassMethod(Class cls, SEL sel) { return cls->isa ? class_getInstanceMethod(cls->isa, sel) : NULL; }
static bool class_respondsToSelector(Class cls, SEL sel) { return cls != NULL && class_getInstanceMethod(cls, sel) != NULL; }
static const char *object_getClassName(id object) { return object ? object->isa->name : "nil"; }

/* (the encodings here are of one character a type) */
static const char *
encoding_type(const char *types, unsigned index)
{
	for (unsigned i = 0; *types != 0; i++) {
		if (i == index) {
			return types;
		}
		types++;
		while (*types >= '0' && *types <= '9') {
			types++;
		}
	}
	return NULL;
}

__attribute__((unused)) static unsigned
method_getNumberOfArguments(Method m)
{
	unsigned n = 0;

	while (encoding_type(m->types, n + 1) != NULL) {
		n++;
	}
	return n;
}

__attribute__((unused)) static void
method_getArgumentType(Method m, unsigned index, char *out, size_t size)
{
	const char *type = encoding_type(m->types, index + 1);

	strncpy(out, type != NULL ? (char[]){ *type, 0 } : "", size);
}

static long nil_answer(void) { return 0; }

/* what objc_msgSend finds for a selector: a class that has not got it is an exception, which ends the program */
static IMP
test_lookup(id object, const char *name)
{
	Method m;

	if (object == NULL) {
		return (IMP)nil_answer;
	}
	if ((m = class_getInstanceMethod(object->isa, sel_registerName(name))) == NULL) {
		printf("unrecognized selector sent: -[%s %s]: SpringBoard would end here\n", object->isa->name, name);
		fflush(stdout);
		abort();
	}
	return m->imp;
}

/*
 * objc_msgSend itself, for the calls the file makes through it directly: the method looked up, and jumped to with the
 * arguments as they were given (the registers the arguments are in kept round the lookup)
 */
void test_msgSend(void);
IMP test_imp_of(id object, SEL sel);
static IMP objc_msgSend = test_msgSend;         /* (a pointer: cast to the call's type, as the file casts objc_msgSend) */
__attribute__((used)) IMP test_imp_of(id object, SEL sel) { return test_lookup(object, sel_getName(sel)); }
#if defined(__x86_64__)
__asm__(".text\n.globl test_msgSend\n.type test_msgSend, @function\ntest_msgSend:\n"
	"push %rbp\nmov %rsp, %rbp\npush %rdi\npush %rsi\npush %rdx\npush %rcx\npush %r8\npush %r9\nsub $128, %rsp\n"
	"movdqu %xmm0, 0(%rsp)\nmovdqu %xmm1, 16(%rsp)\nmovdqu %xmm2, 32(%rsp)\nmovdqu %xmm3, 48(%rsp)\n"
	"movdqu %xmm4, 64(%rsp)\nmovdqu %xmm5, 80(%rsp)\nmovdqu %xmm6, 96(%rsp)\nmovdqu %xmm7, 112(%rsp)\n"
	"call test_imp_of@PLT\nmov %rax, %r11\n"
	"movdqu 0(%rsp), %xmm0\nmovdqu 16(%rsp), %xmm1\nmovdqu 32(%rsp), %xmm2\nmovdqu 48(%rsp), %xmm3\n"
	"movdqu 64(%rsp), %xmm4\nmovdqu 80(%rsp), %xmm5\nmovdqu 96(%rsp), %xmm6\nmovdqu 112(%rsp), %xmm7\n"
	"add $128, %rsp\npop %r9\npop %r8\npop %rcx\npop %rdx\npop %rsi\npop %rdi\npop %rbp\njmp *%r11\n"
	".size test_msgSend, .-test_msgSend\n");
#elif defined(__aarch64__)
__asm__(".text\n.globl test_msgSend\n.type test_msgSend, %function\ntest_msgSend:\n"
	"stp x29, x30, [sp, #-224]!\nmov x29, sp\nstp x0, x1, [sp, #16]\nstp x2, x3, [sp, #32]\nstp x4, x5, [sp, #48]\n"
	"stp x6, x7, [sp, #64]\nstr x8, [sp, #80]\nstp q0, q1, [sp, #96]\nstp q2, q3, [sp, #128]\nstp q4, q5, [sp, #160]\n"
	"stp q6, q7, [sp, #192]\nbl test_imp_of\nmov x16, x0\n"
	"ldp x0, x1, [sp, #16]\nldp x2, x3, [sp, #32]\nldp x4, x5, [sp, #48]\nldp x6, x7, [sp, #64]\nldr x8, [sp, #80]\n"
	"ldp q0, q1, [sp, #96]\nldp q2, q3, [sp, #128]\nldp q4, q5, [sp, #160]\nldp q6, q7, [sp, #192]\n"
	"ldp x29, x30, [sp], #224\nbr x16\n.size test_msgSend, .-test_msgSend\n");
#else
#error "objc_msgSend: no stand-in for this processor"
#endif

/* (the file's own SEND macros, left out: a frame comes back in memory on x86_64, which objc_msgSend above cannot know) */
#define SEND(type, object, name)        ((type (*)(id, SEL))test_lookup((id)(object), name))((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))test_lookup((id)(object), name))((id)(object), sel_registerName(name), argument)

/* the methods of the test's classes */
struct test_rect { double x, y, w, h; };
static struct test_rect layer_frame(id self, SEL s) { (void)self, (void)s; return (struct test_rect){ 0, 0, 390, 844 }; }
static id layer_nothing(id self, SEL s) { (void)self, (void)s; return NULL; }
static BOOL layer_no(id self, SEL s) { (void)self, (void)s; return NO; }
static id layer_mask(id self, SEL s) { (void)s; return self->mask; }
static id layer_sublayers(id self, SEL s) { (void)s; return self->sublayers; }
static float layer_opacity(id self, SEL s) { (void)self, (void)s; return 1; }
static id layer_name(id self, SEL s) { (void)s; return self->name; }
static id string_itself(id self, SEL s) { (void)s; return self; }
static const char *string_text(id self, SEL s) { (void)s; return self->text; }
static long array_count(id self, SEL s) { (void)s; return self->count; }
static id array_item(id self, SEL s, long i) { (void)s; return i >= 0 && i < self->count ? self->items[i] : NULL; }

static struct test_class nsobject_meta = { NULL, "NSObject", NULL, { { NULL, NULL, NULL } } };
static struct test_class layer_class = { &nsobject_meta, "CALayer", NULL, {
	{ "frame", "{CGRect=dddd}16@0:8", (IMP)layer_frame },
	{ "contents", "@16@0:8", (IMP)layer_nothing },
	{ "filters", "@16@0:8", (IMP)layer_nothing },
	{ "backgroundFilters", "@16@0:8", (IMP)layer_nothing },
	{ "compositingFilter", "@16@0:8", (IMP)layer_nothing },
	{ "backgroundColor", "^v16@0:8", (IMP)layer_nothing },
	{ "delegate", "@16@0:8", (IMP)layer_nothing },
	{ "animationKeys", "@16@0:8", (IMP)layer_nothing },
	{ "mask", "@16@0:8", (IMP)layer_mask },
	{ "sublayers", "@16@0:8", (IMP)layer_sublayers },
	{ "isHidden", "B16@0:8", (IMP)layer_no },
	{ "shouldRasterize", "B16@0:8", (IMP)layer_no },
	{ "name", "@16@0:8", (IMP)layer_name },
	{ "opacity", "f16@0:8", (IMP)layer_opacity },
} };
static struct test_class string_class = { &nsobject_meta, "NSString", NULL, {
	{ "description", "@16@0:8", (IMP)string_itself },
	{ "UTF8String", "r*16@0:8", (IMP)string_text },
} };
static struct test_class array_class = { &nsobject_meta, "NSArray", NULL, {
	{ "count", "q16@0:8", (IMP)array_count },
	{ "objectAtIndex:", "@24@0:8q16", (IMP)array_item },
} };

/* SpringBoard's switcher: the coordinator's class, and what its one instance was asked */
static struct { int calls; long sources[16]; } activated;

static BOOL
activate_switcher(id self, SEL s, long source, BOOL animated)
{
	(void)self, (void)s, (void)animated;
	if (activated.calls < 16) {
		activated.sources[activated.calls] = source;
	}
	activated.calls++;
	return source == 3;             /* the first sources are refused, the fourth is taken */
}

static struct test_object the_coordinator;
static id shared_coordinator(id self, SEL s) { (void)self, (void)s; return &the_coordinator; }

static struct test_class coordinator_meta = { NULL, "SBMainSwitcherControllerCoordinator", NULL, { { NULL, NULL, NULL } } };
static struct test_class coordinator_class = { &coordinator_meta, "SBMainSwitcherControllerCoordinator", NULL,
	{ { NULL, NULL, NULL } } };
static bool coordinator_there;

static Class
objc_getClass(const char *name)
{
	if (coordinator_there && strcmp(name, coordinator_class.name) == 0) {
		return &coordinator_class;
	}
	return NULL;
}

/* ---- the rest of what that part of the file calls ------------------------------------------------------------ */

typedef const void *CFTypeRef;
static CFTypeRef CFCopyDescription(CFTypeRef object) { (void)object; abort(); }
static void CFRelease(CFTypeRef object) { (void)object; }
struct mach_header;
static int sysctlbyname(const char *name, void *old, size_t *old_size, void *new, size_t new_size)
{
	(void)name, (void)old, (void)old_size, (void)new, (void)new_size;
	return -1;
}
#define RTLD_DEFAULT NULL
static void *dlsym(void *handle, const char *name) { (void)handle, (void)name; return NULL; }
static bool replace_method(const char *c, const char *s, uintptr_t expected, IMP replacement, IMP *original_out)
{
	(void)c, (void)s, (void)expected, (void)replacement, (void)original_out;
	return false;
}

/* what is said: kept, line by line */
static char *said;
static size_t said_size;
static int said_lines;

static void
say(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	said = realloc(said, said_size + (size_t)n + 1);
	memcpy(said + said_size, line, (size_t)n + 1);
	said_size += (size_t)n;
	said_lines++;
}

static void
forget_said(void)
{
	free(said);
	said = NULL;
	said_size = 0;
	said_lines = 0;
}

static int
said_count(const char *text)
{
	int n = 0;

	for (const char *at = said; at != NULL && (at = strstr(at, text)) != NULL; at++) {
		n++;
	}
	return n;
}

#pragma GCC diagnostic ignored "-Wunused-function"
#include "inject_sb_remote.c"
#pragma GCC diagnostic warning "-Wunused-function"
#ifndef LAYERS_DEPTH_MAX
#define LAYERS_DEPTH_MAX 128            /* (a file that walks a tree to any depth: what the test expects of it) */
#endif

/* ---- layer trees ------------------------------------------------------------------------------------------------ */

static id
string(const char *text)
{
	id s = calloc(1, sizeof(*s));

	s->isa = &string_class;
	s->text = text;
	return s;
}

static id
array(id *items, long count)
{
	id a = calloc(1, sizeof(*a));

	a->isa = &array_class;
	a->items = items;
	a->count = count;
	return a;
}

/* a chain DEPTH layers deep: each the only sublayer of the one above it (or its mask, every MASK_EVERY-th) */
static id
chain(int depth, int mask_every)
{
	id *layers = calloc((size_t)depth, sizeof(*layers)), name = string("a layer");

	for (int i = 0; i < depth; i++) {
		layers[i] = calloc(1, sizeof(*layers[i]));
		layers[i]->isa = &layer_class;
		layers[i]->name = name;
	}
	for (int i = 0; i + 1 < depth; i++) {
		if (mask_every > 0 && i % mask_every == mask_every - 1) {
			layers[i]->mask = layers[i + 1];
		} else {
			layers[i]->sublayers = array(&layers[i + 1], 1);
		}
	}
	return layers[0];
}

/* walked as layers() does a window's, on a thread with the 1 MB stack of SpringBoard's main thread */
static void *
walk(void *layer)
{
	layer_line(layer, "", 1);
	return NULL;
}

static void
walk_on_main_stack(id layer)
{
	pthread_attr_t attributes;
	pthread_t thread;

	forget_said();
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, 1024 * 1024);
	if (pthread_create(&thread, &attributes, walk, layer) != 0) {
		CHECK(false, "no thread");
		return;
	}
	pthread_join(thread, NULL);
	pthread_attr_destroy(&attributes);
}

/* a tree of an ordinary depth is said whole, a layer a line, masks included */
static void
scenario_layers_whole(void)
{
	walk_on_main_stack(chain(60, 7));
	CHECK(said_lines == 60, "%d lines for 60 layers", said_lines);
	CHECK(said_count("MASK CALayer") == 8, "%d masks said, not 8", said_count("MASK CALayer"));
	CHECK(said_count("left out") == 0, "a tree of 60 cut short");
}

/* a tree deeper than the stack holds is said down to LAYERS_DEPTH_MAX and no further, and says that it is cut */
static void
scenario_layers_deep(void)
{
	walk_on_main_stack(chain(5000, 0));
	CHECK(said_lines == LAYERS_DEPTH_MAX + 1, "%d lines, not %d", said_lines, LAYERS_DEPTH_MAX + 1);
	CHECK(said_count("left out: deeper than") == 1, "not said that the tree was cut short");
	walk_on_main_stack(chain(5000, 3));
	CHECK(said_lines == LAYERS_DEPTH_MAX + 1, "a chain of masks: %d lines, not %d", said_lines, LAYERS_DEPTH_MAX + 1);
	CHECK(said_count("left out: deeper than") == 1, "a chain of masks: not said that it was cut short");
}

/* ---- the switcher ------------------------------------------------------------------------------------------------- */

static void
coordinator(bool there, IMP shared, const char *activate_types)
{
	coordinator_there = there;
	memset(&coordinator_meta.methods, 0, sizeof(coordinator_meta.methods));
	memset(&coordinator_class.methods, 0, sizeof(coordinator_class.methods));
	if (shared != NULL) {
		coordinator_meta.methods[0] = (struct test_method){ "sharedInstance", "@16@0:8", shared };
	}
	if (activate_types != NULL) {
		coordinator_class.methods[0] = (struct test_method){ "activateMainSwitcherNoninteractivelyWithSource:animated:",
			activate_types, (IMP)activate_switcher };
	}
	the_coordinator.isa = &coordinator_class;
	memset(&activated, 0, sizeof(activated));
	forget_said();
}

/* SpringBoard's as on 16.5: the sources are tried in turn until one is taken */
static void
scenario_switcher_opens(void)
{
	coordinator(true, (IMP)shared_coordinator, "B28@0:8q16B24");
	switcher();
	CHECK(activated.calls == 4 && activated.sources[0] == 0 && activated.sources[3] == 3,
	    "%d calls, not the sources 0 to 3", activated.calls);
	CHECK(said_count("from source 3: taken") == 1, "the source taken not said");
}

/* no coordinator, or one without the method: nothing asked, nothing ended */
static void
scenario_switcher_missing(void)
{
	coordinator(false, NULL, NULL);
	switcher();
	CHECK(activated.calls == 0, "called without a coordinator");
	coordinator(true, (IMP)shared_coordinator, NULL);
	switcher();
	CHECK(activated.calls == 0 && said_count("has no") == 1, "a coordinator without the method: %d calls", activated.calls);
}

/* a coordinator class that has no +sharedInstance in this build: not asked for one (that is an exception: a respring) */
static void
scenario_switcher_no_shared_instance(void)
{
	coordinator(true, NULL, "B28@0:8q16B24");
	switcher();
	CHECK(activated.calls == 0 && said_count("has no") == 1, "%d calls", activated.calls);
}

/* a method of the name that takes an object for its source is not given the numbers 0 to 9 for one */
static void
scenario_switcher_other_types(void)
{
	coordinator(true, (IMP)shared_coordinator, "B28@0:8@16B24");
	switcher();
	CHECK(activated.calls == 0, "a method taking an object given %d numbers", activated.calls);
	CHECK(said_count("left alone") == 1, "not said why the switcher was left alone");
	coordinator(true, (IMP)shared_coordinator, "B32@0:8q16B24@28");
	switcher();
	CHECK(activated.calls == 0, "a method of three arguments called with two");
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "layers_whole", scenario_layers_whole },
	{ "layers_deep", scenario_layers_deep },
	{ "switcher_opens", scenario_switcher_opens },
	{ "switcher_missing", scenario_switcher_missing },
	{ "switcher_no_shared_instance", scenario_switcher_no_shared_instance },
	{ "switcher_other_types", scenario_switcher_other_types },
};

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (argc < 2) {
			printf("%s\n", scenarios[i].name);
		} else if (strcmp(argv[1], scenarios[i].name) == 0) {
			scenarios[i].run();
			printf("%s %s: %d checks, %d failed\n", failures ? "FAIL" : "ok", argv[1], checks, failures);
			return failures != 0;
		}
	}
	if (argc >= 2) {
		printf("FAIL: no scenario %s\n", argv[1]);
		return 1;
	}
	return 0;
}
