# Apple ID and the App Store

Signing in with an Apple ID, and the App Store that needs one. Darwin's account and store daemons expect
a phone Apple made: a Secure Enclave that wraps the keychain's most protected items, fuses that say what
kind of part this is, an activation record, a FairPlay identity. This phone has none of them. What is
here is code of ours inside those daemons, each library a small change at the one place where the daemon
would otherwise stop, and nothing else.

| Where | Inside | What it does |
|---|---|---|
| `src/inject_kc.c` | `akd` (AuthKit) | Sign-in stores about twenty GrandSlam tokens in the keychain; one asks for a protection class that exists only with a passcode the Secure Enclave knows. That one store is let through, so the account is not signed out again. It also gives the store's bag shim the value `absinthe-requests` it lacks (`src/absinthe_bag.h`). |
| `src/inject_store.c` | `appstored` | The storefront -- which country's store this device is in -- answered inside the daemon, so the App Store's front page loads instead of spinning. |
| `src/inject_mescal.m` | `appstored`, `itunesstored`, `AppStore` | The App Store's SAP (Mescal) signature on the storefront-token request, without which the catalog is not returned. |
| `src/inject_mad.c` | `mobileactivationd` | The four fusing answers of MobileGestalt read as a development part, so the device is activated with no record, the state the App Store asks for. |
| `first-generation/` | Darwin 16.5's daemons | The same work on the first generation: `inject_store.c` (appstored), `inject_fairplay.c` (watches fairplayd), `store_test.m` (the account and its storefront, as accountsd shows them). Built by `userland/userland/build.sh` (STOREFRONT=1). |

`darwin/userland/inj26.sh` builds the libraries of `src/` for Darwin 26/27, and
`darwin/userland/root26-darwin.sh` puts them into the daemons: STOREFRONT=1 the store's, SIGNIN=1 akd's.
`darwin/userland/rc.jobs.store` is the job that starts the App Store and watches it.

