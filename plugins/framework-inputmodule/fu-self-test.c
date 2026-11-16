/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-device-private.h"
#include "fu-framework-inputmodule-device.h"
#include "fu-framework-inputmodule-firmware.h"
#include "fu-framework-inputmodule-struct.h"

static void
fu_framework_inputmodule_version_func(void)
{
	g_autoptr(FuDevice) device = g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_DEVICE, NULL);
	fu_device_set_version_raw(device, 0x0020);
	g_assert_cmpstr(fu_device_get_version(device), ==, "0.2.0");
	fu_device_set_version_raw(device, 0x1234);
	g_assert_cmpstr(fu_device_get_version(device), ==, "18.3.4");
}

static void
fu_framework_inputmodule_detach_func(gconstpointer user_data)
{
	g_autoptr(FwupdJsonParser) parser = fwupd_json_parser_new();
	g_autoptr(FwupdJsonNode) node = NULL;
	g_autoptr(FwupdJsonObject) object = NULL;
	g_autoptr(FuContext) ctx = g_object_new(FU_TYPE_CONTEXT, NULL);
	g_autoptr(FuDevice) device =
	    g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_DEVICE, "context", ctx, NULL);
	g_autoptr(FuDeviceEvent) ev_control =
	    fu_device_event_new("ControlTransfer:Direction=0x01,RequestType=0x01,Recipient=0x01,"
				"Request=0x22,Value=0x0003,Idx=0x0000,Data=,Length=0x0");
	g_autoptr(FuDeviceEvent) ev_bulk =
	    fu_device_event_new("BulkTransfer:Endpoint=0x01,Data=MqwCAA==,Length=0x4");
	g_autoptr(GError) error = NULL;
	const guint8 command[] = {0x32, 0xac, 0x02, 0x00};
	const gchar *json =
	    "{\"IdVendor\":12972,\"IdProduct\":32,\"Device\":32,\"UsbInterfaces\":[{\"Length\":9,"
	    "\"DescriptorType\":4,\"InterfaceNumber\":0,\"InterfaceClass\":2,\"InterfaceSubClass\":"
	    "2,\"InterfaceProtocol\":0},{\"Length\":9,\"DescriptorType\":4,\"InterfaceNumber\":1,"
	    "\"InterfaceClass\":10,\"UsbEndpoints\":[{\"DescriptorType\":5,\"EndpointAddress\":1,"
	    "\"MaxPacketSize\":64},{\"DescriptorType\":5,\"EndpointAddress\":130,\"MaxPacketSize\":"
	    "64}]}]}";

	gboolean ret;

	node = fwupd_json_parser_load_from_data(parser, json, FWUPD_JSON_LOAD_FLAG_NONE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(node);
	object = fwupd_json_node_get_object(node, &error);
	g_assert_no_error(error);
	g_assert_nonnull(object);
	ret = fu_device_from_json(device, object, &error);
	g_assert_no_error(error);
	g_assert_true(ret);
	fu_device_add_flag(device, FWUPD_DEVICE_FLAG_EMULATED);
	fu_device_set_backend_id(device, "/sys/devices/usb1/1-2");
	g_assert_true(fu_device_probe(device, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(fu_device_get_version(device), ==, "0.2.0");
	g_assert_cmpstr(fu_device_get_physical_id(device), ==, "DEVPATH=/devices/usb1/1-2");
	fu_device_event_set_data(ev_control, "Data", NULL, 0);
	fu_device_event_set_data(ev_bulk, "Data", command, GPOINTER_TO_UINT(user_data));
	fu_device_add_event(device, ev_control);
	fu_device_add_event(device, ev_bulk);
	ret = fu_device_detach(device, &error);
	if (GPOINTER_TO_UINT(user_data) == sizeof(command)) {
		g_assert_no_error(error);
		g_assert_true(ret);
		g_assert_true(fu_device_has_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));
	} else {
		g_assert_error(error, FWUPD_ERROR, FWUPD_ERROR_WRITE);
		g_assert_false(ret);
		g_assert_false(fu_device_has_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));
	}
}

static void
fu_framework_inputmodule_firmware_func(void)
{
	g_autoptr(FuStructFrameworkInputmoduleUf2) st = fu_struct_framework_inputmodule_uf2_new();
	g_autoptr(FuFirmware) firmware = g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE, NULL);
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GBytes) actual = NULL;
	g_autoptr(GError) error = NULL;

	fu_struct_framework_inputmodule_uf2_set_target_addr(st, 0x10000000);
	fu_struct_framework_inputmodule_uf2_set_num_blocks(st, 1);
	blob = g_bytes_new(st->buf->data, st->buf->len);
	g_assert_true(
	    fu_firmware_parse_bytes(firmware, blob, 0, FU_FIRMWARE_PARSE_FLAG_NONE, &error));
	g_assert_no_error(error);
	actual = fu_firmware_get_bytes(firmware, &error);
	g_assert_no_error(error);
	g_assert_true(g_bytes_equal(actual, blob));
}

static void
fu_framework_inputmodule_firmware_invalid_func(void)
{
	/* Each mutation must fail independently, including the serial sector. */
	const struct {
		guint offset;
		guint32 value;
	} cases[] = {
	    {0, 0},
	    {4, 0},
	    {8, 0},
	    {12, 0x100ff000},
	    {12, 0x10000001},
	    {12, 0x20000000},
	    {16, 512},
	    {20, 1},
	    {24, 2},
	    {28, 0},
	    {508, 0},
	};
	for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(FuStructFrameworkInputmoduleUf2) st =
		    fu_struct_framework_inputmodule_uf2_new();
		g_autoptr(FuFirmware) firmware =
		    g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE, NULL);
		g_autoptr(GBytes) blob = NULL;
		g_autoptr(GError) error = NULL;
		fu_struct_framework_inputmodule_uf2_set_target_addr(st, 0x10000000);
		fu_struct_framework_inputmodule_uf2_set_num_blocks(st, 1);
		fu_memwrite_uint32(st->buf->data + cases[i].offset,
				   cases[i].value,
				   G_LITTLE_ENDIAN);
		blob = g_bytes_new(st->buf->data, st->buf->len);
		g_assert_false(fu_firmware_parse_bytes(firmware,
						       blob,
						       0,
						       FU_FIRMWARE_PARSE_FLAG_NONE,
						       &error));
		g_assert_nonnull(error);
	}
	for (guint len = 0; len < 512; len += 511) {
		guint8 buf[512] = {0};
		g_autoptr(FuFirmware) firmware =
		    g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE, NULL);
		g_autoptr(GBytes) blob = g_bytes_new(buf, len);
		g_autoptr(GError) error = NULL;
		g_assert_false(fu_firmware_parse_bytes(firmware,
						       blob,
						       0,
						       FU_FIRMWARE_PARSE_FLAG_NONE,
						       &error));
		g_assert_nonnull(error);
	}
}

static void
fu_framework_inputmodule_firmware_boundary_func(void)
{
	/* fill every allowed flash page, ending immediately before the serial sector */
	g_autoptr(GByteArray) buf = g_byte_array_new();
	g_autoptr(FuStructFrameworkInputmoduleUf2) st = fu_struct_framework_inputmodule_uf2_new();
	g_autoptr(FuFirmware) firmware = g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE, NULL);
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GError) error = NULL;
	const guint32 n_blocks = 0xff000 / 256;

	for (guint32 i = 0; i < n_blocks; i++) {
		fu_struct_framework_inputmodule_uf2_set_target_addr(st, 0x10000000 + i * 256);
		fu_struct_framework_inputmodule_uf2_set_num_blocks(st, n_blocks);
		fu_struct_framework_inputmodule_uf2_set_block_no(st, i);
		g_byte_array_append(buf, st->buf->data, st->buf->len);
	}
	blob = g_bytes_new(buf->data, buf->len);
	g_assert_true(
	    fu_firmware_parse_bytes(firmware, blob, 0, FU_FIRMWARE_PARSE_FLAG_NONE, &error));
	g_assert_no_error(error);
	/* corrupt a later block to point inside the manufacturing sector */
	fu_memwrite_uint32(buf->data + 512 + 12, 0x100ff000, G_LITTLE_ENDIAN);
	g_clear_pointer(&blob, g_bytes_unref);
	blob = g_bytes_new(buf->data, buf->len);
	g_clear_object(&firmware);
	firmware = g_object_new(FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE, NULL);
	g_assert_false(
	    fu_firmware_parse_bytes(firmware, blob, 0, FU_FIRMWARE_PARSE_FLAG_NONE, &error));
	g_assert_error(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/framework-inputmodule/detach",
			     GUINT_TO_POINTER(4),
			     fu_framework_inputmodule_detach_func);
	g_test_add_data_func("/framework-inputmodule/detach/short",
			     GUINT_TO_POINTER(2),
			     fu_framework_inputmodule_detach_func);
	g_test_add_func("/framework-inputmodule/firmware/boundary",
			fu_framework_inputmodule_firmware_boundary_func);
	g_test_add_func("/framework-inputmodule/version", fu_framework_inputmodule_version_func);
	g_test_add_func("/framework-inputmodule/firmware", fu_framework_inputmodule_firmware_func);
	g_test_add_func("/framework-inputmodule/firmware/invalid",
			fu_framework_inputmodule_firmware_invalid_func);
	return g_test_run();
}
