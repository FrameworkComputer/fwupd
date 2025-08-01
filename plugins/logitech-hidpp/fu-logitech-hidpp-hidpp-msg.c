/*
 * Copyright 2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>

#include "fu-logitech-hidpp-hidpp-msg.h"
#include "fu-logitech-hidpp-hidpp.h"
#include "fu-logitech-hidpp-struct.h"

FuLogitechHidppHidppMsg *
fu_logitech_hidpp_msg_new(void)
{
	return g_new0(FuLogitechHidppHidppMsg, 1);
}

gsize
fu_logitech_hidpp_msg_get_payload_length(FuLogitechHidppHidppMsg *msg)
{
	if (msg->report_id == FU_LOGITECH_HIDPP_REPORT_ID_SHORT)
		return 0x07;
	if (msg->report_id == FU_LOGITECH_HIDPP_REPORT_ID_LONG)
		return 0x14;
	if (msg->report_id == FU_LOGITECH_HIDPP_REPORT_ID_VERY_LONG)
		return 0x2f;
	if (msg->report_id == HIDPP_REPORT_NOTIFICATION)
		return 0x08;
	return 0x0;
}

const gchar *
fu_logitech_hidpp_msg_fcn_id_to_string(FuLogitechHidppHidppMsg *msg)
{
	g_return_val_if_fail(msg != NULL, NULL);
	switch (msg->sub_id) {
	case FU_LOGITECH_HIDPP_SUBID_SET_REGISTER:
	case FU_LOGITECH_HIDPP_SUBID_GET_REGISTER:
	case FU_LOGITECH_HIDPP_SUBID_SET_LONG_REGISTER:
	case FU_LOGITECH_HIDPP_SUBID_GET_LONG_REGISTER:
	case FU_LOGITECH_HIDPP_SUBID_SET_VERY_LONG_REGISTER:
	case FU_LOGITECH_HIDPP_SUBID_GET_VERY_LONG_REGISTER:
		return fu_logitech_hidpp_register_to_string(msg->function_id);
		break;
	default:
		break;
	}
	return NULL;
}

gboolean
fu_logitech_hidpp_msg_is_reply(FuLogitechHidppHidppMsg *msg1, FuLogitechHidppHidppMsg *msg2)
{
	g_return_val_if_fail(msg1 != NULL, FALSE);
	g_return_val_if_fail(msg2 != NULL, FALSE);
	if (msg1->device_id != msg2->device_id &&
	    msg1->device_id != FU_LOGITECH_HIDPP_DEVICE_IDX_WIRED &&
	    msg2->device_id != FU_LOGITECH_HIDPP_DEVICE_IDX_WIRED)
		return FALSE;
	if (msg1->flags & FU_LOGITECH_HIDPP_HIDPP_MSG_FLAG_IGNORE_SUB_ID ||
	    msg2->flags & FU_LOGITECH_HIDPP_HIDPP_MSG_FLAG_IGNORE_SUB_ID)
		return TRUE;
	if (msg1->sub_id != msg2->sub_id)
		return FALSE;
	if (msg1->flags & FU_LOGITECH_HIDPP_HIDPP_MSG_FLAG_IGNORE_FNCT_ID ||
	    msg2->flags & FU_LOGITECH_HIDPP_HIDPP_MSG_FLAG_IGNORE_FNCT_ID)
		return TRUE;
	if (msg1->function_id != msg2->function_id)
		return FALSE;
	return TRUE;
}

/* HID++ error */
gboolean
fu_logitech_hidpp_msg_is_error(FuLogitechHidppHidppMsg *msg, GError **error)
{
	g_return_val_if_fail(msg != NULL, FALSE);
	if (msg->sub_id == FU_LOGITECH_HIDPP_SUBID_ERROR_MSG) {
		const gchar *str = fu_logitech_hidpp_err_to_string(msg->data[1]);
		const FuErrorMapEntry entries[] = {
		    {FU_LOGITECH_HIDPP_ERR_INVALID_SUBID, FWUPD_ERROR_NOT_SUPPORTED, str},
		    {FU_LOGITECH_HIDPP_ERR_TOO_MANY_DEVICES, FWUPD_ERROR_NOT_SUPPORTED, str},
		    {FU_LOGITECH_HIDPP_ERR_REQUEST_UNAVAILABLE, FWUPD_ERROR_NOT_SUPPORTED, str},
		    {FU_LOGITECH_HIDPP_ERR_INVALID_ADDRESS, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR_INVALID_VALUE, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR_ALREADY_EXISTS, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR_INVALID_PARAM_VALUE, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR_CONNECT_FAIL, FWUPD_ERROR_INTERNAL, str},
		    {FU_LOGITECH_HIDPP_ERR_BUSY, FWUPD_ERROR_BUSY, str},
		    {FU_LOGITECH_HIDPP_ERR_UNKNOWN_DEVICE, FWUPD_ERROR_NOT_FOUND, str},
		    {FU_LOGITECH_HIDPP_ERR_RESOURCE_ERROR, FWUPD_ERROR_NOT_FOUND, str},
		    {FU_LOGITECH_HIDPP_ERR_WRONG_PIN_CODE, FWUPD_ERROR_AUTH_FAILED, str},
		};
		return fu_error_map_entry_to_gerror(msg->data[1],
						    entries,
						    G_N_ELEMENTS(entries),
						    error);
	}
	if (msg->sub_id == FU_LOGITECH_HIDPP_SUBID_ERROR_MSG_20) {
		const gchar *str = fu_logitech_hidpp_err2_to_string(msg->data[1]);
		const FuErrorMapEntry entries[] = {
		    {FU_LOGITECH_HIDPP_ERR2_INVALID_ARGUMENT, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR2_OUT_OF_RANGE, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR2_HW_ERROR, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR2_INVALID_FEATURE_INDEX, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR2_INVALID_FUNCTION_ID, FWUPD_ERROR_INVALID_DATA, str},
		    {FU_LOGITECH_HIDPP_ERR2_BUSY, FWUPD_ERROR_BUSY, str},
		    {FU_LOGITECH_HIDPP_ERR2_UNSUPPORTED, FWUPD_ERROR_NOT_SUPPORTED, str},
		};
		return fu_error_map_entry_to_gerror(msg->data[1],
						    entries,
						    G_N_ELEMENTS(entries),
						    error);
	}
	return TRUE;
}

void
fu_logitech_hidpp_msg_copy(FuLogitechHidppHidppMsg *msg_dst, const FuLogitechHidppHidppMsg *msg_src)
{
	g_return_if_fail(msg_dst != NULL);
	g_return_if_fail(msg_src != NULL);
	memset(msg_dst->data, 0x00, sizeof(msg_dst->data));
	msg_dst->device_id = msg_src->device_id;
	msg_dst->sub_id = msg_src->sub_id;
	msg_dst->function_id = msg_src->function_id;
	memcpy(msg_dst->data, msg_src->data, sizeof(msg_dst->data)); /* nocheck:blocked */
}

/* filter HID++1.0 messages */
gboolean
fu_logitech_hidpp_msg_is_hidpp10_compat(FuLogitechHidppHidppMsg *msg)
{
	g_return_val_if_fail(msg != NULL, FALSE);
	if (msg->sub_id == 0x40 || msg->sub_id == 0x41 || msg->sub_id == 0x49 ||
	    msg->sub_id == 0x4b || msg->sub_id == 0x8f) {
		return TRUE;
	}
	return FALSE;
}

gboolean
fu_logitech_hidpp_msg_verify_swid(FuLogitechHidppHidppMsg *msg)
{
	g_return_val_if_fail(msg != NULL, FALSE);
	if ((msg->function_id & 0x0f) != FU_LOGITECH_HIDPP_HIDPP_MSG_SW_ID)
		return FALSE;
	return TRUE;
}
