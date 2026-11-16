/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_FRAMEWORK_INPUTMODULE_DEVICE (fu_framework_inputmodule_device_get_type())
G_DECLARE_FINAL_TYPE(FuFrameworkInputmoduleDevice,
		     fu_framework_inputmodule_device,
		     FU,
		     FRAMEWORK_INPUTMODULE_DEVICE,
		     FuUsbDevice)
