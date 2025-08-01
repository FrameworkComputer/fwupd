/*
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fu-firmware.h"

#define FU_TYPE_COSWID_FIRMWARE (fu_coswid_firmware_get_type())
G_DECLARE_DERIVABLE_TYPE(FuCoswidFirmware, fu_coswid_firmware, FU, COSWID_FIRMWARE, FuFirmware)

struct _FuCoswidFirmwareClass {
	FuFirmwareClass parent_class;
};

FuFirmware *
fu_coswid_firmware_new(void);
const gchar *
fu_coswid_firmware_get_product(FuCoswidFirmware *self) G_GNUC_NON_NULL(1);
