/*
 * store_test: the iTunes Store account this device has, and the storefront on it.
 *
 * usage: store_test show
 *        store_test set STOREFRONT      (e.g. "143469-2,8")
 *
 * Why it exists. The App Store's front page is a "bag" -- AppleMediaServices' configuration, fetched
 * from bag.itunes.apple.com -- and AMS will not load one until it knows the storefront, the number
 * that says which country's store this device belongs to. A stock iPhone with no SIM and no account
 * gets that number from the bag server itself: it asks with none, the server picks one from the
 * address the request came from and returns it in an X-Set-Apple-Store-Front header, and AMS writes
 * it onto the local account. The first half works here -- net_test fetches the bag and the header
 * comes back (rc.jobs.store) -- and the second never happens: appstored opens no socket at all, AMS
 * reads an empty cache in its first 40 ms and stops. So: put the number the server gave THIS phone
 * on the account by hand, with AMS's own setter, and see whether that is the only thing missing.
 *
 * Nothing here is invented. The value to pass is the one bag.itunes.apple.com returned for this
 * device, and it is a region, not an account: no Apple ID is signed in, none can be, and the store
 * must still refuse to install anything -- there is nothing to buy with.
 *
 * How. The account AMS talks about is not an ordinary one: asking accountsd for accounts of type
 * com.apple.account.iTunesStore returns none, while appstored prints a local account with a stable
 * identifier every boot (username = local, storefront = (null)). AppleMediaServices keeps its own
 * notion of it, so this uses AMS's own calls rather than guessing at the database:
 * ACAccountStore's ams_localiTunesAccount, and ams_localiTunesAccountAfterUpdatingStorefront:,
 * which is the call AMS itself makes when the bag server returns an x-set-apple-store-front header.
 * Everything is guarded by respondsToSelector and every answer is printed, so a run says which of
 * them this build of AMS actually has.
 * accountsd answers only a client that holds the entitlements its own daemons hold
 * (store_test.entitlements, taken from appstored's own signature with ldid -e:
 * com.apple.accounts.appleaccount.fullaccess, com.apple.private.accounts.allaccounts), so build.sh
 * signs this one with ldid.
 *
 * WHAT IT ANSWERED, and it is not what was hoped for. As mobile (uid 501, "user 501" below: init runs
 * this as root and appstored is mobile's):
 *   store_test: before: ams_localiTunesAccount: user local
 *   store_test: before: ams_localiTunesAccount: id 7BA65770-7DC2-488D-8B57-81B17A8B12F0
 *   store_test: before: ams_localiTunesAccount: STOREFRONT (null)
 *   store_test: set: in memory now 143469-2,8
 *   store_test: set: ams_saveAccount: done
 *   store_test: after: ams_localiTunesAccount: STOREFRONT (null)
 * The setter works and the value does not keep. accountsd has no account of type
 * com.apple.account.iTunesStore at all -- which is right for a signed-out iPhone -- and the local
 * account AMS hands out is not one of its rows: its identifier is a different one every boot
 * (143B8A92..., 8D31AF5C..., C976F334... in three runs of appstored, 7BA65770... here), so it is made
 * fresh each time with a null storefront and there is nothing on disk to write to. The only thing
 * that ever fills it is the bag response itself (_setStorefrontFromURLResponse:bagData:), which is
 * the fetch that never happens.
 * So a storefront cannot be put on this device from another process, and the next place to try is
 * inside appstored -- the port already puts a library of ours into stock programs (INJECT in
 * build.sh, src/inject_sb.c and its kin), which would both set the storefront on the account AMS is
 * really using and show whether a bag request is ever built at all.
 * This tool stays because the three facts it establishes are worth keeping: AMS's local account is
 * per boot, accountsd holds no iTunes account, and ams_setStorefront: does not persist.
 */
#import <Accounts/Accounts.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

@interface ACAccount (AppleMediaServices)
- (NSString *)ams_storefront;
- (void)ams_setStorefront:(NSString *)storefront;
- (BOOL)ams_isLocalAccount;
@end

@interface ACAccountStore (AppleMediaServices)
+ (instancetype)ams_sharedAccountStore;
- (ACAccount *)ams_localiTunesAccount;
- (ACAccount *)ams_localiTunesAccountAfterUpdatingStorefront:(NSString *)storefront;
- (void)ams_saveAccount:(ACAccount *)account;
@end

static NSString *const kiTunesStore = @"com.apple.account.iTunesStore";

/* one field a line: the console drops long ones (see rc.jobs.store), and the storefront is the point */
static void
describe(const char *what, ACAccount *account)
{
	if (account == nil) {
		printf("store_test: %s: (none)\n", what);
		return;
	}
	printf("store_test: %s: user %s\n", what, account.username.UTF8String ?: "(null)");
	printf("store_test: %s: id %s\n", what, account.identifier.UTF8String ?: "(null)");
	printf("store_test: %s: STOREFRONT %s\n", what,
	    [account respondsToSelector:@selector(ams_storefront)] ? (account.ams_storefront.UTF8String ?: "(null)") : "?");
}

static void
show(ACAccountStore *store, const char *when)
{
	ACAccountType *type = [store accountTypeWithAccountTypeIdentifier:kiTunesStore];
	NSArray<ACAccount *> *accounts = type != nil ? [store accountsWithAccountType:type] : nil;
	char what[64];

	printf("store_test: %s: accountsd has %lu account(s) of type %s\n", when, (unsigned long)accounts.count,
	    kiTunesStore.UTF8String);
	for (ACAccount *account in accounts) {
		snprintf(what, sizeof(what), "%s: accountsd", when);
		describe(what, account);
	}
	if ([store respondsToSelector:@selector(ams_localiTunesAccount)]) {
		snprintf(what, sizeof(what), "%s: ams_localiTunesAccount", when);
		describe(what, store.ams_localiTunesAccount);
	} else {
		printf("store_test: %s: this AMS has no ams_localiTunesAccount\n", when);
	}
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		const char *action = argc > 1 ? argv[1] : "show";
		/* appstored is mobile's, and so is the account database; init runs this as root, so step down */
		if (argc > 2 && strcmp(argv[1], "user") == 0) {
			uid_t who = (uid_t)atoi(argv[2]);

			if (setgid(who) != 0 || setuid(who) != 0) {
				printf("store_test: could not become %d\n", who);
			}
			action = argc > 3 ? argv[3] : "show";
			argv += 2;
			argc -= 2;
		}
		/* the categories with the storefront live in this framework, which nothing has loaded for us */
		void *ams = dlopen("/System/Library/PrivateFrameworks/AppleMediaServices.framework/AppleMediaServices", RTLD_LAZY);
		ACAccountStore *store;

		setvbuf(stdout, NULL, _IOLBF, 0);
		printf("store_test: uid %d, euid %d; AppleMediaServices %s\n", getuid(), geteuid(),
		    ams != NULL ? "loaded" : dlerror());
		store = [ACAccountStore respondsToSelector:@selector(ams_sharedAccountStore)] ?
		    ACAccountStore.ams_sharedAccountStore : [ACAccountStore new];
		printf("store_test: account store = %s\n", [store description].UTF8String ?: "(null)");
		show(store, "before");
		if (strcmp(action, "set") == 0 && argc > 2) {
			NSString *wanted = @(argv[2]);

			/* what AMS itself does when the bag server returns an x-set-apple-store-front header */
			if ([store respondsToSelector:@selector(ams_localiTunesAccountAfterUpdatingStorefront:)]) {
				describe("set: ams_localiTunesAccountAfterUpdatingStorefront:",
				    [store ams_localiTunesAccountAfterUpdatingStorefront:wanted]);
			} else {
				printf("store_test: set: this AMS has no ams_localiTunesAccountAfterUpdatingStorefront:\n");
			}
			/* and the long way round, in case that one only answers and does not keep */
			if ([store respondsToSelector:@selector(ams_localiTunesAccount)]) {
				ACAccount *local = store.ams_localiTunesAccount;

				if (local != nil && [local respondsToSelector:@selector(ams_setStorefront:)]) {
					[local ams_setStorefront:wanted];
					printf("store_test: set: in memory now %s\n", local.ams_storefront.UTF8String ?: "(null)");
					if ([store respondsToSelector:@selector(ams_saveAccount:)]) {
						[store ams_saveAccount:local];
						printf("store_test: set: ams_saveAccount: done\n");
					} else {
						dispatch_semaphore_t done = dispatch_semaphore_create(0);

						[store saveAccount:local withCompletionHandler:^(BOOL saved, NSError *error) {
							printf("store_test: set: saveAccount: %s%s%s\n", saved ? "saved" : "NOT SAVED",
							    error != nil ? ", error = " : "",
							    error != nil ? error.description.UTF8String : "");
							dispatch_semaphore_signal(done);
						}];
						if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
							printf("store_test: set: accountsd did not answer in 30 s\n");
						}
					}
				}
			}
			show([ACAccountStore respondsToSelector:@selector(ams_sharedAccountStore)] ?
			    ACAccountStore.ams_sharedAccountStore : [ACAccountStore new], "after");
		}
	}
	return 0;
}
