/*
 * Copyright 2025 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-ilitek-touch-device.h"
#include "fu-ilitek-touch-struct.h"

struct _FuIlitekTouchDevice {
	FuHidrawDevice parent_instance;
	guint8 iface_reset;
};

G_DEFINE_TYPE(FuIlitekTouchDevice, fu_ilitek_touch_device, FU_TYPE_HIDRAW_DEVICE)

#define FU_ILITEK_TOUCH_VENDOR_USAGE_PAGE  0xFF00
#define FU_ILITEK_TOUCH_VENDOR_USAGE_ID    0x0001
// #define FU_ILITEK_TOUCH_FIRMWARE_USAGE_ID  0x27
// #define FU_ILITEK_TOUCH_FIRMWARE_REPORT_ID 0x27
// #define FU_ILITEK_TOUCH_USI_VER_REPORT_ID  0x28

static gboolean
fu_ilitek_touch_device_detach(FuDevice *device, FuProgress *progress, GError **error)
{

	/* success */
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static void
fu_ilitek_touch_device_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-fw");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 70, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 29, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 1, "reload");
}

static void
fu_ilitek_touch_device_init(FuIlitekTouchDevice *self)
{
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_INPUT_KEYBOARD);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_set_remove_delay(FU_DEVICE(self), 15000); /* 15s */
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_HEX);
	fu_device_add_protocol(FU_DEVICE(self), "com.microsoft.uf2");
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_ADD_COUNTERPART_GUIDS);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_REPLUG_MATCH_GUID);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_RETRY_OPEN);
	/* revisions indicate incompatible hardware */
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_ADD_INSTANCE_ID_REV);
	fu_udev_device_add_open_flag(FU_UDEV_DEVICE(self), FU_IO_CHANNEL_OPEN_FLAG_WRITE);
	fu_device_retry_set_delay(FU_DEVICE(self), 100);
}

static gboolean
fu_ilitek_touch_device_setup(FuDevice *device, GError **error)
{
	g_autoptr(FuHidDescriptor) descriptor = NULL;
	g_autoptr(FuHidReport) report = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GByteArray) req = fu_struct_ilitek_touch_request_new();
	guint16 version = 0x0000;

	descriptor = fu_hidraw_device_parse_descriptor(FU_HIDRAW_DEVICE(device), error);
	if (descriptor == NULL) {
		g_prefix_error_literal(error, "failed to parse descriptor: ");
		return FALSE;
	}
	report = fu_hid_descriptor_find_report(descriptor,
					       error,
					       "usage-page",
					       FU_ILITEK_TOUCH_VENDOR_USAGE_PAGE,
					       "usage",
					       FU_ILITEK_TOUCH_VENDOR_USAGE_ID,
					       "collection",
					       0x01,
					       NULL);
	if (report == NULL) {
		/* Wrong usage page, this hidraw instance is not what we're looking for */
		return FALSE;
	}

	// Get protocol version
	req->data.read_len = 3;
	req->data.message_id = 0x42;
	// TODO: Fetch and use

	// Get USI/MPP status
	req->data.read_len = 16;
	req->data.message_id = 0x20;
	// TODO: Fetch and use

	// Get firmware version
	req->data.read_len = 8;
	req->data.message_id = 0x40;
	if (!fu_hidraw_device_get_feature(FU_HIDRAW_DEVICE(device),
					 req->data,
					 req->len,
					 FU_IO_CHANNEL_FLAG_NONE,
					 error)) {
		g_prefix_error_literal(error, "failed to write packet: ");
		return FALSE;
	}

	fu_device_set_version_raw(device, req->data.data);

	return TRUE;
}

static gchar *
fu_ilitek_touch_device_convert_version(FuDevice *device, guint64 version_raw)
{
	return fu_version_from_uint64(version_raw, fu_device_get_version_format(device));
}

static void
fu_ilitek_touch_device_class_init(FuIlitekTouchDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->setup = fu_ilitek_touch_device_setup;
	device_class->convert_version = fu_ilitek_touch_device_convert_version;
	device_class->set_progress = fu_ilitek_touch_device_set_progress;
}
