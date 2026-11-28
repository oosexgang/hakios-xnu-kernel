/*
 * bluetool_stub: the Mach service com.apple.BlueTool, for a Bluetooth controller the kernel has set up already.
 *
 * On an iPhone, bluetoothd asks BlueTool (its own daemon) to run a "boot script" that loads Apple's chip with its
 * firmware, and waits up to 30 s for the answer. On zeus the kernel driver (hakios.bt.sm8450.c) has loaded the
 * WCN6855's own firmware before Darwin starts, and BlueTool's scripts are for Apple's chips. This answers every
 * request as done, the way bluetoothd reads BlueTool's answer: a reply dictionary with "result" 0 (success),
 * "firmware" and "manufacturer". The requests come as { kMsgId: "RunBuiltinScript", kMsgArgs: { script, btaddress } }
 * (what bluetoothd sends; read from its binary, nothing of BlueTool's is used). Each request is said on the console.
 *
 * The iOS SDK has no XPC headers; the few functions used are declared here. libxpc is part of libSystem.
 */
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);

extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)

xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
void xpc_connection_resume(xpc_connection_t connection);
void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
xpc_type_t xpc_get_type(xpc_object_t object);
xpc_object_t xpc_retain(xpc_object_t object);
void xpc_release(xpc_object_t object);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
xpc_object_t xpc_dictionary_get_value(xpc_object_t dictionary, const char *key);
const char *xpc_dictionary_get_string(xpc_object_t dictionary, const char *key);
void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *value);

/* what bluetoothd is told the controller runs: the WCN6855's firmware build, and Qualcomm's company id (0x1d) */
#define FIRMWARE        "WCN6855 BTFW.HSP.2.1.0 (loaded by the kernel)"
#define MANUFACTURER    0x1d

static void
answer(xpc_connection_t peer, xpc_object_t message)
{
	const char *id = xpc_dictionary_get_string(message, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(message, "kMsgArgs");
	const char *script = NULL, *address = NULL;
	xpc_object_t reply;

	if (args != NULL && xpc_get_type(args) == &_xpc_type_dictionary) {
		script = xpc_dictionary_get_string(args, "script");
		address = xpc_dictionary_get_string(args, "btaddress");
	}
	if (script == NULL) {
		script = xpc_dictionary_get_string(message, "script");
	}
	fprintf(stderr, "bluetool_stub: %s (script \"%s\", address %s): answered as done\n", id ? id : "a request",
	    script ? script : "?", address ? address : "?");
	reply = xpc_dictionary_create_reply(message);
	if (reply == NULL) {
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	xpc_dictionary_set_string(reply, "firmware", FIRMWARE);
	xpc_dictionary_set_int64(reply, "manufacturer", MANUFACTURER);
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
}

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(),
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == NULL) {
		fprintf(stderr, "bluetool_stub: com.apple.BlueTool cannot be listened on\n");
		return 1;
	}
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_retain(peer);
		xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
			if (xpc_get_type(event) == &_xpc_type_dictionary) {
				answer(peer, event);
			} else if (xpc_get_type(event) == &_xpc_type_error) {
				xpc_release(peer);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	fprintf(stderr, "bluetool_stub: com.apple.BlueTool is answered from here on\n");
	dispatch_main();
}
