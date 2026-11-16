/*
 * softpipesweep: one function made into a pipeline through libhakios-air on softpipe, as the driver makes
 * CoreAnimation's under QEMU (RWA_SOFTWARE=1, tools/air/softpipesweep.sh): a fragment function with the
 * vertex stage the library generates and one BGRA8 target; a vertex function with the fragment function
 * given (tools/air/rast_frag.ll's red) and every attribute a float4 of buffer 30, as a vertex descriptor
 * would configure it; a kernel as a compute pipeline. Prints "ok NAME" or "refused NAME:
 * why"; a crash in the software path -- NIR's validation, nir_to_tgsi -- is a process that does not print at
 * all, and is a window server that restarts on the emulator.
 *
 *     softpipesweep fragment|kernel FILE.bc ENTRY
 *     softpipesweep vertex FILE.bc ENTRY FRAGMENT.bc FRAGMENT_ENTRY
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
enum { BGRA8 = 80 };
static void *load(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	void *b = malloc(*n);
	if (fread(b, 1, *n, f) != *n) exit(2);
	fclose(f);
	return b;
}
int main(int argc, char **argv) {
	if (argc < 4 || (!strcmp(argv[1], "vertex") && argc < 6)) {
		fprintf(stderr, "usage: softpipesweep fragment|kernel FILE.bc ENTRY\n"
				"       softpipesweep vertex FILE.bc ENTRY FRAGMENT.bc FRAGMENT_ENTRY\n");
		return 2;
	}
	size_t n, fn = 0;
	void *bc = load(argv[2], &n);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	if (!d) { printf("device: %s\n", why); return 2; }
	int made;
	if (!strcmp(argv[1], "kernel")) {
		struct rwa_compute_info info;
		made = rwa_compute_create(d, bc, n, argv[3], &info, why, sizeof(why)) != NULL;
	} else {
		struct rwa_render_shape shape = { .color_format = { BGRA8 }, .color_count = 1, .sample_count = 1 };
		struct rwa_render_info info;
		if (!strcmp(argv[1], "vertex")) {
			void *fb = load(argv[4], &fn);
			for (unsigned i = 0; i < 16 && i < RWA_MAX_VERTEX_ATTRIBUTES; i++) {
				shape.vertex_attribute[i].format = 31;   /* MTLVertexFormatFloat4 */
				shape.vertex_attribute[i].offset = 16 * i;
				shape.vertex_attribute[i].buffer_index = 30;
			}
			shape.vertex_layout[30].stride = 256;
			shape.vertex_layout[30].step_function = 1;   /* per vertex */
			made = rwa_render_create(d, bc, n, argv[3], fb, fn, argv[5], &shape, &info, why,
						 sizeof(why)) != NULL;
		} else {
			made = rwa_render_create(d, NULL, 0, NULL, bc, n, argv[3], &shape, &info, why,
						 sizeof(why)) != NULL;
		}
	}
	if (made)
		printf("ok %s\n", argv[3]);
	else
		printf("refused %s: %s\n", argv[3], why);
	return 0;
}
