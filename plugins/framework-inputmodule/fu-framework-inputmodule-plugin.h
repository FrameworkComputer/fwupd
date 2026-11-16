/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_FRAMEWORK_INPUTMODULE_PLUGIN (fu_framework_inputmodule_plugin_get_type())
G_DECLARE_FINAL_TYPE(FuFrameworkInputmodulePlugin,
		     fu_framework_inputmodule_plugin,
		     FU,
		     FRAMEWORK_INPUTMODULE_PLUGIN,
		     FuPlugin)
