/*
 * AHSFirmwareReadyNotifier: the IOKit service iOS's haptic server waits for, published when the
 * phone's motor plays what iOS plays (bsd/dev/board/haptic.c, haptic=5).
 *
 * On a phone whose haptics are "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics, the
 * device tree's /product/haptics: what makes iOS play its haptics as waveforms on the audio device
 * "Actuator", which is ours: components/audio/audio_plugin.c), audiomxd starts its haptic server --
 * the listener of com.apple.audio.hapticd, AudioToolbox's -[AVHapticServer init] -- only once the
 * haptics' VAD is routed to the actuator and an IOKit service of the class AHSFirmwareReadyNotifier
 * is there (HapticServerManager::Initialize, inside _AudioToolboxServerInit: an
 * IOServiceMatching("AHSFirmwareReadyNotifier") notification, then a check for one already
 * registered). Apple's haptics driver publishes it when the Taptic Engine's firmware is up; there is
 * none here, the wait never ended, and every haptic of iOS 27 went nowhere: CoreHaptics' engines
 * failed to connect (NSCocoaErrorDomain 4099), the system sounds' vibrations with '!itl' (the server
 * not made). Matching goes by class name, so it is a class of this name, with nothing in it.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>

class AHSFirmwareReadyNotifier : public IOService
{
	OSDeclareDefaultStructors(AHSFirmwareReadyNotifier);
};

OSDefineMetaClassAndStructors(AHSFirmwareReadyNotifier, IOService);

extern "C" void HakiosHapticsReady(void);
extern "C" void HakiosHapticsGone(void);

static AHSFirmwareReadyNotifier * notifier;

/* Once the motor plays for iOS: the service registered under the platform, until the motor is given up on. */
void
HakiosHapticsReady(void)
{
	AHSFirmwareReadyNotifier * made;
	IOService * platform;

	if (notifier != NULL) {
		return;
	}
	made = OSTypeAlloc(AHSFirmwareReadyNotifier);
	if ((platform = IOService::getPlatform()) == NULL || made == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		IOLog("HakiosHaptics: AHSFirmwareReadyNotifier cannot be made: iOS's haptic server will not start\n");
		return;
	}
	if (!made->attach(platform)) {
		made->release();
		return;
	}
	made->setName("AHSFirmwareReadyNotifier");
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return;
	}
	made->registerService();
	notifier = made;
	IOLog("HakiosHaptics: AHSFirmwareReadyNotifier registered: iOS's haptic server (com.apple.audio.hapticd) may start\n");
}

/* The motor given up on (a fault of the chip's): the service taken away, so that iOS's haptic server is not told of
 * firmware that is ready while nothing plays. */
void
HakiosHapticsGone(void)
{
	if (notifier != NULL) {
		notifier->terminate(kIOServiceSynchronous);
		OSSafeReleaseNULL(notifier);
		IOLog("HakiosHaptics: AHSFirmwareReadyNotifier taken away: the motor is given up on\n");
	}
}
