/*
 * Copyright 2025 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-framework16-keyboard-device.h"
#include "fu-framework16-keyboard-plugin.h"

struct _FuFramework16KeyboardPlugin {
	FuPlugin parent_instance;
};

G_DEFINE_TYPE(FuFramework16KeyboardPlugin, fu_framework16_keyboard_plugin, FU_TYPE_PLUGIN)

static void
fu_framework16_keyboard_plugin_init(FuFramework16KeyboardPlugin *self)
{
	fu_plugin_add_flag(FU_PLUGIN(self), FWUPD_PLUGIN_FLAG_MUTABLE_ENUMERATION);
}

static void
fu_framework16_keyboard_plugin_constructed(GObject *obj)
{
	FuPlugin *plugin = FU_PLUGIN(obj);
	fu_plugin_add_device_gtype(plugin, FU_TYPE_FRAMEWORK16_KEYBOARD_DEVICE);
	fu_plugin_add_udev_subsystem(plugin, "hidraw");
}

static void
fu_framework16_keyboard_plugin_class_init(FuFramework16KeyboardPluginClass *klass)
{
	FuPluginClass *plugin_class = FU_PLUGIN_CLASS(klass);
	plugin_class->constructed = fu_framework16_keyboard_plugin_constructed;
}
