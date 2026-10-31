/*
 * gl_test: one triangle through the OpenGL ES library of the image (lib/libOSMesa.8.dylib: Mesa,
 * with its Adreno driver when the kernel has kept the GPU on -- boot-arg adreno_keep=1 -- and its
 * software one when not), read back and put on the panel. It says which driver drew, how long
 * it took and what a few pixels are, so that the picture need not be seen to be believed.
 *
 *   gl_test [SECONDS [WIDTH HEIGHT]]   keep the picture on the panel for so long (default 6: the
 *                                      window server draws over it with its next frame)
 *   gl_test fill [WIDTH HEIGHT]        no picture: how long the GPU takes over a frame of that size
 *                                      (default 1125 x 2436, what the window server draws), by what
 *                                      is in it -- see fill()
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* GL/osmesa.h and GLES2/gl2.h of Mesa, as far as they are used here */
typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);

extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
extern void glBindTexture(unsigned target, unsigned texture);
extern void glTexImage2D(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
    unsigned type, const void *pixels);
extern void glTexParameteri(unsigned target, unsigned name, int value);
extern void glGenFramebuffers(int n, unsigned *names);
extern void glBindFramebuffer(unsigned target, unsigned framebuffer);
extern void glFramebufferTexture2D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level);
extern unsigned glCheckFramebufferStatus(unsigned target);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float red, float green, float blue, float alpha);
extern void glClear(unsigned mask);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glBindAttribLocation(unsigned program, unsigned index, const char *name);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned name, int *value);
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern void glEnable(unsigned what);
extern void glDisable(unsigned what);
extern void glBlendFunc(unsigned source, unsigned destination);
extern int glGetUniformLocation(unsigned program, const char *name);
extern void glUniform1i(int location, int value);
extern void glUniform4f(int location, float x, float y, float z, float w);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_ES_PROFILE               0x38
#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_ONE                          1
#define GL_ONE_MINUS_SRC_ALPHA          0x0303
#define GL_BLEND                        0x0be2
#define GL_LINEAR                       0x2601
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER                  0x8d40

/* the kernel's GPU service takes pixels for the panel (iokit/Kernel/RedwoodGPU.cpp: kRedwoodShow) */
struct show {
	uint64_t pixels;
	uint32_t width, height, row_bytes, pad;
};

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

static unsigned
shader(unsigned type, const char *text)
{
	unsigned name = glCreateShader(type);
	int good = 0;

	glShaderSource(name, 1, &text, NULL);
	glCompileShader(name);
	glGetShaderiv(name, GL_COMPILE_STATUS, &good);
	if (!good) {
		printf("gl_test: a shader does not compile\n");
	}
	return name;
}

/*
 * What a frame costs by what is in it. Each kind of frame is drawn sixteen times into a texture
 * of the size given, each time waited for (glFinish), and the shortest time is the one told:
 * the window server draws its own frames meanwhile, and they are in the way of some.
 *   - nothing but a clear: what a frame costs before anything is drawn in it (the GPU draws a
 *     picture of this size in pieces, in memory of its own, and writes each piece out);
 *   - no clear, one small rectangle: the same, and the picture as it was has to be read in first;
 *   - a clear and 1, 2, 4, 8 rectangles over all of it, in one colour: what covering every pixel
 *     once costs;
 *   - the same with a texture of the same size and blending, which is what a layer of the
 *     window server's is.
 * From these it can be told what a slow frame is slow at: the pieces written to memory and read
 * from it, or the pixels drawn.
 */
static void
fill(int width, int height)
{
	static const float all[] = { -1, -1, 1, -1, -1, 1, 1, 1 }, some[] = { -0.1f, -0.1f, 0.1f, -0.1f, -0.1f, 0.1f, 0.1f, 0.1f };
	static const struct {
		const char *what;
		int clear, layers, textured, small;
	} kinds[] = {
		{ "a clear and nothing else", 1, 0, 0, 0 },
		{ "no clear, one small rectangle", 0, 1, 0, 1 },
		{ "a clear, 1 rectangle over all of it", 1, 1, 0, 0 },
		{ "a clear, 2 rectangles", 1, 2, 0, 0 },
		{ "a clear, 4 rectangles", 1, 4, 0, 0 },
		{ "a clear, 8 rectangles", 1, 8, 0, 0 },
		{ "a clear, 1 rectangle with a texture, blended", 1, 1, 1, 0 },
		{ "a clear, 2 such", 1, 2, 1, 0 },
		{ "a clear, 4 such", 1, 4, 1, 0 },
		{ "a clear, 8 such", 1, 8, 1, 0 },
	};
	unsigned target = 0, source = 0, framebuffer = 0, program;
	uint32_t *pixels = malloc((size_t)width * (size_t)height * 4);
	int textured, colour, good = 0;

	if (pixels == NULL) {
		return;
	}
	for (size_t i = 0; i < (size_t)width * (size_t)height; i++) {
		pixels[i] = 0x80404040u + ((uint32_t)i * 2654435761u >> 28);
	}
	glGenTextures(1, &source);
	glBindTexture(GL_TEXTURE_2D, source);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	free(pixels);
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
	    "attribute vec2 where; varying vec2 at;\n"
	    "void main() { gl_Position = vec4(where, 0.0, 1.0); at = where * 0.5 + 0.5; }\n"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec2 at; uniform sampler2D picture; uniform int textured; uniform vec4 colour;\n"
	    "void main() { gl_FragColor = textured != 0 ? texture2D(picture, at) * colour : colour; }\n"));
	glBindAttribLocation(program, 0, "where");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	if (!good || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("gl_test: fill: no program or no framebuffer\n");
		return;
	}
	glUseProgram(program);
	textured = glGetUniformLocation(program, "textured");
	colour = glGetUniformLocation(program, "colour");
	glUniform1i(glGetUniformLocation(program, "picture"), 0);
	glBindTexture(GL_TEXTURE_2D, source);
	glViewport(0, 0, width, height);
	glEnableVertexAttribArray(0);
	glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
	printf("gl_test: fill: frames of %d x %d (%d KB), the shortest of 16 each:\n", width, height, width * height / 256);
	for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		uint64_t least = ~0ull, most = 0;

		glUniform1i(textured, kinds[k].textured);
		if (kinds[k].textured) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		} else {
			glDisable(GL_BLEND);
		}
		glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, kinds[k].small ? some : all);
		for (int frame = 0; frame < 16; frame++) {
			uint64_t began = now_us(), took;

			if (kinds[k].clear) {
				glClear(GL_COLOR_BUFFER_BIT);
			}
			for (int layer = 0; layer < kinds[k].layers; layer++) {
				glUniform4f(colour, 0.2f + 0.1f * (float)layer, 0.5f, 0.3f, 0.5f);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			glFinish();
			took = now_us() - began;
			least = took < least ? took : least;
			most = took > most ? took : most;
		}
		printf("gl_test: fill: %-46s %5llu us (the longest %llu)\n", kinds[k].what, least, most);
	}
	printf("gl_test: fill: error %#x\n", glGetError());
}

int
main(int argc, char **argv)
{
	static const int attributes[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		                          OSMESA_CONTEXT_MAJOR_VERSION, 2, 0 };
	static const float corners[] = {
		/* where, and what colour */
		-0.8f, -0.6f, 1, 0, 0,
		0.8f, -0.6f, 0, 1, 0,
		0.0f, 0.7f, 0, 0, 1,
	};
	static unsigned char window[16 * 16 * 4];
	int seconds = argc > 1 ? atoi(argv[1]) : 6, width = argc > 3 ? atoi(argv[2]) : 1080;
	int height = argc > 3 ? atoi(argv[3]) : 2400;
	unsigned texture = 0, framebuffer = 0, program, coloured = 0;
	uint8_t *drawn, *shown;
	uint64_t began, drew, read;
	io_service_t service;
	io_connect_t connection = 0;
	OSMesaContext context;
	int good = 0;

	context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		printf("gl_test: no OpenGL ES context\n");
		return 1;
	}
	printf("gl_test: %s; %s; %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_VERSION));
	if (argc > 1 && strcmp(argv[1], "fill") == 0) {
		fill(argc > 3 ? atoi(argv[2]) : 1125, argc > 3 ? atoi(argv[3]) : 2436);
		return 0;
	}

	began = now_ms();
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("gl_test: the framebuffer is not complete\n");
		return 1;
	}
	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
	    "attribute vec2 where; attribute vec3 colour; varying vec3 c;\n"
	    "void main() { gl_Position = vec4(where, 0.0, 1.0); c = colour; }\n"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec3 c;\n"
	    "void main() { gl_FragColor = vec4(c, 1.0); }\n"));
	glBindAttribLocation(program, 0, "where");
	glBindAttribLocation(program, 1, "colour");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	if (!good) {
		printf("gl_test: the program does not link\n");
		return 1;
	}
	glUseProgram(program);
	glViewport(0, 0, width, height);
	glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), corners);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), corners + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	drew = now_ms();

	drawn = malloc((size_t)width * (size_t)height * 4);
	shown = malloc((size_t)width * (size_t)height * 4);
	if (drawn == NULL || shown == NULL) {
		return 1;
	}
	memset(drawn, 0x55, (size_t)width * (size_t)height * 4);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, drawn);
	read = now_ms();
	/* the panel's rows go down and its pixels are blue first */
	for (int y = 0; y < height; y++) {
		const uint8_t *from = drawn + (size_t)(height - 1 - y) * (size_t)width * 4;
		uint8_t *to = shown + (size_t)y * (size_t)width * 4;

		for (int x = 0; x < width; x++) {
			to[4 * x] = from[4 * x + 2];
			to[4 * x + 1] = from[4 * x + 1];
			to[4 * x + 2] = from[4 * x];
			to[4 * x + 3] = 255;
			coloured += from[4 * x] > 60 || from[4 * x + 1] > 60 || from[4 * x + 2] > 60;
		}
	}
#define PIXEL(x, y) drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4], drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4 + 1], \
	    drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4 + 2], drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4 + 3]
	printf("gl_test: %d x %d drawn in %llu ms, read back in %llu ms; error %#x; red green blue alpha: a corner %u %u %u %u, "
	    "the middle %u %u %u %u, near the red corner %u %u %u %u, near the green %u %u %u %u, near the blue %u %u %u %u; "
	    "%u of %u pixels inside the triangle (%u%%; 26%% by geometry)\n", width, height, drew - began, read - drew,
	    glGetError(), PIXEL(2, 2), PIXEL(width / 2, height / 2), PIXEL(width * 15 / 100, height * 22 / 100),
	    PIXEL(width * 85 / 100, height * 22 / 100), PIXEL(width / 2, height * 80 / 100), coloured,
	    (unsigned)(width * height), (unsigned)((uint64_t)coloured * 100 / (uint64_t)(width * height)));

	service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		printf("gl_test: no GPU service in this kernel to put the picture on the panel with\n");
		return 0;
	}
	for (int i = 0; i < seconds * 5; i++) {
		struct show show = { (uint64_t)(uintptr_t)shown, (uint32_t)width, (uint32_t)height, (uint32_t)width * 4, 0 };
		kern_return_t result = IOConnectCallStructMethod(connection, 0x7f, &show, sizeof(show), NULL, NULL);

		if (i == 0) {
			printf("gl_test: the picture is on the panel (%#x), for %d seconds\n", result, seconds);
		}
		usleep(200000);
	}
	return 0;
}
