/*
 * bt_test: Bluetooth as an application sees it, through CoreBluetooth (bluetoothd's own client side).
 *
 * usage: bt_test [SECONDS [SERVICE-UUID...]]
 *        bt_test pair NAME SECONDS SERVICE-UUID...
 *   Waits for the central manager's state (powered on, or why not), scans for LE devices for SECONDS (default 30),
 *   all of them or, with SERVICE-UUIDs, only those that advertise one of these services,
 *   says each device the first time it is seen (its name if it advertises one, its RSSI, how many advertising
 *   packets) and, at the end, how many devices were seen. Lines start with "bt_test:".
 *
 * With "pair", NAME with "_" for each space (init's job lines have no quoting), the scan stops at the first device that advertises exactly NAME (and no other is touched), connects to
 * it, lists its services and characteristics and reads every readable characteristic: one that needs encryption makes
 * bluetoothd pair (SMP). It stays connected until SECONDS have passed since the start; "bt_test: PAIR ..." lines say how
 * far it got.
 *
 * bluetoothd checks that an application may use Bluetooth (TCC); src/bt_test.entitlements carries the right.
 *
 * bluetoothd gives each scan the state of the process behind it (foreground-running, background-running, daemon ...),
 * and bt_test, not an application, is a "daemon" (0x20). A daemon's scan without services or matching rules is never
 * given a device (bluetoothd's doesDeviceMatch, at 0x10058edc4 in iOS 16.5's: only a foreground scan may match
 * everything), so here only a scan with SERVICE-UUIDs shows devices.
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

@interface Scanner : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property(nonatomic, copy) NSString *pairName;
@property(nonatomic, strong) CBPeripheral *target;
@property(nonatomic) unsigned reads, readErrors;
@property(nonatomic, strong) CBCentralManager *manager;
@property(nonatomic, strong) NSMutableDictionary<NSUUID *, NSNumber *> *seen;
@property(nonatomic) NSTimeInterval seconds;
@property(nonatomic, strong) NSArray<CBUUID *> *services;
@end

static const char *
state_name(CBManagerState state)
{
	switch (state) {
	case CBManagerStateUnknown: return "unknown";
	case CBManagerStateResetting: return "resetting";
	case CBManagerStateUnsupported: return "unsupported";
	case CBManagerStateUnauthorized: return "unauthorized (TCC)";
	case CBManagerStatePoweredOff: return "powered off";
	case CBManagerStatePoweredOn: return "powered on";
	}
	return "?";
}

@implementation Scanner

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
	fprintf(stderr, "bt_test: central manager state %ld: %s\n", (long)central.state, state_name(central.state));
	if (central.state != CBManagerStatePoweredOn || central.isScanning) {
		return;
	}
	fprintf(stderr, "bt_test: scanning for %.0f s for %s\n", self.seconds,
	    self.services ? [self.services description].UTF8String : "every device");
	[central scanForPeripheralsWithServices:self.services options:@{ CBCentralManagerScanOptionAllowDuplicatesKey : @YES }];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(self.seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		[central stopScan];
		unsigned long packets = 0;
		for (NSNumber *n in self.seen.allValues) {
			packets += n.unsignedLongValue;
		}
		fprintf(stderr, "bt_test: SCAN DONE: %lu devices, %lu advertising packets\n", (unsigned long)self.seen.count,
		    packets);
		exit(0);
	});
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)peripheral
    advertisementData:(NSDictionary<NSString *, id> *)data RSSI:(NSNumber *)rssi
{
	NSNumber *count = self.seen[peripheral.identifier];

	self.seen[peripheral.identifier] = @(count.unsignedLongValue + 1);
	if (count == nil) {
		NSString *name = data[CBAdvertisementDataLocalNameKey] ?: peripheral.name;

		if (self.pairName && self.target == nil && [name isEqualToString:self.pairName]) {
			fprintf(stderr, "bt_test: PAIR found \"%s\" (RSSI %d dBm), connecting\n", name.UTF8String, rssi.intValue);
			[central stopScan];
			self.target = peripheral;
			peripheral.delegate = self;
			[central connectPeripheral:peripheral options:nil];
		}
		fprintf(stderr, "bt_test: device %lu: RSSI %d dBm%s%s%s\n", (unsigned long)self.seen.count, rssi.intValue,
		    name ? ", \"" : "", name ? name.UTF8String : "", name ? "\"" : "");
	}
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
	fprintf(stderr, "bt_test: PAIR connected, discovering services\n");
	[peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
	fprintf(stderr, "bt_test: PAIR connect failed: %s\n", error.description.UTF8String);
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
	fprintf(stderr, "bt_test: PAIR disconnected: %s\n", error ? error.description.UTF8String : "no error");
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
	fprintf(stderr, "bt_test: PAIR %lu services%s%s\n", (unsigned long)peripheral.services.count, error ? ": " : "",
	    error ? error.description.UTF8String : "");
	for (CBService *service in peripheral.services) {
		fprintf(stderr, "bt_test: PAIR service %s\n", service.UUID.UUIDString.UTF8String);
		[peripheral discoverCharacteristics:nil forService:service];
	}
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
	for (CBCharacteristic *c in service.characteristics) {
		fprintf(stderr, "bt_test: PAIR characteristic %s/%s properties %#lx\n", service.UUID.UUIDString.UTF8String,
		    c.UUID.UUIDString.UTF8String, (unsigned long)c.properties);
		if (c.properties & CBCharacteristicPropertyRead) {
			[peripheral readValueForCharacteristic:c];
		}
	}
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error
{
	if (error) {
		self.readErrors++;
		fprintf(stderr, "bt_test: PAIR read %s failed: %s\n", c.UUID.UUIDString.UTF8String, error.description.UTF8String);
	} else {
		self.reads++;
		fprintf(stderr, "bt_test: PAIR read %s: %lu bytes\n", c.UUID.UUIDString.UTF8String, (unsigned long)c.value.length);
	}
}

@end

int
main(int argc, char **argv)
{
	@autoreleasepool {
		Scanner *scanner = [Scanner new];

		int first = 1;

		if (argc > 3 && strcmp(argv[1], "pair") == 0) {
			/* job lines have no quoting: "_" in NAME stands for a space */
			scanner.pairName = [@(argv[2]) stringByReplacingOccurrencesOfString:@"_" withString:@" "];
			first = 3;
		}
		scanner.seconds = argc > first ? atof(argv[first]) : 30;
		if (argc > first + 1) {
			NSMutableArray<CBUUID *> *services = [NSMutableArray array];

			for (int i = first + 1; i < argc; i++) {
				[services addObject:[CBUUID UUIDWithString:@(argv[i])]];
			}
			scanner.services = services;
		}
		scanner.seen = [NSMutableDictionary dictionary];
		scanner.manager = [[CBCentralManager alloc] initWithDelegate:scanner queue:dispatch_get_main_queue()];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((scanner.seconds + 60) * NSEC_PER_SEC)),
		    dispatch_get_main_queue(), ^{
			fprintf(stderr, "bt_test: gave up: the state stayed %s\n", state_name(scanner.manager.state));
			exit(1);
		});
		dispatch_main();
	}
}
