/*
 * HakiosAudio.driver: the output device of Darwin on this machine.
 *
 * Audio devices reach the audio server's hardware layer (the "HAL" of CoreAudio, inside
 * mediaserverd) in two ways: kernel drivers through IOAudio2 -- that is how an iPhone's own
 * speaker, codec and microphones arrive, and none of it exists here -- and plug-ins in
 * /System/Library/Audio/Plug-Ins/HAL, which is how Bluetooth, AirPlay, CarPlay and USB audio
 * arrive. This is such a plug-in. Its interface is the one macOS documents for user-space
 * audio drivers (AudioServerPlugIn: a CFPlugIn with one COM-style table of functions; the HAL
 * asks for objects and their properties, then runs the I/O); the Darwin SDK has no header for it,
 * so what is needed of it is declared below.
 *
 * Above the HAL sits the virtual audio device (/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin),
 * through which everything in Darwin is routed. It knows an iPhone's own devices by their names
 * (their UIDs) and makes a port of each: "Speaker" becomes the speaker port, the route an iPhone
 * with nothing plugged in plays through, with a volume, the volume display and the place in
 * Control Center that go with it. So this presents four devices under those names:
 *
 *   "Speaker"   a stereo output whose samples are written to /dev/hakios-audio, where the
 *               kernel takes them to whatever can play them (see sys/board/audio.h), and a
 *               stereo input that is always silent (on an iPhone: what the amplifiers measure)
 *   "Hawking"   one silent input and nothing else. The virtual audio device will not build the
 *               speaker route without a second virtual device of its own (its type 'vhaw', a
 *               mandatory companion of the default one in the iPhone X's routing tables) whose
 *               only port is this input; without it everything stays on the null port.
 *   "Actuator"  one output of one channel: the iPhone's Taptic Engine, on which Darwin plays its
 *               haptics as waveforms (through the chain 'd22_actuator_general'); its samples go to
 *               the kernel's motor driver, /dev/hakios-haptic (see actuator_write())
 *   "Codec"     one input of one channel: on an iPhone, the audio codec that the built-in
 *               microphones are on, of which the virtual audio device makes the built-in microphone
 *               port; its samples come from the kernel, /dev/hakios-mic (the phone's main
 *               microphone, boot-arg mic=4), which listens only while this input runs (see codec_read())
 *
 * CoreAudio's own null device ("Null_Device", described in the framework's Info.plist, playing
 * into nowhere) stays as it is: it is the only device the virtual audio device takes while it
 * starts, and ours are taken a moment later, when it says it allows all devices.
 *
 * What is played through the speaker port passes the iPhone X's chain of effects for its own
 * speakers first ('d22_speaker_general', from /Library/Audio/Tunings/D22: equalisers,
 * compressors, speaker protection), at the volume Darwin has set.
 *
 * The device has no clock but the system's: it claims to play exactly HAKIOS_AUDIO_RATE frames
 * a second of mach_absolute_time(), and the HAL's I/O thread paces itself by that.
 *
 * The speaker's output has a volume control, as the iPhone's own speaker has: what is set on it
 * goes to the kernel (ioctl HAKIOS_AUDIO_SET_VOLUME), which sets the amplifiers' level by it -- on
 * the phone, so far under the loudest it allows. Its scale is 48 dB in a straight line of
 * decibels, 3 dB for each of Darwin's sixteen steps; 0 is silence. What Darwin does with it (the phone,
 * 15 October): the virtual audio device sets it once, to its top (0 dB), when it takes the
 * device, and leaves it there -- its routing tables keep the iPhone X's speaker in software volume
 * mode, so the volume buttons and Control Center scale the samples themselves, and at 0 what is
 * written is silence (on which the kernel keeps the amplifiers off).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../userland/userland/src/board/audio.h"

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin', kAudioSelectorControlPropertyItemKind = 'clkk',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
};

/* ---- the devices ---------------------------------------------------------------------------------- */

/*
 * Object numbers are ours to choose. A device's streams follow it: its output (if it has one),
 * then its input.
 */
enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,          /* the speaker output's volume control */
	OBJECT_CODEC_SOURCE,            /* the codec input's data source control: its microphones */
	OBJECT_LAST = OBJECT_CODEC_SOURCE
};

#define VOLUME_RANGE_DB         48.0    /* from the loudest the kernel allows down to the quietest step above 0 */

struct device {
	AudioObjectID object;
	CFStringRef uid;                /* the name the virtual audio device knows it by */
	AudioObjectID output, input;    /* its streams; 0 = none */
	UInt32 output_channels, input_channels;
	/* its clock, running while anybody does I/O */
	unsigned running;               /* how many clients have started I/O */
	UInt64 anchor;                  /* mach_absolute_time() when the first of them did (read and written atomically) */
};

#define MIC_SOURCE              'imic'
/*
 * iOS 26/27 (HAKIOS_IOS27; the identity an iPhone 11, N104): VirtualAudio takes the device "Codec" for the built-in
 * microphone and makes its port of the product's four microphones (bottom, back, front, the second bottom one:
 * 'imic', 'smic', 'fmic', 'lmic', from its transducer database, not from us), whose chains take four channels
 * (Library/Audio/Tunings/N104/VAD: busChannelCounts 4, an AUChannelSelector). A route with more microphones than the
 * codec says it has input channels is refused ("could not get input device"), and the codec says so only through the
 * custom property 'MCCs' (MaxInputChannelCount; 2 when not said). So here the codec has four input channels, each the
 * same samples of the phone's one microphone, declares 'MCCs' with 4, and its data source control has the four and
 * takes any of them as the current ones (VirtualAudio sets all four at once: 16 bytes).
 */
#ifdef HAKIOS_IOS27
#define CODEC_CHANNELS          4
static const UInt32 mic_sources[] = { 'imic', 'smic', 'fmic', 'lmic' };
static const char *const mic_names[] = { "Bottom", "Back", "Front", "Bottom 2" };
#else
#define CODEC_CHANNELS          1
static const UInt32 mic_sources[] = { MIC_SOURCE };
static const char *const mic_names[] = { "Bottom" };
#endif
#define MIC_SOURCES             (sizeof(mic_sources) / sizeof(mic_sources[0]))
static UInt32 mic_selected[MIC_SOURCES] = { MIC_SOURCE };
static UInt32 mic_selected_count = 1;
#define CUSTOM_MAX_INPUT_CHANNELS 'MCCs'        /* the codec's custom property: { "MaxInputChannelCount": N } */
#define CUSTOM_TYPE_PROPERTY_LIST 'plst'        /* kAudioServerPlugInCustomPropertyDataTypeCFPropertyList */

static struct device devices[] = {
	{ .object = OBJECT_SPEAKER, .uid = CFSTR("Speaker"), .output = OBJECT_SPEAKER_OUTPUT, .input = OBJECT_SPEAKER_INPUT,
	  .output_channels = HAKIOS_AUDIO_CHANNELS, .input_channels = 2 },
	{ .object = OBJECT_HAWKING, .uid = CFSTR("Hawking"), .input = OBJECT_HAWKING_INPUT, .input_channels = 1 },
	{ .object = OBJECT_ACTUATOR, .uid = CFSTR("Actuator"), .output = OBJECT_ACTUATOR_OUTPUT, .output_channels = 1 },
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .input = OBJECT_CODEC_INPUT, .input_channels = CODEC_CHANNELS },
};
#define DEVICES                 (sizeof(devices) / sizeof(devices[0]))

#define TRANSPORT_BUILT_IN      'bltn'
/*
 * The codec's input has data sources, one for each microphone, and the virtual audio device will not
 * take a codec without them (VirtualAudio asks for kAudioDevicePropertyDataSources on its input, and
 * keeps those it knows: 'imic', 'smic', 'fmic', 'lmic', 'tmic', 'umic', 'imi1' to 'imi4'). A plug-in
 * has them as a data source control on the input, which the HAL answers those properties from. There
 * is one microphone here, at the bottom: 'imic', the iPhone's internal (bottom) one.
 */
#define MANUFACTURER            CFSTR("HakiOS")
/* how often a device says where its clock is, in frames: also the largest I/O buffer the HAL will use */
#define TIMESTAMP_PERIOD        16384

static AudioServerPlugInHostRef host;
/*
 * The properties' values. Held only while they are read or changed: the HAL asks for properties while the
 * I/O runs, and nothing slow -- a system call, a line to the log -- is done with it held. The I/O thread
 * never takes a lock (see get_zero_time_stamp()).
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* the starts and stops of the devices' I/O, one at a time, with what they open, close and say */
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
/* the volume's changes, one at a time, so that the kernel is given them in the order they were made */
static pthread_mutex_t volume_lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;                   /* /dev/hakios-audio */
static Float64 ticks_per_frame;
static UInt64 written, refused;         /* frames since the speaker's I/O started */
static Boolean inactive[OBJECT_LAST + 1];       /* streams the HAL has switched off */
static Float32 volume = 1.0f;           /* the speaker's volume control, 0 to 1 (Darwin sets it once it routes there) */

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

/*
 * The volume control's scale: a straight line of decibels over VOLUME_RANGE_DB. (What is not a number is silence:
 * NaN passes both tests of a clamp, and lround() makes 0 of it -- the loudest the kernel allows.)
 */
static Float32
scalar_to_db(Float32 scalar)
{
	scalar = !(scalar > 0) ? 0 : scalar > 1 ? 1 : scalar;
	return (Float32)(-VOLUME_RANGE_DB * (1.0 - scalar));
}

static Float32
db_to_scalar(Float32 db)
{
	Float32 scalar = (Float32)(1.0 + db / VOLUME_RANGE_DB);

	return !(scalar > 0) ? 0 : scalar > 1 ? 1 : scalar;
}

/* The volume, to the kernel. With volume_lock held. */
static void
volume_to_kernel(Float32 level)
{
	struct hakios_audio_volume v = { (uint32_t)lround(-scalar_to_db(level) * 8), level <= 0 };

	if (sink < 0 || ioctl(sink, HAKIOS_AUDIO_SET_VOLUME, &v) < 0) {
		say("hakios: audio: the volume %.3f could not be given to the kernel (%s)\n", level, strerror(errno));
		return;
	}
	say("hakios: audio: the volume is %.3f: %s%.1f dB under the loudest\n", level, v.muted ? "silence, " : "",
	    v.below / 8.0);
}

/* The device with this object number, or the one this stream belongs to; NULL = neither. */
static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object || (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

/* The one format of a stream: 16-bit samples at HAKIOS_AUDIO_RATE, so many channels. */
static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		       .mSampleRate = HAKIOS_AUDIO_RATE, .mFormatID = kAudioFormatLinearPCM,
		       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		       .mBytesPerPacket = 2 * channels, .mFramesPerPacket = 1, .mBytesPerFrame = 2 * channels,
		       .mChannelsPerFrame = channels, .mBitsPerChannel = 16,
	};
}

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;            /* one driver, never freed: no counting */
	return S_OK;
}

static ULONG
add_ref(__unused void *driver)
{
	return 1;
}

static ULONG
release(__unused void *driver)
{
	return 1;
}

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_frame = 1e9 * timebase.denom / timebase.numer / HAKIOS_AUDIO_RATE;
	sink = open(HAKIOS_AUDIO_DEVICE, O_WRONLY);
	say("hakios: audio: the speaker is with the HAL (%s: %s)\n", HAKIOS_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open");
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef driver, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object,
    __unused const AudioServerPlugInClientInfo *client)
{
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt64 action,
    __unused void *info)
{
	return 0;
}

/* A property's value: `size` bytes of it, or a list with nothing in it, or (neither) no such property. */
struct value {
	UInt32 size;
	bool empty;
	bool string;                    /* a CFString: whoever is given it gets a reference of their own */
	union {
		UInt32 words[4];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
	} u;
};

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	const struct device *d = device_of(object);
	struct value v;

	memset(&v, 0, sizeof(v));
#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)        (v.u.decimal = (x), v.size = sizeof(Float32))
	if (object == OBJECT_SPEAKER_VOLUME) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioLevelControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioVolumeControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_SPEAKER);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioLevelControlPropertyScalarValue:
			FLOAT(volume);
			break;
		case kAudioLevelControlPropertyDecibelValue:
			FLOAT(scalar_to_db(volume));
			break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -VOLUME_RANGE_DB;
			v.u.range.mMaximum = 0;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			FLOAT(0);       /* (converts what it is given: get_property_data()) */
			break;
		}
		return v;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioSelectorControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioDataSourceControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_CODEC);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeInput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioSelectorControlPropertyCurrentItem:
			for (UInt32 i = 0; i < mic_selected_count; i++) {
				LIST(mic_selected[i]);
			}
			break;
		case kAudioSelectorControlPropertyAvailableItems:
			for (UInt32 i = 0; i < MIC_SOURCES; i++) {
				LIST(mic_sources[i]);
			}
			break;
		case kAudioSelectorControlPropertyItemName:
			STRING(CFSTR(""));
			for (UInt32 i = 0; i < MIC_SOURCES && qualifier_size == sizeof(UInt32) && qualifier != NULL; i++) {
				if (*(const UInt32 *)qualifier == mic_sources[i]) {
					static CFStringRef names[MIC_SOURCES];

					if (names[i] == NULL) {
						names[i] = CFStringCreateWithCString(NULL, mic_names[i], kCFStringEncodingUTF8);
					}
					STRING(names[i]);
				}
			}
			break;
		}
		return v;
	}
	if (object != OBJECT_PLUGIN && d == NULL) {
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(kAudioObjectClassID);
		return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID : object == d->object ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner:
		WORD(object == OBJECT_PLUGIN ? 0 : object == d->object ? OBJECT_PLUGIN : d->object);
		return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			for (unsigned i = 0; i < DEVICES; i++) {
				LIST(devices[i].object);
			}
			break;
		case kAudioPlugInPropertyBoxList:
			v.empty = true;
			break;
		case kAudioPlugInPropertyTranslateUIDToBox:
			WORD(0);
			break;
		case kAudioPlugInPropertyTranslateUIDToDevice:
			WORD(0);
			for (unsigned i = 0; i < DEVICES; i++) {
				if (qualifier_size == sizeof(CFStringRef) && qualifier != NULL && *(const CFStringRef *)qualifier != NULL &&
				    CFEqual(*(const CFStringRef *)qualifier, devices[i].uid)) {
					WORD(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyResourceBundle:
			STRING(CFSTR(""));
			break;
		}
	} else if (object == d->object) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			STRING(d->uid);
			break;
		case kAudioDevicePropertyTransportType:
			WORD(TRANSPORT_BUILT_IN);
			break;
		case kAudioDevicePropertyRelatedDevices:
			WORD(d->object);
			break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
			WORD(0);
			break;
		case kAudioDevicePropertyDeviceIsAlive:
			WORD(1);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			WORD(d->output != 0);
			break;
		case kAudioDevicePropertyDeviceIsRunning:
			WORD(d->running != 0);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			/* the streams of one direction, or of both (and the speaker's volume control, which it owns) */
			if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
				LIST(d->output);
			}
			if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
				LIST(d->input);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeOutput) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList:
			if (d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (d->object == OBJECT_CODEC) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioDevicePropertyNominalSampleRate:
			v.u.number = HAKIOS_AUDIO_RATE;
			v.size = sizeof(Float64);
			break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			v.u.range.mMinimum = v.u.range.mMaximum = HAKIOS_AUDIO_RATE;
			v.size = sizeof(AudioValueRange);
			break;
#ifdef HAKIOS_IOS27
		case 'cust':                    /* kAudioObjectPropertyCustomPropertyInfoList: the codec's 'MCCs' */
			if (d->object == OBJECT_CODEC) {
				v.u.words[0] = CUSTOM_MAX_INPUT_CHANNELS;
				v.u.words[1] = CUSTOM_TYPE_PROPERTY_LIST;
				v.u.words[2] = 0;       /* no qualifier */
				v.size = 3 * sizeof(UInt32);
			} else {
				v.empty = true;
			}
			break;
		case CUSTOM_MAX_INPUT_CHANNELS:
			if (d->object == OBJECT_CODEC) {
				static CFDictionaryRef channels;

				if (channels == NULL) {
					int n = CODEC_CHANNELS;
					CFNumberRef count = CFNumberCreate(NULL, kCFNumberIntType, &n);
					const void *key = CFSTR("MaxInputChannelCount"), *value = count;

					channels = CFDictionaryCreate(NULL, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
					    &kCFTypeDictionaryValueCallBacks);
					CFRelease(count);
				}
				STRING((CFStringRef)channels);  /* (a property list given with a reference of its own, as a string is) */
			}
			break;
#endif
		case kAudioDevicePropertyPreferredChannelsForStereo:
			v.u.words[0] = 1;
			v.u.words[1] = 2;
			v.size = 2 * sizeof(UInt32);
			break;
		case kAudioDevicePropertyZeroTimeStampPeriod:
			WORD(TIMESTAMP_PERIOD);
			break;
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive:
			WORD(!inactive[object]);
			break;
		case kAudioStreamPropertyDirection:
			WORD(object == d->input);
			break;
		case kAudioStreamPropertyTerminalType:
			WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
			break;
		case kAudioStreamPropertyStartingChannel:
			WORD(1);
			break;
		case kAudioStreamPropertyLatency:
			WORD(0);
			break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_of(object);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			v.u.ranged.mFormat = format_of(object);
			v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = HAKIOS_AUDIO_RATE;
			v.size = sizeof(AudioStreamRangedDescription);
			break;
		}
	}
#undef WORD
#undef LIST
#undef STRING
#undef FLOAT
	return v;
}

/*
 * What the HAL (or the virtual audio device through it) asks of an object that it does not have,
 * the first times for each object, selector and scope: what a device it will not take may lack.
 */
#define UNKNOWN_TOLD            256
static UInt32 unknown_told[UNKNOWN_TOLD][3];
static unsigned unknown_count;
/*
 * (the HAL asks from several threads at once: two at the last free place both took it, the second one past
 * the end. Never taken by the I/O thread, which asks for no property)
 */
static pthread_mutex_t unknown_lock = PTHREAD_MUTEX_INITIALIZER;

static void
unknown(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 s = address->mSelector, c = address->mScope;
	bool told = false;

	pthread_mutex_lock(&unknown_lock);
	for (unsigned i = 0; i < unknown_count && !told; i++) {
		told = unknown_told[i][0] == object && unknown_told[i][1] == s && unknown_told[i][2] == c;
	}
	if (!told && unknown_count < UNKNOWN_TOLD) {
		unknown_told[unknown_count][0] = object;
		unknown_told[unknown_count][1] = s;
		unknown_told[unknown_count][2] = c;
		unknown_count++;
	} else {
		told = true;
	}
	pthread_mutex_unlock(&unknown_lock);
	if (told) {
		return;
	}
	say("hakios: audio: asked of object %u (%s) what it does not have: '%c%c%c%c' (%#x), scope '%c%c%c%c', element %u\n",
	    (unsigned)object, device_of(object) ? CFStringGetCStringPtr(device_of(object)->uid, kCFStringEncodingUTF8) : "-",
	    (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff), (int)(s >> 8 & 0xff), (int)(s & 0xff), (unsigned)s,
	    (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement);
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	if (v.size == 0 && !v.empty) {
		unknown(object, address);
	}
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !has_property(driver, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	/* the rate and the formats can be "set", to the ones there are; the volume can be set */
	if (object == OBJECT_SPEAKER_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_CODEC_SOURCE) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_PLUGIN) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	*size = property(object, address, qualifier_size, qualifier).size;
	return 0;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 room, UInt32 *size,
    void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(driver, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_SPEAKER_VOLUME && (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		/* what is in the buffer, converted where it is */
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		/* a list may be asked for in part; anything else must fit */
		if (address->mSelector != kAudioObjectPropertyOwnedObjects && address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier, UInt32 size,
    const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
		    !(wanted > 0) ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&volume_lock);
		pthread_mutex_lock(&lock);
		volume = wanted;
		pthread_mutex_unlock(&lock);
		volume_to_kernel(wanted);
		pthread_mutex_unlock(&volume_lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_SPEAKER_VOLUME, 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		/* one or more of the microphones listed (VirtualAudio sets all of a port's at once); all of them are the one there is */
		const UInt32 *items = data, count = size / sizeof(UInt32);
		AudioObjectPropertyAddress changed = { kAudioSelectorControlPropertyCurrentItem, kAudioObjectPropertyScopeGlobal, 0 };

		if (size == 0 || size % sizeof(UInt32) != 0 || count > MIC_SOURCES) {
			return kAudioHardwareBadPropertySizeError;
		}
		for (UInt32 i = 0; i < count; i++) {
			bool known = false;

			for (UInt32 j = 0; j < MIC_SOURCES; j++) {
				known = known || items[i] == mic_sources[j];
			}
			if (!known) {
				return kAudioHardwareIllegalOperationError;
			}
		}
		pthread_mutex_lock(&lock);
		memcpy(mic_selected, items, size);
		mic_selected_count = count;
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_CODEC_SOURCE, 1, &changed);
		}
		return 0;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == HAKIOS_AUDIO_RATE ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	/* a format: the one we have, or nothing */
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *wanted = data, format = format_of(object);

		return wanted->mSampleRate == format.mSampleRate && wanted->mFormatID == format.mFormatID &&
		       wanted->mChannelsPerFrame == format.mChannelsPerFrame && wanted->mBitsPerChannel == format.mBitsPerChannel &&
		       (wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger) ? 0 : kAudioDeviceUnsupportedFormatError;
	}
}

/*
 * The microphone (the device "Codec"): /dev/hakios-mic is open while the input runs, and the kernel
 * listens only while it is open. A read gives what has come and never waits; what it does not
 * give is silence. Said when it starts and stops, with how much came.
 */
static int mic = -1;
static UInt64 mic_frames, mic_heard, mic_short;
static UInt64 mic_first;                /* the frames asked for before the first samples came (said at the stop) */
static int mic_peak;

static void
codec_open(void)
{
	mic = open("/dev/hakios-mic", O_RDONLY | O_NONBLOCK);
	mic_frames = mic_heard = mic_short = mic_first = 0;
	mic_peak = 0;
	say("hakios: audio: the microphone is listened to (/dev/hakios-mic: %s)\n", mic < 0 ? strerror(errno) : "open");
}

static void
codec_close(void)
{
	say("hakios: audio: the microphone stops: %llu.%02llu s asked for, %llu.%02llu s of it from the kernel (peak %d of 32768), "
	    "%llu reads short\n", mic_frames / HAKIOS_AUDIO_RATE, mic_frames % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE,
	    mic_heard / HAKIOS_AUDIO_RATE, mic_heard % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE, mic_peak, mic_short);
	if (mic_heard > 0) {
		say("hakios: audio: the microphone's first samples were %llu.%02llu s after the input started\n",
		    mic_first / HAKIOS_AUDIO_RATE, mic_first % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE);
	}
	if (mic >= 0) {
		close(mic);
	}
	mic = -1;
}

static void
codec_read_mono(int16_t *samples, UInt32 frames)
{
	ssize_t got = mic >= 0 ? read(mic, samples, (size_t)frames * 2) : -1;
	UInt32 have = got > 0 ? (UInt32)got / 2 : 0;

	/* (said when the input stops: this is the I/O thread, which must not wait on the log) */
	if (have > 0 && mic_heard == 0) {
		mic_first = mic_frames;
	}
	for (UInt32 i = 0; i < have; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		mic_peak = v > mic_peak ? v : mic_peak;
	}
	if (have < frames) {
		memset(samples + have, 0, (size_t)(frames - have) * 2);
		mic_short += mic_heard > 0;
	}
	mic_frames += frames;
	mic_heard += have;
}

/* the codec's input: the microphone's samples in each of its CODEC_CHANNELS channels */
static void
codec_read(int16_t *samples, UInt32 frames)
{
#if CODEC_CHANNELS > 1
	static int16_t mono[4096];

	for (UInt32 done = 0; done < frames;) {
		UInt32 n = frames - done < 4096 ? frames - done : 4096;

		codec_read_mono(mono, n);
		for (UInt32 i = 0; i < n; i++) {
			for (UInt32 c = 0; c < CODEC_CHANNELS; c++) {
				samples[(done + i) * CODEC_CHANNELS + c] = mono[i];
			}
		}
		done += n;
	}
#else
	codec_read_mono(samples, frames);
#endif
}

static void haptic_open(void);
static void haptic_stopped(void);

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);
	bool first;

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&io_lock);
	pthread_mutex_lock(&lock);
	first = d->running++ == 0;
	if (first) {
		__atomic_store_n(&d->anchor, mach_absolute_time(), __ATOMIC_RELEASE);
		if (d->object == OBJECT_SPEAKER) {
			written = refused = 0;
		}
	}
	pthread_mutex_unlock(&lock);
	if (first) {
		/* (the speaker's: the actuator has an output too, and starting it is no reason to count the speaker's afresh) */
		if (d->object == OBJECT_SPEAKER) {
			say("hakios: audio: playing\n");
		}
		if (d->object == OBJECT_ACTUATOR) {
			haptic_open();
		}
		if (d->object == OBJECT_CODEC) {
			codec_open();
		}
	}
	pthread_mutex_unlock(&io_lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);
	bool last;

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&io_lock);
	pthread_mutex_lock(&lock);
	last = d->running != 0 && --d->running == 0;
	pthread_mutex_unlock(&lock);
	if (last) {
		if (d->object == OBJECT_CODEC) {
			codec_close();
		} else if (d->object == OBJECT_ACTUATOR) {
			haptic_stopped();
		} else if (d->output != 0) {
			say("hakios: audio: stopped after %llu.%02llu s (%llu frames not taken by %s)\n", written / HAKIOS_AUDIO_RATE,
			    written % HAKIOS_AUDIO_RATE * 100 / HAKIOS_AUDIO_RATE, refused, HAKIOS_AUDIO_DEVICE);
		}
	}
	pthread_mutex_unlock(&io_lock);
	return 0;
}

/*
 * Where the device's clock was last at a multiple of TIMESTAMP_PERIOD frames, and when. The I/O thread asks
 * every cycle, at real-time priority, and takes no lock here: the one it took was held elsewhere across an
 * ioctl, an open, a close and lines to the log, any of which could keep it waiting past its cycle. The whole
 * periods since the anchor are worked out from the anchor alone each time, as the count kept did.
 */
static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	Float64 ticks_per_period = ticks_per_frame * TIMESTAMP_PERIOD;
	UInt64 now = mach_absolute_time(), anchor, periods;
	struct device *d = device_of(object);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	anchor = __atomic_load_n(&d->anchor, __ATOMIC_ACQUIRE);
	periods = now > anchor ? (UInt64)((Float64)(now - anchor) / ticks_per_period) : 0;
	/* (the last period that began at or before now, its start rounded as the host time given is) */
	while (periods > 0 && now < anchor + (UInt64)((Float64)periods * ticks_per_period)) {
		periods--;
	}
	while (now >= anchor + (UInt64)((Float64)(periods + 1) * ticks_per_period)) {
		periods++;
	}
	*sample_time = (Float64)periods * TIMESTAMP_PERIOD;
	*host_time = anchor + (UInt64)((Float64)periods * ticks_per_period);
	*seed = 1;
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

/*
 * What Darwin plays on the actuator -- the haptics: the system's feedbacks and the ringer's vibration,
 * CoreHaptics' patterns, UIKit's feedbacks, as waveforms of one channel -- goes to the kernel's motor
 * driver (/dev/hakios-haptic, bsd/dev/board/haptic.c, haptic=5), which plays Android's own
 * waveforms on the phone's motor by it. (Darwin plays on this device at all only because the device
 * tree says the phone's haptics are "closed-loop": see mkidentity.py.) Said the first time, and when
 * the actuator stops how often and how loud.
 */
static int haptic = -1;          /* the motor driver; -2: not there (the kernel runs without haptic=5) */
static unsigned haptic_played;   /* I/O cycles with something to feel in them, since the actuator started */
static int haptic_peak;

/* (when the actuator starts, not on the I/O thread: an open() there could keep it past its deadline) */
static void
haptic_open(void)
{
	if (__atomic_load_n(&haptic, __ATOMIC_ACQUIRE) == -1) {
		int fd = open("/dev/hakios-haptic", O_WRONLY | O_NONBLOCK);

		__atomic_store_n(&haptic, fd >= 0 ? fd : -2, __ATOMIC_RELEASE);
	}
	haptic_played = 0;
	haptic_peak = 0;
}

static void
haptic_stopped(void)
{
	say("hakios: audio: the actuator stops: %u cycles played, peak %d of 32768%s\n", haptic_played, haptic_peak,
	    __atomic_load_n(&haptic, __ATOMIC_ACQUIRE) >= 0 ? "" : " (no /dev/hakios-haptic: not felt)");
}

static void
actuator_write(const int16_t *samples, UInt32 frames)
{
	static bool said;
	int peak = 0, fd = __atomic_load_n(&haptic, __ATOMIC_ACQUIRE);

	for (UInt32 i = 0; i < frames; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	if (fd >= 0) {
		(void)write(fd, samples, (size_t)frames * 2);
	}
	if (peak > 0) {
		haptic_played++;
		haptic_peak = peak > haptic_peak ? peak : haptic_peak;
	}
	/* (said once, the first time: the rest when the actuator stops, off this thread) */
	if (peak > 0 && !said) {
		said = true;
		say("hakios: audio: the actuator plays: %u frames, peak %d of 32768%s\n", (unsigned)frames, peak,
		    fd >= 0 ? "" : " (no /dev/hakios-haptic: not felt)");
	}
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle, void *buffer,
    __unused void *secondary)
{
	const struct device *d = device_of(stream);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_CODEC_INPUT) {
		codec_read(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationReadInput) {
		/* the speaker's and Hawking's inputs: silence */
		memset(buffer, 0, (size_t)frames * 2 * d->input_channels);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_ACTUATOR_OUTPUT) {
		actuator_write(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output) {
		/* the kernel takes it all or leaves it: this thread must not wait */
		if (sink < 0 || write(sink, buffer, (size_t)frames * HAKIOS_AUDIO_FRAME) < 0) {
			refused += frames;
		}
		written += frames;
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	configuration_change, configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
void *
HakiosAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver : NULL;
}
