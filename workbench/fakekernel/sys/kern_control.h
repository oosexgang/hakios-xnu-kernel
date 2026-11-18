/*
 * Kernel controls (sys/kern_control.h): what a driver registers. Registering is the test's own (ctl_register(),
 * ctl_deregister()): it keeps the registration and calls the control's functions as XNU's sockets would.
 */
#ifndef FAKE_KERN_CONTROL_H
#define FAKE_KERN_CONTROL_H
#include "../fakekernel.h"

#define MAX_KCTL_NAME                   96
#define CTL_FLAG_PRIVILEGED             0x1
#define CTL_FLAG_REG_ID_UNIT            0x2
#define CTL_FLAG_REG_SOCK_STREAM        0x4

typedef void *kern_ctl_ref;
struct sockaddr_ctl {
	u_char sc_len, sc_family;
	u_int16_t ss_sysaddr;
	u_int32_t sc_id, sc_unit, sc_reserved[5];
};
typedef errno_t (*ctl_connect_func)(kern_ctl_ref kctlref, struct sockaddr_ctl *sac, void **unitinfo);
typedef errno_t (*ctl_disconnect_func)(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo);
typedef errno_t (*ctl_send_func)(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo, mbuf_t m, int flags);
typedef errno_t (*ctl_setopt_func)(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t len);
typedef errno_t (*ctl_getopt_func)(kern_ctl_ref kctlref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t *len);
struct kern_ctl_reg {
	char ctl_name[MAX_KCTL_NAME];
	u_int32_t ctl_id, ctl_unit, ctl_flags, ctl_sendsize, ctl_recvsize;
	ctl_connect_func ctl_connect;
	ctl_disconnect_func ctl_disconnect;
	ctl_send_func ctl_send;
	ctl_setopt_func ctl_setopt;
	ctl_getopt_func ctl_getopt;
};
errno_t ctl_register(struct kern_ctl_reg *userkctl, kern_ctl_ref *kctlref);
errno_t ctl_deregister(kern_ctl_ref kctlref);
#endif
