/* Part of the loader (included by loader.c, not compiled alone): where a download over USB goes, and how much of it fits. */

/*
 * A download goes to one of three places, as the command before it said:
 *
 *   0   the payload's area (PAYLOAD_BASE): the kernel and what travels with it;
 *   1   the resident image's area of the kernel's window ("oem ramdisk"), from its start or, with
 *       append ("oem ramdisk append"), after what it holds already;
 *   2   inside the resident image, from patch_at on ("oem ramdisk patch"): nothing beyond its end.
 *
 * Where in that place it begins (*before, bytes) and how many bytes may come (*room); or, when there
 * is no room at all, why -- the reply to the host. Nothing here adds two numbers a host chose: a size
 * near 2^64 cannot make a small sum.
 */
static const char *
download_room(int to, int append, uint64_t patch_at, uint64_t resident, uint64_t window, uint64_t *before, uint64_t *room)
{
	uint64_t area = window > RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE ? window - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE : 0;

	*before = 0;
	*room = 0;
	if (to == 2) {
		/* the image the patch was for may have gone since ("oem ramdisk none", "oem dsu", a run's own) */
		if (patch_at >= resident) {
			return "no resident image there";
		}
		*before = patch_at;
		*room = resident - patch_at;
		return NULL;
	}
	if (to == 1) {
		*before = append ? resident : 0;
		if (*before >= area) {
			return "no memory window";
		}
		*room = area - *before;
		return NULL;
	}
	*room = PAYLOAD_MAX_SIZE - 0x100000;
	return NULL;
}
