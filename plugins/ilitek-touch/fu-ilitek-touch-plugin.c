/*
 * Copyright 2025 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-ilitek-touch-device.h"
#include "fu-ilitek-touch-plugin.h"

struct _FuIlitekTouchPlugin {
	FuPlugin parent_instance;
};

G_DEFINE_TYPE(FuIlitekTouchPlugin, fu_ilitek_touch_plugin, FU_TYPE_PLUGIN)

static void
fu_ilitek_touch_plugin_init(FuIlitekTouchPlugin *self)
{
}

static void
fu_ilitek_touch_plugin_constructed(GObject *obj)
{
	FuPlugin *plugin = FU_PLUGIN(obj);
	fu_plugin_add_device_gtype(plugin, FU_TYPE_ILITEK_TOUCH_DEVICE);
	fu_plugin_add_udev_subsystem(plugin, "hidraw");
}

static void
fu_ilitek_touch_plugin_class_init(FuIlitekTouchPluginClass *klass)
{
	FuPluginClass *plugin_class = FU_PLUGIN_CLASS(klass);
	plugin_class->constructed = fu_ilitek_touch_plugin_constructed;
}
