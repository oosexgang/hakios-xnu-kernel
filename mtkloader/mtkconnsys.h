/*
 * What Android's device tree says about CONNSYS, the Wi-Fi/Bluetooth/GPS subsystem of the MT6768
 * (with the MT6631 radio): step 1 of docs/wifi-x6826b.md, reading only. MediaTek's driver
 * (common_main/platform/mt6768.c in MiCode/MTK_kernel_modules merlin-r-oss) takes the addresses of
 * every block it touches from its node's "reg" by index (the MCU config, TOP RGU, INFRACFG_AO, SPM,
 * the MCU's misc and HIF blocks ...), its interrupt from "interrupts", and the memory the firmware
 * runs from from a reserved-memory node; none of that is a constant of the source. LK hands us the
 * very tree Android boots with, so the values are printed from it here, before XNU starts.
 *
 * Every node whose name or "compatible" says consys, conninfra, connsys, wifi, wlan or gps is
 * printed: its name, its compatible strings, and its "reg" and "interrupts" as the raw 32-bit
 * words they are (the parent's #address-cells and #size-cells are printed too, so nothing about
 * them is assumed here).
 */

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

static uint32_t
dt_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int
dt_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* `needle` anywhere in the first `len` bytes of `hay` (which may hold several NUL-separated strings) */
static int
dt_contains(const char *hay, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len; i++) {
		uint32_t j = 0;

		while (needle[j] && i + j < len && hay[i + j] == needle[j]) {
			j++;
		}
		if (!needle[j]) {
			return 1;
		}
	}
	return 0;
}

static int
dt_wanted(const char *text, uint32_t len)
{
	static const char *words[] = { "consys", "conninfra", "connsys", "wifi", "wlan", "gps",
	    /* and the sensors (27 October): how the accelerometer and the rest are reached */
	    "sensor", "accel", "gsensor", "gyro", "alsps", "magnet", "scp",
	    /* and the bridge to CONNSYS that WMT talks through (28 October) */
	    "btif" };

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		if (dt_contains(text, len, words[i])) {
			return 1;
		}
	}
	return 0;
}

static uint32_t
dt_len(const char *s)
{
	uint32_t n = 0;

	while (s[n]) {
		n++;
	}
	return n;
}

static void
dt_words(const char *what, const uint8_t *value, uint32_t len)
{
	out("    ");
	out(what);
	out(":");
	for (uint32_t i = 0; i + 4 <= len && i < 4 * 48; i += 4) {
		out(" ");
		out_hex(dt_be32(value + i));
	}
	out("\n");
}

static void
connsys_dt_report(uint64_t dtb)
{
	const uint8_t *fdt = (const uint8_t *)(uintptr_t)dtb;
	const uint8_t *structs, *p, *end;
	const char *strings;
	/* the node being walked, and its parent's cell sizes, a level per depth */
	struct {
		const char *name;
		const uint8_t *compatible, *reg, *interrupts, *size, *ranges;
		uint32_t compatible_len, reg_len, interrupts_len, size_len, ranges_len;
		uint32_t address_cells, size_cells;     /* this node's own #address-cells/#size-cells, for its children */
	} level[24];
	int depth = -1;
	unsigned shown = 0;

	if (dtb == 0 || dt_be32(fdt) != FDT_MAGIC) {
		out("connsys: no flattened device tree from LK\n");
		return;
	}
	structs = fdt + dt_be32(fdt + 8);
	strings = (const char *)(fdt + dt_be32(fdt + 12));
	end = structs + dt_be32(fdt + 36);
	out("connsys: Android's device tree, the nodes that say consys/conninfra/wifi/gps:\n");
	for (p = structs; p < end;) {
		uint32_t token = dt_be32(p);

		p += 4;
		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;

			p += (dt_len(name) + 1 + 3) & ~3u;
			if (++depth >= 24) {
				out("connsys: the tree is deeper than this walk keeps; stopped\n");
				return;
			}
			level[depth].name = name;
			level[depth].compatible = level[depth].reg = level[depth].interrupts = NULL;
			level[depth].size = level[depth].ranges = NULL;
			level[depth].compatible_len = level[depth].reg_len = level[depth].interrupts_len = 0;
			level[depth].size_len = level[depth].ranges_len = 0;
			level[depth].address_cells = 2;
			level[depth].size_cells = 1;
		} else if (token == FDT_PROP) {
			uint32_t len = dt_be32(p), name_offset = dt_be32(p + 4);
			const char *prop = strings + name_offset;
			const uint8_t *value = p + 8;

			p += 8 + ((len + 3) & ~3u);
			if (depth < 0) {
				continue;
			}
			if (dt_streq(prop, "compatible")) {
				level[depth].compatible = value;
				level[depth].compatible_len = len;
			} else if (dt_streq(prop, "reg")) {
				level[depth].reg = value;
				level[depth].reg_len = len;
			} else if (dt_streq(prop, "interrupts")) {
				level[depth].interrupts = value;
				level[depth].interrupts_len = len;
			} else if (dt_streq(prop, "size")) {         /* reserved memory placed by the kernel: how much */
				level[depth].size = value;
				level[depth].size_len = len;
			} else if (dt_streq(prop, "alloc-ranges")) { /* ... and where it may go */
				level[depth].ranges = value;
				level[depth].ranges_len = len;
			} else if (dt_streq(prop, "#address-cells") && len == 4) {
				level[depth].address_cells = dt_be32(value);
			} else if (dt_streq(prop, "#size-cells") && len == 4) {
				level[depth].size_cells = dt_be32(value);
			}
		} else if (token == FDT_END_NODE) {
			if (depth < 0) {
				break;
			}
			const char *name = level[depth].name;
			int wanted = dt_wanted(name, dt_len(name)) ||
			    (level[depth].compatible && dt_wanted((const char *)level[depth].compatible, level[depth].compatible_len));

			if (wanted && shown < 48) {
				shown++;
				out("  ");
				out(depth > 0 ? level[depth - 1].name : "");
				out("/");
				out(name);
				if (depth > 0) {
					out(" (parent cells: address ");
					out_dec(level[depth - 1].address_cells);
					out(", size ");
					out_dec(level[depth - 1].size_cells);
					out(")");
				}
				out("\n");
				if (level[depth].compatible) {
					out("    compatible:");
					for (uint32_t i = 0; i < level[depth].compatible_len;) {
						const char *s = (const char *)level[depth].compatible + i;

						out(" ");
						out(s);
						i += dt_len(s) + 1;
					}
					out("\n");
				}
				if (level[depth].reg) {
					dt_words("reg", level[depth].reg, level[depth].reg_len);
				}
				if (level[depth].interrupts) {
					dt_words("interrupts", level[depth].interrupts, level[depth].interrupts_len);
				}
				if (level[depth].size) {
					dt_words("size", level[depth].size, level[depth].size_len);
				}
				if (level[depth].ranges) {
					dt_words("alloc-ranges", level[depth].ranges, level[depth].ranges_len);
				}
			}
			depth--;
		} else if (token == FDT_NOP) {
			continue;
		} else {
			break;                          /* FDT_END, or something this walk does not know */
		}
	}
	out("connsys: ");
	out_dec(shown);
	out(" nodes\n");
}
