/*
 * Adapted from iOSPlus10 userland/bluetooth/negroni-bluetool.c, Copyright (c) 2026 iOSPlus10 contributors, MIT License
 * (devices/mayfly/drivers/bluetooth/THIRD-PARTY-NOTICE).
 */
/*
 * qca-bluetool: stand-in for /usr/sbin/BlueTool -R (launchd job com.apple.BlueTool, Mach service
 * com.apple.BlueTool) on the NE2211. Our own code (docs/bluetooth-plan.md section 4).
 *
 * Why [V, 20H392 binaries]: bluetoothd asks BlueTool to "boot"/"init" the chip (XPC dictionary kMsgId
 * RunBuiltinScript, kMsgArgs {script, btaddress}; bluetoothd 0x100272190-0x100272290) and waits for the reply
 * (reply handler 0x10027249c: int64 "result" 0x100272580, 0 = ok, 10/13 = "no platform/chipset/firmware
 * combination"; string "firmware" 0x10027261c; int64 "manufacturer" 0x100272630). The stock BlueTool only knows
 * Broadcom chips (.hcd boot scripts; overrides in /etc/bluetool only for ReleaseType Vendor/Internal,
 * BlueTool 0x100016054-0x100016280), so it fails on the QCA6490. Here the kernel (patches 0330-0332) already
 * powered the chip and loaded its firmware: every script request succeeds without touching the controller.
 * Replies as the stock BlueTool builds them (BlueTool 0x100042238-0x1000426ac): RunBuiltinScript -> result,
 * manufacturer, firmware; RunCommand -> result, output; RunExternalScript -> result.
 * Log: stdout/stderr lines "qca-bluetool: ..." and os_log (subsystem com.apple.bluetooth via syslog).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <stdint.h>
#include <dispatch/dispatch.h>

/* the iOS SDK has no xpc/xpc.h: what is used of it, as libxpc exports it */
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef void *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const char _xpc_type_dictionary, _xpc_type_connection, _xpc_type_error;
#define XPC_TYPE_DICTIONARY ((xpc_type_t)&_xpc_type_dictionary)
#define XPC_TYPE_CONNECTION ((xpc_type_t)&_xpc_type_connection)
#define XPC_TYPE_ERROR ((xpc_type_t)&_xpc_type_error)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1ULL << 0)
extern xpc_type_t xpc_get_type(xpc_object_t);
extern const char *xpc_dictionary_get_string(xpc_object_t, const char *);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
extern void xpc_connection_set_target_queue(xpc_connection_t, dispatch_queue_t);
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_release(xpc_object_t);
extern void xpc_retain(xpc_object_t);

/* marked unavailable in the iOS SDK headers; libxpc on iOS exports it (launchd daemons use it) */
extern xpc_connection_t qb_create_mach_service(const char *name, dispatch_queue_t q, uint64_t flags)
    __asm__("_xpc_connection_create_mach_service");

#define QCA_MANUFACTURER 29     /* 0x001d, Qualcomm; only used by bluetoothd for a metrics string (0x100272664) */
#define FIRMWARE "QCA6490 hpbtfw21.tlv (loaded by the kernel, qcabt=6)"

static void say(const char *fmt, const char *a, const char *b)
{
	char line[512];
	snprintf(line, sizeof(line), fmt, a ? a : "-", b ? b : "-");
	fprintf(stderr, "qca-bluetool: %s\n", line);
	syslog(LOG_NOTICE, "qca-bluetool: %s", line);
}

static void handle(xpc_connection_t peer, xpc_object_t msg)
{
	if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) {
		return;
	}
	const char *id = xpc_dictionary_get_string(msg, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(msg, "kMsgArgs");
	const char *script = NULL, *cmd = NULL;
	if (args && xpc_get_type(args) == XPC_TYPE_DICTIONARY) {
		script = xpc_dictionary_get_string(args, "script");
		cmd = xpc_dictionary_get_string(args, "command");
	}
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (!reply) {
		say("%s without a reply port (%s): ignored", id, script);
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	if (id && strcmp(id, "RunBuiltinScript") == 0) {
		xpc_dictionary_set_int64(reply, "manufacturer", QCA_MANUFACTURER);
		xpc_dictionary_set_string(reply, "firmware", FIRMWARE);
		say("RunBuiltinScript \"%s\" -> result 0 (chip booted by the kernel)%s", script, "");
	} else if (id && strcmp(id, "RunCommand") == 0) {
		xpc_dictionary_set_string(reply, "output", "");
		say("RunCommand \"%s\" -> result 0, no output%s", cmd, "");
	} else {
		say("%s (script %s) -> result 0", id, script);
	}
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	openlog("qca-bluetool", LOG_PID, LOG_DAEMON);
	xpc_connection_t l = qb_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);
	if (!l) {
		say("cannot listen on %s%s", "com.apple.BlueTool", "");
		return 1;
	}
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) { handle(peer, msg); });
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	say("listening on %s (stand-in for BlueTool -R)%s", "com.apple.BlueTool", "");
	dispatch_main();
}
