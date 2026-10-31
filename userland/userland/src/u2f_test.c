/*
 * u2f_test: what this GPU makes of an unsigned 32-bit integer turned into a float, through
 * OpenGL ES and nothing of ours.
 *
 * The AIR work found that the Adreno gives 4294967040 for (float)0xFFFFFFFF where C gives
 * 4294967296 -- the low eight bits dropped instead of rounded (docs/air.md). Three things could
 * do that: the a6xx instruction itself, ir3's lowering of it, or our own AIR frontend asking for
 * the wrong thing. This program takes the third out of the question by not involving it: the
 * conversion is written in GLSL ES, compiled by the same Mesa and the same ir3 that the window
 * server's drawing goes through, and run on the same GPU.
 *
 * The value is a uniform, never a constant: a constant would be folded by the compiler on this
 * side and the answer would be the host's, which is the whole thing being asked about.
 *
 * The float comes back as its four bytes through an RGBA8 framebuffer, so the bits are exact:
 * a float render target is not needed and not every driver has one.
 *
 *   u2f_test            the table below
 *   u2f_test HEX...     those values instead (e.g. u2f_test 0xffffffff 0x1000001)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* OSMesa and GLES as far as they are used here (see gl_test.c, which does the same) */
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
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern void glGetShaderInfoLog(unsigned shader, int size, int *length, char *log);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned name, int *value);
extern void glGetProgramInfoLog(unsigned program, int size, int *length, char *log);
extern void glUseProgram(unsigned program);
extern int glGetUniformLocation(unsigned program, const char *name);
extern void glUniform1ui(int location, unsigned value);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);
extern void glFinish(void);
extern void glGenVertexArrays(int n, unsigned *names);
extern void glBindVertexArray(unsigned name);

#define GL_VENDOR                       0x1F00
#define GL_RENDERER                     0x1F01
#define GL_VERSION                      0x1F02
#define GL_TEXTURE_2D                   0x0DE1
#define GL_RGBA                         0x1908
#define GL_RGBA8                        0x8058
#define GL_UNSIGNED_BYTE                0x1401
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_NEAREST                      0x2600
#define GL_FRAMEBUFFER                  0x8D40
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_VERTEX_SHADER                0x8B31
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_TRIANGLES                    0x0004

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38

/*
 * The whole of the question. gl_VertexID makes the triangle, so there are no attributes and no
 * buffers; the conversion is of a uniform, and its result leaves as four bytes.
 */
static const char *const vertex_text =
    "#version 300 es\n"
    "void main() {\n"
    "	vec2 at = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "	gl_Position = vec4(at * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *const fragment_text =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform uint value;\n"
    "out vec4 colour;\n"
    "void main() {\n"
    "	uint bits = floatBitsToUint(float(value));\n"
    "	colour = vec4(float((bits >> 24) & 255u), float((bits >> 16) & 255u),\n"
    "	              float((bits >> 8) & 255u), float(bits & 255u)) / 255.0;\n"
    "}\n";

static unsigned
shader(unsigned type, const char *text)
{
	unsigned name = glCreateShader(type);
	char log[1024] = { 0 };
	int good = 0;

	glShaderSource(name, 1, &text, NULL);
	glCompileShader(name);
	glGetShaderiv(name, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(name, (int)sizeof(log) - 1, NULL, log);
		printf("u2f_test: a shader does not compile: %s\n", log);
	}
	return name;
}

/* the float a C compiler on this machine makes of the same value */
static uint32_t
reference(uint32_t value)
{
	float f = (float)value;
	uint32_t bits;

	memcpy(&bits, &f, sizeof(bits));
	return bits;
}

static float
as_float(uint32_t bits)
{
	float f;

	memcpy(&f, &bits, sizeof(f));
	return f;
}

int
main(int argc, char **argv)
{
	static const int attributes[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		                          OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0, 0 };
	/*
	 * Values where the answer tells the three apart. Rounding only decides where the integer is
	 * not representable: above 2^24 the gaps open, and these sit in them.
	 *   0xffffffff  255 away from one neighbour and 1 from the other: nearest says the far one
	 *   0xffffff80  exactly halfway: nearest-even says 2^32, whose significand is even
	 *   0x01000001  halfway in the first gap of all (2^24 + 1): nearest-even says 2^24
	 *   0x01000003  halfway in the same gap the other way: nearest-even says 2^24 + 4
	 *   0x7fffffff  the signed maximum, 1 away from 2^31
	 *   0x00ffffff  the last integer that is exact: every rule must agree here
	 */
	static const uint32_t table[] = { 0xffffffffu, 0xffffff80u, 0x01000001u, 0x01000003u, 0x7fffffffu, 0x00ffffffu };
	static unsigned char window[16 * 16 * 4];
	const uint32_t *values = table;
	unsigned count = sizeof(table) / sizeof(table[0]);
	uint32_t given[16];
	unsigned texture = 0, framebuffer = 0, program, array = 0;
	int location, good = 0, differ = 0;
	OSMesaContext context;

	/*
	 * Unbuffered: this program is killed where it stands when the session's time is up, and
	 * a buffered line is a line that was never said. The first run of it said nothing at all
	 * -- not even the driver's name, which is printed before any work -- and the reason was
	 * here, not on the GPU.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 1) {
		count = (unsigned)(argc - 1) > sizeof(given) / sizeof(given[0]) ? sizeof(given) / sizeof(given[0])
		                                                                : (unsigned)(argc - 1);
		for (unsigned i = 0; i < count; i++) {
			given[i] = (uint32_t)strtoul(argv[1 + i], NULL, 0);
		}
		values = given;
	}

	context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		printf("u2f_test: no OpenGL ES 3.0 context\n");
		return 1;
	}
	printf("u2f_test: context made; %s; %s; %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_VERSION));

	glGenVertexArrays(1, &array);
	glBindVertexArray(array);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("u2f_test: the framebuffer is not complete\n");
		return 1;
	}
	glViewport(0, 0, 1, 1);

	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER, vertex_text));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER, fragment_text));
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	if (!good) {
		char log[1024] = { 0 };

		glGetProgramInfoLog(program, (int)sizeof(log) - 1, NULL, log);
		printf("u2f_test: the program does not link: %s\n", log);
		return 1;
	}
	glUseProgram(program);
	location = glGetUniformLocation(program, "value");
	if (location < 0) {
		printf("u2f_test: the uniform is gone: the compiler folded what it was asked not to\n");
		return 1;
	}

	for (unsigned i = 0; i < count; i++) {
		unsigned char pixel[4] = { 0, 0, 0, 0 };
		uint32_t bits, want = reference(values[i]);

		glUniform1ui(location, values[i]);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
		glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		bits = (uint32_t)pixel[0] << 24 | (uint32_t)pixel[1] << 16 | (uint32_t)pixel[2] << 8 | pixel[3];
		printf("u2f_test: 0x%08x -> %#010x %-16.9g  C says %#010x %-16.9g  %s\n", values[i], bits, (double)as_float(bits),
		    want, (double)as_float(want), bits == want ? "same" : "DIFFERENT");
		differ += bits != want;
	}
	printf("u2f_test: %u value(s), %d different; error %#x\n", count, differ, glGetError());
	return differ != 0;
}
