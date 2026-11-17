/*
 * The IORegistry service bluetoothd finds a controller by. bluetoothd (Darwin 27 as 16.7) looks for a service named
 * "marconi-bt", then "bluetooth" (IOServiceNameMatching), and leaves at once without one ("No bluetooth on this
 * device."). From it it reads:
 *   local-mac-address   the controller's address, 6 bytes, as written (FindLocalMacAddress)
 *   transport-encoding  4 bytes: 3 is "H4BC", the kernel control com.apple.uart.sk.bluetooth and a Skywalk channel
 *                       (bsd/dev/board/bt_host.c); without BT_HCI_TRANSPORT in its environment it goes by this
 *   transport-speed     4 bytes: the UART's speed it then asks for
 * and Settings' Bluetooth (BluetoothManager) attaches only once the service is there. The address is the one the
 * loader put into the device tree for MobileGestalt (/chosen mac-address-bluetooth0): the same in both places.
 *
 * As HakiOS's MediaTek port and the OnePlus Nord 2 port of iOS built on it found (frxncyyy/ios-denniz, piece 0722,
 * Apple Public Source License 2.0).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <sys/errno.h>

/* (declared for C in include/board/bt.h, which the IOKit side's include paths do not reach) */
extern "C" int board_bt_publish(void);
extern "C" void board_bt_unpublish(void);

class BoardBluetooth : public IOService
{
	OSDeclareDefaultStructors(BoardBluetooth);
};

OSDefineMetaClassAndStructors(BoardBluetooth, IOService);

static BoardBluetooth * published;

/* the address the loader gave the phone's Bluetooth: true when it is there and not all zeros */
static bool
loader_address(uint8_t address[6])
{
	IORegistryEntry * chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	OSData * data = chosen != NULL ? OSDynamicCast(OSData, chosen->getProperty("mac-address-bluetooth0")) : NULL;
	bool found = false;

	if (data != NULL && data->getLength() >= 6) {
		memcpy(address, data->getBytesNoCopy(), 6);
		found = (address[0] | address[1] | address[2] | address[3] | address[4] | address[5]) != 0;
	}
	OSSafeReleaseNULL(chosen);
	return found;
}

int
board_bt_publish(void)
{
	BoardBluetooth * made;
	IOService * platform;
	uint8_t address[6];
	uint32_t encoding = 3, speed = 3000000;

	if (published != NULL) {
		return 0;
	}
	if (!loader_address(address)) {
		IOLog("bt: no address in /chosen mac-address-bluetooth0: no service \"bluetooth\" (bluetoothd would leave)\n");
		return ENXIO;
	}
	made = OSTypeAlloc(BoardBluetooth);
	if ((platform = IOService::getPlatform()) == NULL || made == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		return ENOMEM;
	}
	made->setName("bluetooth");
	made->setProperty("local-mac-address", address, sizeof(address));
	made->setProperty("transport-encoding", &encoding, sizeof(encoding));
	made->setProperty("transport-speed", &speed, sizeof(speed));
	if (!made->attach(platform)) {
		made->release();
		return ENXIO;
	}
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return ENXIO;
	}
	made->registerService();
	published = made;
	IOLog("bt: service \"bluetooth\" registered (address %02x:%02x:%02x:%02x:%02x:%02x, transport H4BC)\n", address[0], address[1],
	    address[2], address[3], address[4], address[5]);
	return 0;
}

/* the service taken away (board_bt_start could not finish): bluetoothd is not to find a controller with nothing behind it */
void
board_bt_unpublish(void)
{
	if (published != NULL) {
		published->terminate(kIOServiceSynchronous);
		OSSafeReleaseNULL(published);
	}
}
