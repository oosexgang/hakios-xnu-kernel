/*
 * fnvsum: the FNV-1a 64 of files, as tools/apfs/apfs-tool census and the kernel's apfs_check
 * hash contents.  Paths one per line on stdin, "HASH<TAB>PATH" out.  (Python is a hundred
 * times slower at it, and an image of the system is gigabytes.)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	static unsigned char buf[1 << 20];
	char path[4096];

	while (fgets(path, sizeof(path), stdin) != NULL) {
		uint64_t h = 0xcbf29ce484222325ULL;
		size_t n;
		FILE *f;

		path[strcspn(path, "\n")] = '\0';
		f = fopen(path, "rb");
		if (f == NULL) {
			printf("-\t%s\n", path);
			continue;
		}
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			for (size_t i = 0; i < n; i++) {
				h = (h ^ buf[i]) * 0x100000001b3ULL;
			}
		}
		fclose(f);
		printf("%016llx\t%s\n", (unsigned long long)h, path);
	}
	return 0;
}
