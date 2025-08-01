/*
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fu-logitech-hidpp-runtime.h"

#define FU_TYPE_LOGITECH_HIDPP_RUNTIME_UNIFYING (fu_logitech_hidpp_runtime_unifying_get_type())
G_DECLARE_FINAL_TYPE(FuLogitechHidppRuntimeUnifying,
		     fu_logitech_hidpp_runtime_unifying,
		     FU,
		     LOGITECH_HIDPP_RUNTIME_UNIFYING,
		     FuLogitechHidppRuntime)
