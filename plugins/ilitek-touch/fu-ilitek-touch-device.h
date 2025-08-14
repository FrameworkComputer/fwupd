/*
 * Copyright 2025 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_ILITEK_TOUCH_DEVICE (fu_ilitek_touch_device_get_type())
G_DECLARE_FINAL_TYPE(FuIlitekTouchDevice,
		     fu_ilitek_touch_device,
		     FU,
		     ILITEK_TOUCH_DEVICE,
		     FuHidrawDevice)
