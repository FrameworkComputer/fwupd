/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE (fu_framework_inputmodule_firmware_get_type())
G_DECLARE_FINAL_TYPE(FuFrameworkInputmoduleFirmware,
		     fu_framework_inputmodule_firmware,
		     FU,
		     FRAMEWORK_INPUTMODULE_FIRMWARE,
		     FuFirmware)
