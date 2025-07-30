/*
 * Copyright 2025 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_FRAMEWORK16_KEYBOARD_DEVICE (fu_framework16_keyboard_device_get_type())
G_DECLARE_FINAL_TYPE(FuFramework16KeyboardDevice,
		     fu_framework16_keyboard_device,
		     FU,
		     FRAMEWORK16_KEYBOARD_DEVICE,
		     FuHidrawDevice)
