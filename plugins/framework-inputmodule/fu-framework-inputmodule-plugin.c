/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-framework-inputmodule-device.h"
#include "fu-framework-inputmodule-firmware.h"
#include "fu-framework-inputmodule-plugin.h"

struct _FuFrameworkInputmodulePlugin {
	FuPlugin parent_instance;
};

G_DEFINE_TYPE(FuFrameworkInputmodulePlugin, fu_framework_inputmodule_plugin, FU_TYPE_PLUGIN)

static void
fu_framework_inputmodule_plugin_init(FuFrameworkInputmodulePlugin *self)
{
	fu_plugin_add_flag(FU_PLUGIN(self), FWUPD_PLUGIN_FLAG_MUTABLE_ENUMERATION);
}

static void
fu_framework_inputmodule_plugin_constructed(GObject *obj)
{
	FuPlugin *plugin = FU_PLUGIN(obj);
	fu_plugin_add_udev_subsystem(plugin, "usb");
	fu_plugin_add_device_gtype(plugin, FU_TYPE_FRAMEWORK_INPUTMODULE_DEVICE);
	fu_plugin_add_firmware_gtype(plugin, FU_TYPE_FRAMEWORK_INPUTMODULE_FIRMWARE);

	/* chain up to parent */
	G_OBJECT_CLASS(fu_framework_inputmodule_plugin_parent_class)->constructed(obj);
}

static void
fu_framework_inputmodule_plugin_class_init(FuFrameworkInputmodulePluginClass *klass)
{
	FuPluginClass *plugin_class = FU_PLUGIN_CLASS(klass);
	plugin_class->constructed = fu_framework_inputmodule_plugin_constructed;
}
