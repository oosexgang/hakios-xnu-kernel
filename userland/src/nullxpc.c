/*
 * nullxpc: XPC services that are there and answer nothing. For a daemon this phone cannot run --
 * CommCenter, which has no baseband to talk to and dies of a SIGSEGV every time it is started --
 * but whose services the system asks for without end when they are missing: SpringBoard looked
 * com.apple.commcenter.xpc up some 480 times a second, and it and launchd took 1.7 of the four
 * cores between them (cputop, 24 October). Every service named on the command line is checked in
 * and listened on; a message that wants a reply gets an empty one.
 *
 * usage: nullxpc SERVICE...
 */
#include <dispatch/dispatch.h>
#include <stdio.h>

/* libxpc's, which the iOS SDK has no header for (it is in libSystem all the same) */
typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
void xpc_connection_set_event_handler(xpc_connection_t connection, void (^handler)(xpc_object_t object));
void xpc_connection_resume(xpc_connection_t connection);
void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
xpc_type_t xpc_get_type(xpc_object_t object);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
void xpc_release(xpc_object_t object);

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		xpc_connection_t listener = xpc_connection_create_mach_service(argv[i], NULL,
		    XPC_CONNECTION_MACH_SERVICE_LISTENER);

		xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
			if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
				return;
			}
			xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
				if (xpc_get_type(message) == XPC_TYPE_DICTIONARY) {
					xpc_object_t reply = xpc_dictionary_create_reply(message);

					if (reply != NULL) {
						xpc_connection_send_message(peer, reply);
						xpc_release(reply);
					}
				}
			});
			xpc_connection_resume(peer);
		});
		xpc_connection_resume(listener);
	}
	dispatch_main();
}
