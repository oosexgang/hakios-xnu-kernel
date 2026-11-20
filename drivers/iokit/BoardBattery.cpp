/*
 * HakiosBattery: the phone's battery as Darwin looks for it, an IOPMPowerSource.
 *
 * powerd finds every service of the class IOPMPowerSource (it calls the first "InternalBattery-0"),
 * listens for its kIOPMMessageBatteryStatusHasChanged and then reads its properties: whether a
 * battery is installed, CurrentCapacity and MaxCapacity (without both it gives up on the battery),
 * whether a charger is connected and charging, voltage, current, cycles, the charger's details.
 * From them it makes the power sources list (IOPSCopyPowerSourcesInfo) and posts its notifications,
 * and everything else follows from that: the status bar's battery, the lock screen's "Charging",
 * the Batteries widget, Settings > Battery.
 *
 * What the values are comes from the audio DSP's battery manager (bsd/dev/board/adsp.c,
 * with adsp=1, unless battmgr=0), by way of bsd/dev/board/battery.c, which calls
 * HakiosBatteryReport() with every reading; the first one makes the service. Nothing here asks
 * the charger anything or tells it anything.
 *
 * As on Apple's newer batteries, CurrentCapacity is the percentage that the fuel gauge shows (and
 * MaxCapacity 100); the charges in mAh are AppleRawCurrentCapacity, AppleRawMaxCapacity (what the
 * gauge has learnt a full charge to be), NominalChargeCapacity and DesignCapacity.
 *
 * How long the charge lasts is said as a fuel gauge of Apple's says it, in minutes: TimeRemaining (-1 while it is
 * not known), and AvgTimeToEmpty and AvgTimeToFull, of which the one that does not apply, or is not known, is 65535.
 * The minutes are battery.c's (board/battery_core.h: the charge at the current averaged over a minute); with
 * battery_time=0 none of the three is there, as before. After the idea of the OnePlus Nord 2 port's piece 0701
 * (frxncyyy/ios-denniz), which found these the keys to say it by; nothing of its code is here.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>

/* The same as struct hakios_battery_state in bsd/sys/board/input.h, which is not seen from here. */
struct hakios_battery_state {
	uint32_t size;
	int32_t percent_hundredths;
	int32_t voltage_uv;
	int32_t current_ua;             /* more than 0 while the battery takes charge */
	int32_t temperature_hundredths;
	int32_t present, external, status;
	int32_t charge_uah, full_uah, design_uah, cycles;
	int32_t usb_type;
	int32_t usb_voltage_uv, usb_current_ua;
	int32_t minutes_to_empty, minutes_to_full;      /* -1 not known or not what is happening, -2 not to be said */
};

extern "C" void HakiosBatteryReport(const void * state, unsigned size);

class HakiosBattery : public IOPMPowerSource
{
	OSDeclareDefaultStructors(HakiosBattery);

public:
	void report(const struct hakios_battery_state * b);

private:
	void number(const char * key, long long value);
	void flag(const char * key, bool value);
	void object(const char * key, OSObject * value);
};

OSDefineMetaClassAndStructors(HakiosBattery, IOPMPowerSource);

static HakiosBattery * battery;

void
HakiosBattery::number(const char * key, long long value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);
	OSNumber * n = OSNumber::withNumber((unsigned long long) value, 32);

	if (symbol != NULL && n != NULL) {
		setPSProperty(symbol, n);
	}
	OSSafeReleaseNULL(n);
	OSSafeReleaseNULL(symbol);
}

void
HakiosBattery::flag(const char * key, bool value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);

	if (symbol != NULL) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
	}
	OSSafeReleaseNULL(symbol);
}

void
HakiosBattery::object(const char * key, OSObject * value)
{
	const OSSymbol * symbol = OSSymbol::withCString(key);

	if (symbol != NULL && value != NULL) {
		setPSProperty(symbol, value);
	}
	OSSafeReleaseNULL(symbol);
}

/*
 * What Darwin calls the charger by (IOPSKeysPrivate.h's kIOPSFamilyCode*, iokit_family_err(sub_iokit_usb, n)),
 * from Linux's POWER_SUPPLY_USB_TYPE_*, as the DSP gives it.
 */
static unsigned
family_code(int usb_type)
{
	switch (usb_type) {
	case 1: return 0xe0004000;      /* SDP, a computer's port: kIOPSFamilyCodeUSBHost */
	case 2: return 0xe0004004;      /* DCP, a charger: kIOPSFamilyCodeUSBChargingPortDedicated */
	case 3: return 0xe0004005;      /* CDP, a computer's charging port: kIOPSFamilyCodeUSBChargingPortDownstream */
	case 5: return 0xe0004009;      /* USB-C: kIOPSFamilyCodeUSBCTypeC */
	case 6: case 7: case 8: return 0xe000400a;      /* USB Power Delivery: kIOPSFamilyCodeUSBCPD */
	default: return 0xe0004007;     /* kIOPSFamilyCodeUSBUnknown */
	}
}

void
HakiosBattery::report(const struct hakios_battery_state * b)
{
	int percent = (b->percent_hundredths + 50) / 100;
	bool charging = b->external && b->status == 1;          /* POWER_SUPPLY_STATUS_CHARGING */

	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	setBatteryInstalled(b->present != 0);
	setExternalConnected(b->external != 0);
	setExternalChargeCapable(b->external != 0);
	setIsCharging(charging);
	flag("FullyCharged", b->external && (b->status == 4 || percent == 100));    /* POWER_SUPPLY_STATUS_FULL */
	flag("AppleRawExternalConnected", b->external != 0);
	setCurrentCapacity((unsigned) percent);
	setMaxCapacity(100);
	/* (the DSP's numbers are signed: one below zero, as a gauge that has not settled may give, is none for powerd, not a
	 * voltage of four billion millivolts) */
	setVoltage((unsigned) (b->voltage_uv > 0 ? b->voltage_uv / 1000 : 0));
	setAmperage(b->current_ua / 1000);
	number("InstantAmperage", b->current_ua / 1000);
	number("Temperature", b->temperature_hundredths);
	if (b->charge_uah > 0) {
		number("AppleRawCurrentCapacity", b->charge_uah / 1000);
	}
	if (b->full_uah > 0) {
		number("AppleRawMaxCapacity", b->full_uah / 1000);
		number("NominalChargeCapacity", b->full_uah / 1000);
	}
	if (b->design_uah > 0) {
		number("DesignCapacity", b->design_uah / 1000);
	}
	if (b->cycles > 0) {
		setCycleCount((unsigned) b->cycles);
	}
	if (b->minutes_to_empty >= -1 && b->minutes_to_full >= -1) {
		/* (battery.c knows at most one of the two at a time) */
		setTimeRemaining(b->minutes_to_empty >= 0 ? b->minutes_to_empty : b->minutes_to_full);
		number("AvgTimeToEmpty", b->minutes_to_empty >= 0 ? b->minutes_to_empty : 65535);
		number("AvgTimeToFull", b->minutes_to_full >= 0 ? b->minutes_to_full : 65535);
	}

	if (b->external) {
		OSDictionary * adapter = OSDictionary::withCapacity(6);
		/* (as the battery's voltage: below zero is none, not a number near 2^64 once cast) */
		long long mv = b->usb_voltage_uv > 0 ? b->usb_voltage_uv / 1000 : 0;
		long long ma = b->usb_current_ua > 0 ? b->usb_current_ua / 1000 : 0;
		OSNumber * n;

		if (adapter != NULL) {
			if ((n = OSNumber::withNumber((unsigned long long) ((mv * ma + 500000) / 1000000), 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsWattsKey, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) ma, 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) mv, 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsVoltage, n);
				n->release();
			}
			if ((n = OSNumber::withNumber((unsigned long long) family_code(b->usb_type), 32)) != NULL) {
				adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, n);
				n->release();
			}
			adapter->setObject("IsWireless", kOSBooleanFalse);
			object(kIOPMPSAdapterDetailsKey, adapter);
			adapter->release();
		}
	} else {
		OSDictionary * none = OSDictionary::withCapacity(1);
		OSNumber * n = OSNumber::withNumber(0ULL, 32);    /* kIOPSFamilyCodeDisconnected */

		if (none != NULL && n != NULL) {
			none->setObject(kIOPMPSAdapterDetailsFamilyKey, n);
			object(kIOPMPSAdapterDetailsKey, none);
		}
		OSSafeReleaseNULL(n);
		OSSafeReleaseNULL(none);
	}
	updateStatus();
}

/* With every reading of the battery (bsd/dev/board/battery.c); the first one makes the service. One caller at a time. */
void
HakiosBatteryReport(const void * state, unsigned size)
{
	const struct hakios_battery_state * b = (const struct hakios_battery_state *) state;
	IOService * platform;

	if (size != sizeof(struct hakios_battery_state) || b->size != sizeof(struct hakios_battery_state)) {
		static bool said;

		if (!said) {
			said = true;
			IOLog("HakiosBattery: the battery's state is %u bytes, not %lu: bsd/sys/board/input.h and this file disagree\n",
			    size, sizeof(struct hakios_battery_state));
		}
		return;
	}
	if (battery == NULL) {
		HakiosBattery * made = OSTypeAlloc(HakiosBattery);

		if ((platform = IOService::getPlatform()) == NULL || made == NULL || !made->init()) {
			OSSafeReleaseNULL(made);
			return;
		}
		made->report(b);        /* (the properties are there before anybody finds the service) */
		if (!made->attach(platform)) {
			made->release();
			return;
		}
		made->setName("HakiosBattery");
		if (!made->start(platform)) {
			made->detach(platform);
			made->release();
			return;
		}
		made->registerService();
		battery = made;
		IOLog("HakiosBattery: the battery's power source is up: %d%%, %s\n", (b->percent_hundredths + 50) / 100,
		    b->external ? (b->status == 1 ? "charging" : "on a charger, not charging") : "not on a charger");
		return;
	}
	battery->report(b);
}
