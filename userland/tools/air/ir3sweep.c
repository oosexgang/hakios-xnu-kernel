/*
 * ir3sweep: the phone's shader compiler over AIR modules, on Linux, with no GPU. Each module is translated,
 * finalised and compiled by ir3 for the Adreno given, exactly as libhakios-air does on the phone
 * (rwa_ir3_cost, tools/mesa/air/air_metal.h), and one line says what came of it:
 *
 *   ok   NAME  translate_ms finalize_ms compile_ms  nir_instructions ir3_instructions  full_regs half_regs constlen
 *   FAIL NAME  why
 *
 *   ir3sweep CHIP_ID FILE.bc[=ENTRY] ...      (CHIP_ID: 0x06060010 is the Adreno 642L, 0x44050001 the 830)
 *
 * The modules are bare bitcode, one function each (tools/metallib.py dump takes a .metallib apart); ENTRY is
 * the function to compile when the file name is not it. tools/air/ir3sweep.sh builds this against the host
 * build of the library (tools/air/mk-host.sh) and runs it over a library's render and compute functions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air_metal.h"

static void *
slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	void *data = NULL;

	if (f == NULL) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) == 0) {
		long n = ftell(f);
		if (n > 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)n)) != NULL &&
		    fread(data, 1, (size_t)n, f) != (size_t)n) {
			free(data);
			data = NULL;
		}
		*size = n > 0 ? (size_t)n : 0;
	}
	fclose(f);
	return data;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: ir3sweep CHIP_ID FILE.bc[=ENTRY] ...\n");
		return 2;
	}
	unsigned chip = (unsigned)strtoul(argv[1], NULL, 0);
	int failed = 0;

	for (int i = 2; i < argc; i++) {
		char path[4096], *entry;
		snprintf(path, sizeof(path), "%s", argv[i]);
		entry = strchr(path, '=');
		if (entry != NULL) {
			*entry++ = 0;
		}
		size_t size = 0;
		void *bitcode = slurp(path, &size);
		if (bitcode == NULL) {
			printf("FAIL\t%s\tcannot be read\n", path);
			failed++;
			continue;
		}
		struct rwa_ir3_cost cost = { 0 };
		char why[512] = "";
		if (rwa_ir3_cost(bitcode, size, entry, chip, &cost, why, sizeof(why)) == 0) {
			printf("ok\t%s\t%.1f %.1f %.1f\t%u %u\t%u %u %u\n", entry ? entry : path, cost.translate_ms,
			       cost.finalize_ms, cost.compile_ms, cost.nir_instructions, cost.instructions,
			       cost.max_full_reg, cost.max_half_reg, cost.constlen);
		} else {
			for (char *p = why; *p; p++) {
				if (*p == '\n' || *p == '\t') {
					*p = ' ';
				}
			}
			printf("FAIL\t%s\t%s\n", entry ? entry : path, why);
			failed++;
		}
		free(bitcode);
	}
	return failed ? 1 : 0;
}
