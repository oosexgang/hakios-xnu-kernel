/*
 * vftcheck: tools/air/vft.ll dispatched through libhakios-air with tools/air/vft_add.ll and
 * tools/air/vft_mul.ll linked (rwa_linked_register) and a visible function table at buffer 1, as a
 * driver makes one: eight bytes a handle, the low word one more than the function's place among the
 * linked ones (nir_air.h, function_tables). Entry 0 names add1, entry 1 mul10; thread t writes
 * table[t & 1](t).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "air_metal.h"
static void *load(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	void *b = malloc(*n);
	if (fread(b, 1, *n, f) != *n) exit(1);
	fclose(f);
	return b;
}
int main(int argc, char **argv) {
	(void)argc;
	size_t n, an, mn;
	void *bc = load(argv[1], &n), *add = load(argv[2], &an), *mul = load(argv[3], &mn);
	struct rwa_linked_function linked[2] = { { "add1", add, an, "add1" }, { "mul10", mul, mn, "mul10" } };
	rwa_linked_register(bc, n, linked, 2);
	char why[512] = "";
	struct rwa_device *d = rwa_device_open(why, sizeof(why));
	struct rwa_compute_info info;
	struct rwa_compute *c = d ? rwa_compute_create(d, bc, n, "vt", &info, why, sizeof(why)) : NULL;
	if (!c) { printf("compute: %s\n", why); return 1; }
	struct rwa_buffer *out = rwa_buffer_create(d, 64, why, sizeof(why));
	struct rwa_buffer *table = rwa_buffer_create(d, 16, why, sizeof(why));
	unsigned long long *h = rwa_buffer_map(table);
	h[0] = 1;   /* add1 */
	h[1] = 2;   /* mul10 */
	struct rwa_buffer *b[2] = { out, table };
	struct rwa_bindings bind = { .buffers = b, .buffer_count = 2 };
	unsigned g[3] = { 1, 1, 1 }, s[3] = { 8, 1, 1 };
	if (rwa_dispatch(c, &bind, g, s, why, sizeof(why))) { printf("dispatch: %s\n", why); return 1; }
	unsigned *o = rwa_buffer_map(out);
	int bad = 0;
	for (unsigned t = 0; t < 8; t++) {
		unsigned want = t & 1 ? t * 10 : t + 1;
		if (o[t] != want) { printf("%u: %u, want %u\n", t, o[t], want); bad++; }
	}
	printf("%s\n", bad ? "WRONG" : "functions out of a visible function table: ok");
	return bad != 0;
}
