/*
 * bt_test: Bluetooth as an application sees it: CoreBluetooth's central manager -- its state, then a scan for what advertises around
 * (names, RSSI, services) for SECONDS (default 30). The way from here to the radio is bluetoothd's: XPC to it, its HCI over
 * /dev/uart.bluetooth's Skywalk channel, and the MediaTek controller behind CONNSYS (bsd/dev/redwood_connsys.c, connsys_bt=2).
 *
 * usage: bt_test [SECONDS [SERVICE-UUID]]   (a service to scan for; without it every advertiser)
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <mach/mach.h>
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);

@interface Scanner : NSObject <CBCentralManagerDelegate>
@property (nonatomic) unsigned seen;
@property (nonatomic, copy) NSArray<CBUUID *> *services;
@end

@implementation Scanner
- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	printf("bt_test: central manager state %ld (5 is powered on)\n", (long)central.state);
	if (central.state == CBManagerStatePoweredOn) {
		[central scanForPeripheralsWithServices:self.services options:@{ CBCentralManagerScanOptionAllowDuplicatesKey : @NO }];
		printf("bt_test: scanning\n");
	}
	fflush(stdout);
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData RSSI:(NSNumber *)RSSI
{
	self.seen++;
	printf("bt_test: #%u %s name=%s rssi=%d adv=%s\n", self.seen, peripheral.identifier.UUIDString.UTF8String,
	    peripheral.name != nil ? peripheral.name.UTF8String : "-", RSSI.intValue,
	    [[advertisementData description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String);
	fflush(stdout);
}
@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		double seconds = argc > 1 ? atof(argv[1]) : 30;
		Scanner *scanner = [Scanner new];

		if (argc > 2) {
			scanner.services = @[ [CBUUID UUIDWithString:[NSString stringWithUTF8String:argv[2]]] ];
			printf("bt_test: for service %s\n", argv[2]);
		}
		CBCentralManager *manager = [[CBCentralManager alloc] initWithDelegate:scanner queue:dispatch_get_main_queue()];

		/* what MobileGestalt says (BluetoothManager asks "bluetooth" before it attaches: no, and Settings' switch stays grey) */
		{
			typedef bool (*bool_answer)(CFStringRef);
			bool_answer answer = (bool_answer)dlsym(RTLD_DEFAULT, "MGGetBoolAnswer");

			for (NSString *key in @[ @"bluetooth", @"BluetoothCapability", @"wifi", @"telephony" ]) {
				printf("bt_test: MobileGestalt %s = %d\n", key.UTF8String, answer != NULL ? (int)answer((__bridge CFStringRef)key) : -1);
			}
		}
		/* the services of bluetoothd, as launchd gives them out */
		for (NSString *service in @[ @"com.apple.bluetooth.xpc", @"com.apple.server.bluetooth.general.xpc", @"com.apple.server.bluetooth", @"com.apple.server.bluetooth.classic.xpc" ]) {
			mach_port_t port = MACH_PORT_NULL;
			kern_return_t kr = bootstrap_look_up(bootstrap_port, service.UTF8String, &port);

			printf("bt_test: %s: bootstrap_look_up %d\n", service.UTF8String, kr);
		}
		printf("bt_test: authorization %ld, state %ld\n", (long)CBManager.authorization, (long)manager.state);
		fflush(stdout);
		/* what Settings uses: BluetoothManager (the old BTSession way to bluetoothd, "bluetooth" asked of MobileGestalt first) */
		void *handle = dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_NOW);
		Class bmclass = NSClassFromString(@"BluetoothManager");

		printf("bt_test: dlopen %p (%s), class %p\n", handle, handle != NULL ? "ok" : dlerror(), (__bridge void *)bmclass);
		id bm = [bmclass performSelector:NSSelectorFromString(@"sharedInstance")];

		{
			/* why: -[BluetoothManager _attach] keeps the error of BTSessionAttach in a variable of its own (0x1e2242c60 of the cache, unslid) */
			IMP attach = class_getMethodImplementation(bmclass, NSSelectorFromString(@"_attach"));
			intptr_t slide = (intptr_t)attach - 0x1d3f11cf0;

			printf("bt_test: BluetoothManager attach error %d (slide %lx)\n", *(int *)(0x1e2242c60 + slide), (unsigned long)slide);
		}
		printf("bt_test: BluetoothManager %s\n", bm != nil ? "made" : "NOT made");
		fflush(stdout);
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:6]];
		if (bm != nil) {
			printf("bt_test: BluetoothManager available %d powered %d powerState %ld\n", (int)[bm performSelector:NSSelectorFromString(@"available")] & 1,
			    (int)[bm performSelector:NSSelectorFromString(@"powered")] & 1, (long)[bm performSelector:NSSelectorFromString(@"powerState")]);
			fflush(stdout);
		}
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
		printf("bt_test: done, %u devices seen\n", scanner.seen);
	}
	return 0;
}
