/* Part of the loader (included by loader.c, not compiled alone): the USB command channel: a small server of the fastboot protocol. */
/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static int download_ramdisk_append; /* ... "oem ramdisk append": after what is there (an image over 4 GB, in pieces) */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = HAKIOS_USB_DEVICE_GUID;
	static const efi_guid qcom_usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		/* Stock Qualcomm UEFI uses its private protocol and event to enable device mode. */
		if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
		    &init_guid, &event))) {
			BS->signal_event(event);
			BS->close_event(event);
		}
		if (EFI_ERROR(BS->locate_protocol(&qcom_usb_guid, NULL, (void **)&USB))) {
			print("usb: HakiOS and vendor device protocols not found\n");
			return 0;
		}
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = READ_SYSREG(CNTFRQ_EL0), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
