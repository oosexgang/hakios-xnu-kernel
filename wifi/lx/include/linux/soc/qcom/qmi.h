/*
 * QMI, the protocol in which the driver talks to the radio's firmware before the radio's own
 * registers exist. The helpers are the kernel's (bsd/sys/redwood_qmi.h, bsd/dev/redwood_qmi.c),
 * under Linux's names; a kernel tree that does not have them yet gets LinuxKPI's header, in
 * which every call fails.
 */
#ifndef _LX_LINUX_SOC_QCOM_QMI_H
#define _LX_LINUX_SOC_QCOM_QMI_H

#ifdef LX_REDWOOD_QMI_H
#include <linux/jiffies.h>
#define REDWOOD_QMI_JIFFIES_TO_MS(j)	jiffies_to_msecs(j)
#include LX_REDWOOD_QMI_H
/*
 * What the driver sends goes past the radio's bus first (src/lx_bus.c), which has to know of one
 * request: the mode the firmware is asked for. The radio's registers answer in mission mode only.
 */
int lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure);
#ifndef LX_QMI_AS_IT_IS
#define qmi_send_request	lx_qmi_send_request
#endif
#else
#include <linux/socket.h>
#include_next <linux/soc/qcom/qmi.h>
#endif

#endif
