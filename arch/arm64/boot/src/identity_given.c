/* Part of the loader (included by loader.c, not compiled alone): which iPhone Darwin is told it runs on, from the payload. */
/* ------------------------------------------------------------------------ */
/* Identity                                                                 */

/*
 * The identity (identity.h, made by mkidentity.py from an iPhone's device tree) is Apple's data: a loader built with
 * it is for its builder's own phone, and one that is to be passed around -- an image in a phone's repository -- is
 * built with the placeholder. Such a loader takes the identity from the payload instead: an entry "identity" that
 * whoever boots the image made from their own IPSW (mkidentity.py --blob, tools/image.py --identity). When the
 * payload has one it wins over what the loader was built with.
 *
 *   u32 magic "IDNT", u32 version 1, u32 size (of the whole blob)
 *   char platform[32]                 the SoC's name (/arm-io compatible), NUL-terminated
 *   u32 width, height, scale          the screen (/product)
 *   u32 count[4]                      properties of the root, /product, /product/haptics, /product/camera
 *   then the properties, section after section: char name[32] (NUL-terminated), u32 length, the value padded to 4
 */
#define IDENTITY_BLOB_MAGIC     0x544e4449u     /* "IDNT" */
#define IDENTITY_SECTIONS       4
#define IDENTITY_MAX_PROPS      128             /* in one section: an iPhone's /product has some sixty */
#define IDENTITY_MOST           0x8000          /* bytes of a blob at most: half the device tree's room */

struct identity_set {
	const struct identity_prop *props;
	uint32_t count;
	const unsigned char *data;      /* each property's offset is from here */
};

struct identity_view {
	const char *platform;
	uint32_t width, height, scale;
	struct identity_set root, product, haptics, camera;
	int given;                      /* from the payload, not built in */
};

static struct identity_prop identity_given_props[IDENTITY_SECTIONS][IDENTITY_MAX_PROPS];

static uint32_t
identity_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* a NUL within the N bytes at P: the name or string there ends inside them */
static int
identity_terminated(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] == 0) {
			return i > 0;
		}
	}
	return 0;
}

/* The identity the loader was built with (identity.h) */
static void
identity_built_in(struct identity_view *v)
{
	memset(v, 0, sizeof(*v));
	v->platform = IDENTITY_PLATFORM_NAME;
	v->width = IDENTITY_SCREEN_WIDTH;
	v->height = IDENTITY_SCREEN_HEIGHT;
	v->scale = IDENTITY_SCREEN_SCALE;
	v->root = (struct identity_set){ identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data };
	v->product = (struct identity_set){ identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data };
	v->haptics = (struct identity_set){ identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data };
	v->camera = (struct identity_set){ identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data };
}

/*
 * The blob of SIZE bytes at BLOB, read into V (its properties point into the blob, which stays where it is): 1, or 0 when
 * it is not one -- a wrong magic or version, a size other than its own, a name or a value that does not end inside it,
 * more properties than there is room for -- and V is left as it was.
 */
static int
identity_from_blob(const uint8_t *blob, uint32_t size, struct identity_view *v)
{
	struct identity_set *sets[IDENTITY_SECTIONS];
	uint32_t at = 12 + 32 + 12 + 4 * IDENTITY_SECTIONS, count[IDENTITY_SECTIONS], width, height, scale;
	struct identity_view read;

	if (blob == 0 || size < at || identity_u32(blob) != IDENTITY_BLOB_MAGIC || identity_u32(blob + 4) != 1 ||
	    identity_u32(blob + 8) != size || !identity_terminated(blob + 12, 32)) {
		return 0;
	}
	width = identity_u32(blob + 44);
	height = identity_u32(blob + 48);
	scale = identity_u32(blob + 52);
	if (width == 0 || height == 0 || scale == 0 || scale > 4) {
		return 0;
	}
	memset(&read, 0, sizeof(read));
	sets[0] = &read.root;
	sets[1] = &read.product;
	sets[2] = &read.haptics;
	sets[3] = &read.camera;
	for (unsigned s = 0; s < IDENTITY_SECTIONS; s++) {
		count[s] = identity_u32(blob + 56 + 4 * s);
		if (count[s] == 0 || count[s] > IDENTITY_MAX_PROPS) {
			return 0;       /* (none in a section is no device tree's: mkidentity.py writes a harmless one instead) */
		}
	}
	for (unsigned s = 0; s < IDENTITY_SECTIONS; s++) {
		for (uint32_t i = 0; i < count[s]; i++) {
			uint32_t length;

			if (size - at < 36 || !identity_terminated(blob + at, 32)) {
				return 0;
			}
			length = identity_u32(blob + at + 32);
			if (length > size - at - 36) {
				return 0;
			}
			identity_given_props[s][i].name = (const char *)(blob + at);
			identity_given_props[s][i].offset = at + 36;
			identity_given_props[s][i].length = length;
			at += 36 + ((length + 3) & ~3u);
			if (at > size) {
				return 0;
			}
		}
		*sets[s] = (struct identity_set){ identity_given_props[s], count[s], blob };
	}
	if (at != size) {
		return 0;               /* (what follows the last property is no part of it) */
	}
	read.platform = (const char *)(blob + 12);
	read.width = width;
	read.height = height;
	read.scale = scale;
	read.given = 1;
	*v = read;
	return 1;
}

/* The identity Darwin is told: the payload's when it has a good one, else the one built in. Says which. */
static void
identity_choose(const uint8_t *blob, uint32_t size, struct identity_view *v)
{
	identity_built_in(v);
	if (blob == 0) {
		return;
	}
	if (size > IDENTITY_MOST) {
		/* (the device tree has 64 KB, the loader's own nodes some 15 of them: an identity has half of it at most) */
		print("identity: the payload's is %d bytes, more than the %d there is room for: the one built in\n", (uint64_t)size,
		    (uint64_t)IDENTITY_MOST);
		return;
	}
	if (identity_from_blob(blob, size, v)) {
		print("identity: from the payload (%s, %dx%d)\n", v->platform, (uint64_t)v->width, (uint64_t)v->height);
	} else {
		print("identity: the payload's is not one (%d bytes): the one built in\n", (uint64_t)size);
	}
}
