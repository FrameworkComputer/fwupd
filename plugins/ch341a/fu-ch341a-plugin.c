/*
 * Copyright 2022 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-ch341a-device.h"
#include "fu-ch341a-plugin.h"

struct _FuCh341aPlugin {
	FuPlugin parent_instance;
};

G_DEFINE_TYPE(FuCh341aPlugin, fu_ch341a_plugin, FU_TYPE_PLUGIN)

static void
fu_ch341a_plugin_init(FuCh341aPlugin *self)
{
}

static void
fu_ch341a_plugin_object_constructed(GObject *obj)
{
	FuPlugin *plugin = FU_PLUGIN(obj);
	fu_plugin_set_name(plugin, "ch341a");
}

static void
fu_ch341a_plugin_constructed(GObject *obj)
{
	FuPlugin *plugin = FU_PLUGIN(obj);
	fu_plugin_add_device_gtype(plugin, FU_TYPE_CH341A_DEVICE);
}

static void
fu_ch341a_plugin_class_init(FuCh341aPluginClass *klass)
{
	FuPluginClass *plugin_class = FU_PLUGIN_CLASS(klass);
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->constructed = fu_ch341a_plugin_object_constructed;
	plugin_class->constructed = fu_ch341a_plugin_constructed;
}
