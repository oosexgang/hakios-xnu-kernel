/*
 * gl_mrt: what the Mali draws into a colour target other than the first, and into an integer one,
 * when OpenGL ES asks -- through Mesa's own GL state tracker, the road CoreAnimation already takes
 * on this phone. The Metal path (tools/mesa/air: air_run, air_metal) reads zeros for exactly
 * these on the Mali and the right numbers for a float target 0; the same cases here say whether
 * that is panfrost on this port, or something the AIR path hands panfrost.
 *
 * Four draws of a full-screen quad into 4x4 targets, each read back at pixel (1, 1):
 *   float0   one RGBA32F target, the shader writes target 0          (the case that works in AIR)
 *   float2   three RGBA32F targets, the shader writes target 2 only  (reads 0 in AIR)
 *   unorm2   three RGBA8 targets, the shader writes target 2 only
 *   uint0    one RGBA32UI target, the shader writes target 0         (reads 0 in AIR)
 * Every line also goes to the kernel as one to keep (kern.redwood.say), so the end of the log has it.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

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
extern void glDrawBuffers(int n, const unsigned *buffers);
extern void glReadBuffer(unsigned buffer);
extern void glViewport(int x, int y, int width, int height);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern void glGetShaderInfoLog(unsigned shader, int size, int *length, char *log);
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
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);
extern void glClearBufferfv(unsigned buffer, int drawbuffer, const float *value);
extern void glClearBufferuiv(unsigned buffer, int drawbuffer, const unsigned *value);

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_ES_PROFILE               0x38
#define GL_TRIANGLE_STRIP               0x0005
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_2D                   0x0de1
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_UNSIGNED_BYTE                0x1401
#define GL_UNSIGNED_INT                 0x1405
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_RGBA8                        0x8058
#define GL_RGBA32F                      0x8814
#define GL_RGBA32UI                     0x8d70
#define GL_RGBA_INTEGER                 0x8d99
#define GL_COLOR                        0x1800
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_FRAMEBUFFER                  0x8d40
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_NONE                         0
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82

static void
say(const char *format, ...)
{
	char line[200];
	char kept[202];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	snprintf(kept, sizeof(kept), "!%s", line);
	sysctlbyname("kern.redwood.say", NULL, NULL, kept, strlen(kept));
}

static unsigned
shader(unsigned type, const char *source)
{
	unsigned s = glCreateShader(type);
	int good = 0;
	char log[160] = "";

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		say("gl_mrt: a shader does not compile: %s", log);
	}
	return s;
}

static unsigned
program(const char *fragment)
{
	unsigned p = glCreateProgram();
	int good = 0;

	glAttachShader(p, shader(GL_VERTEX_SHADER,
	    "#version 300 es\nin vec2 where;\nvoid main() { gl_Position = vec4(where, 0.0, 1.0); }\n"));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, fragment));
	glBindAttribLocation(p, 0, "where");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &good);
	if (!good) {
		say("gl_mrt: a program does not link");
	}
	return p;
}

/* One case: `targets` textures of `internal`, the shader, which target to read, and how. */
static const char *only;        /* argv[1]: just the case of that name */

static void
run(const char *name, int targets, int internal, unsigned format, unsigned type, const char *fragment, int read)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	unsigned textures[3], framebuffer, buffers[3], p, error;
	uint32_t pixel[4 * 16];

	if (only != NULL && strcmp(only, name) != 0) {
		return;
	}
	glGenTextures(targets, textures);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	for (int i = 0; i < targets; i++) {
		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, internal, 4, 4, 0, format, type, NULL);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (unsigned)i, GL_TEXTURE_2D, textures[i], 0);
		buffers[i] = GL_COLOR_ATTACHMENT0 + (unsigned)i;
	}
	glDrawBuffers(targets, buffers);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		say("gl_mrt: %s: the framebuffer is not complete", name);
		return;
	}
	/* a known clear, so a target the draw never reached says so: 7 everywhere */
	for (int i = 0; i < targets; i++) {
		static const float f7[4] = { 7, 7, 7, 7 };
		static const unsigned u7[4] = { 7, 7, 7, 7 };

		if (type == GL_UNSIGNED_INT) {
			glClearBufferuiv(GL_COLOR, i, u7);
		} else {
			glClearBufferfv(GL_COLOR, i, f7);
		}
	}
	p = program(fragment);
	glUseProgram(p);
	glViewport(0, 0, 4, 4);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, quad);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish();
	memset(pixel, 0x55, sizeof(pixel));
	glReadBuffer(GL_COLOR_ATTACHMENT0 + (unsigned)read);
	glReadPixels(0, 0, 4, 4, format, type, pixel);
	error = glGetError();
	if (type == GL_FLOAT) {
		const float *f = (const float *)pixel + 4 * 5;

		say("gl_mrt: %s: target %d reads %g %g %g %g (error %#x)", name, read, (double)f[0], (double)f[1],
		    (double)f[2], (double)f[3], error);
	} else if (type == GL_UNSIGNED_INT) {
		const uint32_t *u = pixel + 4 * 5;

		say("gl_mrt: %s: target %d reads %u %u %u %u (error %#x)", name, read, u[0], u[1], u[2], u[3], error);
	} else {
		const uint8_t *b = (const uint8_t *)pixel + 4 * 5;

		say("gl_mrt: %s: target %d reads %u %u %u %u /255 (error %#x)", name, read, b[0], b[1], b[2], b[3], error);
	}
}

int
main(int argc, char **argv)
{
	static const int attributes[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		                          OSMESA_CONTEXT_MAJOR_VERSION, 3, 0 };
	static unsigned char window[16 * 16 * 4];
	OSMesaContext context;

	only = argc > 1 ? argv[1] : NULL;
	context = OSMesaCreateContextAttribs(attributes, NULL);

	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		say("gl_mrt: no OpenGL ES 3 context");
		return 1;
	}
	say("gl_mrt: %s; %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
	say("gl_mrt: every target is cleared to 7 first; expected 0.25 0.5 0.75 1 (float), 64 128 191 255 (unorm), 1 2 3 4 (uint)");
	run("float0", 1, GL_RGBA32F, GL_RGBA, GL_FLOAT,
	    "#version 300 es\nprecision highp float;\nlayout(location = 0) out vec4 c0;\n"
	    "void main() { c0 = vec4(0.25, 0.5, 0.75, 1.0); }\n", 0);
	run("float2", 3, GL_RGBA32F, GL_RGBA, GL_FLOAT,
	    "#version 300 es\nprecision highp float;\nlayout(location = 2) out vec4 c2;\n"
	    "void main() { c2 = vec4(0.25, 0.5, 0.75, 1.0); }\n", 2);
	run("unorm2", 3, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
	    "#version 300 es\nprecision highp float;\nlayout(location = 2) out vec4 c2;\n"
	    "void main() { c2 = vec4(0.25, 0.5, 0.75, 1.0); }\n", 2);
	run("uint0", 1, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
	    "#version 300 es\nprecision highp float;\nlayout(location = 0) out highp uvec4 c0;\n"
	    "void main() { c0 = uvec4(1u, 2u, 3u, 4u); }\n", 0);
	return 0;
}
