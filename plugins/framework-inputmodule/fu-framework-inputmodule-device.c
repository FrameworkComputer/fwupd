/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-framework-inputmodule-device.h"
#include "fu-framework-inputmodule-firmware.h"

struct _FuFrameworkInputmoduleDevice {
	FuUsbDevice parent_instance;
	guint8 iface_control;
	guint8 ep_out;
};

G_DEFINE_TYPE(FuFrameworkInputmoduleDevice, fu_framework_inputmodule_device, FU_TYPE_USB_DEVICE)

static gchar *
fu_framework_inputmodule_device_convert_version(FuDevice *device, guint64 version_raw)
{
	/* Firmware uses an 8-bit major and two 4-bit minor/patch fields. */
	return g_strdup_printf("%u.%u.%u",
			       (guint)((version_raw >> 8) & 0xff),
			       (guint)((version_raw >> 4) & 0xf),
			       (guint)(version_raw & 0xf));
}

static gboolean
fu_framework_inputmodule_device_probe(FuDevice *device, GError **error)
{
	FuFrameworkInputmoduleDevice *self = FU_FRAMEWORK_INPUTMODULE_DEVICE(device);
	g_autoptr(FuUsbInterface) intf_control = NULL;
	g_autoptr(FuUsbInterface) intf_data = NULL;
	g_autoptr(GPtrArray) endpoints = NULL;
	g_autofree gchar *devpath = NULL;

	intf_control = fu_usb_device_get_interface(FU_USB_DEVICE(self), 0x02, 0x02, 0x00, error);
	if (intf_control == NULL)
		return FALSE;
	intf_data = fu_usb_device_get_interface(FU_USB_DEVICE(self), 0x0a, 0x00, 0x00, error);
	if (intf_data == NULL)
		return FALSE;
	endpoints = fu_usb_interface_get_endpoints(intf_data);
	for (guint i = 0; i < endpoints->len; i++) {
		FuUsbEndpoint *ep = g_ptr_array_index(endpoints, i);
		if (fu_usb_endpoint_get_direction(ep) == FU_USB_DIRECTION_HOST_TO_DEVICE)
			self->ep_out = fu_usb_endpoint_get_address(ep);
	}
	if (self->ep_out == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "CDC data interface has no OUT endpoint");
		return FALSE;
	}
	/* match FuBlockDevice's USB topology ID across the ROM bootloader transition */
	devpath = fu_udev_device_get_devpath(FU_UDEV_DEVICE(self));
	if (devpath != NULL) {
		g_autofree gchar *physical_id = g_strdup_printf("DEVPATH=%s", devpath);
		fu_device_set_physical_id(device, physical_id);
	}
	fu_device_set_version_raw(device, fu_usb_device_get_release(FU_USB_DEVICE(self)));
	self->iface_control = fu_usb_interface_get_number(intf_control);
	fu_usb_device_add_interface(FU_USB_DEVICE(self), self->iface_control);
	fu_usb_device_add_interface(FU_USB_DEVICE(self), fu_usb_interface_get_number(intf_data));
	return TRUE;
}

static gboolean
fu_framework_inputmodule_device_detach(FuDevice *device, FuProgress *progress, GError **error)
{
	FuFrameworkInputmoduleDevice *self = FU_FRAMEWORK_INPUTMODULE_DEVICE(device);
	guint8 buf[] = {0x32, 0xac, 0x02, 0x00};
	gsize actual_len = 0;

	/* match opening a CDC serial port: assert DTR and RTS. */
	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_CLASS,
					    FU_USB_RECIPIENT_INTERFACE,
					    0x22,
					    0x03,
					    self->iface_control,
					    NULL,
					    0,
					    NULL,
					    2000,
					    error))
		return FALSE;
	if (!fu_usb_device_bulk_transfer(FU_USB_DEVICE(self),
					 self->ep_out,
					 buf,
					 sizeof(buf),
					 &actual_len,
					 2000,
					 error))
		return FALSE;
	if (actual_len != sizeof(buf)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_WRITE,
			    "short bootloader command: %zu bytes",
			    actual_len);
		return FALSE;
	}
	/* this command has no reply; the firmware immediately enters ROM USB boot. */
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static void
fu_framework_inputmodule_device_set_progress(FuDevice *device, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "prepare-fw");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 5, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 90, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 0, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 5, "reload");
}

static void
fu_framework_inputmodule_device_init(FuFrameworkInputmoduleDevice *self)
{
	fu_device_set_name(FU_DEVICE(self), "LED Matrix Input Module");
	fu_device_set_vendor(FU_DEVICE(self), "Framework");
	fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_COMPUTER);
	fu_device_add_protocol(FU_DEVICE(self), "com.microsoft.uf2");
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_firmware_gtype(FU_DEVICE(self), FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_ADD_COUNTERPART_GUIDS);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_REPLUG_MATCH_GUID);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_RETRY_OPEN);
	fu_device_set_remove_delay(FU_DEVICE(self), 15000);
	fu_device_retry_set_delay(FU_DEVICE(self), 100);
}

static void
fu_framework_inputmodule_device_class_init(FuFrameworkInputmoduleDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	device_class->probe = fu_framework_inputmodule_device_probe;
	device_class->detach = fu_framework_inputmodule_device_detach;
	device_class->convert_version = fu_framework_inputmodule_device_convert_version;
	device_class->set_progress = fu_framework_inputmodule_device_set_progress;
}
