/*
 * poster_test [SECONDS]: what there is of the wallpaper, for a run nobody watches -- after SECONDS
 * (default 150), so that it comes at the end of the log that is kept: the processes that draw it
 * (PosterBoard, PaperBoard, the poster extensions) and the directories of /var where iOS keeps it,
 * with what is in them a few levels down. On iOS 16 the wallpaper is drawn by PosterBoard and its
 * extensions, and hosted into SpringBoard's layers; here the screen is black behind the icons.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

/* A line into the kernel's log (kern.redwood.say): a job's own output is kept nowhere. */
static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
}

static int
wanted(const char *name)
{
	static const char *words[] = { "poster", "paperboard", "wallpaper" };

	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		size_t n = strlen(words[i]);

		for (const char *at = name; *at != 0; at++) {
			if (strncasecmp(at, words[i], n) == 0) {
				return 1;
			}
		}
	}
	return 0;
}

/* A small property list, as CoreFoundation describes it, a line at a time. */
static void
show_plist(const char *path)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CFReadStreamRef stream = url ? CFReadStreamCreateWithFile(NULL, url) : NULL;
	CFPropertyListRef plist = NULL;
	char text[4096];

	if (stream != NULL && CFReadStreamOpen(stream)) {
		plist = CFPropertyListCreateWithStream(NULL, stream, 0, kCFPropertyListImmutable, NULL, NULL);
		CFReadStreamClose(stream);
	}
	if (plist != NULL) {
		CFStringRef description = CFCopyDescription(plist);

		if (description != NULL && CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8)) {
			for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
				say("poster_test:     %s\n", line);
			}
		}
		if (description != NULL) {
			CFRelease(description);
		}
		CFRelease(plist);
	}
	if (stream != NULL) {
		CFRelease(stream);
	}
	if (url != NULL) {
		CFRelease(url);
	}
}

/* Every entry below `path`, `depth` levels down; `all`: also those whose names do not say wallpaper. */
static void
walk(const char *path, int depth, int all, unsigned *printed)
{
	DIR *dir = opendir(path);
	struct dirent *entry;

	if (dir == NULL) {
		return;
	}
	while ((entry = readdir(dir)) != NULL && *printed < 200) {
		char child[1024];
		struct stat st;
		int match;

		/* (the gallery's posters: a hundred of them, and not what is on the screen) */
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, "descriptors") == 0 ||
		    strcmp(entry->d_name, "versions") == 0) {
			continue;
		}
		snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		if (lstat(child, &st) != 0) {
			continue;
		}
		match = all || wanted(entry->d_name);
		if (match) {
			say("poster_test: %s %s (%lld bytes)\n", S_ISDIR(st.st_mode) ? "dir " : "file", child, (long long)st.st_size);
			(*printed)++;
		}
		if (match && S_ISREG(st.st_mode) && st.st_size < 4096 && strstr(child, ".plist") != NULL) {
			show_plist(child);
		}
		if (S_ISDIR(st.st_mode) && depth > 0) {
			walk(child, depth - 1, match, printed);
		}
	}
	closedir(dir);
}

int
main(int argc, char **argv)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	struct kinfo_proc *procs;
	size_t size = 0;
	unsigned printed = 0;

	sleep(argc > 1 ? (unsigned)atoi(argv[1]) : 150);
	if (sysctl(mib, 4, NULL, &size, NULL, 0) == 0 && (procs = malloc(size)) != NULL &&
	    sysctl(mib, 4, procs, &size, NULL, 0) == 0) {
		unsigned count = (unsigned)(size / sizeof(*procs)), shown = 0;

		for (unsigned i = 0; i < count; i++) {
			if (wanted(procs[i].kp_proc.p_comm) || strcasestr(procs[i].kp_proc.p_comm, "SpringBoard") != NULL) {
				say("poster_test: process %d %s\n", procs[i].kp_proc.p_pid, procs[i].kp_proc.p_comm);
				shown++;
			}
		}
		say("poster_test: %u of %u processes draw or show the wallpaper\n", shown, count);
	}
	walk("/var/mobile/Library/Application Support/PRBPosterExtensionDataStore", 8, 1, &printed);
	walk("/var/mobile/Library/Preferences", 1, 0, &printed);
	walk("/var/mobile/Library/Logs/CrashReporter", 2, 1, &printed);
	walk("/var/mobile/Containers/Data", 6, 0, &printed);
	walk("/var/mobile/Containers/Shared", 5, 0, &printed);
	say("poster_test: %u entries\n", printed);
	return 0;
}
