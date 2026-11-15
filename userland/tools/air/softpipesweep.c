/*
 * softpipesweep: one fragment function made into a render pipeline through libhakios-air, with the vertex
 * stage the library generates and one BGRA8 target, as the driver makes CoreAnimation's under QEMU
 * (RWA_SOFTWARE=1, tools/air/softpipesweep.sh). Prints "ok NAME" or "refused NAME: why"; a crash in the
 * software path -- NIR's validation, nir_to_tgsi -- is a process that does not print at all, and is a
 * window server that restarts on the emulator.
 *
 *     softpipesweep FILE.bc ENTRY
 */
#include <stdio.h>
#include <stdlib.h>
#include "air_metal.h"
enum { BGRA8 = 80 };
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: softpipesweep FILE.bc ENTRY\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *bc = malloc(n);
	if (fread(bc, 1, n, f) != (size_t)n) return 2;
	fclose(f);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	if (!d) { printf("device: %s\n", why); return 2; }
	struct rwa_render_shape shape = { .color_format = { BGRA8 }, .color_count = 1, .sample_count = 1 };
	struct rwa_render_info info;
	struct rwa_render *r = rwa_render_create(d, NULL, 0, NULL, bc, n, argv[2], &shape, &info, why, sizeof(why));
	if (r)
		printf("ok %s\n", argv[2]);
	else
		printf("refused %s: %s\n", argv[2], why);
	return 0;
}
