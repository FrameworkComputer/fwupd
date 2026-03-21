/*
 * Copyright 2025 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuBackend"

#include "config.h"

#include <fwupdplugin.h>
#include <glob.h>

#include "fu-context-private.h"
#include "fu-hidraw-backend.h"
#include "fu-udev-device-private.h"

struct _FuHidrawBackend {
	FuBackend parent_instance;
};

G_DEFINE_TYPE(FuHidrawBackend, fu_hidraw_backend, FU_TYPE_BACKEND)

static gboolean
fu_hidraw_backend_coldplug(FuBackend *backend, FuProgress *progress, GError **error)
{
	FuContext *ctx = fu_backend_get_context(backend);
	glob_t gl = {0};

	if (glob("/dev/hidraw*", 0, NULL, &gl) != 0) {
		/* no hidraw devices found, not an error */
		return TRUE;
	}

	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, gl.gl_pathc);
	for (gsize i = 0; i < gl.gl_pathc; i++) {
		const gchar *device_file = gl.gl_pathv[i];
		g_autoptr(FuDevice) device = NULL;
		g_autoptr(GPtrArray) possible_plugins = NULL;

		device = g_object_new(FU_TYPE_HIDRAW_DEVICE,
				      "backend",
				      backend,
				      "context",
				      ctx,
				      "backend-id",
				      device_file,
				      NULL);
		fu_udev_device_set_device_file(FU_UDEV_DEVICE(device), device_file);
		fu_udev_device_set_subsystem(FU_UDEV_DEVICE(device), "hidraw");

		/* notify plugins using fu_plugin_add_udev_subsystem() */
		possible_plugins =
		    fu_context_get_plugin_names_for_udev_subsystem(ctx, "hidraw", NULL);
		if (possible_plugins != NULL) {
			for (guint j = 0; j < possible_plugins->len; j++) {
				const gchar *plugin_name =
				    g_ptr_array_index(possible_plugins, j);
				fu_device_add_possible_plugin(device, plugin_name);
			}
		}

		fu_backend_device_added(backend, device);
		fu_progress_step_done(progress);
	}

	globfree(&gl);
	return TRUE;
}

static FuDevice *
fu_hidraw_backend_create_device(FuBackend *backend, const gchar *backend_id, GError **error)
{
	FuContext *ctx = fu_backend_get_context(backend);
	g_autoptr(FuDevice) device = NULL;

	device = g_object_new(FU_TYPE_HIDRAW_DEVICE,
			      "backend",
			      backend,
			      "context",
			      ctx,
			      "backend-id",
			      backend_id,
			      NULL);
	fu_udev_device_set_device_file(FU_UDEV_DEVICE(device), backend_id);
	fu_udev_device_set_subsystem(FU_UDEV_DEVICE(device), "hidraw");
	return g_steal_pointer(&device);
}

static void
fu_hidraw_backend_init(FuHidrawBackend *self)
{
}

static void
fu_hidraw_backend_class_init(FuHidrawBackendClass *klass)
{
	FuBackendClass *backend_class = FU_BACKEND_CLASS(klass);
	backend_class->coldplug = fu_hidraw_backend_coldplug;
	backend_class->create_device = fu_hidraw_backend_create_device;
}

FuBackend *
fu_hidraw_backend_new(FuContext *ctx)
{
	return FU_BACKEND(g_object_new(FU_TYPE_HIDRAW_BACKEND,
				       "name",
				       "hidraw",
				       "context",
				       ctx,
				       "device-gtype",
				       FU_TYPE_HIDRAW_DEVICE,
				       NULL));
}
