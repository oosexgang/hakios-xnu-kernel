/*
 * libhakios-sb.dylib: code of ours inside SpringBoard (see inject.c for how it gets there).
 *
 *   - The phone is "activated". SpringBoard asks mobileactivationd, which is not running (and
 *     could not talk to Apple anyway); without an answer it treats the device as bricked and
 *     shows the first-boot setup ("hello") instead of the lock screen.
 *   - The setup assistant is never needed, whatever the reason: nobody is going to go through
 *     it here.
 *   - SpringBoard starts a few seconds late where the kernel's boot-args say so (sb_delay=N;
 *     "xl run" does for the phone). /private/var is empty at every boot, so the applications
 *     are registered anew each time, one after another; one that is registered after
 *     SpringBoard has laid out the home screen ends up on a later page, and which one that is
 *     differs from boot to boot. On the phone that happens; under QEMU, where everything takes
 *     five times as long, it does not -- and there the delay is harmful: for a reason not yet
 *     found the window server then never takes our touch screen service for its own.
 *     (A wrapper program in the launchd job would be the obvious way to wait; launchd panics
 *     the system when a system job runs anything that is not Apple's.)
 *   - The picture SpringBoard keeps of an application that leaves the screen (for the application
 *     switcher and the next launch) is saved as JPEG or PNG, not as an ASTC texture: see
 *     snapshot_output_format().
 *   - Where CoreAnimation draws with its software renderer (QEMU; the phone without gpu=1) the
 *     "vibrant" views draw their content as it is: see plain_vibrancy().
 *   - A press of the side button that is long over is not taken for a long press (Siri) because
 *     SpringBoard was busy waking the display: see lock_long_press().
 *   - A long press on the lock screen's wallpaper does not open the poster switcher, which the
 *     window server here shows as a black screen (16.5; iOS 27 opens it): see poster_switcher_allowed().
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.hakios.unlock      what the lock screen does after "swipe up to open"
 *       com.hakios.layers      write down the layers SpringBoard has on the screen: see layers()
 *       com.hakios.switcher    open the switcher of running applications: see switcher()
 *     In any other program given this library (INJECT in build.sh) only the last of them is
 *     answered, with that program's layers.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* where what is said goes: the standard error, which is the console's for SpringBoard (an application's is nowhere) */
static int say_fd = 2;

static void
say(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(say_fd, line, (size_t)n);
}

/*
 * Boot-arg touch_lat=1: a line for every turn of SpringBoard's main run loop -- when it woke up (a
 * touch, a timer, the display link ...), when CoreAnimation's commit of what that turn changed
 * began and when it was done (our observers just before and after its own, of order 2000000), in
 * microseconds of mach_absolute_time, as the other "tl:" lines (tools/latency.py, tools/frames.py):
 *   tl: a WOKE COMMIT DONE
 * (a commit with nothing in it takes some microseconds; one that sends changes, a good deal more)
 */
#define CA_COMMIT_ORDER 2000000
static uint64_t main_woke, commit_began;
static mach_timebase_info_data_t timebase;

static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		main_woke = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
		commit_began = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
		say("tl: a %llu %llu %llu\n", main_woke, commit_began, now_us());
		main_woke = 0;
	}
}

static void
watch_main_loop(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *arg;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "touch_lat=1")) == NULL ||
	    (arg != args && arg[-1] != ' ')) {
		return;
	}
	mach_timebase_info(&timebase);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	{
		CFRunLoopObserverContext after = { .info = (void *)1 };

		CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
		    CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
	}
}

/* ---- methods of SpringBoard's classes, replaced --------------------------------------------------- */

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/*
 * Replace -[CLASS SELECTOR] (or +[CLASS SELECTOR]: `kind` is that first character), whose code must now
 * be at the build's verified unslid address `expected` (what a replacement does depends on what the
 * original did). The original goes into *original_out (when that is not NULL) before the replacement is
 * set, because another thread can call the replacement at once; returns whether it was replaced.
 * (dyld interposing of the C functions underneath does not reach calls made from inside the
 * shared cache: there one library branches straight into the other.)
 */
static bool
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement,
    IMP *original_out)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
	    class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return false;
	}
	if (original_out != NULL) {
		*original_out = original;
	}
	method_setImplementation(method, replacement);
	return true;
}

static bool
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement, IMP *original_out)
{
	return replace_method_of_kind('-', class_name, selector, expected, replacement, original_out);
}

/* UIKitCore 24B119 (iOS 27.2); the values below are unslid IMPs from its dyld shared cache. */
#if defined(HAKIOS_IOS27)
#define UIDeviceBatteryLevelIMP             0x1855c6becUL
#define UIDeviceBatteryStateIMP             0x184c6c448UL
#define UIBatteryViewShowsPercentageIMP     0x184dd3838UL
#define UIBatteryViewSetShowsPercentageIMP  0x1847a8908UL
#define UIBatteryViewShouldShowBoltIMP      0x184d8deacUL
#define UIBatteryViewChargePercentIMP       0x184a90bbcUL
#define UIBatteryViewSetChargePercentIMP    0x184e58184UL
#else
#define UIDeviceBatteryLevelIMP             0x1897021b0UL
#define UIDeviceBatteryStateIMP             0x189209794UL
#define UIBatteryViewShowsPercentageIMP     0x189a9370cUL
#define UIBatteryViewSetShowsPercentageIMP  0x18926e150UL
#define UIBatteryViewShouldShowBoltIMP      0x189203e28UL
#define UIBatteryViewChargePercentIMP       0x189a935d8UL
#define UIBatteryViewSetChargePercentIMP    0x18928d310UL
#endif

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * int at +8: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no answer.
 */
#define LOCKDOWN_STATE(manager)         (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED              2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}

/* -[SBSetupManager _setSetupRequiredReason:]: 0 = no need for the setup assistant */
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("hakios: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] chooses the file format of the picture
 * SpringBoard takes of an application that leaves the screen: 0 = PNG, 1 and 2 = ATX (an ASTC texture),
 * 3 = JPEG. A device with a wide colour display that can decode ASTC gets ATX, and Darwin compresses it on
 * the processor: fast with the vector code of Apple's cores, but here it takes one core more than ten
 * seconds per picture (under QEMU; seconds on the phone) -- and all that time the application waits for
 * the answer, until the watchdog for scene updates (10 s, there is no setting to scale it) kills it:
 * no application survived going to the background. JPEG takes a fraction of a second; it has no
 * transparency, so a picture that needs it becomes a PNG (which is what Darwin itself does on a device
 * without ASTC).
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/* ---- battery state from the Qualcomm charger stack --------------------------------------------------- */

/*
 * Darwin reads battery level through UIDevice.batteryLevel and UIDevice.batteryState; SpringBoard's
 * status bar reads it the same way via SBBatteryController. Where the battery has no power source
 * of its own in the kernel (battery=1, the loader's way through Qualcomm's UEFI charger protocols),
 * we hook the UIDevice accessors to return what XNU publishes as kern.redwood.battery_soc (0..100)
 * and kern.redwood.battery_charging.
 * When the sysctl is missing (battery=1 not in boot-args, or the driver never started) we hand
 * back the value the real implementation computed; the status bar then shows whatever Darwin last
 * knew, which is better than crashing.
 * With adsp=1 the battery is a power source (the service HakiosBattery, an IOPMPowerSource:
 * iokit/Kernel/BoardBattery.cpp), which powerd reads like an iPhone's own, and from it Darwin knows
 * everything by itself; once that service is there, the hooks hand everything to Darwin's own code.
 */
static float  (*original_battery_level)(id self, SEL sel);
static long   (*original_battery_state)(id self, SEL sel);
static BOOL   (*original_battery_view_shows_percentage)(id self, SEL sel);
static void   (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL   (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void   (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);

static int
sysctl_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

/* Is the battery a power source that Darwin reads by itself (see above)? Once it is, it stays one. */
static bool
battery_power_source(void)
{
	static bool found;
	io_service_t service;

	if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("HakiosBattery"))) != 0) {
		IOObjectRelease(service);
		found = true;
	}
	return found;
}

/* UIDevice.batteryLevel: 0.0..1.0; -1.0 if monitoring not enabled */
static float
battery_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_level ? original_battery_level(self, sel) : -1.0f;
	}
	return (float)soc / 100.0f;
}

/* UIDevice.batteryState: 0=unknown 1=unplugged 2=charging 3=full */
static long
battery_state(id self, SEL sel)
{
	int charging = sysctl_int("kern.redwood.battery_charging");
	int present = sysctl_int("kern.redwood.battery_present");
	int plugged = sysctl_int("kern.redwood.battery_charger_present");
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_state ? original_battery_state(self, sel) : 0;
	}
	if (!present) return 0;
	if (plugged && soc >= 100) return 3;
	return charging ? 2 : 1;
}

/*
 * UIKitCore's status-bar view has its own presentation policy, separate from UIDevice. With the power
 * source it is Darwin's own again: the switch Battery Percentage in Settings > Battery, which the image
 * turns on (SBShowBatteryPercentage in userland/var/mobile/Library/Preferences/com.apple.springboard.plist).
 */
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
	return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}

static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}

static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
	int charging = sysctl_int("kern.redwood.battery_charging");
	int present = sysctl_int("kern.redwood.battery_present");
	int plugged = sysctl_int("kern.redwood.battery_charger_present");

	if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
	return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}

/*
 * The actual number the status bar draws. _UIBatteryView keeps the fill fraction (0.0..1.0) in
 * chargePercent; it is set through setChargePercent: and read back for layout. Darwin feeds it from
 * BatteryCenter / IOPowerSources, which is empty on this phone, so without us it sits near zero
 * (the "1%" the user saw). We override both sides: setChargePercent: so the stored value is ours
 * no matter who writes it, and the getter so any layout path that reads it agrees.
 */
static double
battery_view_charge_percent(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}

static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}

/* The big charging ring on the lock screen (CoverSheet) reads its fill level the same way. */
static double
ring_charging_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}

/* Keep SpringBoard's normal battery observers awake while the XNU cache changes. */
static void
battery_post_notifications(void)
{
	Class uidev = objc_getClass("UIDevice");
	Class notification_center = objc_getClass("NSNotificationCenter");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
	id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
	    sel_registerName("defaultCenter")) : nil;

	if (center != nil && !battery_power_source()) {
		SEL post = sel_registerName("postNotificationName:object:");
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		battery_post_notifications();
	});
}

/* ---- the lock screen's clock without a GPU ------------------------------------------------------- */

#define HAKIOS_GL "/hakios/lib/libhakios-gl.dylib"

/* Does the window server draw with OpenGL ES? The same question, asked the same way, as opengl_es() in inject.c. */
static bool
gpu_draws(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;

	if (access(HAKIOS_GL, R_OK) != 0) {
		return false;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				return argument[3] != '0';
			}
		}
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("HakiosGPU"));
	if (gpu != 0) {
		IOObjectRelease(gpu);
	}
	return gpu != 0;
}

/*
 * The clock of the lock screen is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are not drawn, they are a mask -- a copy of them through a colour matrix that keeps red
 * as alpha -- through which the filter vibrantColorMatrix lets the wallpaper behind come out
 * lighter; and a gradient over the whole screen, red at the top, tones the wallpaper down the
 * same way. On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's
 * software renderer shows neither: the digits come out red, and the top of the wallpaper as it
 * was. Why was not found out; that renderer is in QuartzCore, not here.
 * Such views have a switch, the user default BSUIVibrancyEffectViewDisabled: with it they show
 * their content as it is drawn, the clock in plain white. It is set for this run of SpringBoard
 * alone (nothing is written), and only where the software renderer draws.
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	say("hakios: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/* ---- the side button's long press ---------------------------------------------------------------- */

/*
 * -[SBLockHardwareButton longPress:] is what the side button's long press recognizer calls: Siri
 * (here, its "turn on Siri" sheet, nobody having turned it on). The recognizer is a timer of
 * SpringBoard's own: started when the main thread sees the press, stopped when it sees the release
 * -- and a release it has not got round to is no release. A press of the side button with the
 * display off wakes the display from inside the handling of that very press, which on an iPhone
 * takes no time. Here it takes as long as the software renderer needs for the first picture of the
 * lock screen: a tenth of a second on the phone, a second under QEMU. That is longer than a long
 * press, so there every wake by the button (and by a tap, which is a press of that button made in
 * inject.c) ended with Siri's sheet over the lock screen, however short the press had been.
 * Where the button's events come from -- inject.c, in backboardd -- it is known whether the button
 * is still held; a long press of a button that is not is dropped here.
 */
#define GESTURE_BEGAN                   1       /* UIGestureRecognizerState */
#define GESTURE_ENDED                   3       /* and cancelled, failed after it */

static void (*original_lock_long_press)(id self, SEL selector, id recognizer);

static bool
lock_button_held(void)
{
	static int token = NOTIFY_TOKEN_INVALID;
	uint64_t state = 0;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.hakios.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return true;
	}
	/* 1 = released; held, or not known: as SpringBoard has it */
	return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}

static void
lock_long_press(id self, SEL selector, id recognizer)
{
	static bool dropping;
	SEL state_selector = sel_registerName("state");
	long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
	    ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;

	if (state == GESTURE_BEGAN) {
		dropping = !lock_button_held();
		if (dropping) {
			say("hakios: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
		}
	}
	if (dropping) {
		if (state >= GESTURE_ENDED) {
			dropping = false;
		}
		return;
	}
	original_lock_long_press(self, selector, recognizer);
}

/* ---- remote control ------------------------------------------------------------------------------ */

#define UNLOCK_SOURCE_COVER_SHEET       24      /* what the lock screen itself passes */

static void
unlock(void)
{
	Class cls = objc_getClass("SBLockScreenManager");
	SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
	SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
	id manager;

	if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
	    class_getInstanceMethod(cls, unlock_ui) == NULL) {
		say("hakios: unlock: SBLockScreenManager is not what it was\n");
		return;
	}
	manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
	if (manager == NULL) {
		say("hakios: unlock: no lock screen manager yet\n");
	} else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
		say("hakios: unlock: not locked\n");
	} else {
		BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);

		say("hakios: unlock: asked, answer %d\n", done);
	}
}

/*
 * The poster switcher: on Darwin 16 a long press on the lock screen's wallpaper shrinks it into a
 * row of posters to choose from or customize. The row is PosterBoard's, an application scene of
 * that process which the lock screen hosts (CSPosterSwitcherViewController, a CALayerHost of the
 * context of PosterBoard's window over an opaque black layer). PosterBoard does lay it out -- its
 * window has the posters, "Customize" and "+", the hosted context is that window's, the scene is
 * in the foreground -- but the window server draws nothing of that context, with either
 * renderer, so what is left is the black layer under it, until a swipe up takes the switcher
 * away. Why is not known: the display is not in secure mode, SpringBoard counts the user as
 * authenticated, the same nesting of hosted contexts works for the wallpaper itself.
 * Until it is, the switcher is not offered, the way SpringBoard itself withholds it where it has
 * none (an iPad, Guided Access): its policy says the capability is not there, and the long press
 * does nothing.
 *
 * iOS 27 (HAKIOS_POSTER_SWITCHER=1, darwin/userland/inj26.sh): offered as iOS offers it. There the long
 * press restarted SpringBoard ("process was nil" in -[SBApplicationSceneUpdateTransaction
 * _notifyObserversOfClientConnection:]) as long as PosterBoard ran as a system daemon, a process
 * FrontBoard did not know; it is a launch angel again (darwin/tools/mkimage26.py), started in the user's
 * domain as on an iPhone, and the switcher opens: the row of posters with "Customize" and "+", and a
 * swipe up takes it away. Boot-arg poster_switcher=0 withholds it, poster_switcher=1 offers it.
 */
#ifndef HAKIOS_POSTER_SWITCHER
#define HAKIOS_POSTER_SWITCHER  0
#endif
static BOOL (*original_poster_switcher_allowed)(id self, SEL selector, id *explanation);

static BOOL
poster_switcher_allowed(id self, SEL selector, id *explanation)
{
	static int wanted = -1;         /* offered as iOS offers it: HAKIOS_POSTER_SWITCHER, or boot-arg poster_switcher=0/1 */

	if (wanted < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at = NULL;

		wanted = HAKIOS_POSTER_SWITCHER;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
			for (at = strstr(args, "poster_switcher="); at != NULL && at != args && at[-1] != ' ';
			    at = strstr(at + 1, "poster_switcher=")) {
			}
		}
		if (at != NULL && (at[16] == '0' || at[16] == '1')) {
			wanted = at[16] == '1';
		}
	}
	if (wanted && original_poster_switcher_allowed != NULL) {
		return original_poster_switcher_allowed(self, selector, explanation);
	}
	if (explanation != NULL) {
		*explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
	}
	return NO;
}

/*
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

static const char *layers_of = "";               /* in another program than SpringBoard: " of" and its name */

static const char *
text_of(id object)
{
	id text = object ? SEND(id, object, "description") : NULL;

	return text ? SEND(const char *, text, "UTF8String") : "-";
}

/*
 * The switcher of running applications lays a wash of wallpaper over every card behind the front
 * one, above the dimming: -[SBAppSwitcherPageView _createOrDestroyWallpaperOverlayView] makes it,
 * PaperBoardUI fills it from the image provider of the wallpaper, and the result is drawn at about
 * fifteen per cent (the layer tree: SBWallpaperEffectView at opacity 0.1497 -> PBUIFakeBlurView ->
 * PBUIWallpaperView -> PBUISnapshotReplicaView -> a UIImageView).
 *
 * Here there is no picture of the wallpaper to put in it. The wallpaper itself is drawn live by
 * PosterBoard, in another process, and hosted into SpringBoard's layer tree; SpringBoard has no
 * bitmap of it, and nothing ever bakes one -- after three and a half minutes of an idle phone
 * /var/mobile/Library holds sixty directories and not one of them is PaperBoard, PosterBoard or
 * Wallpaper. So the provider hands over a placeholder: 846 x 1827 pixels of one colour,
 * (179, 77, 179), which CoreAnimation compresses to ASTC and draws over the two cards on the left
 * at 14.9 per cent. That is what made them purple instead of merely dimmed.
 *
 * The dimming itself is right and stays: two draws of a flat black at 11.4 per cent, which is the
 * shadow one expects to see. Only the wash is left out, because a wash of a colour that is not the
 * wallpaper's is worse than no wash at all. When something here can give PaperBoardUI a real
 * picture of the wallpaper, this hook is what should go -- and until then switcher_wallpaper=1
 * among the boot-args puts it back as Darwin asks for it, so that whoever works on the wallpaper can
 * see at once whether their picture arrives, without rebuilding an image to find out.
 */
static void (*original_switcher_wallpaper_overlay)(id self, SEL selector);

static void
switcher_wallpaper_overlay(id self, SEL selector)
{
	static int wanted = -1;         /* boot-arg switcher_wallpaper=1: have it back, placeholder and all */
	static unsigned told;

	if (wanted < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *at;

		wanted = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
		    (at = strstr(args, "switcher_wallpaper=1")) != NULL && (at == args || at[-1] == ' ');
	}
	if (told++ == 0) {
		say("hakios: the switcher's wash of wallpaper is %s\n", wanted ?
		    "drawn as Darwin asks (switcher_wallpaper=1)" : "left out: there is no picture of the wallpaper for it");
	}
	if (wanted) {
		original_switcher_wallpaper_overlay(self, selector);
	}
}

/*
 * SIRI'S ORB, which is where SpringBoard dies on this port.
 *
 * -[SpringBoard applicationDidFinishLaunching:] builds the banner manager, which builds
 * SBAssistantController, which builds Siri's presentation controller, whose -prewarmOrbViewShaders
 * makes a SUICOrbView whose -_updateMetalTextureRasterSize: asks Metal for something Metal will not
 * give: _MTLReportFailure, then abort. 48 times in the boot of 21 October, every 8.3 seconds, with
 * backboardd alive throughout for the first time.
 *
 * The message Metal reported is in none of our logs -- SpringBoard's own os_log output never
 * reaches them, where backboardd's did -- so this stands one frame above the fall and says what the
 * process has to work with: whether MTLCreateSystemDefaultDevice() answers in SpringBoard at all,
 * and with which class. That one line separates two very different diseases: our device never
 * reaching this process, and our device reaching it and refusing something.
 *
 * With siri_orb=0 among the boot-args the prewarm is skipped. That fixes nothing and is not meant
 * to: it asks what SpringBoard does NEXT, which is the only way to see the wall behind this one
 * while this one still stands.
 */
static void (*original_prewarm_orb_view_shaders)(id self, SEL selector);

static bool
boot_arg_is_set(const char *word)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;

	return sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 &&
	    (at = strstr(args, word)) != NULL && (at == args || at[-1] == ' ');
}

static void
prewarm_orb_view_shaders(id self, SEL selector)
{
	static int skip = -1;
	id (*create)(void) = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
	id device = create ? create() : nil;

	if (skip < 0) {
		skip = boot_arg_is_set("siri_orb=0");
	}
	if (create == NULL) {
		say("hakios: Siri's orb: MTLCreateSystemDefaultDevice is not even a symbol here\n");
	} else {
		say("hakios: Siri's orb prewarms its shaders; MTLCreateSystemDefaultDevice() = %s\n",
		    device ? object_getClassName(device) : "nil");
	}
	if (skip) {
		say("hakios: Siri's orb: the prewarm is skipped (siri_orb=0), so what follows is the next wall\n");
		return;
	}
	original_prewarm_orb_view_shaders(self, selector);
	say("hakios: Siri's orb: the prewarm RETURNED -- it did not abort this time\n");
}

/*
 * SiriUIActivation arrives only when SpringBoard first wants Siri, which is after everything else
 * this library replaces at startup. So this waits for the class rather than for a named image: the
 * notifier runs once per image already loaded and once per image afterwards, and the first time the
 * class is there the replacement goes in.
 */
static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	static bool tried;      /* (once: an address that did not match the first time will not match on a later image) */

	if (tried || objc_getClass("SiriPresentationViewController") == NULL) {
		return;
	}
	tried = true;
	replace_method("SiriPresentationViewController", "prewarmOrbViewShaders", 0x1f0144c30UL, (IMP)prewarm_orb_view_shaders,
	    (IMP *)&original_prewarm_orb_view_shaders);
	say("hakios: Siri's orb: the prewarm is %s\n",
	    original_prewarm_orb_view_shaders ? "watched" : "NOT watched (the address moved)");
}

static void
layer_filter(id filter, const char *what, int depth)
{
	static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
		                              "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
	char line[1024];
	size_t n;

	if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
		say("hakios: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));     /* a name alone */
		return;
	}
	n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
	    text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
		id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
		id value = SEND1(id, filter, "valueForKey:", id, key);
		float m[20];

		if (value == NULL) {
			continue;
		}
		if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
			SEND1(void, value, "getValue:", void *, m);
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
			    "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
		} else {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
		}
	}
	say("hakios: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}

/* (what snprintf says it would have written, held to the buffer: else the next size left wraps round) */
#define HELD_MORE(n) ((n) = (n) >= sizeof(more) ? sizeof(more) - 1 : (n))
/*
 * How deep layer_line() goes: each level is a frame of more than a kilobyte and a half (more, flat) on the main
 * thread's stack, which is 1 MB; a tree (or a chain of masks) deeper than this is said to be, not walked.
 */
#define LAYERS_DEPTH_MAX 128

static void
layer_line(id layer, const char *role, int depth)
{
	struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
	const char *class_name = object_getClassName(layer);
	id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
	id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
	id background = SEND(id, layer, "backgroundFilters");
	void *colour = SEND(void *, layer, "backgroundColor");
	char more[1024] = "";
	size_t n = 0;

	if (SEND(BOOL, layer, "isHidden")) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN"); HELD_MORE(n);
	}
	if (contents != NULL) {
		id text = (id)CFCopyDescription((CFTypeRef)contents);
		const char *from = text_of(text);
		char flat[300];
		size_t k = 0;

		/*
		 * What a CGImage says about itself runs over several lines -- its colour space, how big
		 * it is, how its pixels are laid out -- and all of that is worth having on the one line,
		 * because the size and the colour space are the first things to look at when a picture
		 * comes out wrong. Every run of spaces and newlines becomes one space.
		 */
		while (from != NULL && *from != 0 && k + 1 < sizeof(flat)) {
			if (*from == ' ' || *from == '\n' || *from == '\t' || *from == '\r') {
				if (k > 0 && flat[k - 1] != ' ') {
					flat[k++] = ' ';
				}
				from++;
				continue;
			}
			flat[k++] = *from++;
		}
		flat[k] = 0;
		n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %s", flat); HELD_MORE(n);
		CFRelease((CFTypeRef)text);
	}
	if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
	    SEND(void *, layer, "contentsMultiplyColor") != NULL) {
		id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?"); HELD_MORE(n);
		CFRelease((CFTypeRef)text);
	}
	if (colour != NULL) {
		id text = (id)CFCopyDescription(colour);
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?"); HELD_MORE(n);
		CFRelease((CFTypeRef)text);
	}
	if (strcmp(class_name, "CAPortalLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
		    SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : ""); HELD_MORE(n);
	} else if (strcmp(class_name, "CABackdropLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
		    SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : ""); HELD_MORE(n);
	} else if (strcmp(class_name, "CAGradientLayer") == 0) {
		struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
		id colours = SEND(id, layer, "colors");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
		    from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations"))); HELD_MORE(n);
		for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
			id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
			const char *numbers = strstr(text_of(text), "( ");

			n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?"); HELD_MORE(n);
			CFRelease((CFTypeRef)text);
		}
	} else if (strcmp(class_name, "CAShapeLayer") == 0) {
		void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
		id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
		const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;

		n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
		    "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd")); HELD_MORE(n);
		if (text_stroke != NULL) {
			CFRelease((CFTypeRef)text_stroke);
		}
		if (text_fill != NULL) {
			CFRelease((CFTypeRef)text_fill);
		}
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
		    SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
		    SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : ""); HELD_MORE(n);
	}
	{
		/* the view a layer belongs to, which is the only thing that names a layer UIKit left
		   unnamed -- and the switcher's wash of wallpaper is such a layer */
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " of %s", object_getClassName(delegate)); HELD_MORE(n);
		}
	}
	{
		id keys = SEND(id, layer, "animationKeys");

		if (SEND(BOOL, layer, "shouldRasterize")) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED"); HELD_MORE(n);
		}
		for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
			    text_of(SEND1(id, keys, "objectAtIndex:", long, i))); HELD_MORE(n);
		}
	}
	say("hakios: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
	    text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
	for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
		layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
	}
	for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
		layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
	}
	if (compositing != NULL) {
		layer_filter(compositing, "compositing filter", depth);
	}
	if (depth >= LAYERS_DEPTH_MAX && (mask != NULL || (sublayers != NULL && SEND(long, sublayers, "count") > 0))) {
		say("hakios: layers%s: %*s  (its mask and sublayers left out: deeper than %d)\n", layers_of, 2 * depth, "",
		    LAYERS_DEPTH_MAX);
		return;
	}
	if (mask != NULL) {
		layer_line(mask, "MASK ", depth + 1);
	}
	for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
		layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
	}
}

/* Every WKWebView under a view: whether WebKit takes it for one in the background, its web process and whether that answers. */
static void
web_views(id view, int depth)
{
	Class web = objc_getClass("WKWebView");
	id subviews;
	bool is_web = false;

	if (view == NULL || depth > 40) {
		return;
	}

	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
		SEL responsive = sel_registerName("_webProcessIsResponsive");

		say("hakios: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
		    layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
		    (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
		    SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
		    (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
		    SEND(BOOL, view, "isHidden") ? "YES" : "no");
	}
	subviews = SEND(id, view, "subviews");
	for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
		web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
	}
}

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
	id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id authentication = application && class_respondsToSelector(object_getClass(application),
	    sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;

	say("hakios: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
	if (authentication != NULL) {
		/* (each asked only where the controller has it: Darwin 27's has no inSecureDisplayMode, and asking it ended
		 * SpringBoard, an unrecognized selector, at every com.hakios.layers) */
		Class controller = object_getClass(authentication);
		int authenticated = class_respondsToSelector(controller, sel_registerName("isAuthenticated")) ?
		    SEND(BOOL, authentication, "isAuthenticated") : -1;
		int passcode = class_respondsToSelector(controller, sel_registerName("hasPasscodeSet")) ?
		    SEND(BOOL, authentication, "hasPasscodeSet") : -1;
		int secure = class_respondsToSelector(controller, sel_registerName("inSecureDisplayMode")) ?
		    SEND(BOOL, authentication, "inSecureDisplayMode") : -1;

		say("hakios: layers: the user is %s, %s passcode, the display %s\n",
		    authenticated < 0 ? "?" : authenticated ? "authenticated" : "NOT authenticated",
		    passcode < 0 ? "? a" : passcode ? "a" : "no", secure < 0 ? "? in secure mode" : secure ? "in secure mode" : "not in secure mode");
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);
		id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
		id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;

		say("hakios: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
		    layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
		    context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
		    scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	if (application != NULL) {
		say("hakios: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
		    SEND(long, application, "applicationState"));
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
	}
	say("hakios: layers%s: end\n", layers_of);
}

/*
 * The switcher of running applications, on the notification com.hakios.switcher (rwtouch notify).
 * A drag up from the bottom edge is the way to it on a phone, but the touches a script puts in do
 * not reach Darwin's system gestures -- nothing comes of such a drag, not even going home -- and two
 * presses of the home button only go home (there is no home button on a phone of this shape, and
 * what is sent as one goes home and no more). SpringBoard opens it itself for anything that is
 * not a gesture (a hardware keyboard, accessibility): SBMainSwitcherControllerCoordinator, which
 * is one for the whole program, takes "activate the main switcher not interactively, from this
 * source". The source is an analytics label; zero is "none", which is what the ones that have no
 * gesture behind them pass.
 * As it stands this is not enough: with three applications running it answers that it took the
 * request (source 0, animated and not animated alike) and the screen does not change -- the
 * application in front stays, and SpringBoard says nothing more. So the switcher still cannot be
 * reached from a script; what is left to try is the transition with a window scene
 * (_activateSwitcherValidatorWithEventLabel:animated:windowScene:), which is what its own
 * gestures use.
 */
static void
switcher(void)
{
	Class coordinator = objc_getClass("SBMainSwitcherControllerCoordinator");
	SEL activate = sel_registerName("activateMainSwitcherNoninteractivelyWithSource:animated:");
	/* (any program can post the notification: what this build's SpringBoard has not got is not asked of it) */
	id shared = coordinator != NULL && class_getClassMethod(coordinator, sel_registerName("sharedInstance")) != NULL ?
	    SEND(id, coordinator, "sharedInstance") : NULL;
	Method method = shared != NULL ? class_getInstanceMethod(object_getClass(shared), activate) : NULL;
	char source_type[2] = "", animated_type[2] = "";

	if (method == NULL) {
		say("hakios: the switcher: %s has no %s\n",
		    coordinator != NULL ? "SBMainSwitcherControllerCoordinator" : "nothing", sel_getName(activate));
		return;
	}
	/* the source is passed as an integer and animated as a BOOL: a method that takes anything else is left alone */
	method_getArgumentType(method, 2, source_type, sizeof(source_type));
	method_getArgumentType(method, 3, animated_type, sizeof(animated_type));
	if (method_getNumberOfArguments(method) != 4 || source_type[0] == 0 || strchr("qQlLiI", source_type[0]) == NULL ||
	    (animated_type[0] != 'B' && animated_type[0] != 'c')) {
		say("hakios: the switcher: %s takes %s here; left alone\n", sel_getName(activate),
		    method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
		return;
	}
	say("hakios: the switcher: opening it (%s, %s)\n", object_getClassName(shared),
	    method_getTypeEncoding(method) != NULL ? method_getTypeEncoding(method) : "?");
	/*
	 * It answers whether it took the request. The source is an analytics label and not every one
	 * of them is allowed from every state, so they are tried in turn until one is taken.
	 */
	/*
	 * Not animated: the animated one is taken and then nothing comes of it -- the transition it
	 * starts is the gesture's to carry, and there is no gesture here.
	 */
	for (long source = 0; source < 10; source++) {
		BOOL taken = ((BOOL (*)(id, SEL, long, BOOL))objc_msgSend)(shared, activate, source, NO);

		say("hakios: the switcher: from source %ld: %s\n", source, taken ? "taken" : "refused");
		if (taken) {
			return;
		}
	}
}

/*
 * Control Center's glyphs whole from its first frames. Its modules and their parts fade in as it
 * comes down: opacity animations, mostly springs that take up to a second to settle at 1. While a
 * layer's opacity is below 1, the window server's OpenGL ES renderer draws the vibrant glyphs in it
 * from a flattened copy that lacks the strokes composited against what is behind them, and draws them
 * whole only at full opacity: the strokes of the media controls, orientation lock, screen mirroring
 * and Focus came in only when the springs had settled, at the end of the presentation. (On an iPhone
 * they are whole all along.) So in Control Center's window an opacity animation is replaced with an
 * eased-out fade of cc_fade=MS between the same values: the modules still fade in and out, and are at
 * full opacity, glyphs whole, after a few frames. Off by default (Darwin's own animations stay as they
 * are; the real fix belongs in the drawing, not in the animations): a workaround, for checks only.
 */
#define CC_FADE_MS      0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;

static bool
in_control_center(id layer)
{
	for (int depth = 0; layer != NULL && depth < 64; depth++) {
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
			return true;
		}
		layer = SEND(id, layer, "superlayer");
	}
	return false;
}

static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
	Class basic = objc_getClass("CABasicAnimation");
	id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
	    SEND(id, animation, "keyPath") : NULL;

	if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
	    class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
		id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
		id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));

		SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
		SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
		SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
		SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
		SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
		SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
		SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
		SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
		SEND1(void, fade, "setDuration:", double, cc_fade_s);
		SEND1(void, fade, "setTimingFunction:", id, ease);
		if (cc_fades++ < 3) {
			say("hakios: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
			    SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
		}
		animation = fade;
	}
	original_add_animation_cc(self, selector, animation, key);
}

static void
control_center_fades(const char *arguments)
{
	const char *setting = strstr(arguments, "cc_fade=");
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (setting != NULL) {
		cc_fade_s = atof(setting + 8) / 1000.0;
	}
	if (cc_fade_s <= 0 || add == NULL) {
		return;
	}
	original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_cc);
}

/*
 * Open the tweak loader of the package manager's prefix, when there is one. Nothing here depends on
 * it: without a prefix (or without ElleKit in it) the file is not there and this does nothing.
 * Whether its tweaks are opened is the loader's to say: not with the boot-arg notweaks=1, nor in a
 * SpringBoard started after one that did not get far past opening them (components/tweaks/inject_tweaks.c).
 */
#define HAKIOS_TWEAK_LOADER    "/hakios/lib/libhakios-tweaks.dylib"

static void
load_tweaks(void)
{
	struct stat st;

	if (stat("/var/jb/usr/lib/TweakInject", &st) != 0) {
		return;                 /* no package manager's prefix here, so no tweaks */
	}
	if (dlopen(HAKIOS_TWEAK_LOADER, RTLD_NOW | RTLD_GLOBAL) == NULL) {
		say("hakios: the tweak loader %s: %s\n", HAKIOS_TWEAK_LOADER, dlerror());
	} else {
		say("hakios: tweaks: %s opened\n", HAKIOS_TWEAK_LOADER);
	}
}

/*
 * The frame rates SpringBoard's display links ask for. After a page of the home screen settles, something of
 * SpringBoard's goes on drawing at exactly 30 a second for most of a second (its run loop and the window server's
 * frames 33.3 ms apart, tl: a and tl: g). It is UIKit's: the display link of UIViewInProcessAnimationManager, which
 * runs the animations done in the application's own process, asks for 20..30. With dl_max=1 (the default) the rates
 * asked of that one are dropped and it runs at the panel's rate; on an iPhone with ProMotion the lower rate saves
 * power, here the panel refreshes at 120 whatever is drawn. The other display links keep what they ask (the clock's
 * 10 and 60, the motion effects' 32): run at 120 they woke SpringBoard six times as often for nothing.
 * With touch_lat=1 every display link made and every rate asked of one is said ("hakios: display link ...").
 */
typedef struct {
	float minimum, maximum, preferred;
} frame_rate_range;

static id (*original_display_link_with)(id self, SEL selector, id target, SEL action);
static void (*original_set_fps)(id self, SEL selector, long fps);
static void (*original_set_range)(id self, SEL selector, frame_rate_range range);
static bool dl_say, dl_max = true;
static unsigned dl_said;
#define DL_SAY_MAX 300
#define DL_FULL_FOR "UIViewInProcessAnimationManager"
static char dl_full;                    /* the key of the mark a display link made for DL_FULL_FOR carries */

/* (a mark on the link itself: UIKit makes one of these for every animation, and a list of them filled up at once) */
static bool
full_rate(id link)
{
	return dl_max && objc_getAssociatedObject(link, &dl_full) != NULL;
}

static const char *
caller(const void *address, char *buffer, size_t size)
{
	Dl_info info;
	const char *image;

	if (dladdr(address, &info) == 0 || info.dli_fname == NULL) {
		snprintf(buffer, size, "%p", address);
		return buffer;
	}
	image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
	snprintf(buffer, size, "%s+%#lx", image, (unsigned long)((uintptr_t)address - (uintptr_t)info.dli_fbase));
	return buffer;
}

static id
display_link_with(id self, SEL selector, id target, SEL action)
{
	id link = original_display_link_with(self, selector, target, action);
	char who[256];

	if (link != NULL && target != NULL && strcmp(object_getClassName(target), DL_FULL_FOR) == 0) {
		objc_setAssociatedObject(link, &dl_full, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
	}
	if (dl_say && dl_said++ < DL_SAY_MAX) {
		say("hakios: display link %p made for -[%s %s] by %s\n", (void *)link, target ? object_getClassName(target) : "nil",
		    action ? sel_getName(action) : "", caller(__builtin_return_address(0), who, sizeof(who)));
	}
	return link;
}

static void
set_fps(id self, SEL selector, long fps)
{
	char who[256];

	if (dl_say && dl_said++ < DL_SAY_MAX) {
		say("hakios: display link %p asked for %ld a second%s by %s\n", (void *)self, fps, full_rate(self) ? " (dropped)" : "",
		    caller(__builtin_return_address(0), who, sizeof(who)));
	}
	original_set_fps(self, selector, full_rate(self) ? 0 : fps);
}

static void
set_range(id self, SEL selector, frame_rate_range range)
{
	char who[256];

	if (dl_say && dl_said++ < DL_SAY_MAX) {
		say("hakios: display link %p asked for %.0f..%.0f, %.0f a second%s by %s\n", (void *)self, range.minimum,
		    range.maximum, range.preferred, full_rate(self) ? " (dropped)" : "", caller(__builtin_return_address(0), who, sizeof(who)));
	}
	if (full_rate(self)) {
		range = (frame_rate_range){ 0, 0, 0 };
	}
	original_set_range(self, selector, range);
}

static bool
boot_arg(const char *arguments, const char *name)
{
	const char *at = strstr(arguments, name);

	return at != NULL && (at == arguments || at[-1] == ' ');
}

/*
 * The animations SpringBoard hands the window server (an application opening from its icon and closing to it, the
 * home screen coming back, folders): the window server moves them at the rate each asks for (CAAnimation's
 * preferredFrameRateRange). Through Metal (the plug-in) it keeps to that, and what asks for nothing went at 24 a
 * second -- frames exactly 41.7 ms apart with the GPU 0.3 ms busy each; through OpenGL ES it did not. With anim_max=1
 * every animation added to a layer of SpringBoard's asks for 80..120 (a copy of it: one already added is read-only,
 * and setting it raised). touch_lat=1 says the first ones and what they asked.
 */
static void (*original_add_animation)(id self, SEL selector, id animation, id key);
static unsigned anim_said;

static void
add_animation_full_rate(id self, SEL selector, id animation, id key)
{
	SEL range_of = sel_registerName("preferredFrameRateRange"), set_range_of = sel_registerName("setPreferredFrameRateRange:");

	if (animation != NULL && class_respondsToSelector(object_getClass(animation), set_range_of)) {
		frame_rate_range asked = ((frame_rate_range (*)(id, SEL))objc_msgSend)(animation, range_of);

		if (dl_say && anim_said++ < 40) {
			say("hakios: animation %s (%s) asked for %.0f..%.0f, %.0f a second\n", object_getClassName(animation),
			    key != NULL ? text_of(key) : "", asked.minimum, asked.maximum, asked.preferred);
		}
		if (asked.maximum == 0 || asked.maximum < 120 || asked.minimum < 80) {
			id copy = SEND(id, animation, "copy");

			((void (*)(id, SEL, frame_rate_range))objc_msgSend)(copy, set_range_of, (frame_rate_range){ 80, 120, 120 });
			original_add_animation(self, selector, copy, key);
			SEND(void, copy, "release");
			return;
		}
	}
	original_add_animation(self, selector, animation, key);
}

static void
animation_rates(const char *arguments)
{
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (!boot_arg(arguments, "anim_max=1") || add == NULL) {
		return;
	}
	original_add_animation = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_full_rate);
	say("hakios: SpringBoard's animations ask for 80..120 a second (anim_max=1)\n");
}

/* two type encodings alike but for the offsets and sizes in them, which are the compiler's to choose */
static bool
same_types(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
	for (;;) {
		while (*a >= '0' && *a <= '9') a++;
		while (*b >= '0' && *b <= '9') b++;
		if (*a != *b) {
			return false;
		}
		if (*a == 0) {
			return true;
		}
		a++, b++;
	}
}

static void
display_link_rates(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	Class link = objc_getClass("CADisplayLink");
	Method made, fps, range;

	if (link == NULL || sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	dl_say = boot_arg(arguments, "touch_lat=1");
	dl_max = !boot_arg(arguments, "dl_max=0");
	animation_rates(arguments);
	if (!dl_say && !dl_max) {
		return;
	}
	/* (in each, the original is kept before the replacement is set: another thread can call it at once) */
	if ((made = class_getClassMethod(link, sel_registerName("displayLinkWithTarget:selector:"))) != NULL) {
		original_display_link_with = (id (*)(id, SEL, id, SEL))method_getImplementation(made);
		method_setImplementation(made, (IMP)display_link_with);
	}
	if ((fps = class_getInstanceMethod(link, sel_registerName("setPreferredFramesPerSecond:"))) != NULL) {
		original_set_fps = (void (*)(id, SEL, long))method_getImplementation(fps);
		method_setImplementation(fps, (IMP)set_fps);
	}
	/* set_range takes the range as three floats by value: with any other argument it would pass on garbage */
	if ((range = class_getInstanceMethod(link, sel_registerName("setPreferredFrameRateRange:"))) != NULL &&
	    !same_types(method_getTypeEncoding(range), "v28@0:8{CAFrameRateRange=fff}16")) {
		say("hakios: display links: -setPreferredFrameRateRange: takes %s here, not a CAFrameRateRange; left alone\n",
		    method_getTypeEncoding(range) != NULL ? method_getTypeEncoding(range) : "?");
		range = NULL;
	}
	if (range != NULL) {
		original_set_range = (void (*)(id, SEL, frame_rate_range))method_getImplementation(range);
		method_setImplementation(range, (IMP)set_range);
	}
	say("hakios: display links: made %s, fps %s, range %s%s\n", made ? "ok" : "NO", fps ? "ok" : "NO", range ? "ok" : "NO",
	    dl_max ? "; " DL_FULL_FOR "'s at the panel's rate (dl_max=1)" : "");
}

__attribute__((constructor))
static void
hakios_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	bool activation, snapshot_format, poster_switcher;
	IMP battery_lvl, battery_st;
	int token;

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "SpringBoard") != 0) {
		static char who[64];

		snprintf(who, sizeof(who), " of %s", name);
		layers_of = who;
		if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
			say_fd = 2;
		}
		say("hakios: in %s[%d]: its layers are said on the notification com.hakios.layers\n", name, getpid());
		notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
			layers();
		});
		return;
	}
	/*
	 * The tweaks of the package manager's prefix, if one is installed: ElleKit's loader
	 * (/var/jb/usr/lib/TweakLoader.dylib) reads /var/jb/usr/lib/TweakInject and opens the tweaks
	 * whose Filter names this process. The kernel could insert it as well (etc/inject.conf), but
	 * only one inserted library per program is reliable here, and SpringBoard's is this one; so
	 * this one opens it, the way a jailbreak's own hook does.
	 */
	load_tweaks();
	/* (the rates SpringBoard's display links and animations ask the window server for: dl_max, anim_max) */
	display_link_rates();
	/*
	 * Everything the Metal driver says, it says with printf, and SpringBoard's standard output
	 * goes nowhere -- so inside this process every refusal our driver prints has been invisible.
	 * That is not a small blindness: the boot of 22 October showed SpringBoard dying in Siri's
	 * orb with MTLCreateSystemDefaultDevice() answering HakiosMetalDevice, which means our
	 * device was there and refused something, and the sentence naming what was written to a
	 * file descriptor that is closed. Pointing stdout at stderr, which IS the console here, is
	 * the whole fix and it costs nothing: the driver keeps its printf, and we get to read it.
	 */
	if (dup2(say_fd, 1) < 0) {
		say("hakios: stdout could not be pointed at the console; the driver's own lines stay lost\n");
	}
	/*
	 * And unbuffered, which is the half of this that is easy to leave out and would waste the
	 * boot it is meant to serve: stdio chose its buffering when stdout was still going nowhere,
	 * so it is fully buffered, and the process we are trying to hear from ABORTS -- the buffer
	 * would go with it, holding exactly the sentence we wanted.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state,
	    NULL);
	_dyld_register_func_for_add_image(image_added);         /* Siri's orb, whose framework comes late */
	replace_method("SBSetupManager", "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason,
	    (IMP *)&original_set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format, NULL);
	plain_vibrancy();
	replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL, (IMP)lock_long_press,
	    (IMP *)&original_lock_long_press);
	poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
	    0x1c507e0fcUL, (IMP)poster_switcher_allowed, (IMP *)&original_poster_switcher_allowed);
	replace_method("SBAppSwitcherPageView", "_createOrDestroyWallpaperOverlayView", 0x1c4af7860UL,
	    (IMP)switcher_wallpaper_overlay, (IMP *)&original_switcher_wallpaper_overlay);
	/*
	 * Battery hooks on UIDevice: these are the methods every app and SpringBoard itself calls, so
	 * hooking them here in the SpringBoard process covers both the status bar and any app that
	 * reads [UIDevice currentDevice].batteryLevel. The addresses are for Darwin 16.5 (20F66).
	 */
	replace_method("UIDevice", "batteryLevel", UIDeviceBatteryLevelIMP, (IMP)battery_level, (IMP *)&original_battery_level);
	replace_method("UIDevice", "batteryState", UIDeviceBatteryStateIMP, (IMP)battery_state, (IMP *)&original_battery_state);
	replace_method("_UIBatteryView", "showsPercentage", UIBatteryViewShowsPercentageIMP, (IMP)battery_view_shows_percentage,
	    (IMP *)&original_battery_view_shows_percentage);
	replace_method("_UIBatteryView", "setShowsPercentage:", UIBatteryViewSetShowsPercentageIMP, (IMP)battery_view_set_shows_percentage,
	    (IMP *)&original_battery_view_set_shows_percentage);
	replace_method("_UIBatteryView", "_shouldShowBolt", UIBatteryViewShouldShowBoltIMP, (IMP)battery_view_should_show_bolt,
	    (IMP *)&original_battery_view_should_show_bolt);
	replace_method("_UIBatteryView", "chargePercent", UIBatteryViewChargePercentIMP, (IMP)battery_view_charge_percent,
	    (IMP *)&original_battery_view_charge_percent);
	replace_method("_UIBatteryView", "setChargePercent:", UIBatteryViewSetChargePercentIMP, (IMP)battery_view_set_charge_percent,
	    (IMP *)&original_battery_view_set_charge_percent);
	replace_method("CSBatteryChargingRingView", "batteryChargingLevel", 0x1c48c2cf4UL, (IMP)ring_charging_level,
	    (IMP *)&original_ring_charging_level);
	battery_lvl = (IMP)original_battery_level;
	battery_st  = (IMP)original_battery_state;
	say("hakios: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
	    "battery level %s, battery state %s, poster switcher %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
	    battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
	    poster_switcher ? "not offered" : "NOT withheld");
	say("hakios: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
	    original_battery_view_shows_percentage ? "ok" : "NO",
	    original_battery_view_set_shows_percentage ? "ok" : "NO",
	    original_battery_view_should_show_bolt ? "ok" : "NO",
	    original_battery_view_charge_percent ? "ok" : "NO",
	    original_battery_view_set_charge_percent ? "ok" : "NO",
	    original_ring_charging_level ? "ok" : "NO");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
	/*
	 * UIDevice won't call batteryLevel/batteryState unless monitoring is enabled. Flip it on once
	 * SpringBoard's run loop is running (the dispatch_after fires on the main queue, which means
	 * we are past UIApplication startup).
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class uidev = objc_getClass("UIDevice");
		SEL current = sel_registerName("currentDevice");
		SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
		id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;

		if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
		}
		watch_main_loop();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
				control_center_fades(arguments);
			}
		}
		say("hakios: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
	    sysctl_int("kern.redwood.battery_soc"), sysctl_int("kern.redwood.battery_present"),
	    sysctl_int("kern.redwood.battery_charger_present"), sysctl_int("kern.redwood.battery_charging"));
		battery_post_notifications();
	});
	notify_register_dispatch("com.hakios.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
		unlock();
	});
	notify_register_dispatch("com.hakios.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
		layers();
	});
	notify_register_dispatch("com.hakios.switcher", &token, dispatch_get_main_queue(), ^(__unused int t) {
		switcher();
	});
	/*
	 * Belt-and-suspenders unlock. The user can always swipe up to open the home screen -- that
	 * works fine when the touch driver is doing its job. But if touch is still probing, has
	 * failed, or the panel is a FocalTech and the firmware upload is still chewing through
	 * ~114 KB of SPI (that takes a hot second during bring-up), the user ends up staring at a
	 * dark lock screen with nothing to swipe. So we just forcibly dismiss it after 10 seconds
	 * so Darwin always lands on the home screen even in the worst case. Harmless if touch is
	 * already working: the user just doesn't need to swipe themselves.
	 * With autounlock=0 among the kernel's arguments the lock screen stays until it is swiped away
	 * (to show the phone starting the way an iPhone does).
	 */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *arg;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "autounlock=0")) != NULL &&
		    (arg == args || arg[-1] == ' ')) {
			say("hakios: the lock screen stays until it is swiped away (autounlock=0)\n");
			return;
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlock();
	});
}
