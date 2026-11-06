/*
 * Pipeline recipes (src/redwoodmetal.m has the measurements behind them, at rw_warm_start's call): what a
 * process built, as one line of text each -- the label, the shape's non-zero words, and for a pipeline
 * whose shaders are not the pipeline library's, its two functions and their constants -- and, read back
 * from userland/share/rwm-recipes.txt at the next start, the same pipelines built by a thread of their
 * own before the window server asks for them. A build the warm-up finished is handed over whole
 * (rwm_warm_take); one it did not reach is built by the caller as before.
 *
 * The lines said here are read by the host (tools that fold them into rwm-recipes.txt): their formats
 * are kept to the character.
 */
#include "rwm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>


namespace {

pthread_mutex_t compile_lock = PTHREAD_MUTEX_INITIALIZER;

/* what this process built, as recipes (malloc'd text), each label once: a few hundred at most, built once
 * each, so a list searched from the start (plain arrays: the boot partition has no room for the templates
 * of std::set and std::string -- 2048 bytes too many in run 401's image with them) */
char **recipes;
size_t nrecipes, recipes_room, recipes_said;

/* a recipe's text, grown as it is written */
struct text {
	char *at = nullptr;
	size_t used = 0, room = 0;

	~text() { free(at); }
	bool add(const char *more, size_t n)
	{
		if (used + n + 1 > room) {
			size_t want = (used + n + 1) * 2;
			char *grown = (char *)realloc(at, want);

			if (grown == nullptr) {
				return false;
			}
			at = grown;
			room = want;
		}
		memcpy(at + used, more, n);
		used += n;
		at[used] = '\0';
		return true;
	}
	bool add(const char *more) { return add(more, strlen(more)); }
	char *take()
	{
		char *t = at;

		at = nullptr;
		used = room = 0;
		return t;
	}
};

bool
recorded(const char *label)
{
	const size_t n = strlen(label);

	for (size_t i = 0; i < nrecipes; i++) {
		if (strncmp(recipes[i], label, n) == 0 && recipes[i][n] == ' ') {
			return true;
		}
	}
	return false;
}

constexpr unsigned warm_max = 512;
constexpr size_t label_skip = 29;       /* "com.apple.coreanimation.draw." in a refusal's line */

struct warm {
	char label[160];
	struct rwa_render_shape shape;
	struct rwa_render *render;
	struct rwa_render_info info;
	int state;      /* 0 to build, 1 built, 2 taken, failed or built by a caller */
	/* not in the pipeline library: its two functions out of QuartzCore-default.metallib, and their constants */
	char vertex[96], fragment[96];
	unsigned nv, nf;
	struct rwa_constant vc[32], fc[32];
};

warm *warms;
unsigned nwarm, warm_built, warm_used, warm_nofunction, warm_refused;
char warm_first_refusal[160];  /* the first build the compiler refused, and why */

void
say(const char *line)
{
	sysctlbyname("kern.redwood.say", NULL, NULL, (void *)line, strlen(line));
}

const char *
short_label(const char *label)
{
	return strlen(label) >= label_skip ? label + label_skip : label;
}

bool
recipe_of(text &line, const char *label, const struct rwa_render_shape *shape)
{
	const uint32_t *w = (const uint32_t *)(const void *)shape;
	char word[24];

	if (!line.add(label) || !line.add(" ", 1)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(*shape) / 4; i++) {
		if (w[i] != 0) {
			snprintf(word, sizeof(word), "%x=%x,", i, w[i]);
			if (!line.add(word)) {
				return false;
			}
		}
	}
	return true;
}

bool
recipe_parse(const char *text, warm *out)
{
	const char *space = strchr(text, ' ');
	uint32_t *w = (uint32_t *)(void *)&out->shape;

	memset(out, 0, sizeof(*out));
	if (space == nullptr || (size_t)(space - text) >= sizeof(out->label)) {
		return false;
	}
	memcpy(out->label, text, (size_t)(space - text));
	for (const char *at = space + 1; *at != '\0' && *at != '\n' && *at != ' ';) {    /* (a space: the functions follow) */
		char *end;
		unsigned long index = strtoul(at, &end, 16), value;

		if (*end != '=' || index >= sizeof(out->shape) / 4) {
			return false;
		}
		value = strtoul(end + 1, &end, 16);
		w[index] = (uint32_t)value;
		if (*end != ',') {
			break;
		}
		at = end + 1;
	}

	const char *bar = strstr(space + 1, " |");

	if (bar != nullptr) {
		const char *v = bar + 2, *f = strchr(v, '|'), *c1 = f != nullptr ? strchr(f + 1, '|') : nullptr;

		if (c1 == nullptr || (size_t)(f - v) >= sizeof(out->vertex) || (size_t)(c1 - f - 1) >= sizeof(out->fragment)) {
			return false;
		}
		memcpy(out->vertex, v, (size_t)(f - v));
		memcpy(out->fragment, f + 1, (size_t)(c1 - f - 1));
		for (int side = 0; side < 2; side++) {
			struct rwa_constant *c = side == 0 ? out->vc : out->fc;
			unsigned *n = side == 0 ? &out->nv : &out->nf;
			const char *at = c1 + 1;

			while (*at != '\0' && *at != '|' && *at != '\n' && *n < 32) {
				char *end;

				c[*n].index = (unsigned)strtoul(at, &end, 16);
				if (*end != ':') {
					return false;
				}
				c[*n].bits = strtoull(end + 1, &end, 16);
				(*n)++;
				at = *end == ';' ? end + 1 : end;
			}
			c1 = strchr(at, '|');
			if (side == 0 && c1 == nullptr) {
				return false;
			}
		}
	}
	return true;
}

/* "rwr PROCESS N K/M text": a recipe in pieces a line can hold */
void
say_recipe(const char *text, size_t n, bool to_host)
{
	const size_t length = strlen(text), pieces = (length + 149) / 150;

	for (size_t k = 0; k < pieces; k++) {
		char line[200];

		snprintf(line, sizeof(line), "rwr %s %zu %zu/%zu %.150s", getprogname(), n, k + 1, pieces, text + k * 150);
		sysctlbyname(to_host ? "kern.redwood.host_keep" : "kern.redwood.say", NULL, NULL, line, strlen(line));
	}
}

} // namespace

extern "C" void
rwm_compile_lock(void)
{
	pthread_mutex_lock(&compile_lock);
}

extern "C" void
rwm_compile_unlock(void)
{
	pthread_mutex_unlock(&compile_lock);
}

/*
 * CoreAnimation's pipelines that blend inside the fragment shader -- draw.PBGRAXm_..., no B<mode> segment:
 * they read the target with [[color(0)]] and composite themselves -- get the fixed function's premultiplied
 * source-over as well. On the Mali that read gives 0 (run 342), so a layer's transparent part came out
 * black (runs 334-336); with the shader's own blend reading 0, its output is the premultiplied source, and
 * the fixed function puts it over what is there. Exact for source-over. Boot-arg rwm_xm_blend=0: left as
 * they were.
 */
extern "C" void
rwm_xm_blend_apply(const char *label, struct rwa_render_shape *shape)
{
	static int on = -1;

	if (on < 0) {
		on = !rwm_boot_arg("rwm_xm_blend=0");
	}
	if (!on || label == nullptr || strncmp(label, "com.apple.coreanimation.draw.PBGRAXm_", 37) != 0 ||
	    shape->color_blend[0].blending_enabled) {
		return;
	}
	shape->color_blend[0].blending_enabled = 1;
	shape->color_blend[0].rgb_blend_operation = 0;              /* Add */
	shape->color_blend[0].alpha_blend_operation = 0;
	shape->color_blend[0].source_rgb_blend_factor = 1;          /* One */
	shape->color_blend[0].source_alpha_blend_factor = 1;
	shape->color_blend[0].destination_rgb_blend_factor = 5;     /* OneMinusSourceAlpha */
	shape->color_blend[0].destination_alpha_blend_factor = 5;
}

extern "C" void
rwm_recipe_record(const char *label, const struct rwa_render_shape *shape, const char *vertex,
		  const char *fragment, const struct rwa_constant *vc, unsigned nv, const struct rwa_constant *fc,
		  unsigned nf)
{
	if (label == nullptr) {
		return;
	}
	pthread_mutex_lock(&compile_lock);
	if (!recorded(label)) {
		text recipe;
		char part[48];
		bool whole = recipe_of(recipe, label, shape);

		/* " |VERTEX|FRAGMENT|i:bits;...|i:bits;..." for one whose shaders are not the pipeline library's */
		if (whole && fragment != nullptr) {
			whole = recipe.add(" |") && recipe.add(vertex != nullptr ? vertex : "") && recipe.add("|", 1) &&
				recipe.add(fragment) && recipe.add("|", 1);
			for (unsigned i = 0; whole && i < nv && i < 32; i++) {
				snprintf(part, sizeof(part), "%x:%llx;", vc[i].index, (unsigned long long)vc[i].bits);
				whole = recipe.add(part);
			}
			whole = whole && recipe.add("|", 1);
			for (unsigned i = 0; whole && i < nf && i < 32; i++) {
				snprintf(part, sizeof(part), "%x:%llx;", fc[i].index, (unsigned long long)fc[i].bits);
				whole = recipe.add(part);
			}
		}
		if (whole && nrecipes == recipes_room) {
			size_t room = recipes_room != 0 ? recipes_room * 2 : 64;
			char **grown = (char **)realloc(recipes, room * sizeof(*recipes));

			whole = grown != nullptr;
			if (whole) {
				recipes = grown;
				recipes_room = room;
			}
		}
		if (whole) {
			recipes[nrecipes++] = recipe.take();
			/* and at once to the kernel's buffer for the host, which comes back whole however long the
			 * run (kern.redwood.host_keep): the log's middle is lost, and with it a long run's recipes
			 * (run 352) */
			say_recipe(recipes[nrecipes - 1], nrecipes - 1, true);
		}
	}
	pthread_mutex_unlock(&compile_lock);
}

/* the recipes into the log, "rwr PROCESS N K/M text" -- all of them on the notification, and once a minute
 * those built since the last time (so that any run, one somebody uses by hand included, adds to the file:
 * run 350 built eight that no recipe had, and said none) */
static void
recipes_say_from(size_t first)
{
	size_t all;
	char head[120];

	/* (the list only grows, and a recipe's text is never changed or freed: what is counted now can be
	 * read outside the lock) */
	pthread_mutex_lock(&compile_lock);
	all = nrecipes;
	pthread_mutex_unlock(&compile_lock);
	if (first >= all && first != 0) {
		return;
	}
	snprintf(head, sizeof(head), "!rwr: %lu recipes from %s; warmed %u of %u, %u used", (unsigned long)all,
		 getprogname(), warm_built, nwarm, warm_used);
	say(head);
	for (size_t n = first; n < all; n++) {
		const char *recipe;

		pthread_mutex_lock(&compile_lock);
		recipe = recipes[n];    /* (the array may be moved by realloc: read under the lock) */
		pthread_mutex_unlock(&compile_lock);
		say_recipe(recipe, n, false);
		usleep(2000);   /* (the kernel's log is drained by a reader; a burst of hundreds outruns it) */
	}
	recipes_said = all;
}

extern "C" void
rwm_recipes_say(void)
{
	recipes_say_from(0);
}

extern "C" void
rwm_recipes_say_new(void)
{
	size_t now;

	pthread_mutex_lock(&compile_lock);
	now = nrecipes;
	pthread_mutex_unlock(&compile_lock);
	if (now > recipes_said) {
		recipes_say_from(recipes_said);
	}
}

/*
 * This process's recipes out of the file: "[name]" heads a process's section, "=K words" is shape K, which
 * the lines after it name as "LABEL @K" (most pipelines share one), and any other line is a whole recipe.
 */
extern "C" unsigned
rwm_warm_load(const char *const *files, unsigned file_count)
{
	char line[2300];
	FILE *f = nullptr;
	bool mine = false;     /* in this process's section of the file */
	static char shapes[32][2048];

	for (unsigned i = 0; i < file_count && f == nullptr; i++) {
		f = fopen(files[i], "r");
	}
	if (f == nullptr) {
		return 0;
	}
	if (warms == nullptr) {
		warms = (warm *)calloc(warm_max, sizeof(*warms));
	}
	while (warms != nullptr && nwarm < warm_max && fgets(line, sizeof(line), f) != nullptr) {
		char *space = strchr(line, ' '), *at = space != nullptr ? strchr(space, '@') : nullptr;

		if (line[0] == '[') {
			size_t n = strlen(getprogname());

			mine = strncmp(line + 1, getprogname(), n) == 0 && line[1 + n] == ']';
			continue;
		}
		if (line[0] == '=') {
			unsigned k = (unsigned)atoi(line + 1);

			if (k < 32 && space != nullptr) {
				snprintf(shapes[k], sizeof(shapes[k]), "x%s", space);
			}
			continue;
		}
		if (line[0] == '#' || space == nullptr || !mine) {
			continue;
		}
		if (at == space + 1) {
			unsigned k = (unsigned)atoi(at + 1);
			char whole[2300];

			if (k >= 32 || shapes[k][0] == '\0') {
				continue;
			}

			const char *rest = strchr(at, ' ');     /* the functions, for one not in the pipeline library */

			snprintf(whole, sizeof(whole), "%.*s%.*s%s", (int)(space - line), line,
				 (int)strcspn(shapes[k] + 1, "\n"), shapes[k] + 1, rest != nullptr ? rest : "");
			if (recipe_parse(whole, &warms[nwarm])) {
				rwm_xm_blend_apply(warms[nwarm].label, &warms[nwarm].shape);
				nwarm++;
			}
		} else if (recipe_parse(line, &warms[nwarm])) {
			rwm_xm_blend_apply(warms[nwarm].label, &warms[nwarm].shape);
			nwarm++;
		}
	}
	fclose(f);
	return nwarm;
}

extern "C" bool
rwm_warm_item(unsigned index, struct rwm_warm_item *item)
{
	if (index >= nwarm) {
		return false;
	}
	item->label = warms[index].label;
	item->vertex = warms[index].vertex;
	item->fragment = warms[index].fragment;
	return true;
}

extern "C" void
rwm_warm_build(unsigned index, struct rwa_device *device, const void *vertex, size_t vertex_size,
	       const void *fragment, size_t fragment_size, bool found)
{
	const struct rwm_air *air = rwm_air();
	warm *w;

	if (index >= nwarm) {
		return;
	}
	w = &warms[index];
	pthread_mutex_lock(&compile_lock);
	if (w->state == 0 && found && air->render_create != nullptr) {
		char why[256] = { 0 };

		if (air->render_constants != nullptr) {
			air->render_constants(w->vc, w->nv, w->fc, w->nf);
		}
		w->render = air->render_create(device, vertex, vertex_size, NULL, fragment, fragment_size, NULL,
					       &w->shape, &w->info, why, sizeof(why));
		w->state = w->render != nullptr ? 1 : 2;
		warm_built += w->render != nullptr;
		if (w->render == nullptr && warm_refused++ == 0) {
			snprintf(warm_first_refusal, sizeof(warm_first_refusal), "%s: %s", short_label(w->label), why);
		}
	} else if (w->state == 0) {
		w->state = 2;
		if (!found && warm_nofunction++ == 0) {
			snprintf(warm_first_refusal, sizeof(warm_first_refusal), "%s: no functions %s/%s",
				 short_label(w->label), w->vertex, w->fragment);
		}
	}
	pthread_mutex_unlock(&compile_lock);
}

extern "C" void
rwm_warm_report(uint64_t elapsed_ns)
{
	char line[160];

	snprintf(line, sizeof(line), "!rwm warm-up: %u of %u pipelines built in %llu ms, %u taken so far; %u without "
		 "functions, %u refused", warm_built, nwarm, (unsigned long long)(elapsed_ns / 1000000ull), warm_used,
		 warm_nofunction, warm_refused);
	say(line);
	if (warm_first_refusal[0] != '\0') {
		snprintf(line, sizeof(line), "!rwm warm-up, the first not built: %.150s", warm_first_refusal);
		say(line);
	}
}

/* a built recipe for this request, if one is ready: its render and info, and it is the caller's */
extern "C" bool
rwm_warm_take(const char *label, const struct rwa_render_shape *shape, struct rwa_render **render,
	      struct rwa_render_info *info)
{
	bool taken = false;

	if (warms == nullptr || label == nullptr) {
		return false;
	}
	pthread_mutex_lock(&compile_lock);
	for (unsigned i = 0; i < nwarm; i++) {
		warm *w = &warms[i];

		if (strcmp(w->label, label) != 0 || memcmp(&w->shape, shape, sizeof(*shape)) != 0) {
			continue;
		}
		if (w->state == 1) {
			*render = w->render;
			*info = w->info;
			taken = true;
			warm_used++;
		}
		w->state = 2;   /* taken, or the caller builds it now */
		break;
	}
	pthread_mutex_unlock(&compile_lock);
	return taken;
}
