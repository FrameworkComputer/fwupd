/*
 * Copyright 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuDevice"

#include "config.h"

#include <gio/gio.h>
#include <string.h>

#include "fwupd-common.h"
#include "fwupd-device-private.h"
#include "fwupd-enums-private.h"

#include "fu-byte-array.h"
#include "fu-bytes.h"
#include "fu-chunk-array.h"
#include "fu-device-event-private.h"
#include "fu-device-private.h"
#include "fu-input-stream.h"
#include "fu-output-stream.h"
#include "fu-quirks.h"
#include "fu-security-attr.h"
#include "fu-string.h"
#include "fu-version-common.h"

#define FU_DEVICE_RETRY_OPEN_COUNT 5
#define FU_DEVICE_RETRY_OPEN_DELAY 500 /* ms */

/**
 * FuDevice:
 *
 * A physical or logical device that is exported to the daemon.
 *
 * See also: [class@FuDeviceLocker], [class@Fwupd.Device]
 */

static void
fu_device_finalize(GObject *object);
static void
fu_device_inhibit_full(FuDevice *self,
		       FwupdDeviceProblem problem,
		       const gchar *inhibit_id,
		       const gchar *reason);

typedef struct {
	gchar *equivalent_id;
	gchar *physical_id;
	gchar *logical_id;
	gchar *backend_id;
	gchar *update_request_id;
	gchar *update_message;
	gchar *update_image;
	gchar *fwupd_version;
	gchar *proxy_guid;
	FuDevice *proxy;    /* noref */
	FuDevice *target;   /* ref */
	FuBackend *backend; /* noref */
	FuContext *ctx;
	gint64 created_usec;
	gint64 modified_usec;
	guint16 vid;
	guint16 pid;
	GHashTable *inhibits;		/* (nullable) */
	GHashTable *metadata;		/* (nullable) */
	GPtrArray *parent_guids;	/* (nullable) (element-type utf-8) */
	GPtrArray *parent_physical_ids; /* (nullable) */
	GPtrArray *parent_backend_ids;	/* (nullable) */
	GPtrArray *events;		/* (nullable) (element-type FuDeviceEvent) */
	guint event_idx;
	guint remove_delay;    /* ms */
	guint acquiesce_delay; /* ms */
	guint request_cnts[FWUPD_REQUEST_KIND_LAST];
	gint order;
	guint priority;
	guint poll_id;
	gint poll_locker_cnt;
	gboolean done_probe;
	gboolean done_setup;
	gboolean device_id_valid;
	guint64 size_min;
	guint64 size_max;
	guint64 required_free; /* bytes */
	gint open_refcount;    /* atomic */
	GType specialized_gtype;
	GType proxy_gtype;
	GType firmware_gtype;
	GPtrArray *possible_plugins; /* (element-type utf-8) */
	GPtrArray *instance_ids;     /* (nullable) (element-type FuDeviceInstanceIdItem) */
	GPtrArray *retry_recs;	     /* (nullable) (element-type FuDeviceRetryRecovery) */
	guint retry_delay;
	GArray *private_flags_registered; /* (nullable) (element-type GQuark) */
	GArray *private_flags;		  /* (nullable) (element-type GQuark) */
	gchar *custom_flags;
	gulong notify_flags_proxy_id;
	GHashTable *instance_hash; /* (nullable) */
	FuProgress *progress;	   /* provided for FuDevice notify callbacks */
} FuDevicePrivate;

typedef struct {
	GQuark domain;
	gint code;
	FuDeviceRetryFunc recovery_func;
} FuDeviceRetryRecovery;

typedef struct {
	FwupdDeviceProblem problem;
	gchar *inhibit_id;
	gchar *reason;
} FuDeviceInhibit;

typedef struct {
	gchar *instance_id;
	gchar *guid;
	FuDeviceInstanceFlags flags;
} FuDeviceInstanceIdItem;

enum {
	PROP_0,
	PROP_PHYSICAL_ID,
	PROP_LOGICAL_ID,
	PROP_BACKEND_ID,
	PROP_EQUIVALENT_ID,
	PROP_UPDATE_MESSAGE,
	PROP_UPDATE_IMAGE,
	PROP_CONTEXT,
	PROP_BACKEND,
	PROP_PROXY,
	PROP_PARENT,
	PROP_PRIVATE_FLAGS,
	PROP_VID,
	PROP_PID,
	PROP_REQUIRED_FREE,
	PROP_LAST
};

enum { SIGNAL_CHILD_ADDED, SIGNAL_CHILD_REMOVED, SIGNAL_REQUEST, SIGNAL_LAST };

static guint signals[SIGNAL_LAST] = {0};

/* ordered for query speed; most frequent first */
enum {
	QUARK_NO_PROBE,
	QUARK_REFCOUNTED_PROXY,
	QUARK_NO_GENERIC_GUIDS,
	QUARK_NO_SERIAL_NUMBER,
	QUARK_IS_FAKE,
	QUARK_LAST,
};

static guint quarks[QUARK_LAST] = {0};

G_DEFINE_TYPE_WITH_PRIVATE(FuDevice, fu_device, FWUPD_TYPE_DEVICE);

#define GET_PRIVATE(o) (fu_device_get_instance_private(o))

static void
fu_device_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	FuDevice *self = FU_DEVICE(object);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	switch (prop_id) {
	case PROP_PHYSICAL_ID:
		g_value_set_string(value, priv->physical_id);
		break;
	case PROP_LOGICAL_ID:
		g_value_set_string(value, priv->logical_id);
		break;
	case PROP_BACKEND_ID:
		g_value_set_string(value, priv->backend_id);
		break;
	case PROP_EQUIVALENT_ID:
		g_value_set_string(value, priv->equivalent_id);
		break;
	case PROP_UPDATE_MESSAGE:
		g_value_set_string(value, priv->update_message);
		break;
	case PROP_UPDATE_IMAGE:
		g_value_set_string(value, priv->update_image);
		break;
	case PROP_CONTEXT:
		g_value_set_object(value, priv->ctx);
		break;
	case PROP_BACKEND:
		g_value_set_object(value, priv->backend);
		break;
	case PROP_PROXY:
		g_value_set_object(value, priv->proxy);
		break;
	case PROP_PARENT:
		g_value_set_object(value, fu_device_get_parent(self));
		break;
	case PROP_PRIVATE_FLAGS:
		g_value_set_uint64(value, priv->private_flags->len);
		break;
	case PROP_VID:
		g_value_set_uint(value, priv->vid);
		break;
	case PROP_PID:
		g_value_set_uint(value, priv->pid);
		break;
	case PROP_REQUIRED_FREE:
		g_value_set_uint64(value, priv->required_free);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_device_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	FuDevice *self = FU_DEVICE(object);
	switch (prop_id) {
	case PROP_PHYSICAL_ID:
		fu_device_set_physical_id(self, g_value_get_string(value));
		break;
	case PROP_LOGICAL_ID:
		fu_device_set_logical_id(self, g_value_get_string(value));
		break;
	case PROP_BACKEND_ID:
		fu_device_set_backend_id(self, g_value_get_string(value));
		break;
	case PROP_EQUIVALENT_ID:
		fu_device_set_equivalent_id(self, g_value_get_string(value));
		break;
	case PROP_UPDATE_MESSAGE:
		fu_device_set_update_message(self, g_value_get_string(value));
		break;
	case PROP_UPDATE_IMAGE:
		fu_device_set_update_image(self, g_value_get_string(value));
		break;
	case PROP_CONTEXT:
		fu_device_set_context(self, g_value_get_object(value));
		break;
	case PROP_BACKEND:
		fu_device_set_backend(self, g_value_get_object(value));
		break;
	case PROP_PROXY:
		fu_device_set_proxy(self, g_value_get_object(value));
		break;
	case PROP_PARENT:
		fu_device_set_parent(self, g_value_get_object(value));
		break;
	case PROP_VID:
		fu_device_set_vid(self, g_value_get_uint(value));
		break;
	case PROP_PID:
		fu_device_set_pid(self, g_value_get_uint(value));
		break;
	case PROP_REQUIRED_FREE:
		fu_device_set_required_free(self, g_value_get_uint64(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_device_register_private_flags(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	const gchar *flags[] = {
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_ICON,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME_CATEGORY,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_VERFMT,
	    FU_DEVICE_PRIVATE_FLAG_ONLY_SUPPORTED,
	    FU_DEVICE_PRIVATE_FLAG_NO_AUTO_INSTANCE_IDS,
	    FU_DEVICE_PRIVATE_FLAG_ENSURE_SEMVER,
	    FU_DEVICE_PRIVATE_FLAG_RETRY_OPEN,
	    FU_DEVICE_PRIVATE_FLAG_REPLUG_MATCH_GUID,
	    FU_DEVICE_PRIVATE_FLAG_INHERIT_ACTIVATION,
	    FU_DEVICE_PRIVATE_FLAG_IS_OPEN,
	    FU_DEVICE_PRIVATE_FLAG_AUTO_PARENT_CHILDREN,
	    FU_DEVICE_PRIVATE_FLAG_ATTACH_EXTRA_RESET,
	    FU_DEVICE_PRIVATE_FLAG_INHIBIT_CHILDREN,
	    FU_DEVICE_PRIVATE_FLAG_NO_AUTO_REMOVE_CHILDREN,
	    FU_DEVICE_PRIVATE_FLAG_USE_PARENT_FOR_OPEN,
	    FU_DEVICE_PRIVATE_FLAG_USE_PROXY_FOR_OPEN,
	    FU_DEVICE_PRIVATE_FLAG_USE_PARENT_FOR_BATTERY,
	    FU_DEVICE_PRIVATE_FLAG_USE_PROXY_FALLBACK,
	    FU_DEVICE_PRIVATE_FLAG_NO_AUTO_REMOVE,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_VENDOR,
	    FU_DEVICE_PRIVATE_FLAG_NO_LID_CLOSED,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_SIGNED,
	    FU_DEVICE_PRIVATE_FLAG_AUTO_PAUSE_POLLING,
	    FU_DEVICE_PRIVATE_FLAG_DELAYED_REMOVAL,
	    FU_DEVICE_PRIVATE_FLAG_IGNORE_SYSTEM_POWER,
	    FU_DEVICE_PRIVATE_FLAG_SAVE_INTO_BACKUP_REMOTE,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_FLAGS,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_VERSION,
	    FU_DEVICE_PRIVATE_FLAG_MD_ONLY_CHECKSUM,
	    FU_DEVICE_PRIVATE_FLAG_ADD_INSTANCE_ID_REV,
	    FU_DEVICE_PRIVATE_FLAG_UNCONNECTED,
	    FU_DEVICE_PRIVATE_FLAG_DISPLAY_REQUIRED,
	    FU_DEVICE_PRIVATE_FLAG_UPDATE_PENDING,
	    FU_DEVICE_PRIVATE_FLAG_ENFORCE_REQUIRES,
	    FU_DEVICE_PRIVATE_FLAG_HOST_FIRMWARE,
	    FU_DEVICE_PRIVATE_FLAG_HOST_FIRMWARE_CHILD,
	    FU_DEVICE_PRIVATE_FLAG_HOST_CPU,
	    FU_DEVICE_PRIVATE_FLAG_HOST_CPU_CHILD,
	    FU_DEVICE_PRIVATE_FLAG_EXPLICIT_ORDER,
	    FU_DEVICE_PRIVATE_FLAG_INSTALL_PARENT_FIRST,
	    FU_DEVICE_PRIVATE_FLAG_REGISTERED,
	    FU_DEVICE_PRIVATE_FLAG_ADD_COUNTERPART_GUIDS,
	    FU_DEVICE_PRIVATE_FLAG_USE_RUNTIME_VERSION,
	    FU_DEVICE_PRIVATE_FLAG_SKIPS_RESTART,
	    FU_DEVICE_PRIVATE_FLAG_COUNTERPART_VISIBLE,
	    FU_DEVICE_PRIVATE_FLAG_DETACH_PREPARE_FIRMWARE,
	    FU_DEVICE_PRIVATE_FLAG_EMULATED_REQUIRE_SETUP,
	    FU_DEVICE_PRIVATE_FLAG_INSTALL_LOOP_RESTART,
	    FU_DEVICE_PRIVATE_FLAG_MD_SET_REQUIRED_FREE,
	};
	GQuark quarks_tmp[G_N_ELEMENTS(flags)] = {0};
	if (G_LIKELY(priv->private_flags_registered->len > 0))
		return;
	for (guint i = 0; i < G_N_ELEMENTS(flags); i++)
		quarks_tmp[i] = g_quark_from_static_string(flags[i]);
	g_array_append_vals(priv->private_flags_registered, quarks, G_N_ELEMENTS(quarks));
	g_array_append_vals(priv->private_flags_registered, quarks_tmp, G_N_ELEMENTS(quarks_tmp));
}

/* private */
gboolean
fu_device_has_private_flag_quark(FuDevice *self, GQuark flag_quark)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	for (guint i = 0; i < priv->private_flags->len; i++) {
		GQuark flag_tmp = g_array_index(priv->private_flags, GQuark, i);
		if (flag_quark == flag_tmp)
			return TRUE;
	}
	return FALSE;
}

static gboolean
fu_device_private_flags_has_registered_quark(FuDevice *self, GQuark flag_quark)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	for (guint i = 0; i < priv->private_flags_registered->len; i++) {
		GQuark flag_tmp = g_array_index(priv->private_flags_registered, GQuark, i);
		if (flag_quark == flag_tmp)
			return TRUE;
	}
	return FALSE;
}

static gboolean
fu_device_add_private_flag_quark(FuDevice *self, GQuark flag_quark)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	if (fu_device_has_private_flag_quark(self, flag_quark))
		return FALSE;
	g_array_append_val(priv->private_flags, flag_quark);
	return TRUE;
}

static gboolean
fu_device_remove_private_flag_quark(FuDevice *self, GQuark flag_quark)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	for (guint i = 0; i < priv->private_flags->len; i++) {
		GQuark flag_tmp = g_array_index(priv->private_flags, GQuark, i);
		if (flag_quark == flag_tmp) {
			g_array_remove_index(priv->private_flags, i);
			return TRUE;
		}
	}
	return FALSE;
}

static GQuark
fu_device_find_private_flag_quark(FuDevice *self, const gchar *flag)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	GQuark flag_quark = g_quark_from_string(flag);
	for (guint i = 0; i < priv->private_flags_registered->len; i++) {
		GQuark flag_tmp = g_array_index(priv->private_flags_registered, GQuark, i);
		if (flag_quark == flag_tmp)
			return flag_tmp;
	}
	return 0;
}

/**
 * fu_device_add_private_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Adds a private flag that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
void
fu_device_add_private_flag(FuDevice *self, const gchar *flag)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	GQuark flag_quark;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(flag != NULL);

	/* ensure */
	fu_device_register_private_flags(self);

	/* do not let devices be updated until re-connected */
	if (g_strcmp0(flag, FU_DEVICE_PRIVATE_FLAG_UNCONNECTED) == 0)
		fu_device_inhibit(self, "unconnected", "Device has been removed");

	/* add counterpart GUIDs already added */
	if (g_strcmp0(flag, FU_DEVICE_PRIVATE_FLAG_COUNTERPART_VISIBLE) == 0) {
		for (guint i = 0; priv->instance_ids != NULL && i < priv->instance_ids->len; i++) {
			FuDeviceInstanceIdItem *item = g_ptr_array_index(priv->instance_ids, i);
			if (item->flags & FU_DEVICE_INSTANCE_FLAG_COUNTERPART)
				item->flags |= FU_DEVICE_INSTANCE_FLAG_VISIBLE;
		}
	}

	/* reset this back to the default */
	if (g_strcmp0(flag, FU_DEVICE_PRIVATE_FLAG_EXPLICIT_ORDER) == 0) {
		GPtrArray *children = fu_device_get_children(self);
		for (guint i = 0; i < children->len; i++) {
			FuDevice *child_tmp = g_ptr_array_index(children, i);
			fu_device_add_private_flag(child_tmp,
						   FU_DEVICE_PRIVATE_FLAG_EXPLICIT_ORDER);
		}
		fu_device_set_order(self, G_MAXINT);
	}

	/* check exists */
	flag_quark = fu_device_find_private_flag_quark(self, flag);
	if (G_UNLIKELY(flag_quark == 0)) {
#ifndef SUPPORTED_BUILD
		g_critical("%s flag %s is unknown -- use fu_device_register_private_flag()",
			   G_OBJECT_TYPE_NAME(self),
			   flag);
#endif
		return;
	}
	if (fu_device_add_private_flag_quark(self, flag_quark))
		g_object_notify(G_OBJECT(self), "private-flags");
}

/**
 * fu_device_remove_private_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Removes a private flag that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
void
fu_device_remove_private_flag(FuDevice *self, const gchar *flag)
{
	GQuark flag_quark;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(flag != NULL);

	/* ensure */
	fu_device_register_private_flags(self);

	if (g_strcmp0(flag, FU_DEVICE_PRIVATE_FLAG_UNCONNECTED) == 0)
		fu_device_uninhibit(self, "unconnected");

	flag_quark = fu_device_find_private_flag_quark(self, flag);
	if (G_UNLIKELY(flag_quark == 0)) {
#ifndef SUPPORTED_BUILD
		g_critical("%s flag %s is unknown -- use fu_device_register_private_flag()",
			   G_OBJECT_TYPE_NAME(self),
			   flag);
#endif
		return;
	}
	if (fu_device_remove_private_flag_quark(self, flag_quark))
		g_object_notify(G_OBJECT(self), "private-flags");
}

/**
 * fu_device_has_private_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Tests for a private flag that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
gboolean
fu_device_has_private_flag(FuDevice *self, const gchar *flag)
{
	GQuark flag_quark;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(flag != NULL, FALSE);

	/* ensure */
	fu_device_register_private_flags(self);

	flag_quark = fu_device_find_private_flag_quark(self, flag);
	if (G_UNLIKELY(flag_quark == 0)) {
#ifndef SUPPORTED_BUILD
		g_critical("%s flag %s is unknown -- use fu_device_register_private_flag()",
			   G_OBJECT_TYPE_NAME(self),
			   flag);
#endif
		return FALSE;
	}
	return fu_device_has_private_flag_quark(self, flag_quark);
}

/**
 * fu_device_get_request_cnt:
 * @self: a #FuDevice
 * @request_kind: the type of request
 *
 * Returns the number of requests of a specific kind. This function is only
 * useful to the daemon, which uses it to synthesize artificial events for
 * plugins not yet ported to [class@Fwupd.Request].
 *
 * Returns: integer, usually 0
 *
 * Since: 1.6.2
 **/
guint
fu_device_get_request_cnt(FuDevice *self, FwupdRequestKind request_kind)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), G_MAXUINT);
	g_return_val_if_fail(request_kind < FWUPD_REQUEST_KIND_LAST, G_MAXUINT);
	return priv->request_cnts[request_kind];
}

/**
 * fu_device_get_possible_plugins:
 * @self: a #FuDevice
 *
 * Gets the list of possible plugin names, typically added from quirk files.
 *
 * Returns: (element-type utf8) (transfer container): plugin names
 *
 * Since: 1.3.3
 **/
GPtrArray *
fu_device_get_possible_plugins(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	return g_ptr_array_ref(priv->possible_plugins);
}

/**
 * fu_device_add_possible_plugin:
 * @self: a #FuDevice
 * @plugin: a plugin name, e.g. `dfu`
 *
 * Adds a plugin name to the list of plugins that *might* be able to handle this
 * device. This is typically called from a quirk handler.
 *
 * Duplicate plugin names are ignored.
 *
 * Since: 1.5.1
 **/
void
fu_device_add_possible_plugin(FuDevice *self, const gchar *plugin)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(plugin != NULL);

	/* add if it does not already exist */
	if (g_ptr_array_find_with_equal_func(priv->possible_plugins, plugin, g_str_equal, NULL))
		return;
	g_ptr_array_add(priv->possible_plugins, g_strdup(plugin));
}

/**
 * fu_device_retry_add_recovery:
 * @self: a #FuDevice
 * @domain: a #GQuark, or %0 for all domains
 * @code: a #GError code
 * @func: (scope async) (nullable): a function to recover the device
 *
 * Sets the optional function to be called when fu_device_retry() fails, which
 * is possibly a device reset.
 *
 * If @func is %NULL then recovery is not possible and an error is returned
 * straight away.
 *
 * Since: 1.4.0
 **/
void
fu_device_retry_add_recovery(FuDevice *self, GQuark domain, gint code, FuDeviceRetryFunc func)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceRetryRecovery *rec;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(domain != 0);

	/* ensure */
	if (priv->retry_recs == NULL)
		priv->retry_recs = g_ptr_array_new_with_free_func(g_free);

	rec = g_new(FuDeviceRetryRecovery, 1);
	rec->domain = domain;
	rec->code = code;
	rec->recovery_func = func;
	g_ptr_array_add(priv->retry_recs, rec);
}

/**
 * fu_device_retry_set_delay:
 * @self: a #FuDevice
 * @delay: delay in ms
 *
 * Sets the recovery delay between failed retries.
 *
 * Since: 1.4.0
 **/
void
fu_device_retry_set_delay(FuDevice *self, guint delay)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->retry_delay = delay;
}

/**
 * fu_device_retry_full:
 * @self: a #FuDevice
 * @func: (scope async) (closure user_data): a function to execute
 * @count: the number of tries to try the function
 * @delay: the delay between each try in ms
 * @user_data: (nullable): a helper to pass to @func
 * @error: (nullable): optional return location for an error
 *
 * Calls a specific function a number of times, optionally handling the error
 * with a reset action.
 *
 * If fu_device_retry_add_recovery() has not been used then all errors are
 * considered non-fatal until the last try.
 *
 * If the reset function returns %FALSE, then the function returns straight away
 * without processing any pending retries.
 *
 * Since: 1.5.5
 **/
gboolean
fu_device_retry_full(FuDevice *self,
		     FuDeviceRetryFunc func,
		     guint count,
		     guint delay,
		     gpointer user_data,
		     GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(func != NULL, FALSE);
	g_return_val_if_fail(count >= 1, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	for (guint i = 0;; i++) {
		g_autoptr(GError) error_local = NULL;

		/* delay */
		if (i > 0)
			fu_device_sleep(self, delay);

		/* run function, if success return success */
		if (func(self, user_data, &error_local))
			break;

		/* sanity check */
		if (error_local == NULL) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "exec failed but no error set!");
			return FALSE;
		}

		/* too many retries */
		if (i >= count - 1) {
			g_propagate_prefixed_error(error,
						   g_steal_pointer(&error_local),
						   "failed after %u retries: ",
						   count);
			return FALSE;
		}

		/* show recoverable error on the console */
		if (priv->retry_recs == NULL || priv->retry_recs->len == 0) {
			g_info("failed on try %u of %u: %s", i + 1, count, error_local->message);
			continue;
		}

		/* find the condition that matches */
		for (guint j = 0; j < priv->retry_recs->len; j++) {
			FuDeviceRetryRecovery *rec = g_ptr_array_index(priv->retry_recs, j);
			if (g_error_matches(error_local, rec->domain, rec->code)) {
				if (rec->recovery_func != NULL) {
					if (!rec->recovery_func(self, user_data, error))
						return FALSE;
				} else {
					g_propagate_prefixed_error(
					    error,
					    g_steal_pointer(&error_local),
					    "device recovery not possible: ");
					return FALSE;
				}
			}
		}
	}

	/* success */
	return TRUE;
}

/**
 * fu_device_retry:
 * @self: a #FuDevice
 * @func: (scope async) (closure user_data): a function to execute
 * @count: the number of tries to try the function
 * @user_data: (nullable): a helper to pass to @func
 * @error: (nullable): optional return location for an error
 *
 * Calls a specific function a number of times, optionally handling the error
 * with a reset action.
 *
 * If fu_device_retry_add_recovery() has not been used then all errors are
 * considered non-fatal until the last try.
 *
 * If the reset function returns %FALSE, then the function returns straight away
 * without processing any pending retries.
 *
 * Since: 1.4.0
 **/
gboolean
fu_device_retry(FuDevice *self,
		FuDeviceRetryFunc func,
		guint count,
		gpointer user_data,
		GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	return fu_device_retry_full(self, func, count, priv->retry_delay, user_data, error);
}

/**
 * fu_device_sleep:
 * @self: a #FuDevice
 * @delay_ms: delay in milliseconds
 *
 * Delays program execution up to 100 seconds, unless the device is emulated where no delays is
 * performed.
 *
 * Long unavoidable delays (more than 1 second) should really use `fu_device_sleep_full()` so that
 * the percentage progress bar is updated.
 *
 * Since: 1.8.11
 **/
void
fu_device_sleep(FuDevice *self, guint delay_ms)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(delay_ms < 100000);

	if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_EMULATED))
		return;
	if (priv->proxy != NULL && fu_device_has_flag(priv->proxy, FWUPD_DEVICE_FLAG_EMULATED))
		return;
	if (delay_ms > 0)
		g_usleep(delay_ms * 1000);
}

/**
 * fu_device_sleep_full:
 * @self: a #FuDevice
 * @delay_ms: delay in milliseconds
 * @progress: a #FuProgress
 *
 * Delays program execution up to 1000 seconds, unless the device is emulated where no delays is
 * performed.
 *
 * Since: 1.8.11
 **/
void
fu_device_sleep_full(FuDevice *self, guint delay_ms, FuProgress *progress)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(delay_ms < 1000000);
	g_return_if_fail(FU_IS_PROGRESS(progress));

	if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_EMULATED))
		return;
	if (priv->proxy != NULL && fu_device_has_flag(priv->proxy, FWUPD_DEVICE_FLAG_EMULATED))
		return;
	if (delay_ms > 0)
		fu_progress_sleep(progress, delay_ms);
}

/**
 * fu_device_set_contents:
 * @self: a #FuDevice
 * @filename: full path to a file
 * @stream: data to write
 * @progress: a #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Writes @stream to @filename, emulating if required.
 *
 * Returns: %TRUE on success
 *
 * Since: 2.0.2
 **/
gboolean
fu_device_set_contents(FuDevice *self,
		       const gchar *filename,
		       GInputStream *stream,
		       FuProgress *progress,
		       GError **error)
{
	FuDeviceEvent *event = NULL;
	g_autofree gchar *event_id = NULL;
	g_autoptr(FuChunkArray) chunks = NULL;
	g_autoptr(GByteArray) buf_tagged = g_byte_array_new();
	g_autoptr(GFile) file = NULL;
	g_autoptr(GOutputStream) ostr = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(filename != NULL, FALSE);
	g_return_val_if_fail(G_IS_INPUT_STREAM(stream), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* need event ID */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED) ||
	    fu_context_has_flag(fu_device_get_context(FU_DEVICE(self)),
				FU_CONTEXT_FLAG_SAVE_EVENTS)) {
		event_id = g_strdup_printf("SetContents:Filename=%s", filename);
	}

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED)) {
		g_autoptr(GBytes) blob1 = NULL;
		g_autoptr(GBytes) blob2 = NULL;

		event = fu_device_load_event(FU_DEVICE(self), event_id, error);
		if (event == NULL)
			return FALSE;
		blob1 = fu_device_event_get_bytes(event, "Data", error);
		if (blob1 == NULL)
			return FALSE;
		blob2 = fu_input_stream_read_bytes(stream, 0x0, G_MAXSIZE, progress, error);
		if (blob2 == NULL)
			return FALSE;
		return fu_bytes_compare(blob1, blob2, error);
	}

	/* save */
	if (event_id != NULL)
		event = fu_device_save_event(FU_DEVICE(self), event_id);

	/* open file */
	file = g_file_new_for_path(filename);
	ostr = G_OUTPUT_STREAM(g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error));
	if (ostr == NULL)
		return FALSE;

	/* write in 32k chunks */
	chunks = fu_chunk_array_new_from_stream(stream,
						FU_CHUNK_ADDR_OFFSET_NONE,
						FU_CHUNK_PAGESZ_NONE,
						0x8000,
						error);
	if (chunks == NULL)
		return FALSE;
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_steps(progress, fu_chunk_array_length(chunks));
	for (guint i = 0; i < fu_chunk_array_length(chunks); i++) {
		g_autoptr(FuChunk) chk = NULL;
		g_autoptr(GBytes) blob = NULL;

		chk = fu_chunk_array_index(chunks, i, error);
		if (chk == NULL)
			return FALSE;
		blob = fu_chunk_get_bytes(chk);
		if (!fu_output_stream_write_bytes(ostr, blob, NULL, error))
			return FALSE;

		/* save */
		if (event != NULL)
			fu_byte_array_append_bytes(buf_tagged, blob);

		/* progress */
		fu_progress_step_done(progress);
	}

	/* save response */
	if (event != NULL)
		fu_device_event_set_data(event, "Data", buf_tagged->data, buf_tagged->len);

	/* success */
	return TRUE;
}

/**
 * fu_device_set_contents_bytes:
 * @self: a #FuDevice
 * @filename: full path to a file
 * @blob: data to write
 * @progress: (nullable): optional #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Writes @blob to @filename, emulating if required.
 *
 * Returns: %TRUE on success
 *
 * Since: 2.0.2
 **/
gboolean
fu_device_set_contents_bytes(FuDevice *self,
			     const gchar *filename,
			     GBytes *blob,
			     FuProgress *progress,
			     GError **error)
{
	g_autoptr(GInputStream) stream = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(filename != NULL, FALSE);
	g_return_val_if_fail(blob != NULL, FALSE);
	g_return_val_if_fail(progress == NULL || FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	stream = g_memory_input_stream_new_from_bytes(blob);
	return fu_device_set_contents(self, filename, stream, progress, error);
}

/**
 * fu_device_get_contents_bytes:
 * @self: a #FuDevice
 * @filename: full path to a file
 * @count: maximum number of bytes to read
 * @progress: (nullable): optional #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Reads a blob of data from the file, emulating if required.
 *
 * Returns: (transfer full): a #GBytes, or %NULL on error
 *
 * Since: 2.0.2
 **/
GBytes *
fu_device_get_contents_bytes(FuDevice *self,
			     const gchar *filename,
			     gsize count,
			     FuProgress *progress,
			     GError **error)
{
	FuDeviceEvent *event = NULL;
	g_autofree gchar *event_id = NULL;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GInputStream) istr = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(filename != NULL, NULL);
	g_return_val_if_fail(progress == NULL || FU_IS_PROGRESS(progress), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* need event ID */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED) ||
	    fu_context_has_flag(fu_device_get_context(FU_DEVICE(self)),
				FU_CONTEXT_FLAG_SAVE_EVENTS)) {
		event_id = g_strdup_printf("GetContents:Filename=%s", filename);
	}

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED)) {
		event = fu_device_load_event(FU_DEVICE(self), event_id, error);
		if (event == NULL)
			return NULL;
		return fu_device_event_get_bytes(event, "Data", error);
	}

	/* save */
	if (event_id != NULL)
		event = fu_device_save_event(FU_DEVICE(self), event_id);

	/* open for reading */
	istr = fu_input_stream_from_path(filename, error);
	if (istr == NULL)
		return NULL;
	blob = fu_input_stream_read_bytes(istr, 0, count, progress, error);
	if (blob == NULL)
		return NULL;

	/* save response */
	if (event != NULL)
		fu_device_event_set_bytes(event, "Data", blob);

	/* success */
	return g_steal_pointer(&blob);
}

/**
 * fu_device_get_contents:
 * @self: a #FuDevice
 * @filename: full path to a file
 * @count: maximum number of bytes to read
 * @progress: (nullable): optional #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Reads a blob of ASCII text from the file, emulating if required.
 *
 * Returns: (transfer full): a #GBytes, or %NULL on error
 *
 * Since: 2.0.12
 **/
gchar *
fu_device_get_contents(FuDevice *self,
		       const gchar *filename,
		       gsize count,
		       FuProgress *progress,
		       GError **error)
{
	FuDeviceEvent *event = NULL;
	g_autofree gchar *event_id = NULL;
	g_autofree gchar *str = NULL;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GInputStream) istr = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(filename != NULL, NULL);
	g_return_val_if_fail(progress == NULL || FU_IS_PROGRESS(progress), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* need event ID */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED) ||
	    fu_context_has_flag(fu_device_get_context(FU_DEVICE(self)),
				FU_CONTEXT_FLAG_SAVE_EVENTS)) {
		event_id = g_strdup_printf("GetContents:Filename=%s", filename);
	}

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED)) {
		event = fu_device_load_event(FU_DEVICE(self), event_id, error);
		if (event == NULL)
			return NULL;
		return g_strdup(fu_device_event_get_str(event, "Data", error));
	}

	/* save */
	if (event_id != NULL)
		event = fu_device_save_event(FU_DEVICE(self), event_id);

	/* open for reading */
	istr = fu_input_stream_from_path(filename, error);
	if (istr == NULL)
		return NULL;
	blob = fu_input_stream_read_bytes(istr, 0, count, progress, error);
	if (blob == NULL)
		return NULL;
	str = fu_strsafe_bytes(blob, G_MAXSIZE);
	if (str == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "invalid ASCII data");
		return NULL;
	}

	/* save response */
	if (event != NULL)
		fu_device_event_set_str(event, "Data", str);

	/* success */
	return g_steal_pointer(&str);
}

/**
 * fu_device_get_smbios_string:
 * @self: a #FuDevice
 * @type: a SMBIOS structure type, e.g. %FU_SMBIOS_STRUCTURE_TYPE_BIOS
 * @length: expected length of the structure, or %FU_SMBIOS_STRUCTURE_LENGTH_ANY
 * @offset: a SMBIOS offset
 * @error: (nullable): optional return location for an error
 *
 * Gets a hardware SMBIOS string.
 *
 * The @type and @offset can be referenced from the DMTF SMBIOS specification:
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_3.1.1.pdf
 *
 * Returns: a string, or %NULL
 *
 * Since: 2.0.10
 **/
const gchar *
fu_device_get_smbios_string(FuDevice *self,
			    guint8 type,
			    guint8 length,
			    guint8 offset,
			    GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceEvent *event = NULL;
	const gchar *str;
	g_autofree gchar *event_id = NULL;
	g_autoptr(GError) error_local = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* need event ID */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED) ||
	    fu_context_has_flag(fu_device_get_context(FU_DEVICE(self)),
				FU_CONTEXT_FLAG_SAVE_EVENTS)) {
		event_id = g_strdup_printf("GetSmbiosString:"
					   "Type=0x%02x,"
					   "Length=0x%02x,"
					   "Offset=0x%02x",
					   type,
					   length,
					   offset);
	}

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED)) {
		event = fu_device_load_event(FU_DEVICE(self), event_id, error);
		if (event == NULL)
			return NULL;
		if (!fu_device_event_check_error(event, error))
			return NULL;
		return fu_device_event_get_str(event, "Data", error);
	}

	/* save */
	if (event_id != NULL)
		event = fu_device_save_event(FU_DEVICE(self), event_id);

	/* use context */
	str = fu_context_get_smbios_string(priv->ctx, type, length, offset, &error_local);
	if (str == NULL) {
		if (event != NULL)
			fu_device_event_set_error(event, error_local);
		g_propagate_error(error, g_steal_pointer(&error_local));
		return NULL;
	}

	/* save response */
	if (event != NULL)
		fu_device_event_set_str(event, "Data", str);

	/* success */
	return str;
}

/**
 * fu_device_query_file_exists:
 * @self: a #FuDevice
 * @filename: filename
 * @exists: (out): if @filename exists
 * @error: (nullable): optional return location for an error
 *
 * Checks if a file exists.
 *
 * Returns: %TRUE for success
 *
 * Since: 2.0.2
 **/
gboolean
fu_device_query_file_exists(FuDevice *self, const gchar *filename, gboolean *exists, GError **error)
{
	FuDeviceEvent *event = NULL;
	gint64 value;
	g_autofree gchar *event_id = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(filename != NULL, FALSE);
	g_return_val_if_fail(exists != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED) ||
	    fu_context_has_flag(fu_device_get_context(FU_DEVICE(self)),
				FU_CONTEXT_FLAG_SAVE_EVENTS)) {
		event_id = g_strdup_printf("FileExists:Filename=%s", filename);
	}

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED)) {
		event = fu_device_load_event(FU_DEVICE(self), event_id, error);
		if (event == NULL)
			return FALSE;
		value = fu_device_event_get_i64(event, "Exists", error);
		if (value == G_MAXINT64)
			return FALSE;
		*exists = value == 1;
		return TRUE;
	}

	/* save */
	if (event_id != NULL)
		event = fu_device_save_event(FU_DEVICE(self), event_id);

	/* check, and save result */
	*exists = g_file_test(filename, G_FILE_TEST_EXISTS);
	if (event != NULL)
		fu_device_event_set_i64(event, "Exists", *exists ? 1 : 0);

	/* success */
	return TRUE;
}

static gboolean
fu_device_poll_locker_open_cb(GObject *device, GError **error)
{
	FuDevice *self = FU_DEVICE(device);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_atomic_int_inc(&priv->poll_locker_cnt);
	return TRUE;
}

static gboolean
fu_device_poll_locker_close_cb(GObject *device, GError **error)
{
	FuDevice *self = FU_DEVICE(device);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_atomic_int_dec_and_test(&priv->poll_locker_cnt);
	return TRUE;
}

/**
 * fu_device_poll_locker_new:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Returns a device locker that prevents polling on the device. If there are no open poll lockers
 * then the poll callback will be called.
 *
 * Use %FU_DEVICE_PRIVATE_FLAG_AUTO_PAUSE_POLLING to opt into this functionality.
 *
 * Returns: (transfer full): a #FuDeviceLocker
 *
 * Since: 1.8.1
 **/
FuDeviceLocker *
fu_device_poll_locker_new(FuDevice *self, GError **error)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	return fu_device_locker_new_full(self,
					 fu_device_poll_locker_open_cb,
					 fu_device_poll_locker_close_cb,
					 error);
}

/**
 * fu_device_poll:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Polls a device, typically querying the hardware for status.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_poll(FuDevice *self, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* subclassed */
	if (device_class->poll != NULL) {
		if (!device_class->poll(self, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
fu_device_poll_cb(gpointer user_data)
{
	FuDevice *self = FU_DEVICE(user_data);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(GError) error_local = NULL;

	/* device is being detached, written, read, or attached */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_AUTO_PAUSE_POLLING) &&
	    priv->poll_locker_cnt > 0) {
		g_debug("ignoring poll callback as an action is in progress");
		return G_SOURCE_CONTINUE;
	}

	if (!fu_device_poll(self, &error_local)) {
		g_warning("disabling polling: %s", error_local->message);
		priv->poll_id = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/**
 * fu_device_set_poll_interval:
 * @self: a #FuPlugin
 * @interval: duration in ms, or 0 to disable
 *
 * Polls the hardware every interval period. If the subclassed `->poll()` method
 * returns %FALSE then a warning is printed to the console and the poll is
 * disabled until the next call to fu_device_set_poll_interval().
 *
 * Since: 1.1.2
 **/
void
fu_device_set_poll_interval(FuDevice *self, guint interval)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));

	if (priv->poll_id != 0) {
		g_source_remove(priv->poll_id);
		priv->poll_id = 0;
	}
	if (interval == 0)
		return;
	if (interval % 1000 == 0) {
		priv->poll_id = g_timeout_add_seconds(interval / 1000, fu_device_poll_cb, self);
	} else {
		priv->poll_id = g_timeout_add(interval, fu_device_poll_cb, self);
	}
}

/**
 * fu_device_get_order:
 * @self: a #FuPlugin
 *
 * Gets the device order, where higher numbers are installed after lower
 * numbers.
 *
 * Returns: the integer value
 *
 * Since: 1.0.8
 **/
gint
fu_device_get_order(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->order;
}

/**
 * fu_device_set_order:
 * @self: a #FuDevice
 * @order: an integer value
 *
 * Sets the device order, where higher numbers are installed after lower
 * numbers.
 *
 * Since: 1.0.8
 **/
void
fu_device_set_order(FuDevice *self, gint order)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->order = order;
}

/**
 * fu_device_get_priority:
 * @self: a #FuPlugin
 *
 * Gets the device priority, where higher numbers are better.
 *
 * Returns: the integer value
 *
 * Since: 1.1.1
 **/
guint
fu_device_get_priority(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->priority;
}

/**
 * fu_device_set_priority:
 * @self: a #FuDevice
 * @priority: an integer value
 *
 * Sets the device priority, where higher numbers are better.
 *
 * Since: 1.1.1
 **/
void
fu_device_set_priority(FuDevice *self, guint priority)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->priority = priority;
}

/**
 * fu_device_get_equivalent_id:
 * @self: a #FuDevice
 *
 * Gets any equivalent ID for a device
 *
 * Returns: (transfer none): a #gchar or NULL
 *
 * Since: 0.6.1
 **/
const gchar *
fu_device_get_equivalent_id(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->equivalent_id;
}

/**
 * fu_device_set_equivalent_id:
 * @self: a #FuDevice
 * @equivalent_id: (nullable): a string
 *
 * Sets any equivalent ID for a device
 *
 * Since: 0.6.1
 **/
void
fu_device_set_equivalent_id(FuDevice *self, const gchar *equivalent_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->equivalent_id, equivalent_id) == 0)
		return;

	/* sanity check */
	if (!fwupd_device_id_is_valid(equivalent_id)) {
		g_critical("%s is not a valid device ID", equivalent_id);
		return;
	}
	if (g_strcmp0(equivalent_id, fu_device_get_id(self)) == 0) {
		g_critical("%s is the same as this device ID", equivalent_id);
		return;
	}

	g_free(priv->equivalent_id);
	priv->equivalent_id = g_strdup(equivalent_id);
	g_object_notify(G_OBJECT(self), "equivalent-id");
}

/**
 * fu_device_get_parent:
 * @self: a #FuDevice
 *
 * Gets any parent device. An parent device is logically "above" the current
 * device and this may be reflected in client tools.
 *
 * This information also allows the plugin to optionally verify the parent
 * device, for instance checking the parent device firmware version.
 *
 * The parent object is not refcounted and if destroyed this function will then
 * return %NULL.
 *
 * Returns: (transfer none): a device or %NULL
 *
 * Since: 1.0.8
 **/
FuDevice *
fu_device_get_parent(FuDevice *self)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return FU_DEVICE(fwupd_device_get_parent(FWUPD_DEVICE(self)));
}

/**
 * fu_device_get_root:
 * @self: a #FuDevice
 *
 * Gets the root parent device. A parent device is logically "above" the current
 * device and this may be reflected in client tools.
 *
 * If there is no parent device defined, then @self is returned.
 *
 * Returns: (transfer full): a device
 *
 * Since: 1.4.0
 **/
FuDevice *
fu_device_get_root(FuDevice *self)
{
	FuDevice *parent;
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	do {
		parent = fu_device_get_parent(self);
		if (parent != NULL)
			self = parent;
	} while (parent != NULL);
	return g_object_ref(self);
}

static void
fu_device_set_composite_id(FuDevice *self, const gchar *composite_id)
{
	GPtrArray *children;

	/* subclassed simple setter */
	fwupd_device_set_composite_id(FWUPD_DEVICE(self), composite_id);

	/* all children */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index(children, i);
		fu_device_set_composite_id(child_tmp, composite_id);
	}
}

/**
 * fu_device_set_parent:
 * @self: a #FuDevice
 * @parent: (nullable): a device
 *
 * Sets any parent device. An parent device is logically "above" the current
 * device and this may be reflected in client tools.
 *
 * This information also allows the plugin to optionally verify the parent
 * device, for instance checking the parent device firmware version.
 *
 * Since: 1.0.8
 **/
void
fu_device_set_parent(FuDevice *self, FuDevice *parent)
{
	g_return_if_fail(FU_IS_DEVICE(self));

	/* debug */
	if (parent != NULL) {
		g_info("setting parent of %s [%s] to be %s [%s]",
		       fu_device_get_name(self),
		       fu_device_get_id(self),
		       fu_device_get_name(parent),
		       fu_device_get_id(parent));
	}

	/* set the composite ID on the children and grandchildren */
	if (parent != NULL)
		fu_device_set_composite_id(self, fu_device_get_composite_id(parent));

	/* if the parent has a context, make the child inherit it */
	if (parent != NULL) {
		if (fu_device_get_context(self) == NULL && fu_device_get_context(parent) != NULL)
			fu_device_set_context(self, fu_device_get_context(parent));
	}

	fwupd_device_set_parent(FWUPD_DEVICE(self), FWUPD_DEVICE(parent));
	g_object_notify(G_OBJECT(self), "parent");
}

/* if the proxy sets this flag copy it to the logical device */
static void
fu_device_incorporate_from_proxy_flags(FuDevice *self, FuDevice *proxy)
{
	const FwupdDeviceFlags flags[] = {FWUPD_DEVICE_FLAG_EMULATED,
					  FWUPD_DEVICE_FLAG_UNREACHABLE};
	for (guint i = 0; i < G_N_ELEMENTS(flags); i++) {
		if (fu_device_has_flag(proxy, flags[i])) {
			g_debug("propagating %s from proxy", fwupd_device_flag_to_string(flags[i]));
			fu_device_add_flag(self, flags[i]);
		}
	}
}

static void
fu_device_proxy_flags_notify_cb(FuDevice *proxy, GParamSpec *pspec, gpointer user_data)
{
	FuDevice *self = FU_DEVICE(user_data);
	fu_device_incorporate_from_proxy_flags(self, proxy);
}

/**
 * fu_device_set_proxy:
 * @self: a #FuDevice
 * @proxy: a device
 *
 * Sets any proxy device. A proxy device can be used to perform an action on
 * behalf of another device, for instance attach()ing it after a successful
 * update.
 *
 * If the %FU_DEVICE_PRIVATE_FLAG_REFCOUNTED_PROXY flag is present on the device then a *strong*
 * reference is used, otherwise the proxy will be cleared if @proxy is destroyed.
 *
 * Since: 1.4.1
 **/
void
fu_device_set_proxy(FuDevice *self, FuDevice *proxy)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));

	/* unchanged */
	if (proxy == priv->proxy)
		return;

	/* disconnect from old proxy */
	if (priv->proxy != NULL && priv->notify_flags_proxy_id != 0) {
		g_signal_handler_disconnect(priv->proxy, priv->notify_flags_proxy_id);
		priv->notify_flags_proxy_id = 0;
	}

	/* copy from proxy */
	if (proxy != NULL) {
		fu_device_incorporate(self, proxy, FU_DEVICE_INCORPORATE_FLAG_PHYSICAL_ID);
		priv->notify_flags_proxy_id =
		    g_signal_connect(FWUPD_DEVICE(proxy),
				     "notify::flags",
				     G_CALLBACK(fu_device_proxy_flags_notify_cb),
				     self);
		fu_device_incorporate_from_proxy_flags(self, proxy);
	}

	/* sometimes strong, sometimes weak */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_REFCOUNTED_PROXY])) {
		g_set_object(&priv->proxy, proxy);
		fu_device_set_target(self, proxy);
	} else {
		if (priv->proxy != NULL)
			g_object_remove_weak_pointer(G_OBJECT(priv->proxy),
						     (gpointer *)&priv->proxy);
		if (proxy != NULL)
			g_object_add_weak_pointer(G_OBJECT(proxy), (gpointer *)&priv->proxy);
		priv->proxy = proxy;
	}
	g_object_notify(G_OBJECT(self), "proxy");
}

/**
 * fu_device_get_proxy:
 * @self: a #FuDevice
 *
 * Gets any proxy device. A proxy device can be used to perform an action on
 * behalf of another device, for instance attach()ing it after a successful
 * update.
 *
 * Unless %FU_DEVICE_PRIVATE_FLAG_REFCOUNTED_PROXY is set, the proxy object is not refcounted and
 * if destroyed this function will then return %NULL.
 *
 * Returns: (transfer none): a device or %NULL
 *
 * Since: 1.4.1
 **/
FuDevice *
fu_device_get_proxy(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->proxy;
}

/**
 * fu_device_get_proxy_with_fallback:
 * @self: a #FuDevice
 *
 * Gets the proxy device if %FU_DEVICE_PRIVATE_FLAG_USE_PROXY_FALLBACK is set, falling back to the
 * device itself.
 *
 * Returns: (transfer none): a device
 *
 * Since: 1.6.2
 **/
FuDevice *
fu_device_get_proxy_with_fallback(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PROXY_FALLBACK) &&
	    priv->proxy != NULL)
		return priv->proxy;
	return self;
}

/**
 * fu_device_get_children:
 * @self: a #FuDevice
 *
 * Gets any child devices. A child device is logically "below" the current
 * device and this may be reflected in client tools.
 *
 * Returns: (transfer none) (element-type FuDevice): child devices
 *
 * Since: 1.0.8
 **/
GPtrArray *
fu_device_get_children(FuDevice *self)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return fwupd_device_get_children(FWUPD_DEVICE(self));
}

/**
 * fu_device_add_child:
 * @self: a #FuDevice
 * @child: Another #FuDevice
 *
 * Sets any child device. An child device is logically linked to the primary
 * device in some way.
 *
 * Since: 1.0.8
 **/
void
fu_device_add_child(FuDevice *self, FuDevice *child)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	GPtrArray *children;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_DEVICE(child));

	/* if parent is emulated, child must be too */
	if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_EMULATED))
		fu_device_add_flag(child, FWUPD_DEVICE_FLAG_EMULATED);

	/* make tests easier */
	fu_device_convert_instance_ids(child);

	/* add if the child does not already exist */
	fwupd_device_add_child(FWUPD_DEVICE(self), FWUPD_DEVICE(child));

	/* propagate inhibits to children */
	if (priv->inhibits != NULL &&
	    fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_INHIBIT_CHILDREN)) {
		g_autoptr(GList) values = g_hash_table_get_values(priv->inhibits);
		for (GList *l = values; l != NULL; l = l->next) {
			FuDeviceInhibit *inhibit = (FuDeviceInhibit *)l->data;
			fu_device_inhibit_full(child,
					       inhibit->problem,
					       inhibit->inhibit_id,
					       inhibit->reason);
		}
	}

	/* ensure the parent has the MAX() of the children's removal delay  */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index(children, i);
		guint remove_delay = fu_device_get_remove_delay(child_tmp);
		if (remove_delay > priv->remove_delay) {
			g_debug("setting remove delay to %ums as child is greater than %ums",
				remove_delay,
				priv->remove_delay);
			priv->remove_delay = remove_delay;
		}
	}

	/* ensure the parent has the MAX() of the children's acquiesce delay  */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index(children, i);
		guint acquiesce_delay = fu_device_get_acquiesce_delay(child_tmp);
		if (acquiesce_delay > priv->acquiesce_delay) {
			g_debug("setting acquiesce delay to %ums as child is greater than %ums",
				acquiesce_delay,
				priv->acquiesce_delay);
			priv->acquiesce_delay = acquiesce_delay;
		}
	}

	/* ensure child has the parent acquiesce delay */
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index(children, i);
		fu_device_set_acquiesce_delay(child_tmp, priv->acquiesce_delay);
	}

	/* copy from main device if unset */
	fu_device_incorporate(
	    child,
	    self,
	    FU_DEVICE_INCORPORATE_FLAG_PHYSICAL_ID | FU_DEVICE_INCORPORATE_FLAG_BACKEND_ID |
		FU_DEVICE_INCORPORATE_FLAG_REMOVE_DELAY |
		FU_DEVICE_INCORPORATE_FLAG_ACQUIESCE_DELAY | FU_DEVICE_INCORPORATE_FLAG_VENDOR_IDS |
		FU_DEVICE_INCORPORATE_FLAG_ICONS | FU_DEVICE_INCORPORATE_FLAG_VENDOR);

	/* ensure the ID is converted */
	if (!fu_device_ensure_id(child, &error))
		g_warning("failed to ensure child: %s", error->message);

	/* ensure the parent is also set on the child */
	fu_device_set_parent(child, self);

	/* signal to the plugin in case this is done after setup */
	g_signal_emit(self, signals[SIGNAL_CHILD_ADDED], 0, child);
}

/**
 * fu_device_remove_child:
 * @self: a #FuDevice
 * @child: Another #FuDevice
 *
 * Removes child device.
 *
 * Since: 1.6.2
 **/
void
fu_device_remove_child(FuDevice *self, FuDevice *child)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_DEVICE(child));

	/* proxy */
	fwupd_device_remove_child(FWUPD_DEVICE(self), FWUPD_DEVICE(child));

	/* signal to the plugin */
	g_signal_emit(self, signals[SIGNAL_CHILD_REMOVED], 0, child);
}

/**
 * fu_device_remove_children:
 * @self: a #FuDevice
 *
 * Removes all child devices.
 *
 * Since: 2.0.0
 **/
void
fu_device_remove_children(FuDevice *self)
{
	GPtrArray *children;

	g_return_if_fail(FU_IS_DEVICE(self));

	/* proxy */
	fwupd_device_remove_children(FWUPD_DEVICE(self));

	/* signal to the plugin */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child = g_ptr_array_index(children, i);
		g_signal_emit(self, signals[SIGNAL_CHILD_REMOVED], 0, child);
	}
}

static void
fu_device_ensure_parent_guids(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	if (priv->parent_guids != NULL)
		return;
	priv->parent_guids = g_ptr_array_new_with_free_func(g_free);
}

/**
 * fu_device_get_parent_guids:
 * @self: a #FuDevice
 *
 * Gets any parent device GUIDs. If a device is added to the daemon that matches
 * any GUIDs added from fu_device_add_parent_guid() then this device is marked the parent of @self.
 *
 * Returns: (transfer none) (element-type utf8): a list of GUIDs
 *
 * Since: 1.0.8
 **/
GPtrArray *
fu_device_get_parent_guids(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	fu_device_ensure_parent_guids(self);
	return priv->parent_guids;
}

/**
 * fu_device_has_parent_guid:
 * @self: a #FuDevice
 * @guid: a GUID
 *
 * Searches the list of parent GUIDs for a string match.
 *
 * Returns: %TRUE if the parent GUID exists
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_has_parent_guid(FuDevice *self, const gchar *guid)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(guid != NULL, FALSE);

	if (priv->parent_guids == NULL)
		return FALSE;
	for (guint i = 0; i < priv->parent_guids->len; i++) {
		const gchar *guid_tmp = g_ptr_array_index(priv->parent_guids, i);
		if (g_strcmp0(guid_tmp, guid) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_device_add_parent_guid:
 * @self: a #FuDevice
 * @guid: a GUID
 *
 * Sets any parent device using a GUID. An parent device is logically linked to
 * the primary device in some way and can be added before or after @self.
 *
 * The GUIDs are searched in order, and so the order of adding GUIDs may be
 * important if more than one parent device might match.
 *
 * If the parent device is removed, any children logically linked to it will
 * also be removed.
 *
 * Since: 1.0.8
 **/
void
fu_device_add_parent_guid(FuDevice *self, const gchar *guid)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(guid != NULL);

	/* ensure */
	fu_device_ensure_parent_guids(self);

	/* make valid */
	if (!fwupd_guid_is_valid(guid)) {
		g_autofree gchar *tmp = fwupd_guid_hash_string(guid);
		if (fu_device_has_parent_guid(self, tmp))
			return;
		g_debug("using %s for %s", tmp, guid);
		g_ptr_array_add(priv->parent_guids, g_steal_pointer(&tmp));
		return;
	}

	/* already valid */
	if (fu_device_has_parent_guid(self, guid))
		return;
	g_ptr_array_add(priv->parent_guids, g_strdup(guid));
}

/**
 * fu_device_get_parent_physical_ids:
 * @self: a #FuDevice
 *
 * Gets any parent device IDs. If a device is added to the daemon that matches
 * the physical ID added from fu_device_add_parent_physical_id() then this
 * device is marked the parent of @self.
 *
 * Returns: (transfer none) (element-type utf8) (nullable): a list of IDs
 *
 * Since: 1.6.2
 **/
GPtrArray *
fu_device_get_parent_physical_ids(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->parent_physical_ids;
}

/**
 * fu_device_has_parent_physical_id:
 * @self: a #FuDevice
 * @physical_id: a device physical ID
 *
 * Searches the list of parent IDs for a string match.
 *
 * Returns: %TRUE if the parent ID exists
 *
 * Since: 1.6.2
 **/
gboolean
fu_device_has_parent_physical_id(FuDevice *self, const gchar *physical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(physical_id != NULL, FALSE);

	if (priv->parent_physical_ids == NULL)
		return FALSE;
	for (guint i = 0; i < priv->parent_physical_ids->len; i++) {
		const gchar *tmp = g_ptr_array_index(priv->parent_physical_ids, i);
		if (g_strcmp0(tmp, physical_id) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_device_add_parent_physical_id:
 * @self: a #FuDevice
 * @physical_id: a device physical ID
 *
 * Sets any parent device using the physical ID. An parent device is logically
 * linked to the primary device in some way and can be added before or after @self.
 *
 * The IDs are searched in order, and so the order of adding IDs may be
 * important if more than one parent device might match.
 *
 * Since: 1.6.2
 **/
void
fu_device_add_parent_physical_id(FuDevice *self, const gchar *physical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(physical_id != NULL);

	/* ensure exists */
	if (priv->parent_physical_ids == NULL)
		priv->parent_physical_ids = g_ptr_array_new_with_free_func(g_free);

	/* already present */
	if (fu_device_has_parent_physical_id(self, physical_id))
		return;
	g_ptr_array_add(priv->parent_physical_ids, g_strdup(physical_id));
}

/**
 * fu_device_get_parent_backend_ids:
 * @self: a #FuDevice
 *
 * Gets any parent device IDs. If a device is added to the daemon that matches
 * the physical ID added from fu_device_add_parent_backend_id() then this
 * device is marked the parent of @self.
 *
 * Returns: (transfer none) (element-type utf8) (nullable): a list of IDs
 *
 * Since: 1.9.7
 **/
GPtrArray *
fu_device_get_parent_backend_ids(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->parent_backend_ids;
}

/**
 * fu_device_has_parent_backend_id:
 * @self: a #FuDevice
 * @backend_id: a device physical ID
 *
 * Searches the list of parent IDs for a string match.
 *
 * Returns: %TRUE if the parent ID exists
 *
 * Since: 1.9.7
 **/
gboolean
fu_device_has_parent_backend_id(FuDevice *self, const gchar *backend_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(backend_id != NULL, FALSE);

	if (priv->parent_backend_ids == NULL)
		return FALSE;
	for (guint i = 0; i < priv->parent_backend_ids->len; i++) {
		const gchar *tmp = g_ptr_array_index(priv->parent_backend_ids, i);
		if (g_strcmp0(tmp, backend_id) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_device_add_parent_backend_id:
 * @self: a #FuDevice
 * @backend_id: a device physical ID
 *
 * Sets any parent device using the physical ID. An parent device is logically
 * linked to the primary device in some way and can be added before or after @self.
 *
 * The IDs are searched in order, and so the order of adding IDs may be
 * important if more than one parent device might match.
 *
 * Since: 1.9.7
 **/
void
fu_device_add_parent_backend_id(FuDevice *self, const gchar *backend_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(backend_id != NULL);

	/* ensure exists */
	if (priv->parent_backend_ids == NULL)
		priv->parent_backend_ids = g_ptr_array_new_with_free_func(g_free);

	/* already present */
	if (fu_device_has_parent_backend_id(self, backend_id))
		return;
	g_ptr_array_add(priv->parent_backend_ids, g_strdup(backend_id));
}

static gboolean
fu_device_add_child_by_type_guid(FuDevice *self, GType type, const gchar *guid, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(FuDevice) child = NULL;
	child = g_object_new(type, "context", priv->ctx, "logical-id", guid, NULL);
	fu_device_add_instance_id(child, guid);
	fu_device_incorporate(child, self, FU_DEVICE_INCORPORATE_FLAG_PHYSICAL_ID);
	if (!fu_device_ensure_id(self, error))
		return FALSE;
	if (!fu_device_probe(child, error))
		return FALSE;
	fu_device_convert_instance_ids(child);
	fu_device_add_child(self, child);
	return TRUE;
}

static gboolean
fu_device_add_child_by_kv(FuDevice *self, const gchar *str, GError **error)
{
	g_auto(GStrv) split = g_strsplit(str, "|", -1);

	/* type same as parent */
	if (g_strv_length(split) == 1) {
		return fu_device_add_child_by_type_guid(self, G_OBJECT_TYPE(self), split[1], error);
	}

	/* type specified */
	if (g_strv_length(split) == 2) {
		GType devtype = g_type_from_name(split[0]);
		if (devtype == 0) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_FOUND,
					    "no GType registered");
			return FALSE;
		}
		return fu_device_add_child_by_type_guid(self, devtype, split[1], error);
	}

	/* more than one '|' */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "unable to add parse child section");
	return FALSE;
}

static gboolean
fu_device_set_quirk_inhibit_section(FuDevice *self, const gchar *value, GError **error)
{
	g_auto(GStrv) sections = NULL;

	g_return_val_if_fail(value != NULL, FALSE);

	/* sanity check */
	sections = g_strsplit(value, ":", -1);
	if (g_strv_length(sections) != 2) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "quirk key not supported, expected k1:v1[,k2:v2][,k3:]");
		return FALSE;
	}

	/* allow empty string to unset quirk */
	if (g_strcmp0(sections[1], "") != 0)
		fu_device_inhibit(self, sections[0], sections[1]);
	else
		fu_device_uninhibit(self, sections[0]);

	/* success */
	return TRUE;
}

/**
 * fu_device_set_quirk_kv:
 * @self: a #FuDevice
 * @key: a string key
 * @value: a string value
 * @source: a #FuContextQuirkSource, e.g. %FU_CONTEXT_QUIRK_SOURCE_DB
 * @error: (nullable): optional return location for an error
 *
 * Sets a specific quirk on the device.
 *
 * Returns: %TRUE on success
 *
 * Since: 2.0.2
 **/
gboolean
fu_device_set_quirk_kv(FuDevice *self,
		       const gchar *key,
		       const gchar *value,
		       FuContextQuirkSource source,
		       GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	guint64 tmp;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(key != NULL, FALSE);
	g_return_val_if_fail(value != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (g_strcmp0(key, FU_QUIRKS_PLUGIN) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_possible_plugin(self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_FLAGS) == 0) {
		fu_device_set_custom_flags(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_NAME) == 0) {
		if (fu_device_get_name(self) != NULL && source >= FU_CONTEXT_QUIRK_SOURCE_DB)
			return TRUE;
		fu_device_set_name(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_SUMMARY) == 0) {
		fu_device_set_summary(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_BRANCH) == 0) {
		fu_device_set_branch(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_VENDOR) == 0) {
		if (fu_device_get_vendor(self) != NULL && source >= FU_CONTEXT_QUIRK_SOURCE_DB)
			return TRUE;
		fu_device_set_vendor(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_VENDOR_ID) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_vendor_id(self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_PROTOCOL) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_protocol(self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_ISSUE) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_issue(self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_VERSION) == 0) {
		fu_device_set_version(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_UPDATE_MESSAGE) == 0) {
		fu_device_set_update_message(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_UPDATE_IMAGE) == 0) {
		fu_device_set_update_image(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_ICON) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		if (fu_device_get_icons(self)->len > 0 &&
		    source >= FU_CONTEXT_QUIRK_SOURCE_FALLBACK)
			return TRUE;
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_icon(self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_GUID) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++) {
			fu_device_add_instance_id_full(self,
						       sections[i],
						       FU_DEVICE_INSTANCE_FLAG_VISIBLE |
							   FU_DEVICE_INSTANCE_FLAG_QUIRKS);
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_GUID_QUIRK) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++) {
			fu_device_add_instance_id_full(self,
						       sections[i],
						       FU_DEVICE_INSTANCE_FLAG_QUIRKS);
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_COUNTERPART_GUID) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++) {
			fu_device_add_instance_id_full(self,
						       sections[i],
						       FU_DEVICE_INSTANCE_FLAG_COUNTERPART);
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_PARENT_GUID) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_parent_guid(self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_PROXY_GUID) == 0) {
		fu_device_set_proxy_guid(self, value);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_FIRMWARE_SIZE_MIN) == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT64, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_firmware_size_min(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_FIRMWARE_SIZE_MAX) == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT64, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_firmware_size_max(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_FIRMWARE_SIZE) == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT64, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_firmware_size(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_INSTALL_DURATION) == 0) {
		if (!fu_strtoull(value, &tmp, 0, 60 * 60 * 24, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_install_duration(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_PRIORITY) == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT8, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_priority(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_BATTERY_THRESHOLD) == 0) {
		if (!fu_strtoull(value, &tmp, 0, 100, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_battery_threshold(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_REMOVE_DELAY) == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_remove_delay(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_ACQUIESCE_DELAY) == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		fu_device_set_acquiesce_delay(self, tmp);
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_VERSION_FORMAT) == 0) {
		fu_device_set_version_format(self, fwupd_version_format_from_string(value));
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_INHIBIT) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++) {
			if (!fu_device_set_quirk_inhibit_section(self, sections[i], error))
				return FALSE;
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_GTYPE) == 0) {
		if (priv->specialized_gtype != G_TYPE_INVALID) {
			g_debug("already set GType to %s, ignoring %s",
				g_type_name(priv->specialized_gtype),
				value);
			return TRUE;
		}
		priv->specialized_gtype = g_type_from_name(value);
		if (priv->specialized_gtype == G_TYPE_INVALID) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "unknown GType name %s",
				    value);
			return FALSE;
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_PROXY_GTYPE) == 0) {
		if (priv->proxy_gtype != G_TYPE_INVALID) {
			g_debug("already set proxy GType to %s, ignoring %s",
				g_type_name(priv->proxy_gtype),
				value);
			return TRUE;
		}
		priv->proxy_gtype = g_type_from_name(value);
		if (priv->proxy_gtype == G_TYPE_INVALID) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "unknown GType name %s",
				    value);
			return FALSE;
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_FIRMWARE_GTYPE) == 0) {
		if (priv->firmware_gtype != G_TYPE_INVALID) {
			g_debug("already set firmware GType to %s, ignoring %s",
				g_type_name(priv->firmware_gtype),
				value);
			return TRUE;
		}
		priv->firmware_gtype = g_type_from_name(value);
		if (priv->firmware_gtype == G_TYPE_INVALID) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "unknown GType name %s",
				    value);
			return FALSE;
		}
		return TRUE;
	}
	if (g_strcmp0(key, FU_QUIRKS_CHILDREN) == 0) {
		g_auto(GStrv) sections = g_strsplit(value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++) {
			if (!fu_device_add_child_by_kv(self, sections[i], error))
				return FALSE;
		}
		return TRUE;
	}

	/* optional device-specific method */
	if (device_class->set_quirk_kv != NULL)
		return device_class->set_quirk_kv(self, key, value, error);

	/* failed */
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "quirk key not supported");
	return FALSE;
}

/**
 * fu_device_get_specialized_gtype:
 * @self: a #FuDevice
 *
 * Gets the specialized type of the device
 *
 * Returns:#GType
 *
 * Since: 1.3.3
 **/
GType
fu_device_get_specialized_gtype(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	return priv->specialized_gtype;
}

/**
 * fu_device_set_specialized_gtype:
 * @self: a #FuDevice
 * @gtype: a #GType
 *
 * Sets the specialized type of the device
 *
 * Since: 2.0.0
 **/
void
fu_device_set_specialized_gtype(FuDevice *self, GType gtype)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(gtype != G_TYPE_INVALID);
	priv->specialized_gtype = gtype;
}

/**
 * fu_device_get_proxy_gtype:
 * @self: a #FuDevice
 *
 * Gets the specialized type of the device
 *
 * Returns:#GType
 *
 * Since: 1.9.15
 **/
GType
fu_device_get_proxy_gtype(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	return priv->proxy_gtype;
}

/**
 * fu_device_set_proxy_gtype:
 * @self: a #FuDevice
 * @gtype: a #GType
 *
 * Sets the specialized type of the device
 *
 * Since: 1.9.15
 **/
void
fu_device_set_proxy_gtype(FuDevice *self, GType gtype)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(gtype != G_TYPE_INVALID);
	priv->proxy_gtype = gtype;
}

/**
 * fu_device_get_firmware_gtype:
 * @self: a #FuDevice
 *
 * Gets the default firmware type for the device.
 *
 * Returns: #GType
 *
 * Since: 1.7.2
 **/
GType
fu_device_get_firmware_gtype(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), G_TYPE_INVALID);
	return priv->firmware_gtype;
}

/**
 * fu_device_set_firmware_gtype:
 * @self: a #FuDevice
 * @firmware_gtype: a #GType
 *
 * Sets the default firmware type for the device.
 *
 * Since: 1.7.2
 **/
void
fu_device_set_firmware_gtype(FuDevice *self, GType firmware_gtype)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	priv->firmware_gtype = firmware_gtype;
}

static void
fu_device_quirks_iter_cb(FuContext *ctx,
			 const gchar *key,
			 const gchar *value,
			 FuContextQuirkSource source,
			 gpointer user_data)
{
	FuDevice *self = FU_DEVICE(user_data);
	g_autoptr(GError) error = NULL;
	if (!fu_device_set_quirk_kv(self, key, value, source, &error)) {
		if (!g_error_matches(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED))
			g_warning("failed to set quirk key %s=%s: %s", key, value, error->message);
	}
}

static void
fu_device_add_guid_quirks(FuDevice *self, const gchar *guid)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(guid != NULL);

	if (priv->ctx == NULL) {
		g_autofree gchar *str = fu_device_to_string(self);
		g_critical("no FuContext assigned for %s", str);
		return;
	}

	/* run the query */
	fu_context_lookup_quirk_by_id_iter(priv->ctx, guid, NULL, fu_device_quirks_iter_cb, self);
}

/**
 * fu_device_set_firmware_size:
 * @self: a #FuDevice
 * @size: Size in bytes
 *
 * Sets the exact allowed size of the firmware blob.
 *
 * Since: 1.2.6
 **/
void
fu_device_set_firmware_size(FuDevice *self, guint64 size)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->size_min = size;
	priv->size_max = size;
}

/**
 * fu_device_set_firmware_size_min:
 * @self: a #FuDevice
 * @size_min: Size in bytes
 *
 * Sets the minimum allowed size of the firmware blob.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_firmware_size_min(FuDevice *self, guint64 size_min)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->size_min = size_min;
}

/**
 * fu_device_set_firmware_size_max:
 * @self: a #FuDevice
 * @size_max: Size in bytes
 *
 * Sets the maximum allowed size of the firmware blob.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_firmware_size_max(FuDevice *self, guint64 size_max)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->size_max = size_max;
}

/**
 * fu_device_get_firmware_size_min:
 * @self: a #FuDevice
 *
 * Gets the minimum size of the firmware blob.
 *
 * Returns: Size in bytes, or 0 if unset
 *
 * Since: 1.2.6
 **/
guint64
fu_device_get_firmware_size_min(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->size_min;
}

/**
 * fu_device_get_firmware_size_max:
 * @self: a #FuDevice
 *
 * Gets the maximum size of the firmware blob.
 *
 * Returns: Size in bytes, or 0 if unset
 *
 * Since: 1.2.6
 **/
guint64
fu_device_get_firmware_size_max(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->size_max;
}

/**
 * fu_device_get_required_free:
 * @self: a #FuDevice
 *
 * Returns the required amount of free firmware space.
 *
 * Returns: size in bytes
 *
 * Since: 2.0.12
 **/
guint64
fu_device_get_required_free(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->required_free;
}

/**
 * fu_device_set_required_free:
 * @self: a #FuDevice
 * @required_free: size in bytes
 *
 * Sets the required amount of free firmware size.
 *
 * NOTE: What we really want to do for EFI devices is check if a *contiguous* block of the right
 * size can be written, but on most machines this causes an SMI which causes all cores to halt.
 * On my desktop this causes **ALL** CPU processes to stop for ~1s, which is clearly unacceptable
 * at every boot. Instead, check for free space at least as big as needed, plus a little extra.
 *
 * Since: 2.0.12
 **/
void
fu_device_set_required_free(FuDevice *self, guint64 required_free)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	if (priv->required_free == required_free)
		return;
	priv->required_free = required_free;
	g_object_notify(G_OBJECT(self), "required-free");
}

/**
 * fu_device_has_guid:
 * @self: a #FuDevice
 * @guid: a GUID, e.g. `WacomAES`
 *
 * Finds out if the device has a specific GUID.
 *
 * Returns: %TRUE if the GUID is found
 *
 * Since: 1.2.2
 **/
gboolean
fu_device_has_guid(FuDevice *self, const gchar *guid)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(guid != NULL, FALSE);

	/* make valid */
	if (!fwupd_guid_is_valid(guid)) {
		g_autofree gchar *tmp = fwupd_guid_hash_string(guid);
		return fwupd_device_has_guid(FWUPD_DEVICE(self), tmp);
	}

	/* already valid */
	return fwupd_device_has_guid(FWUPD_DEVICE(self), guid);
}

static void
fu_device_instance_id_free(FuDeviceInstanceIdItem *item)
{
	g_free(item->instance_id);
	g_free(item->guid);
	g_free(item);
}

static FuDeviceInstanceIdItem *
fu_device_get_instance_id(FuDevice *self, const gchar *instance_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	if (priv->instance_ids == NULL)
		return NULL;
	for (guint i = 0; i < priv->instance_ids->len; i++) {
		FuDeviceInstanceIdItem *item = g_ptr_array_index(priv->instance_ids, i);
		if (g_strcmp0(instance_id, item->instance_id) == 0)
			return item;
		if (g_strcmp0(instance_id, item->guid) == 0)
			return item;
	}
	return NULL;
}

/**
 * fu_device_has_instance_id:
 * @self: a #FuDevice
 * @instance_id: a instance ID, e.g. `WacomAES`
 * @flags: instance ID flags
 *
 * Finds out if the device has this specific instance ID.
 *
 * NOTE: The instance IDs are only added to the actual base #FwupdDevice after
 * fu_device_convert_instance_ids() has been called -- normally as part of `FuDevice->setup()`.
 * This ensures that incorporating a baseclass to a target device with the flag
 * %FU_DEVICE_PRIVATE_FLAG_NO_GENERIC_GUIDS does not have generic instance IDs added.
 *
 * Returns: %TRUE if the instance ID is found
 *
 * Since: 2.0.4
 **/
gboolean
fu_device_has_instance_id(FuDevice *self, const gchar *instance_id, FuDeviceInstanceFlags flags)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(instance_id != NULL, FALSE);

	for (guint i = 0; priv->instance_ids != NULL && i < priv->instance_ids->len; i++) {
		FuDeviceInstanceIdItem *item = g_ptr_array_index(priv->instance_ids, i);
		if ((item->flags & flags) == 0)
			continue;
		if (g_strcmp0(item->instance_id, instance_id) == 0 ||
		    g_strcmp0(item->guid, instance_id) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_device_add_instance_id_full:
 * @self: a #FuDevice
 * @instance_id: a instance ID or GUID, e.g. `WacomAES`
 * @flags: instance ID flags
 *
 * Adds an instance ID or GUID with all parameters set.
 *
 * A counterpart GUID is the GUID of the same device in bootloader or runtime mode,
 * if they have a different device PCI or USB ID.
 *
 * Since: 1.2.9
 **/
void
fu_device_add_instance_id_full(FuDevice *self,
			       const gchar *instance_id,
			       FuDeviceInstanceFlags flags)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceInstanceIdItem *item;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(instance_id != NULL);

	/* some devices in recovery mode expect this to work */
	if ((flags & FU_DEVICE_INSTANCE_FLAG_COUNTERPART) > 0 &&
	    fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_COUNTERPART_VISIBLE)) {
		g_debug("making %s also visible", instance_id);
		flags |= FU_DEVICE_INSTANCE_FLAG_VISIBLE;
	}

	/* add to cache */
	item = fu_device_get_instance_id(self, instance_id);
	if (item != NULL) {
		if ((item->flags & FU_DEVICE_INSTANCE_FLAG_QUIRKS) == 0 &&
		    (flags & FU_DEVICE_INSTANCE_FLAG_QUIRKS) > 0) {
			/* visible -> visible+quirks */
			fu_device_add_guid_quirks(self, item->guid);
		}
		item->flags |= flags;
	} else {
		item = g_new0(FuDeviceInstanceIdItem, 1);
		if (fwupd_guid_is_valid(instance_id)) {
			item->guid = g_strdup(instance_id);
		} else {
			item->instance_id = g_strdup(instance_id);
			item->guid = fwupd_guid_hash_string(instance_id);
		}
		item->flags |= flags;
		if (priv->instance_ids == NULL)
			priv->instance_ids = g_ptr_array_new_with_free_func(
			    (GDestroyNotify)fu_device_instance_id_free);
		g_ptr_array_add(priv->instance_ids, item);

		/* we want the quirks to match so the plugin is set */
		if (flags & FU_DEVICE_INSTANCE_FLAG_QUIRKS)
			fu_device_add_guid_quirks(self, item->guid);
	}

	/* already done by ->setup(), so this must be ->registered() */
	if (priv->done_setup) {
		if (item->instance_id != NULL)
			fwupd_device_add_instance_id(FWUPD_DEVICE(self), item->instance_id);
		fwupd_device_add_guid(FWUPD_DEVICE(self), item->guid);
	}
}

/**
 * fu_device_add_instance_id:
 * @self: a #FuDevice
 * @instance_id: the instance ID, e.g. `PCI\VEN_10EC&DEV_525A`
 *
 * Adds an visible, quirked, instance ID to the device.
 *
 * Since: 1.2.5
 **/
void
fu_device_add_instance_id(FuDevice *self, const gchar *instance_id)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(instance_id != NULL);
	fu_device_add_instance_id_full(self,
				       instance_id,
				       FU_DEVICE_INSTANCE_FLAG_VISIBLE |
					   FU_DEVICE_INSTANCE_FLAG_QUIRKS);
}

/**
 * fu_device_get_counterpart_guids:
 * @self: a #FuDevice
 *
 * Returns all the counterpart GUIDs.
 *
 * Returns: (transfer container) (element-type utf8): list of GUIDs
 *
 * Since: 1.9.21
 **/
GPtrArray *
fu_device_get_counterpart_guids(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(GPtrArray) guids = g_ptr_array_new_with_free_func(g_free);

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);

	for (guint i = 0; priv->instance_ids != NULL && i < priv->instance_ids->len; i++) {
		FuDeviceInstanceIdItem *item = g_ptr_array_index(priv->instance_ids, i);
		if (item->flags & FU_DEVICE_INSTANCE_FLAG_COUNTERPART)
			g_ptr_array_add(guids, g_strdup(item->guid));
	}
	return g_steal_pointer(&guids);
}

/**
 * fu_device_get_metadata:
 * @self: a #FuDevice
 * @key: the key
 *
 * Gets an item of metadata from the device.
 *
 * Returns: a string value, or %NULL for unfound.
 *
 * Since: 0.1.0
 **/
const gchar *
fu_device_get_metadata(FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(key != NULL, NULL);
	if (priv->metadata == NULL)
		return NULL;
	return g_hash_table_lookup(priv->metadata, key);
}

/**
 * fu_device_get_metadata_boolean:
 * @self: a #FuDevice
 * @key: the key
 *
 * Gets an item of metadata from the device.
 *
 * Returns: a boolean value, or %FALSE for unfound or failure to parse.
 *
 * Since: 0.9.7
 **/
gboolean
fu_device_get_metadata_boolean(FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	const gchar *tmp;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(key != NULL, FALSE);

	if (priv->metadata == NULL)
		return FALSE;
	tmp = g_hash_table_lookup(priv->metadata, key);
	if (tmp == NULL)
		return FALSE;
	return g_strcmp0(tmp, "true") == 0;
}

/**
 * fu_device_get_metadata_integer:
 * @self: a #FuDevice
 * @key: the key
 *
 * Gets an item of metadata from the device.
 *
 * Returns: an integer value, or %G_MAXUINT for unfound or failure to parse.
 *
 * Since: 0.9.7
 **/
guint
fu_device_get_metadata_integer(FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	const gchar *tmp;
	guint64 val = 0;
	g_autoptr(GError) error_local = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), G_MAXUINT);
	g_return_val_if_fail(key != NULL, G_MAXUINT);

	if (priv->metadata == NULL)
		return G_MAXUINT;
	tmp = g_hash_table_lookup(priv->metadata, key);
	if (tmp == NULL)
		return G_MAXUINT;
	if (!fu_strtoull(tmp, &val, 0, G_MAXUINT, FU_INTEGER_BASE_AUTO, &error_local)) {
		g_warning("could not convert %s to integer: %s", tmp, error_local->message);
		return G_MAXUINT;
	}
	return (guint)val;
}

/**
 * fu_device_remove_metadata:
 * @self: a #FuDevice
 * @key: the key
 *
 * Removes an item of metadata on the device.
 *
 * Since: 1.3.3
 **/
void
fu_device_remove_metadata(FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	if (priv->metadata == NULL)
		return;
	g_hash_table_remove(priv->metadata, key);
}

/**
 * fu_device_set_metadata:
 * @self: a #FuDevice
 * @key: the key
 * @value: the string value
 *
 * Sets an item of metadata on the device.
 *
 * Since: 0.1.0
 **/
void
fu_device_set_metadata(FuDevice *self, const gchar *key, const gchar *value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	g_return_if_fail(value != NULL);
	if (priv->metadata == NULL)
		priv->metadata = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_hash_table_insert(priv->metadata, g_strdup(key), g_strdup(value));
}

/**
 * fu_device_set_metadata_boolean:
 * @self: a #FuDevice
 * @key: the key
 * @value: the boolean value
 *
 * Sets an item of metadata on the device. When @value is set to %TRUE
 * the actual stored value is `true`.
 *
 * Since: 0.9.7
 **/
void
fu_device_set_metadata_boolean(FuDevice *self, const gchar *key, gboolean value)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);

	fu_device_set_metadata(self, key, value ? "true" : "false");
}

/**
 * fu_device_set_metadata_integer:
 * @self: a #FuDevice
 * @key: the key
 * @value: the unsigned integer value
 *
 * Sets an item of metadata on the device. The integer is stored as a
 * base-10 string internally.
 *
 * Since: 0.9.7
 **/
void
fu_device_set_metadata_integer(FuDevice *self, const gchar *key, guint value)
{
	g_autofree gchar *tmp = g_strdup_printf("%u", value);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);

	fu_device_set_metadata(self, key, tmp);
}

/* ensure the name does not have the vendor name as the prefix */
static void
fu_device_fixup_vendor_name(FuDevice *self)
{
	const gchar *name = fu_device_get_name(self);
	const gchar *vendor = fu_device_get_vendor(self);
	if (name != NULL && vendor != NULL) {
		g_autofree gchar *name_up = g_utf8_strup(name, -1);
		g_autofree gchar *vendor_up = g_utf8_strup(vendor, -1);
		if (g_strcmp0(name_up, vendor_up) == 0) {
#ifndef SUPPORTED_BUILD
			g_warning("name and vendor are the same for %s [%s]",
				  fu_device_get_name(self),
				  fu_device_get_id(self));
#endif
			return;
		}
		if (g_str_has_prefix(name_up, vendor_up)) {
			gsize vendor_len = strlen(vendor);
			g_autofree gchar *name1 = g_strdup(name + vendor_len);
			g_autofree gchar *name2 = fu_strstrip(name1);
			g_debug("removing vendor prefix of '%s' from '%s'", vendor, name);
			fwupd_device_set_name(FWUPD_DEVICE(self), name2);
		}
	}
}

/**
 * fu_device_set_vendor:
 * @self: a #FuDevice
 * @vendor: a device vendor
 *
 * Sets the vendor name on the device.
 *
 * Since: 1.6.2
 **/
void
fu_device_set_vendor(FuDevice *self, const gchar *vendor)
{
	g_autofree gchar *vendor_safe = NULL;

	/* trim any leading and trailing spaces */
	if (vendor != NULL)
		vendor_safe = fu_strstrip(vendor);

	/* proxy */
	fwupd_device_set_vendor(FWUPD_DEVICE(self), vendor_safe);
	fu_device_fixup_vendor_name(self);
}

static gchar *
fu_device_sanitize_name(const gchar *value)
{
	gboolean last_was_space = FALSE;
	guint last_non_space = 0;
	g_autoptr(GString) new = g_string_new(NULL);

	/* add each printable char with maximum of one whitespace char */
	for (guint i = 0; value[i] != '\0'; i++) {
		const gchar tmp = value[i];
		if (!g_ascii_isprint(tmp))
			continue;
		if (g_ascii_isspace(tmp) || tmp == '_') {
			if (new->len == 0)
				continue;
			if (last_was_space)
				continue;
			last_was_space = TRUE;
			g_string_append_c(new, ' ');
		} else {
			last_was_space = FALSE;
			g_string_append_c(new, tmp);
			last_non_space = new->len;
		}
	}
	g_string_truncate(new, last_non_space);
	g_string_replace(new, "(TM)", "™", 0);
	g_string_replace(new, "(R)", "", 0);
	if (new->len == 0)
		return NULL;
	return g_string_free(g_steal_pointer(&new), FALSE);
}

/**
 * fu_device_set_name:
 * @self: a #FuDevice
 * @value: a device name
 *
 * Sets the name on the device. Any invalid parts will be converted or removed.
 *
 * Since: 0.7.1
 **/
void
fu_device_set_name(FuDevice *self, const gchar *value)
{
	g_autofree gchar *value_safe = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(value != NULL);

	/* overwriting? */
	value_safe = fu_device_sanitize_name(value);
	if (value_safe == NULL) {
		g_info("ignoring name value: '%s'", value);
		return;
	}
	if (g_strcmp0(value_safe, fu_device_get_name(self)) == 0)
		return;

	/* changing */
	if (fu_device_get_name(self) != NULL) {
		const gchar *id = fu_device_get_id(self);
		g_debug("%s device overwriting name value: %s->%s",
			id != NULL ? id : "unknown",
			fu_device_get_name(self),
			value_safe);
	}

	fwupd_device_set_name(FWUPD_DEVICE(self), value_safe);
	fu_device_fixup_vendor_name(self);
}

/**
 * fu_device_set_id:
 * @self: a #FuDevice
 * @id: a string, e.g. `tbt-port1`
 *
 * Sets the ID on the device. The ID should represent the *connection* of the
 * device, so that any similar device plugged into a different slot will
 * have a different @id string.
 *
 * The @id will be converted to a SHA1 hash if required before the device is
 * added to the daemon, and plugins should not assume that the ID that is set
 * here is the same as what is returned by fu_device_get_id().
 *
 * Since: 0.7.1
 **/
void
fu_device_set_id(FuDevice *self, const gchar *id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	GPtrArray *children;
	g_autofree gchar *id_hash = NULL;
	g_autofree gchar *id_hash_old = g_strdup(fwupd_device_get_id(FWUPD_DEVICE(self)));

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(id != NULL);

	/* allow sane device-id to be set directly */
	if (fwupd_device_id_is_valid(id)) {
		id_hash = g_strdup(id);
	} else {
		id_hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, id, -1);
		g_debug("using %s for %s", id_hash, id);
	}
	fwupd_device_set_id(FWUPD_DEVICE(self), id_hash);
	priv->device_id_valid = TRUE;

	/* ensure the parent ID is set */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *devtmp = g_ptr_array_index(children, i);
		fwupd_device_set_parent_id(FWUPD_DEVICE(devtmp), id_hash);

		/* update the composite ID of the child with the new ID if required; this will
		 * propagate to grandchildren and great-grandchildren as required */
		if (id_hash_old != NULL &&
		    g_strcmp0(fu_device_get_composite_id(devtmp), id_hash_old) == 0)
			fu_device_set_composite_id(devtmp, id_hash);
	}
}

/**
 * fu_device_set_version_format:
 * @self: a #FuDevice
 * @fmt: the version format, e.g. %FWUPD_VERSION_FORMAT_PLAIN
 *
 * Sets the device version format.
 *
 * Since: 1.4.0
 **/
void
fu_device_set_version_format(FuDevice *self, FwupdVersionFormat fmt)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	/* same */
	if (fu_device_get_version_format(self) == fmt)
		return;
	if (fu_device_get_version_format(self) != FWUPD_VERSION_FORMAT_UNKNOWN) {
		g_debug("changing verfmt for %s: %s->%s",
			fu_device_get_id(self),
			fwupd_version_format_to_string(fu_device_get_version_format(self)),
			fwupd_version_format_to_string(fmt));
	}
	fwupd_device_set_version_format(FWUPD_DEVICE(self), fmt);

	/* convert this, now we know */
	if (device_class->convert_version != NULL) {
		if (fu_device_get_version_raw(self) != 0) {
			g_autofree gchar *version =
			    device_class->convert_version(self, fu_device_get_version_raw(self));
			fu_device_set_version(self, version);
		}
		if (fu_device_get_version_lowest_raw(self) != 0) {
			g_autofree gchar *version =
			    device_class->convert_version(self,
							  fu_device_get_version_lowest_raw(self));
			fu_device_set_version_lowest(self, version);
		}
	}
}

/**
 * fu_device_set_version:
 * @self: a #FuDevice
 * @version: (nullable): a string, e.g. `1.2.3`
 *
 * Sets the device version, sanitizing the string if required.
 *
 * Since: 1.2.9
 **/
void
fu_device_set_version(FuDevice *self, const gchar *version)
{
	g_autofree gchar *version_safe = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));

	/* sanitize if required */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_ENSURE_SEMVER)) {
		version_safe =
		    fu_version_ensure_semver(version, fu_device_get_version_format(self));
		if (g_strcmp0(version, version_safe) != 0)
			g_debug("converted '%s' to '%s'", version, version_safe);
	} else {
		version_safe = g_strdup(version);
	}

	/* print a console warning for an invalid version, if semver */
	if (version_safe != NULL &&
	    !fu_version_verify_format(version_safe, fu_device_get_version_format(self), &error))
#ifdef SUPPORTED_BUILD
		g_warning("%s", error->message);
#else
		g_critical("%s", error->message);
#endif

	/* if different */
	if (g_strcmp0(fu_device_get_version(self), version_safe) != 0) {
		if (fu_device_get_version(self) != NULL) {
			g_debug("changing version for %s: %s->%s",
				fu_device_get_id(self),
				fu_device_get_version(self),
				version_safe);
		}
		fwupd_device_set_version(FWUPD_DEVICE(self), version_safe);
	}
}

/**
 * fu_device_set_version_lowest:
 * @self: a #FuDevice
 * @version: (nullable): a string, e.g. `1.2.3`
 *
 * Sets the device lowest version, sanitizing the string if required.
 *
 * Since: 1.4.0
 **/
void
fu_device_set_version_lowest(FuDevice *self, const gchar *version)
{
	g_autofree gchar *version_safe = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));

	/* sanitize if required */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_ENSURE_SEMVER)) {
		version_safe =
		    fu_version_ensure_semver(version, fu_device_get_version_format(self));
		if (g_strcmp0(version, version_safe) != 0)
			g_debug("converted '%s' to '%s'", version, version_safe);
	} else {
		version_safe = g_strdup(version);
	}

	/* print a console warning for an invalid version, if semver */
	if (version_safe != NULL &&
	    !fu_version_verify_format(version_safe, fu_device_get_version_format(self), &error))
#ifdef SUPPORTED_BUILD
		g_warning("%s", error->message);
#else
		g_critical("%s", error->message);
#endif

	/* if different */
	if (g_strcmp0(fu_device_get_version_lowest(self), version_safe) != 0) {
		if (fu_device_get_version_lowest(self) != NULL) {
			g_debug("changing version lowest for %s: %s->%s",
				fu_device_get_id(self),
				fu_device_get_version_lowest(self),
				version_safe);
		}
		fwupd_device_set_version_lowest(FWUPD_DEVICE(self), version_safe);
	}
}

/**
 * fu_device_set_version_bootloader:
 * @self: a #FuDevice
 * @version: (nullable): a string, e.g. `1.2.3`
 *
 * Sets the device bootloader version, sanitizing the string if required.
 *
 * Since: 1.4.0
 **/
void
fu_device_set_version_bootloader(FuDevice *self, const gchar *version)
{
	g_autofree gchar *version_safe = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));

	/* sanitize if required */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_ENSURE_SEMVER)) {
		version_safe =
		    fu_version_ensure_semver(version, fu_device_get_version_format(self));
		if (g_strcmp0(version, version_safe) != 0)
			g_debug("converted '%s' to '%s'", version, version_safe);
	} else {
		version_safe = g_strdup(version);
	}

	/* print a console warning for an invalid version, if semver */
	if (version_safe != NULL &&
	    !fu_version_verify_format(version_safe, fu_device_get_version_format(self), &error))
#ifdef SUPPORTED_BUILD
		g_warning("%s", error->message);
#else
		g_critical("%s", error->message);
#endif

	/* if different */
	if (g_strcmp0(fu_device_get_version_bootloader(self), version_safe) != 0) {
		if (fu_device_get_version_bootloader(self) != NULL) {
			g_debug("changing version for %s: %s->%s",
				fu_device_get_id(self),
				fu_device_get_version_bootloader(self),
				version_safe);
		}
		fwupd_device_set_version_bootloader(FWUPD_DEVICE(self), version_safe);
	}
}

/**
 * fu_device_set_version_raw:
 * @self: a #FuDevice
 * @version_raw: an integer
 *
 * Sets the raw device version from a integer value and the device version format.
 *
 * Since: 1.9.8
 **/
void
fu_device_set_version_raw(FuDevice *self, guint64 version_raw)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	fwupd_device_set_version_raw(FWUPD_DEVICE(self), version_raw);
	if (device_class->convert_version != NULL) {
		g_autofree gchar *version = device_class->convert_version(self, version_raw);
		if (version != NULL)
			fu_device_set_version(self, version);
	}
}

/**
 * fu_device_set_version_lowest_raw:
 * @self: a #FuDevice
 * @version_raw: an integer
 *
 * Sets the raw device version from a integer value and the device version format.
 *
 * Since: 2.0.7
 **/
void
fu_device_set_version_lowest_raw(FuDevice *self, guint64 version_raw)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	fwupd_device_set_version_lowest_raw(FWUPD_DEVICE(self), version_raw);
	if (device_class->convert_version != NULL) {
		g_autofree gchar *version = device_class->convert_version(self, version_raw);
		if (version != NULL)
			fu_device_set_version_lowest(self, version);
	}
}

/* private */
gboolean
fu_device_is_updatable(FuDevice *self)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	return fu_device_has_flag(self, FWUPD_DEVICE_FLAG_UPDATABLE) ||
	       fu_device_has_flag(self, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN);
}

static void
fu_device_inhibit_free(FuDeviceInhibit *inhibit)
{
	g_free(inhibit->inhibit_id);
	g_free(inhibit->reason);
	g_free(inhibit);
}

static void
fu_device_ensure_inhibits(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FwupdDeviceProblem problems = FWUPD_DEVICE_PROBLEM_NONE;
	guint nr_inhibits = g_hash_table_size(priv->inhibits);

	/* was okay -> not okay */
	if (nr_inhibits > 0) {
		g_autofree gchar *reasons_str = NULL;
		g_autoptr(GList) values = g_hash_table_get_values(priv->inhibits);
		g_autoptr(GPtrArray) reasons = g_ptr_array_new();

		/* updatable -> updatable-hidden -- which is required as devices might have
		 * inhibits and *not* be automatically updatable */
		if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_UPDATABLE)) {
			fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_UPDATABLE);
			fwupd_device_add_flag(FWUPD_DEVICE(self),
					      FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN);
		}

		/* update the update error */
		for (GList *l = values; l != NULL; l = l->next) {
			FuDeviceInhibit *inhibit = (FuDeviceInhibit *)l->data;
			g_ptr_array_add(reasons, inhibit->reason);
			problems |= inhibit->problem;
		}
		reasons_str = fu_strjoin(", ", reasons);
		fu_device_set_update_error(self, reasons_str);
	} else {
		if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN)) {
			fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN);
			fwupd_device_add_flag(FWUPD_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
		}
		fu_device_set_update_error(self, NULL);
	}

	/* sync with baseclass */
	fwupd_device_set_problems(FWUPD_DEVICE(self), problems);
}

static gchar *
fu_device_problem_to_inhibit_reason(FuDevice *self, guint64 device_problem)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	if (device_problem == FWUPD_DEVICE_PROBLEM_UNREACHABLE)
		return g_strdup("Device is unreachable, or out of wireless range");
	if (device_problem == FWUPD_DEVICE_PROBLEM_UPDATE_PENDING)
		return g_strdup("Device is waiting for the update to be applied");
	if (device_problem == FWUPD_DEVICE_PROBLEM_REQUIRE_AC_POWER)
		return g_strdup("Device requires AC power to be connected");
	if (device_problem == FWUPD_DEVICE_PROBLEM_LID_IS_CLOSED)
		return g_strdup("Device cannot be used while the lid is closed");
	if (device_problem == FWUPD_DEVICE_PROBLEM_IS_EMULATED)
		return g_strdup("Device is emulated");
	if (device_problem == FWUPD_DEVICE_PROBLEM_UPDATE_IN_PROGRESS)
		return g_strdup("An update is in progress");
	if (device_problem == FWUPD_DEVICE_PROBLEM_IN_USE)
		return g_strdup("Device is in use");
	if (device_problem == FWUPD_DEVICE_PROBLEM_DISPLAY_REQUIRED)
		return g_strdup("Device requires a display to be plugged in");
	if (device_problem == FWUPD_DEVICE_PROBLEM_LOWER_PRIORITY) {
		if (fu_device_get_equivalent_id(self) == NULL)
			return g_strdup("Device is lower priority than an equivalent device");
		return g_strdup_printf("Device is lower priority than equivalent device %s",
				       fu_device_get_equivalent_id(self));
	}
	if (device_problem == FWUPD_DEVICE_PROBLEM_MISSING_LICENSE)
		return g_strdup("Device does not have the necessary license installed");
	if (device_problem == FWUPD_DEVICE_PROBLEM_SYSTEM_POWER_TOO_LOW) {
		if (priv->ctx == NULL)
			return g_strdup("System power is too low");
		return g_strdup_printf("System power is too low (%u%%, requires %u%%)",
				       fu_context_get_battery_level(priv->ctx),
				       fu_context_get_battery_threshold(priv->ctx));
	}
	if (device_problem == FWUPD_DEVICE_PROBLEM_POWER_TOO_LOW) {
		if (fu_device_get_battery_level(self) == FWUPD_BATTERY_LEVEL_INVALID ||
		    fu_device_get_battery_threshold(self) == FWUPD_BATTERY_LEVEL_INVALID) {
			return g_strdup_printf("Device battery power is too low");
		}
		return g_strdup_printf("Device battery power is too low (%u%%, requires %u%%)",
				       fu_device_get_battery_level(self),
				       fu_device_get_battery_threshold(self));
	}
	return NULL;
}

static void
fu_device_inhibit_full(FuDevice *self,
		       FwupdDeviceProblem problem,
		       const gchar *inhibit_id,
		       const gchar *reason)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceInhibit *inhibit;

	g_return_if_fail(FU_IS_DEVICE(self));

	/* lazy create as most devices will not need this */
	if (priv->inhibits == NULL) {
		priv->inhibits = g_hash_table_new_full(g_str_hash,
						       g_str_equal,
						       NULL,
						       (GDestroyNotify)fu_device_inhibit_free);
	}

	/* can fallback */
	if (inhibit_id == NULL)
		inhibit_id = fwupd_device_problem_to_string(problem);

	/* already exists */
	inhibit = g_hash_table_lookup(priv->inhibits, inhibit_id);
	if (inhibit != NULL)
		return;

	/* create new */
	inhibit = g_new0(FuDeviceInhibit, 1);
	inhibit->problem = problem;
	inhibit->inhibit_id = g_strdup(inhibit_id);
	if (reason != NULL) {
		inhibit->reason = g_strdup(reason);
	} else {
		inhibit->reason = fu_device_problem_to_inhibit_reason(self, problem);
	}
	g_hash_table_insert(priv->inhibits, inhibit->inhibit_id, inhibit);

	/* refresh */
	fu_device_ensure_inhibits(self);

	/* propagate to children */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_INHIBIT_CHILDREN)) {
		GPtrArray *children = fu_device_get_children(self);
		for (guint i = 0; i < children->len; i++) {
			FuDevice *child = g_ptr_array_index(children, i);
			fu_device_inhibit(child, inhibit_id, reason);
		}
	}
}

/**
 * fu_device_inhibit:
 * @self: a #FuDevice
 * @inhibit_id: an ID used for uninhibiting, e.g. `low-power`
 * @reason: (nullable): a string, e.g. `Cannot update as foo [bar] needs reboot`
 *
 * Prevent the device from being updated, changing it from %FWUPD_DEVICE_FLAG_UPDATABLE
 * to %FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN if not already inhibited.
 *
 * If the device already has an inhibit with the same @inhibit_id then the request
 * is ignored.
 *
 * Since: 1.6.0
 **/
void
fu_device_inhibit(FuDevice *self, const gchar *inhibit_id, const gchar *reason)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(inhibit_id != NULL);
	fu_device_inhibit_full(self, FWUPD_DEVICE_PROBLEM_NONE, inhibit_id, reason);
}

/**
 * fu_device_has_inhibit:
 * @self: a #FuDevice
 * @inhibit_id: an ID used for inhibiting, e.g. `low-power`
 *
 * Check if the device already has an inhibit with a specific ID.
 *
 * Returns: %TRUE if added
 *
 * Since: 1.8.0
 **/
gboolean
fu_device_has_inhibit(FuDevice *self, const gchar *inhibit_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(inhibit_id != NULL, FALSE);

	if (priv->inhibits == NULL)
		return FALSE;
	return g_hash_table_contains(priv->inhibits, inhibit_id);
}

/**
 * fu_device_remove_problem:
 * @self: a #FuDevice
 * @problem: a #FwupdDeviceProblem, e.g. %FWUPD_DEVICE_PROBLEM_SYSTEM_POWER_TOO_LOW
 *
 * Allow the device from being updated if there are no other inhibitors,
 * changing it from %FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN to %FWUPD_DEVICE_FLAG_UPDATABLE.
 *
 * If the device already has no inhibit with the @inhibit_id then the request
 * is ignored.
 *
 * Since: 1.8.1
 **/
void
fu_device_remove_problem(FuDevice *self, FwupdDeviceProblem problem)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(problem != FWUPD_DEVICE_PROBLEM_UNKNOWN);
	return fu_device_uninhibit(self, fwupd_device_problem_to_string(problem));
}

/**
 * fu_device_has_problem:
 * @self: a #FuDevice
 * @problem: a #FwupdDeviceProblem, e.g. %FWUPD_DEVICE_PROBLEM_SYSTEM_POWER_TOO_LOW
 *
 * Query if a device has a specific problem.
 *
 * Returns: %TRUE if the device has this problem
 *
 * Since: 1.8.11
 **/
gboolean
fu_device_has_problem(FuDevice *self, FwupdDeviceProblem problem)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(problem != FWUPD_DEVICE_PROBLEM_UNKNOWN, FALSE);
	return fu_device_has_inhibit(self, fwupd_device_problem_to_string(problem));
}

/**
 * fu_device_add_problem:
 * @self: a #FuDevice
 * @problem: a #FwupdDeviceProblem, e.g. %FWUPD_DEVICE_PROBLEM_SYSTEM_POWER_TOO_LOW
 *
 * Prevent the device from being updated, changing it from %FWUPD_DEVICE_FLAG_UPDATABLE
 * to %FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN if not already inhibited.
 *
 * If the device already has an inhibit with the same @problem then the request
 * is ignored.
 *
 * Since: 1.8.1
 **/
void
fu_device_add_problem(FuDevice *self, FwupdDeviceProblem problem)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(problem != FWUPD_DEVICE_PROBLEM_UNKNOWN);
	fu_device_inhibit_full(self, problem, NULL, NULL);
}

/**
 * fu_device_uninhibit:
 * @self: a #FuDevice
 * @inhibit_id: an ID used for uninhibiting, e.g. `low-power`
 *
 * Allow the device from being updated if there are no other inhibitors,
 * changing it from %FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN to %FWUPD_DEVICE_FLAG_UPDATABLE.
 *
 * If the device already has no inhibit with the @inhibit_id then the request
 * is ignored.
 *
 * Since: 1.6.0
 **/
void
fu_device_uninhibit(FuDevice *self, const gchar *inhibit_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(inhibit_id != NULL);

	if (priv->inhibits == NULL)
		return;
	if (g_hash_table_remove(priv->inhibits, inhibit_id))
		fu_device_ensure_inhibits(self);

	/* propagate to children */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_INHIBIT_CHILDREN)) {
		GPtrArray *children = fu_device_get_children(self);
		for (guint i = 0; i < children->len; i++) {
			FuDevice *child = g_ptr_array_index(children, i);
			fu_device_uninhibit(child, inhibit_id);
		}
	}
}

/**
 * fu_device_ensure_id:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * If not already set, generates a device ID with the optional physical and
 * logical IDs.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_ensure_id(FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autofree gchar *device_id = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* already set */
	if (priv->device_id_valid)
		return TRUE;

	/* nothing we can do! */
	if (priv->physical_id == NULL) {
		g_autofree gchar *tmp = fu_device_to_string(self);
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "cannot ensure ID: %s",
			    tmp);
		return FALSE;
	}

	/* logical may be NULL */
	device_id =
	    g_strjoin(":", fu_device_get_physical_id(self), fu_device_get_logical_id(self), NULL);
	fu_device_set_id(self, device_id);
	return TRUE;
}

/**
 * fu_device_get_logical_id:
 * @self: a #FuDevice
 *
 * Gets the logical ID set for the device, which disambiguates devices with the
 * same physical ID.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.2
 **/
const gchar *
fu_device_get_logical_id(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->logical_id;
}

/**
 * fu_device_set_logical_id:
 * @self: a #FuDevice
 * @logical_id: a string, e.g. `dev2`
 *
 * Sets the logical ID on the device. This is designed to disambiguate devices
 * with the same physical ID.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_logical_id(FuDevice *self, const gchar *logical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->logical_id, logical_id) == 0)
		return;

	/* not allowed after ->probe() and ->setup() have completed */
	if (priv->done_setup) {
		g_warning("cannot change %s logical ID from %s to %s as "
			  "FuDevice->setup() has already completed",
			  fu_device_get_id(self),
			  priv->logical_id,
			  logical_id);
		return;
	}

	g_free(priv->logical_id);
	priv->logical_id = g_strdup(logical_id);
	priv->device_id_valid = FALSE;
	g_object_notify(G_OBJECT(self), "logical-id");
}

/**
 * fu_device_get_backend_id:
 * @self: a #FuDevice
 *
 * Gets the ID set for the device as recognized by the backend. This is typically
 * a Linux sysfs path or USB platform ID. If unset, it also falls back to the
 * physical ID as this may be the same value.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.5.8
 **/
const gchar *
fu_device_get_backend_id(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	if (priv->backend_id != NULL)
		return priv->backend_id;
	return priv->physical_id;
}

/**
 * fu_device_set_backend_id:
 * @self: a #FuDevice
 * @backend_id: a string, e.g. `dev2`
 *
 * Sets the backend ID on the device. This is designed to disambiguate devices
 * with the same physical ID. This is typically a Linux sysfs path or USB
 * platform ID.
 *
 * Since: 1.5.8
 **/
void
fu_device_set_backend_id(FuDevice *self, const gchar *backend_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->backend_id, backend_id) == 0)
		return;

	g_free(priv->backend_id);
	priv->backend_id = g_strdup(backend_id);
	priv->device_id_valid = FALSE;
	g_object_notify(G_OBJECT(self), "backend-id");
}

/**
 * fu_device_get_backend:
 * @self: a #FuDevice
 *
 * Gets the backend, if set with fu_device_set_backend().
 *
 * Returns: (transfer none): a #FuBackend or %NULL if not sset
 *
 * Since: 2.0.0
 **/
FuBackend *
fu_device_get_backend(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->backend;
}

/**
 * fu_device_set_backend:
 * @self: a #FuDevice
 * @backend: a #FuBackend
 *
 * Sets the backend that created this device.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_backend(FuDevice *self, FuBackend *backend)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(backend == NULL || FU_IS_BACKEND(backend));

	/* same */
	if (priv->backend == backend)
		return;

	/* not already set */
	if (priv->ctx == NULL)
		fu_device_set_context(self, fu_backend_get_context(backend));

	/* there is no ref on backend to prevent a loop */
	if (priv->backend != NULL)
		g_object_remove_weak_pointer(G_OBJECT(priv->backend), (gpointer *)&priv->backend);
	if (backend != NULL)
		g_object_add_weak_pointer(G_OBJECT(backend), (gpointer *)&priv->backend);
	priv->backend = backend; /* no ref */

	/* in case anything cares */
	g_object_notify(G_OBJECT(self), "backend");
}

/**
 * fu_device_get_backend_parent_with_subsystem:
 * @self: a #FuDevice
 * @subsystem: (nullable): an optional device subsystem, e.g. "usb:usb_device"
 * @error: (nullable): optional return location for an error
 *
 * Creates a device parent (of the correct type) using the current backend for a given device kind.
 *
 * NOTE: The backend must implement `FuBackendClass->get_device_parent` for this method to work --
 * for cases where the plugin has created both parent and child, and used `fu_device_add_child()`,
 * using `fu_device_get_parent()` is probably more appropriate.
 *
 * Returns: (transfer full): a #FuDevice or %NULL if not found or unimplemented
 *
 * Since: 2.0.0
 **/
FuDevice *
fu_device_get_backend_parent_with_subsystem(FuDevice *self, const gchar *subsystem, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceEvent *event = NULL;
	g_autofree gchar *event_id = NULL;
	g_autoptr(FuDevice) parent = NULL;
	g_autoptr(GError) error_local = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* sanity check */
	if (priv->backend == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no backend set for device");
		return NULL;
	}

	/* need event ID */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED) ||
	    fu_context_has_flag(fu_device_get_context(FU_DEVICE(self)),
				FU_CONTEXT_FLAG_SAVE_EVENTS)) {
		event_id = g_strdup_printf("GetBackendParent:Subsystem=%s", subsystem);
	}

	/* emulated */
	if (fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_EMULATED)) {
		GType gtype;
		const gchar *gtype_str;
		const gchar *id;

		/* we might have to propagate this to preserve compat with older emulation files */
		if (fu_device_check_fwupd_version(self, "2.0.8")) {
			event = fu_device_load_event(FU_DEVICE(self), event_id, error);
			if (event == NULL)
				return NULL;
			if (!fu_device_event_check_error(event, error))
				return NULL;
		} else {
			event = fu_device_load_event(FU_DEVICE(self), event_id, NULL);
			if (event == NULL) {
				g_debug("falling back for emulation recorded by fwupd %s",
					priv->fwupd_version);
				parent = fu_backend_get_device_parent(priv->backend,
								      self,
								      subsystem,
								      error);
				if (parent != self)
					fu_device_set_target(parent, self);
				return g_steal_pointer(&parent);
			}
		}

		/* missing GType is 'no parent found' */
		gtype_str = fu_device_event_get_str(event, "GType", NULL);
		if (gtype_str == NULL) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no parent with subsystem %s",
				    subsystem);
			return NULL;
		}
		gtype = g_type_from_name(gtype_str);
		if (gtype == G_TYPE_INVALID) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no GType %s",
				    gtype_str);
			return NULL;
		}
		parent =
		    g_object_new(gtype, "context", fu_device_get_context(FU_DEVICE(self)), NULL);
		fu_device_add_flag(parent, FWUPD_DEVICE_FLAG_EMULATED);
		id = fu_device_event_get_str(event, "DeviceId", NULL);
		if (id != NULL)
			fu_device_set_id(parent, id);
		id = fu_device_event_get_str(event, "BackendId", NULL);
		if (id != NULL)
			fu_device_set_backend_id(parent, id);
		id = fu_device_event_get_str(event, "PhysicalId", NULL);
		if (id != NULL)
			fu_device_set_physical_id(parent, id);
		if (parent != self)
			fu_device_set_target(parent, self);
		return g_steal_pointer(&parent);
	}

	/* save */
	if (event_id != NULL)
		event = fu_device_save_event(FU_DEVICE(self), event_id);

	/* call into the backend */
	parent = fu_backend_get_device_parent(priv->backend, self, subsystem, &error_local);
	if (parent == NULL) {
		if (event != NULL)
			fu_device_event_set_error(event, error_local);
		g_propagate_error(error, g_steal_pointer(&error_local));
		return NULL;
	}
	if (!fu_device_probe(parent, &error_local)) {
		if (event != NULL)
			fu_device_event_set_error(event, error_local);
		g_propagate_error(error, g_steal_pointer(&error_local));
		return NULL;
	}

	/* save response */
	if (event != NULL) {
		fu_device_event_set_str(event, "GType", G_OBJECT_TYPE_NAME(parent));
		if (fu_device_get_id(self) != NULL)
			fu_device_event_set_str(event, "DeviceId", fu_device_get_id(parent));
		if (fu_device_get_backend_id(parent) != NULL) {
			fu_device_event_set_str(event,
						"BackendId",
						fu_device_get_backend_id(parent));
		}
		if (fu_device_get_physical_id(parent) != NULL) {
			fu_device_event_set_str(event,
						"PhysicalId",
						fu_device_get_physical_id(parent));
		}
	}

	if (parent != self)
		fu_device_set_target(parent, self);
	return g_steal_pointer(&parent);
}

/**
 * fu_device_get_backend_parent:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Creates a device parent (of the correct type) using the current backend.
 *
 * NOTE: The backend must implement `FuBackendClass->get_device_parent` for this method to work --
 * for cases where the plugin has created both parent and child, and used `fu_device_add_child()`,
 * using `fu_device_get_parent()` is probably more appropriate.
 *
 * Returns: (transfer full): a #FuDevice or %NULL if not found or unimplemented
 *
 * Since: 2.0.0
 **/
FuDevice *
fu_device_get_backend_parent(FuDevice *self, GError **error)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);
	return fu_device_get_backend_parent_with_subsystem(self, NULL, error);
}

/**
 * fu_device_get_update_request_id:
 * @self: a #FuDevice
 *
 * Gets the update request ID as specified from `LVFS::UpdateRequestId`.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.8.6
 **/
const gchar *
fu_device_get_update_request_id(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->update_request_id;
}

/**
 * fu_device_set_update_request_id:
 * @self: a #FuDevice
 * @update_request_id: a string, e.g. `org.freedesktop.fwupd.request.do-not-power-off`
 *
 * Sets the update request ID as specified in `LVFS::UpdateRequestId`.
 *
 * Since: 1.8.6
 **/
void
fu_device_set_update_request_id(FuDevice *self, const gchar *update_request_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->update_request_id, update_request_id) == 0)
		return;

	g_free(priv->update_request_id);
	priv->update_request_id = g_strdup(update_request_id);
}

/**
 * fu_device_get_update_message:
 * @self: a #FuDevice
 *
 * Gets the update message string.
 *
 * Returns: the update message string, or %NULL if unset
 *
 * Since: 2.0.0
 **/
const gchar *
fu_device_get_update_message(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->update_message;
}

/**
 * fu_device_set_update_message:
 * @self: a #FuDevice
 * @update_message: (nullable): the update message string
 *
 * Sets the update message string.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_update_message(FuDevice *self, const gchar *update_message)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->update_message, update_message) == 0)
		return;

	g_free(priv->update_message);
	priv->update_message = g_strdup(update_message);
	g_object_notify(G_OBJECT(self), "update-message");
}

/**
 * fu_device_get_update_image:
 * @self: a #FuDevice
 *
 * Gets the update image URL.
 *
 * Returns: the update image URL, or %NULL if unset
 *
 * Since: 2.0.0
 **/
const gchar *
fu_device_get_update_image(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->update_image;
}

/**
 * fu_device_set_update_image:
 * @self: a #FuDevice
 * @update_image: (nullable): the update image URL
 *
 * Sets the update image URL.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_update_image(FuDevice *self, const gchar *update_image)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->update_image, update_image) == 0)
		return;

	g_free(priv->update_image);
	priv->update_image = g_strdup(update_image);
	g_object_notify(G_OBJECT(self), "update-image");
}

/**
 * fu_device_check_fwupd_version:
 * @self: a #FuDevice
 * @fwupd_version: (not nullable): the version, e.g. `2.0.8`
 *
 * Checks the fwupd version that created the emulation.
 *
 * Returns: %TRUE if @fwupd_version version is >= to the emulation version
 *
 * Since: 2.0.13
 **/
gboolean
fu_device_check_fwupd_version(FuDevice *self, const gchar *fwupd_version)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(fwupd_version != NULL, FALSE);
	if (priv->fwupd_version == NULL)
		return FALSE;
	return fu_version_compare(priv->fwupd_version,
				  fwupd_version,
				  FWUPD_VERSION_FORMAT_TRIPLET) >= 0;
}

/**
 * fu_device_set_fwupd_version:
 * @self: a #FuDevice
 * @fwupd_version: (nullable): the version, e.g. `2.0.8`
 *
 * Sets the fwupd version that created the emulation.
 *
 * NOTE: This can only be set on devices with the %FWUPD_DEVICE_FLAG_EMULATED flag set.
 *
 * Since: 2.0.8
 **/
void
fu_device_set_fwupd_version(FuDevice *self, const gchar *fwupd_version)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(fu_device_has_flag(self, FWUPD_DEVICE_FLAG_EMULATED));

	/* not changed */
	if (g_strcmp0(priv->fwupd_version, fwupd_version) == 0)
		return;

	g_free(priv->fwupd_version);
	priv->fwupd_version = g_strdup(fwupd_version);
}

/**
 * fu_device_get_proxy_guid:
 * @self: a #FuDevice
 *
 * Gets the proxy GUID device, which is set to let the engine match up the
 * proxy between plugins.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.4.1
 **/
const gchar *
fu_device_get_proxy_guid(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->proxy_guid;
}

/**
 * fu_device_set_proxy_guid:
 * @self: a #FuDevice
 * @proxy_guid: a string, e.g. `USB\VID_413C&PID_B06E&hub`
 *
 * Sets the GUID of the proxy device. The proxy device may update @self.
 *
 * Since: 1.4.1
 **/
void
fu_device_set_proxy_guid(FuDevice *self, const gchar *proxy_guid)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));

	/* not changed */
	if (g_strcmp0(priv->proxy_guid, proxy_guid) == 0)
		return;

	g_free(priv->proxy_guid);
	priv->proxy_guid = g_strdup(proxy_guid);
}

/**
 * fu_device_set_physical_id:
 * @self: a #FuDevice
 * @physical_id: a string that identifies the physical device connection
 *
 * Sets the physical ID on the device which represents the electrical connection
 * of the device to the system. Multiple #FuDevices can share a physical ID.
 *
 * The physical ID is used to remove logical devices when a physical device has
 * been removed from the system.
 *
 * A sysfs or devpath is not a physical ID, but could be something like
 * `PCI_SLOT_NAME=0000:3e:00.0`.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_physical_id(FuDevice *self, const gchar *physical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(physical_id != NULL);

	/* not changed */
	if (g_strcmp0(priv->physical_id, physical_id) == 0)
		return;

	/* not allowed after ->probe() and ->setup() have completed */
	if (priv->done_setup) {
		g_warning("cannot change %s physical ID from %s to %s as "
			  "FuDevice->setup() has already completed",
			  fu_device_get_id(self),
			  priv->physical_id,
			  physical_id);
		return;
	}

	g_free(priv->physical_id);
	priv->physical_id = g_strdup(physical_id);
	priv->device_id_valid = FALSE;
	g_object_notify(G_OBJECT(self), "physical-id");
}

/**
 * fu_device_get_physical_id:
 * @self: a #FuDevice
 *
 * Gets the physical ID set for the device, which represents the electrical
 * connection used to compare devices.
 *
 * Multiple #FuDevices can share a single physical ID.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.2
 **/
const gchar *
fu_device_get_physical_id(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->physical_id;
}

/**
 * fu_device_remove_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Removes a device flag from the device.
 *
 * Since: 1.6.0
 **/
void
fu_device_remove_flag(FuDevice *self, FwupdDeviceFlags flag)
{
	/* proxy */
	fwupd_device_remove_flag(FWUPD_DEVICE(self), flag);

	/* allow it to be updatable again */
	if (flag & FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)
		fu_device_uninhibit(self, "needs-activation");
	if (flag & FWUPD_DEVICE_FLAG_UNREACHABLE)
		fu_device_uninhibit(self, "unreachable");
}

/**
 * fu_device_add_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Adds a device flag to the device.
 *
 * Since: 0.1.0
 **/
void
fu_device_add_flag(FuDevice *self, FwupdDeviceFlags flag)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	/* none is not used as an "exported" flag */
	if (flag == FWUPD_DEVICE_FLAG_NONE)
		return;

	/* emulated device reinstalling do not need a replug or shutdown */
	if (flag == FWUPD_DEVICE_FLAG_EMULATED) {
		if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_NEEDS_REBOOT)) {
			g_debug("removing needs-reboot for emulated device");
			fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
		}
		if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN)) {
			g_debug("removing needs-shutdown for emulated device");
			fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN);
		}
	}

	/* we only inhibit when the flags contains UPDATABLE, and that might be discovered by
	 * probing the hardware *after* the battery level has been set */
	if (flag == FWUPD_DEVICE_FLAG_UPDATABLE && priv->inhibits != NULL)
		fu_device_ensure_inhibits(self);

	/* an emulated device cannot be used to record emulation events,
	 * and an emulated device cannot be tagged for emulation */
	if (flag == FWUPD_DEVICE_FLAG_EMULATED)
		fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_CAN_EMULATION_TAG);
	if (flag == FWUPD_DEVICE_FLAG_CAN_EMULATION_TAG &&
	    fu_device_has_flag(self, FWUPD_DEVICE_FLAG_EMULATED))
		return;

	/* being both a bootloader and requiring a bootloader is invalid */
	if (flag & FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER)
		fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	if (flag & FWUPD_DEVICE_FLAG_IS_BOOTLOADER)
		fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER);

	/* being both a signed and unsigned is invalid */
	if (flag & FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD)
		fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
	if (flag & FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD)
		fu_device_remove_flag(self, FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);

	/* one implies the other */
	if (flag & FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE)
		flag |= FWUPD_DEVICE_FLAG_CAN_VERIFY;
	if (flag & FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES)
		flag |= FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED;
	fwupd_device_add_flag(FWUPD_DEVICE(self), flag);

	/* activatable devices shouldn't be allowed to update again until activated */
	/* don't let devices be updated until activated */
	if (flag & FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)
		fu_device_inhibit(self, "needs-activation", "Pending activation");

	/* do not let devices be updated until back in range */
	if (flag & FWUPD_DEVICE_FLAG_UNREACHABLE)
		fu_device_add_problem(self, FWUPD_DEVICE_PROBLEM_UNREACHABLE);

	/* fixup and maybe show a warning if the remove delay was forgotten */
	if ((flag & FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG) && priv->remove_delay == 0) {
		priv->remove_delay = FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE;
#ifndef SUPPORTED_BUILD
		g_critical("FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG added but remove delay is unset! -- "
			   "add something like fu_device_set_remove_delay(FU_DEVICE(self), "
			   "FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE) to the %s _init()",
			   G_OBJECT_TYPE_NAME(self));
#endif
	}
}

/**
 * fu_device_register_private_flag:
 * @self: a #FuDevice
 * @flag: a string
 *
 * Registers a private device flag so that it can be set from quirk files and printed
 * correctly in debug output.
 *
 * Since: 2.0.0
 **/
void
fu_device_register_private_flag(FuDevice *self, const gchar *flag)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	GQuark flag_quark;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(flag != NULL);

#ifndef SUPPORTED_BUILD
	/* ensure not already the name of an internal or exported flag */
	if (fwupd_device_flag_from_string(flag) != FWUPD_DEVICE_FLAG_UNKNOWN) {
		g_critical("%s private flag %s already exists as an exported flag",
			   G_OBJECT_TYPE_NAME(self),
			   flag);
		return;
	}
#endif
	/* ensure */
	fu_device_register_private_flags(self);

	/* sanity check */
	flag_quark = fu_device_find_private_flag_quark(self, flag);
	if (G_UNLIKELY(flag_quark != 0)) {
		g_critical("already registered private %s flag %s", G_OBJECT_TYPE_NAME(self), flag);
		return;
	}

	/* add new */
	flag_quark = g_quark_from_static_string(flag);
	g_array_append_val(priv->private_flags_registered, flag_quark);
}

static void
fu_device_set_custom_flag(FuDevice *self, const gchar *hint)
{
	FwupdDeviceFlags flag;
	GQuark flag_quark;

	g_return_if_fail(hint != NULL);

	/* is this a negated device flag */
	if (g_str_has_prefix(hint, "~")) {
		flag = fwupd_device_flag_from_string(hint + 1);
		if (flag != FWUPD_DEVICE_FLAG_UNKNOWN) {
			fu_device_remove_flag(self, flag);
			return;
		}
		flag_quark = fu_device_find_private_flag_quark(self, hint + 1);
		if (flag_quark != 0) {
			fu_device_remove_private_flag_quark(self, flag_quark);
			return;
		}
		return;
	}

	/* is this a known device flag */
	flag = fwupd_device_flag_from_string(hint);
	if (flag != FWUPD_DEVICE_FLAG_UNKNOWN) {
		fu_device_add_flag(self, flag);
		return;
	}
	flag_quark = fu_device_find_private_flag_quark(self, hint);
	if (flag_quark != 0) {
		fu_device_add_private_flag_quark(self, flag_quark);
		return;
	}
}

/**
 * fu_device_set_custom_flags:
 * @self: a #FuDevice
 * @custom_flags: a string
 *
 * Sets the custom flags from the quirk system that can be used to
 * affect device matching. The actual string format is defined by the plugin.
 *
 * Since: 1.1.0
 **/
void
fu_device_set_custom_flags(FuDevice *self, const gchar *custom_flags)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(custom_flags != NULL);

	/* save what was set so we can use it for incorporating a superclass */
	g_free(priv->custom_flags);
	priv->custom_flags = g_strdup(custom_flags);

	/* look for any standard FwupdDeviceFlags */
	if (custom_flags != NULL) {
		g_auto(GStrv) hints = g_strsplit(custom_flags, ",", -1);
		for (guint i = 0; hints[i] != NULL; i++)
			fu_device_set_custom_flag(self, hints[i]);
	}
}

/**
 * fu_device_get_custom_flags:
 * @self: a #FuDevice
 *
 * Gets the custom flags for the device from the quirk system.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.0
 **/
const gchar *
fu_device_get_custom_flags(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->custom_flags;
}

/**
 * fu_device_get_remove_delay:
 * @self: a #FuDevice
 *
 * Returns the maximum delay expected when replugging the device going into
 * bootloader mode.
 *
 * Returns: time in milliseconds
 *
 * Since: 1.0.2
 **/
guint
fu_device_get_remove_delay(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->remove_delay;
}

/**
 * fu_device_set_remove_delay:
 * @self: a #FuDevice
 * @remove_delay: the value in milliseconds
 *
 * Sets the amount of time a device is allowed to return in bootloader mode.
 *
 * NOTE: this should be less than 3000ms for devices that just have to reset
 * and automatically re-enumerate, but significantly longer if it involves a
 * user removing a cable, pressing several buttons and removing a cable.
 * A suggested value for this would be 10,000ms.
 *
 * Since: 1.0.2
 **/
void
fu_device_set_remove_delay(FuDevice *self, guint remove_delay)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->remove_delay = remove_delay;
}

/**
 * fu_device_get_acquiesce_delay:
 * @self: a #FuDevice
 *
 * Returns the time the daemon should wait for devices to finish hotplugging
 * after the update has completed.
 *
 * Returns: time in milliseconds
 *
 * Since: 1.8.3
 **/
guint
fu_device_get_acquiesce_delay(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0);
	return priv->acquiesce_delay;
}

/**
 * fu_device_set_acquiesce_delay:
 * @self: a #FuDevice
 * @acquiesce_delay: the value in milliseconds
 *
 * Sets the time the daemon should wait for devices to finish hotplugging
 * after the update has completed.
 *
 * Devices subclassing from [class@FuUsbDevice] and [class@FuUdevDevice] use
 * a value of 2,500ms, and other devices use 50ms by default. This can be also
 * be set using `AcquiesceDelay=` in a quirk file.
 *
 * Since: 1.8.3
 **/
void
fu_device_set_acquiesce_delay(FuDevice *self, guint acquiesce_delay)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->acquiesce_delay = acquiesce_delay;
}

/**
 * fu_device_set_update_state:
 * @self: a #FuDevice
 * @update_state: the state, e.g. %FWUPD_UPDATE_STATE_PENDING
 *
 * Sets the update state, clearing the update error as required.
 *
 * Since: 1.6.2
 **/
void
fu_device_set_update_state(FuDevice *self, FwupdUpdateState update_state)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	if (update_state == FWUPD_UPDATE_STATE_SUCCESS ||
	    update_state == FWUPD_UPDATE_STATE_PENDING ||
	    update_state == FWUPD_UPDATE_STATE_NEEDS_REBOOT)
		fu_device_set_update_error(self, NULL);
	if (update_state == FWUPD_UPDATE_STATE_NEEDS_REBOOT) {
		fu_device_add_problem(self, FWUPD_DEVICE_PROBLEM_UPDATE_IN_PROGRESS);
	} else {
		fu_device_remove_problem(self, FWUPD_DEVICE_PROBLEM_UPDATE_IN_PROGRESS);
	}
	fwupd_device_set_update_state(FWUPD_DEVICE(self), update_state);
}

static void
fu_device_ensure_battery_inhibit(FuDevice *self)
{
	if (fu_device_get_battery_level(self) == FWUPD_BATTERY_LEVEL_INVALID ||
	    fu_device_get_battery_level(self) >= fu_device_get_battery_threshold(self)) {
		fu_device_remove_problem(self, FWUPD_DEVICE_PROBLEM_POWER_TOO_LOW);
		return;
	}
	fu_device_add_problem(self, FWUPD_DEVICE_PROBLEM_POWER_TOO_LOW);
}

/**
 * fu_device_get_battery_level:
 * @self: a #FuDevice
 *
 * Returns the battery level.
 *
 * Returns: value in percent
 *
 * Since: 1.5.8
 **/
guint
fu_device_get_battery_level(FuDevice *self)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), G_MAXUINT);

	/* use the parent if the child is unset */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PARENT_FOR_BATTERY) &&
	    fwupd_device_get_battery_level(FWUPD_DEVICE(self)) == FWUPD_BATTERY_LEVEL_INVALID) {
		FuDevice *parent = fu_device_get_parent(self);
		if (parent != NULL)
			return fu_device_get_battery_level(parent);
	}
	return fwupd_device_get_battery_level(FWUPD_DEVICE(self));
}

/**
 * fu_device_set_battery_level:
 * @self: a #FuDevice
 * @battery_level: the percentage value
 *
 * Sets the battery level, or %FWUPD_BATTERY_LEVEL_INVALID.
 *
 * Setting this allows fwupd to show a warning if the device change is too low
 * to perform the update.
 *
 * Since: 1.5.8
 **/
void
fu_device_set_battery_level(FuDevice *self, guint battery_level)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(battery_level <= FWUPD_BATTERY_LEVEL_INVALID);
	fwupd_device_set_battery_level(FWUPD_DEVICE(self), battery_level);
	fu_device_ensure_battery_inhibit(self);
}

/**
 * fu_device_get_battery_threshold:
 * @self: a #FuDevice
 *
 * Returns the battery threshold under which a firmware update cannot be
 * performed.
 *
 * If fu_device_set_battery_threshold() has not been used, a default value is
 * used instead.
 *
 * Returns: value in percent
 *
 * Since: 1.6.0
 **/
guint
fu_device_get_battery_threshold(FuDevice *self)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), FWUPD_BATTERY_LEVEL_INVALID);

	/* use the parent if the child is unset */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PARENT_FOR_BATTERY) &&
	    fwupd_device_get_battery_threshold(FWUPD_DEVICE(self)) == FWUPD_BATTERY_LEVEL_INVALID) {
		FuDevice *parent = fu_device_get_parent(self);
		if (parent != NULL)
			return fu_device_get_battery_threshold(parent);
	}
	return fwupd_device_get_battery_threshold(FWUPD_DEVICE(self));
}

/**
 * fu_device_set_battery_threshold:
 * @self: a #FuDevice
 * @battery_threshold: the percentage value
 *
 * Sets the battery level, or %FWUPD_BATTERY_LEVEL_INVALID for the default.
 *
 * Setting this allows fwupd to show a warning if the device change is too low
 * to perform the update.
 *
 * Since: 1.6.0
 **/
void
fu_device_set_battery_threshold(FuDevice *self, guint battery_threshold)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(battery_threshold <= FWUPD_BATTERY_LEVEL_INVALID);
	fwupd_device_set_battery_threshold(FWUPD_DEVICE(self), battery_threshold);
	fu_device_ensure_battery_inhibit(self);
}

/**
 * fu_device_get_created_usec:
 * @self: a #FuDevice
 *
 * Gets when the device was created.
 *
 * If the usec-precision value has not been set with fu_device_set_created_usec(),
 * the exported seconds-precision fallback value is returned instead.
 *
 * Returns: value in microseconds, or -1 for invalid
 *
 * Since: 2.0.0
 **/
gint64
fu_device_get_created_usec(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), -1);

	if (priv->created_usec > 0)
		return priv->created_usec;
	return fwupd_device_get_created(FWUPD_DEVICE(self)) * G_USEC_PER_SEC;
}

/**
 * fu_device_set_created_usec:
 * @self: a #FuDevice
 * @created_usec: value in microseconds
 *
 * Sets when the device was created.
 *
 * NOTE: This also sets the seconds-precision fallback value.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_created_usec(FuDevice *self, gint64 created_usec)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(created_usec == 0 || created_usec > 10000000000);

	priv->created_usec = created_usec;
	fwupd_device_set_created(FWUPD_DEVICE(self), created_usec / G_USEC_PER_SEC);
}

/**
 * fu_device_get_modified_usec:
 * @self: a #FuDevice
 *
 * Gets when the device was modified.
 *
 * If the usec-precision value has not been set with fu_device_set_modified_usec(),
 * the exported seconds-precision fallback value is returned instead.
 *
 * Returns: value in microseconds, or -1 for invalid
 *
 * Since: 2.0.0
 **/
gint64
fu_device_get_modified_usec(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), -1);

	if (priv->modified_usec > 0)
		return priv->modified_usec;
	return fwupd_device_get_modified(FWUPD_DEVICE(self)) * G_USEC_PER_SEC;
}

/**
 * fu_device_get_vid:
 * @self: a #FuUdevDevice
 *
 * Gets the device vendor code.
 *
 * Returns: a vendor code, or 0 if unset or invalid
 *
 * Since: 2.0.0
 **/
guint16
fu_device_get_vid(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0x0000);
	return priv->vid;
}

/**
 * fu_device_set_vid:
 * @self: a #FuUdevDevice
 * @vid: an integer ID
 *
 * Sets the vendor ID.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_vid(FuDevice *self, guint16 vid)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	if (priv->vid == vid)
		return;
	priv->vid = vid;
	g_object_notify(G_OBJECT(self), "vid");
}

/**
 * fu_device_get_pid:
 * @self: a #FuUdevDevice
 *
 * Gets the device product code.
 *
 * Returns: a product code, or 0 if unset or invalid
 *
 * Since: 2.0.0
 **/
guint16
fu_device_get_pid(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), 0x0000);
	return priv->pid;
}

/**
 * fu_device_set_pid:
 * @self: a #FuUdevDevice
 * @pid: an integer ID
 *
 * Sets the product ID.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_pid(FuDevice *self, guint16 pid)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	if (priv->pid == pid)
		return;
	priv->pid = pid;
	g_object_notify(G_OBJECT(self), "pid");
}

/**
 * fu_device_set_modified_usec:
 * @self: a #FuDevice
 * @modified_usec: value in microseconds
 *
 * Sets when the device was modified.
 *
 * NOTE: This also sets the seconds-precision fallback value.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_modified_usec(FuDevice *self, gint64 modified_usec)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(modified_usec == 0 || modified_usec > 10000000000);

	priv->modified_usec = modified_usec;
	fwupd_device_set_modified(FWUPD_DEVICE(self), modified_usec / G_USEC_PER_SEC);
}

static gchar *
fu_device_instance_flag_to_string_trunc(FuDeviceInstanceFlags flags)
{
	g_autofree gchar *tmp = fu_device_instance_flags_to_string(flags);
	g_auto(GStrv) split = g_strsplit(tmp, ",", -1);
	for (guint i = 0; split[i] != NULL; i++) {
		if (strlen(split[i]) > 2)
			split[i][2] = '\0';
	}
	return g_strjoinv(",", split);
}

static void
fu_device_to_string_impl(FuDevice *self, guint idt, GString *str)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	for (guint i = 0; priv->instance_ids != NULL && i < priv->instance_ids->len; i++) {
		FuDeviceInstanceIdItem *item = g_ptr_array_index(priv->instance_ids, i);
		g_autofree gchar *flags_str = fu_device_instance_flag_to_string_trunc(item->flags);
		g_autofree gchar *title = g_strdup_printf("InstanceId[%s]", flags_str);
		if (item->instance_id != NULL) {
			g_autofree gchar *tmp2 =
			    g_strdup_printf("%s ← %s", item->guid, item->instance_id);
			fwupd_codec_string_append(str, idt, title, tmp2);
		} else {
			fwupd_codec_string_append(str, idt, title, item->guid);
		}
	}
	fwupd_codec_string_append(str, idt, "EquivalentId", priv->equivalent_id);
	fwupd_codec_string_append(str, idt, "PhysicalId", priv->physical_id);
	fwupd_codec_string_append(str, idt, "LogicalId", priv->logical_id);
	fwupd_codec_string_append(str, idt, "BackendId", priv->backend_id);
	fwupd_codec_string_append_hex(str, idt, "Vid", priv->vid);
	fwupd_codec_string_append_hex(str, idt, "Pid", priv->pid);
	fwupd_codec_string_append(str, idt, "UpdateRequestId", priv->update_request_id);
	fwupd_codec_string_append(str, idt, FWUPD_RESULT_KEY_UPDATE_MESSAGE, priv->update_message);
	fwupd_codec_string_append(str, idt, FWUPD_RESULT_KEY_UPDATE_IMAGE, priv->update_image);
	fwupd_codec_string_append(str, idt, "FwupdVersion", priv->fwupd_version);
	fwupd_codec_string_append(str, idt, "ProxyGuid", priv->proxy_guid);
	fwupd_codec_string_append_int(str, idt, "RemoveDelay", priv->remove_delay);
	fwupd_codec_string_append_int(str, idt, "AcquiesceDelay", priv->acquiesce_delay);
	fwupd_codec_string_append(str, idt, "CustomFlags", priv->custom_flags);
	if (priv->specialized_gtype != G_TYPE_INVALID)
		fwupd_codec_string_append(str, idt, "GType", g_type_name(priv->specialized_gtype));
	if (priv->proxy_gtype != G_TYPE_INVALID)
		fwupd_codec_string_append(str, idt, "ProxyGType", g_type_name(priv->proxy_gtype));
	if (priv->firmware_gtype != G_TYPE_INVALID) {
		fwupd_codec_string_append(str,
					  idt,
					  "FirmwareGType",
					  g_type_name(priv->firmware_gtype));
	}
	fwupd_codec_string_append_size(str, idt, "FirmwareSizeMin", priv->size_min);
	fwupd_codec_string_append_size(str, idt, "FirmwareSizeMax", priv->size_max);
	fwupd_codec_string_append_int(str, idt, "RequiredFree", priv->required_free);
	if (priv->order != G_MAXINT) {
		g_autofree gchar *order = g_strdup_printf("%i", priv->order);
		fwupd_codec_string_append(str, idt, "Order", order);
	}
	fwupd_codec_string_append_int(str, idt, "Priority", priv->priority);
	if (priv->metadata != NULL) {
		g_autoptr(GList) keys = g_hash_table_get_keys(priv->metadata);
		for (GList *l = keys; l != NULL; l = l->next) {
			const gchar *key = l->data;
			const gchar *value = g_hash_table_lookup(priv->metadata, key);
			fwupd_codec_string_append(str, idt, key, value);
		}
	}
	for (guint i = 0; i < priv->possible_plugins->len; i++) {
		const gchar *name = g_ptr_array_index(priv->possible_plugins, i);
		fwupd_codec_string_append(str, idt, "PossiblePlugin", name);
	}
	if (priv->parent_physical_ids != NULL && priv->parent_physical_ids->len > 0) {
		g_autofree gchar *flags = fu_strjoin(",", priv->parent_physical_ids);
		fwupd_codec_string_append(str, idt, "ParentPhysicalIds", flags);
	}
	if (priv->parent_backend_ids != NULL && priv->parent_backend_ids->len > 0) {
		g_autofree gchar *flags = fu_strjoin(",", priv->parent_backend_ids);
		fwupd_codec_string_append(str, idt, "ParentBackendIds", flags);
	}
	if (priv->private_flags != NULL && priv->private_flags->len != 0) {
		g_autoptr(GPtrArray) tmpv = g_ptr_array_new();
		for (guint64 i = 0; i < priv->private_flags->len; i++) {
			GQuark flag_quark = g_array_index(priv->private_flags, GQuark, i);
			g_ptr_array_add(tmpv, (gpointer)g_quark_to_string(flag_quark));
		}
		if (tmpv->len > 0) {
			g_autofree gchar *tmps = fu_strjoin(",", tmpv);
			fwupd_codec_string_append(str, idt, "PrivateFlags", tmps);
		}
	}
	if (priv->instance_hash != NULL) {
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, priv->instance_hash);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			g_autofree gchar *title =
			    g_strdup_printf("InstanceKey[%s]", (const gchar *)key);
			fwupd_codec_string_append(str, idt, title, value);
		}
	}
	if (priv->inhibits != NULL) {
		g_autoptr(GList) values = g_hash_table_get_values(priv->inhibits);
		for (GList *l = values; l != NULL; l = l->next) {
			FuDeviceInhibit *inhibit = (FuDeviceInhibit *)l->data;
			g_autofree gchar *val =
			    g_strdup_printf("[%s] %s", inhibit->inhibit_id, inhibit->reason);
			fwupd_codec_string_append(str, idt, "Inhibit", val);
		}
	}
	if (priv->events != NULL) {
		fwupd_codec_string_append(str, idt, "Events", "");
		for (guint i = 0; i < priv->events->len; i++) {
			FuDeviceEvent *event = g_ptr_array_index(priv->events, i);
			if (i > 10) {
				g_autofree gchar *msg =
				    g_strdup_printf("…and %u more events", priv->events->len - 10);
				fwupd_codec_string_append(str, idt + 1, "", msg);
				break;
			}
			fwupd_codec_add_string(FWUPD_CODEC(event), idt + 1, str);
		}
	}
	if (priv->proxy != NULL) {
		fwupd_codec_string_append(str, idt, "Proxy", "");
		fu_device_to_string_impl(priv->proxy, idt + 1, str);
	}
}

static GPtrArray *
fu_device_get_common_class_parents(FuDevice *self, FuDevice *donor)
{
	g_autoptr(GPtrArray) array = g_ptr_array_new();
	for (GType gtype = G_OBJECT_TYPE(self); gtype != FWUPD_TYPE_DEVICE;
	     gtype = g_type_parent(gtype)) {
		FuDeviceClass *device_class = g_type_class_peek(gtype);
		for (GType gtype_donor = G_OBJECT_TYPE(donor); gtype_donor != FWUPD_TYPE_DEVICE;
		     gtype_donor = g_type_parent(gtype_donor)) {
			if (gtype == gtype_donor)
				g_ptr_array_add(array, device_class);
		}
	}
	return g_steal_pointer(&array);
}

/**
 * fu_device_add_string:
 * @self: a #FuDevice
 * @idt: indent level
 * @str: a string to append to
 *
 * Add daemon-specific device metadata to an existing string.
 *
 * Since: 1.7.1
 **/
void
fu_device_add_string(FuDevice *self, guint idt, GString *str)
{
	GPtrArray *children;
	gpointer device_class_to_string_last = NULL;
	g_autoptr(GList) device_class_list = NULL;

	/* add baseclass */
	fwupd_codec_add_string(FWUPD_CODEC(self), idt, str);

	/* run every unique ->to_string() in each subclass */
	for (GType gtype = G_OBJECT_TYPE(self); gtype != G_TYPE_INVALID;
	     gtype = g_type_parent(gtype)) {
		FuDeviceClass *device_class = g_type_class_peek(gtype);
		if (!FU_IS_DEVICE_CLASS(device_class))
			break;
		device_class_list = g_list_prepend(device_class_list, device_class);
	}
	for (GList *l = device_class_list; l != NULL; l = l->next) {
		FuDeviceClass *device_class = FU_DEVICE_CLASS(l->data);
		if (device_class->to_string != NULL &&
		    device_class->to_string != device_class_to_string_last) {
			device_class->to_string(self, idt + 1, str);
			device_class_to_string_last = device_class->to_string;
		}
	}

	/* print children also */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child = g_ptr_array_index(children, i);
		fu_device_add_string(child, idt + 1, str);
	}
}

/**
 * fu_device_to_string:
 * @self: a #FuDevice
 *
 * This allows us to easily print the device, the release and the
 * daemon-specific metadata.
 *
 * Returns: a string value, or %NULL for invalid.
 *
 * Since: 0.9.8
 **/
gchar *
fu_device_to_string(FuDevice *self)
{
	GString *str = g_string_new(NULL);
	fu_device_add_string(self, 0, str);
	return g_string_free(str, FALSE);
}

/**
 * fu_device_set_context:
 * @self: a #FuDevice
 * @ctx: (nullable): optional #FuContext
 *
 * Sets the optional context which may be useful to this device.
 * This is typically set after the device has been created, but before
 * the device has been opened or probed.
 *
 * Since: 1.6.0
 **/
void
fu_device_set_context(FuDevice *self, FuContext *ctx)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_CONTEXT(ctx) || ctx == NULL);

#ifndef SUPPORTED_BUILD
	if (priv->ctx != NULL && ctx == NULL) {
		g_critical("clearing device context for %s [%s]",
			   fu_device_get_name(self),
			   fu_device_get_id(self));
		return;
	}
#endif

	if (g_set_object(&priv->ctx, ctx))
		g_object_notify(G_OBJECT(self), "context");
}

/**
 * fu_device_get_context:
 * @self: a #FuDevice
 *
 * Gets the context assigned for this device.
 *
 * Returns: (transfer none): the #FuContext object, or %NULL
 *
 * Since: 1.6.0
 **/
FuContext *
fu_device_get_context(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	return priv->ctx;
}

/**
 * fu_device_get_results:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Gets the results of the last update operation on the device by calling a vfunc.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.6.2
 **/
gboolean
fu_device_get_results(FuDevice *self, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->get_results == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "getting results not supported by device");
		return FALSE;
	}

	/* call vfunc */
	return device_class->get_results(self, error);
}

/**
 * fu_device_write_firmware:
 * @self: a #FuDevice
 * @firmware: a #FuFirmware
 * @progress: a #FuProgress
 * @flags: install flags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Writes firmware to the device by calling a plugin-specific vfunc.
 *
 * Returns: %TRUE on success
 *
 * Since: 2.0.7
 **/
gboolean
fu_device_write_firmware(FuDevice *self,
			 FuFirmware *firmware,
			 FuProgress *progress,
			 FwupdInstallFlags flags,
			 GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autofree gchar *str = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FU_IS_FIRMWARE(firmware), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->write_firmware == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "writing firmware not supported by device");
		return FALSE;
	}

	/* call vfunc */
	str = fu_firmware_to_string(firmware);
	g_info("installing onto %s:\n%s", fu_device_get_id(self), str);
	g_set_object(&priv->progress, progress);
	if (!device_class->write_firmware(self, firmware, priv->progress, flags, error))
		return FALSE;

	/* the device set an UpdateMessage (possibly from a quirk, or XML file)
	 * but did not do an event; guess something */
	if (priv->request_cnts[FWUPD_REQUEST_KIND_POST] == 0 &&
	    fu_device_get_update_message(self) != NULL) {
		const gchar *update_request_id = fu_device_get_update_request_id(self);
		g_autoptr(FwupdRequest) request = fwupd_request_new();
		fwupd_request_set_kind(request, FWUPD_REQUEST_KIND_POST);
		if (update_request_id != NULL) {
			fwupd_request_set_id(request, update_request_id);
			fwupd_request_add_flag(request, FWUPD_REQUEST_FLAG_ALLOW_GENERIC_MESSAGE);
		} else {
			fu_device_add_request_flag(self, FWUPD_REQUEST_FLAG_NON_GENERIC_MESSAGE);
			fwupd_request_set_id(request, FWUPD_REQUEST_ID_REMOVE_REPLUG);
		}
		fwupd_request_set_message(request, fu_device_get_update_message(self));
		fwupd_request_set_image(request, fu_device_get_update_image(self));
		if (!fu_device_emit_request(self, request, progress, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

/**
 * fu_device_prepare_firmware:
 * @self: a #FuDevice
 * @stream: a #GInputStream
 * @flags: #FuFirmwareParseFlags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Prepares the firmware by calling an optional device-specific vfunc for the
 * device, which can do things like decompressing or parsing of the firmware
 * data.
 *
 * For all firmware, this checks the size of the firmware if limits have been
 * set using fu_device_set_firmware_size_min(), fu_device_set_firmware_size_max()
 * or using a quirk entry.
 *
 * Returns: (transfer full): a new #GBytes, or %NULL for error
 *
 * Since: 1.1.2
 **/
FuFirmware *
fu_device_prepare_firmware(FuDevice *self,
			   GInputStream *stream,
			   FuProgress *progress,
			   FuFirmwareParseFlags flags,
			   GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(FuFirmware) firmware = NULL;
	gsize fw_size;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(G_IS_INPUT_STREAM(stream), NULL);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* optionally subclassed */
	if (device_class->prepare_firmware != NULL) {
		firmware = device_class->prepare_firmware(self, stream, progress, flags, error);
		if (firmware == NULL)
			return NULL;
	} else if (priv->firmware_gtype != G_TYPE_INVALID) {
		firmware = g_object_new(priv->firmware_gtype, NULL);
		if (!fu_firmware_parse_stream(firmware, stream, 0x0, flags, error))
			return NULL;
	} else {
		firmware = fu_firmware_new();
		if (!fu_firmware_parse_stream(firmware, stream, 0x0, flags, error))
			return NULL;
	}

	/* check size */
	fw_size = fu_firmware_get_size(firmware);
	if (fw_size != 0) {
		if (priv->size_max > 0 && fw_size > priv->size_max) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "firmware is 0x%04x bytes larger than the allowed "
				    "maximum size of 0x%04x bytes",
				    (guint)(fw_size - priv->size_max),
				    (guint)priv->size_max);
			return NULL;
		}
		if (priv->size_min > 0 && fw_size < priv->size_min) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "firmware is %04x bytes smaller than the allowed "
				    "minimum size of %04x bytes",
				    (guint)(priv->size_min - fw_size),
				    (guint)priv->size_max);
			return NULL;
		}
	}

	/* success */
	return g_steal_pointer(&firmware);
}

/**
 * fu_device_read_firmware:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @flags: #FuFirmwareParseFlags, e.g. %FU_FIRMWARE_PARSE_FLAG_NONE
 * @error: (nullable): optional return location for an error
 *
 * Reads firmware from the device by calling a plugin-specific vfunc.
 * The device subclass should try to ensure the firmware does not contain any
 * serial numbers or user-configuration values and can be used to calculate the
 * device checksum.
 *
 * The return value can be converted to a blob of memory using fu_firmware_write().
 *
 * Returns: (transfer full): a #FuFirmware, or %NULL for error
 *
 * Since: 2.0.11, although a simpler version was added in 1.0.8
 **/
FuFirmware *
fu_device_read_firmware(FuDevice *self,
			FuProgress *progress,
			FuFirmwareParseFlags flags,
			GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(GBytes) fw = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* call vfunc */
	g_set_object(&priv->progress, progress);
	if (device_class->read_firmware != NULL)
		return device_class->read_firmware(self, progress, error);

	/* use the default FuFirmware when only ->dump_firmware is provided */
	fw = fu_device_dump_firmware(self, progress, error);
	if (fw == NULL)
		return NULL;
	if (priv->firmware_gtype != G_TYPE_INVALID) {
		g_autoptr(FuFirmware) firmware = g_object_new(priv->firmware_gtype, NULL);
		if (!fu_firmware_parse_bytes(firmware, fw, 0x0, flags, error))
			return NULL;
		return g_steal_pointer(&firmware);
	}
	return fu_firmware_new_from_bytes(fw);
}

/**
 * fu_device_dump_firmware:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Reads the raw firmware image from the device by calling a plugin-specific
 * vfunc. This raw firmware image may contain serial numbers or device-specific
 * configuration but should be a byte-for-byte match compared to using an
 * external SPI programmer.
 *
 * Returns: (transfer full): a #GBytes, or %NULL for error
 *
 * Since: 1.5.0
 **/
GBytes *
fu_device_dump_firmware(FuDevice *self, FuProgress *progress, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* use the default FuFirmware when only ->dump_firmware is provided */
	if (device_class->dump_firmware == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "dumping firmware is not supported by device");
		return NULL;
	}

	/* proxy */
	g_set_object(&priv->progress, progress);
	return device_class->dump_firmware(self, progress, error);
}

/**
 * fu_device_detach:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Detaches a device from the application into bootloader mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_detach(FuDevice *self, GError **error)
{
	g_autoptr(FuProgress) progress = fu_progress_new(G_STRLOC);
	return fu_device_detach_full(self, progress, error);
}

/**
 * fu_device_detach_full:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Detaches a device from the application into bootloader mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.7.0
 **/
gboolean
fu_device_detach_full(FuDevice *self, FuProgress *progress, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->detach == NULL)
		return TRUE;

	/* call vfunc */
	g_set_object(&priv->progress, progress);
	return device_class->detach(self, progress, error);
}

/**
 * fu_device_attach:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Attaches a device from the bootloader into application mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_attach(FuDevice *self, GError **error)
{
	g_autoptr(FuProgress) progress = fu_progress_new(G_STRLOC);
	return fu_device_attach_full(self, progress, error);
}

/**
 * fu_device_attach_full:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Attaches a device from the bootloader into application mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.7.0
 **/
gboolean
fu_device_attach_full(FuDevice *self, FuProgress *progress, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->attach == NULL)
		return TRUE;

	/* call vfunc */
	g_set_object(&priv->progress, progress);
	return device_class->attach(self, progress, error);
}

/**
 * fu_device_reload:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Reloads a device that has just gone from bootloader into application mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.3.3
 **/
gboolean
fu_device_reload(FuDevice *self, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->reload == NULL)
		return TRUE;

	/* call vfunc */
	return device_class->reload(self, error);
}

/**
 * fu_device_prepare:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @flags: install flags
 * @error: (nullable): optional return location for an error
 *
 * Prepares a device for update. A different plugin can handle each of
 * FuDevice->prepare(), FuDevice->detach() and FuDevice->write_firmware().
 *
 * Returns: %TRUE on success
 *
 * Since: 1.3.3
 **/
gboolean
fu_device_prepare(FuDevice *self, FuProgress *progress, FwupdInstallFlags flags, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->prepare == NULL)
		return TRUE;

	/* call vfunc */
	g_set_object(&priv->progress, progress);
	return device_class->prepare(self, progress, flags, error);
}

/**
 * fu_device_cleanup:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @flags: install flags
 * @error: (nullable): optional return location for an error
 *
 * Cleans up a device after an update. A different plugin can handle each of
 * FuDevice->write_firmware(), FuDevice->attach() and FuDevice->cleanup().
 *
 * Returns: %TRUE on success
 *
 * Since: 1.3.3
 **/
gboolean
fu_device_cleanup(FuDevice *self, FuProgress *progress, FwupdInstallFlags flags, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (device_class->cleanup == NULL)
		return TRUE;

	/* call vfunc */
	g_set_object(&priv->progress, progress);
	return device_class->cleanup(self, progress, flags, error);
}

static gboolean
fu_device_open_cb(FuDevice *self, gpointer user_data, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	return device_class->open(self, error);
}

static gboolean
fu_device_open_internal(FuDevice *self, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	/* already open */
	g_atomic_int_inc(&priv->open_refcount);
	if (priv->open_refcount > 1)
		return TRUE;

	/* probe */
	if (!fu_device_probe(self, error)) {
		g_prefix_error(error, "failed to probe: ");
		return FALSE;
	}

	/* ensure the device ID is already setup */
	if (!fu_device_ensure_id(self, error)) {
		g_prefix_error(error, "failed to ensure ID: ");
		return FALSE;
	}

	/* subclassed */
	if (device_class->open != NULL) {
		if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_RETRY_OPEN)) {
			if (!fu_device_retry_full(self,
						  fu_device_open_cb,
						  FU_DEVICE_RETRY_OPEN_COUNT,
						  FU_DEVICE_RETRY_OPEN_DELAY,
						  NULL,
						  error)) {
				g_prefix_error(error, "failed to retry subclass open: ");
				return FALSE;
			}
		} else {
			if (!device_class->open(self, error)) {
				g_prefix_error(error, "failed to subclass open: ");
				return FALSE;
			}
		}
	}

	/* setup */
	if (!fu_device_setup(self, error)) {
		g_prefix_error(error, "failed to setup: ");
		return FALSE;
	}

	/* ensure the device ID is still valid */
	if (!fu_device_ensure_id(self, error)) {
		g_prefix_error(error, "failed to ensure ID: ");
		return FALSE;
	}

	/* success */
	fu_device_add_private_flag(self, FU_DEVICE_PRIVATE_FLAG_IS_OPEN);
	return TRUE;
}

/**
 * fu_device_open:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Opens a device, optionally running a object-specific vfunc.
 *
 * Plugins can call fu_device_open() multiple times without calling
 * fu_device_close(), but only the first call will actually invoke the vfunc.
 *
 * It is expected that plugins issue the same number of fu_device_open() and
 * fu_device_close() methods when using a specific @self.
 *
 * If the `->probe()`, `->open()` and `->setup()` actions all complete
 * successfully the internal device flag %FU_DEVICE_PRIVATE_FLAG_IS_OPEN will
 * be set.
 *
 * NOTE: It is important to still call fu_device_close() even if this function
 * fails as the device may still be partially initialized.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_open(FuDevice *self, GError **error)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* skip */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_IS_FAKE])) {
		fu_device_add_private_flag(self, FU_DEVICE_PRIVATE_FLAG_IS_OPEN);
		if (!fu_device_probe(self, error))
			return FALSE;
		if (!fu_device_setup(self, error))
			return FALSE;
		return fu_device_ensure_id(self, error);
	}

	/* use parent */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PARENT_FOR_OPEN)) {
		FuDevice *parent = fu_device_get_parent(self);
		if (parent == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "no parent device");
			return FALSE;
		}
		return fu_device_open_internal(parent, error);
	}
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PROXY_FOR_OPEN)) {
		FuDevice *proxy = fu_device_get_proxy(self);
		if (proxy == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "no proxy device");
			return FALSE;
		}
		if (!fu_device_open_internal(proxy, error))
			return FALSE;
	}
	return fu_device_open_internal(self, error);
}

static gboolean
fu_device_close_internal(FuDevice *self, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	/* not yet open */
	if (priv->open_refcount == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "cannot close device, refcount already zero");
		return FALSE;
	}
	if (!g_atomic_int_dec_and_test(&priv->open_refcount))
		return TRUE;

	/* subclassed */
	if (device_class->close != NULL) {
		if (!device_class->close(self, error))
			return FALSE;
	}

	/* success */
	fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_IS_OPEN);
	return TRUE;
}

/**
 * fu_device_close:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Closes a device, optionally running a object-specific vfunc.
 *
 * Plugins can call fu_device_close() multiple times without calling
 * fu_device_open(), but only the last call will actually invoke the vfunc.
 *
 * It is expected that plugins issue the same number of fu_device_open() and
 * fu_device_close() methods when using a specific @self.
 *
 * An error is returned if this method is called without having used the
 * fu_device_open() method beforehand.
 *
 * If the close action completed successfully the internal device flag
 * %FU_DEVICE_PRIVATE_FLAG_IS_OPEN will be cleared.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_close(FuDevice *self, GError **error)
{
	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* skip */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_IS_FAKE])) {
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_IS_OPEN);
		return TRUE;
	}

	/* close the device first in case the plugin needs to use the proxy or parent */
	if (!fu_device_close_internal(self, error))
		return FALSE;

	/* use parent */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PARENT_FOR_OPEN)) {
		FuDevice *parent = fu_device_get_parent(self);
		if (parent == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "no parent device");
			return FALSE;
		}
		return fu_device_close_internal(parent, error);
	}
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_USE_PROXY_FOR_OPEN)) {
		FuDevice *proxy = fu_device_get_proxy(self);
		if (proxy == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "no proxy device");
			return FALSE;
		}
		if (!fu_device_close_internal(proxy, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

/**
 * fu_device_probe:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Probes a device, setting parameters on the object that does not need
 * the device open or the interface claimed.
 * If the device is not compatible then an error should be returned.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_probe(FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* already done */
	if (priv->done_probe)
		return TRUE;

	/* device self-assigned */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_NO_PROBE])) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "not probing");
		return FALSE;
	}

	/* subclassed */
	if (device_class->probe != NULL) {
		if (!device_class->probe(self, error))
			return FALSE;
	}

	/* vfunc skipped device */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_NO_PROBE])) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "not probing");
		return FALSE;
	}

	/* success */
	priv->done_probe = TRUE;
	return TRUE;
}

/**
 * fu_device_probe_complete:
 * @self: a #FuDevice
 *
 * Tell the device that all probing has finished. This allows it to release any resources that are
 * only valid during coldplug or hotplug.
 *
 * Since: 1.8.12
 **/
void
fu_device_probe_complete(FuDevice *self)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_if_fail(FU_IS_DEVICE(self));

	if (device_class->probe_complete != NULL)
		device_class->probe_complete(self);
}

/**
 * fu_device_rescan:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Rescans a device, re-adding GUIDs or flags based on some hardware change.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.3.1
 **/
gboolean
fu_device_rescan(FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* remove all GUIDs */
	if (priv->instance_ids != NULL)
		g_ptr_array_set_size(priv->instance_ids, 0);
	g_ptr_array_set_size(fu_device_get_instance_ids(self), 0);
	g_ptr_array_set_size(fu_device_get_guids(self), 0);

	/* subclassed */
	if (device_class->rescan != NULL) {
		if (!device_class->rescan(self, error)) {
			fu_device_convert_instance_ids(self);
			return FALSE;
		}
	}

	fu_device_convert_instance_ids(self);
	return TRUE;
}

/**
 * fu_device_set_progress:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 *
 * Sets steps on the progress object used to write firmware.
 *
 * Since: 1.7.0
 **/
void
fu_device_set_progress(FuDevice *self, FuProgress *progress)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_PROGRESS(progress));

	/* subclassed */
	if (device_class->set_progress == NULL)
		return;
	device_class->set_progress(self, progress);
}

/**
 * fu_device_convert_instance_ids:
 * @self: a #FuDevice
 *
 * Converts all the Device instance IDs added using fu_device_add_instance_id()
 * into actual GUIDs, **unless** %FU_DEVICE_PRIVATE_FLAG_NO_AUTO_INSTANCE_IDS has
 * been set.
 *
 * Plugins will only need to need to call this manually when adding child
 * devices, as fu_device_setup() automatically calls this after the
 * fu_device_probe() and fu_device_setup() virtual functions have been run.
 *
 * Since: 1.2.5
 **/
void
fu_device_convert_instance_ids(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	gboolean no_generic_guids;

	g_return_if_fail(FU_IS_DEVICE(self));

	/* already set */
	if (fu_device_get_guids(self)->len > 0)
		return;

	/* convert the FuDevice IDs to FwupdDevice IDs */
	no_generic_guids = fu_device_has_private_flag_quark(self, quarks[QUARK_NO_GENERIC_GUIDS]);
	if (priv->instance_ids != NULL) {
		for (guint i = 0; i < priv->instance_ids->len; i++) {
			FuDeviceInstanceIdItem *item = g_ptr_array_index(priv->instance_ids, i);
			if ((item->flags & FU_DEVICE_INSTANCE_FLAG_VISIBLE) == 0)
				continue;
			if ((item->flags & FU_DEVICE_INSTANCE_FLAG_GENERIC) > 0 && no_generic_guids)
				continue;
			if (item->instance_id != NULL)
				fwupd_device_add_instance_id(FWUPD_DEVICE(self), item->instance_id);
			fwupd_device_add_guid(FWUPD_DEVICE(self), item->guid);
		}
	}

	/* OEM specific hardware */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_NO_AUTO_INSTANCE_IDS))
		return;
}

/**
 * fu_device_setup:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Sets up a device, setting parameters on the object that requires
 * the device to be open and have the interface claimed.
 * If the device is not compatible then an error should be returned.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_setup(FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	GPtrArray *children;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* skip */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_IS_FAKE])) {
		fu_device_convert_instance_ids(self);
		return TRUE;
	}

	/* should have already been called */
	if (!fu_device_probe(self, error))
		return FALSE;

	/* already done */
	if (priv->done_setup)
		return TRUE;

	/* subclassed */
	if (device_class->setup != NULL) {
		if (!device_class->setup(self, error))
			return FALSE;
	}

	/* vfunc skipped device */
	if (fu_device_has_private_flag_quark(self, quarks[QUARK_NO_PROBE])) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "not probing");
		return FALSE;
	}

	/* run setup on the children too (unless done already) */
	children = fu_device_get_children(self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index(children, i);
		if (!fu_device_setup(child_tmp, error))
			return FALSE;
	}

	/* convert the instance IDs to GUIDs */
	fu_device_convert_instance_ids(self);

	/* subclassed */
	if (device_class->ready != NULL) {
		if (!device_class->ready(self, error))
			return FALSE;
	}

	priv->done_setup = TRUE;
	return TRUE;
}

/**
 * fu_device_activate:
 * @self: a #FuDevice
 * @progress: a #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Activates up a device, which normally means the device switches to a new
 * firmware version. This should only be called when data loss cannot occur.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.2.6
 **/
gboolean
fu_device_activate(FuDevice *self, FuProgress *progress, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* subclassed */
	if (device_class->activate != NULL) {
		g_set_object(&priv->progress, progress);
		if (!device_class->activate(self, progress, error))
			return FALSE;
	}

	return TRUE;
}

/**
 * fu_device_probe_invalidate:
 * @self: a #FuDevice
 *
 * Normally when calling fu_device_probe() multiple times it is only done once.
 * Calling this method causes the next requests to fu_device_probe() and
 * fu_device_setup() actually probe the hardware.
 *
 * This should be done in case the backing device has changed, for instance if
 * a USB device has been replugged.
 *
 * Since: 1.1.2
 **/
void
fu_device_probe_invalidate(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	priv->done_probe = FALSE;
	priv->done_setup = FALSE;
	if (device_class->invalidate != NULL)
		device_class->invalidate(self);
}

/**
 * fu_device_report_metadata_pre:
 * @self: a #FuDevice
 *
 * Collects metadata that would be useful for debugging a failed update report.
 *
 * Returns: (transfer full) (nullable): a #GHashTable, or %NULL if there is no data
 *
 * Since: 1.5.0
 **/
GHashTable *
fu_device_report_metadata_pre(FuDevice *self)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	g_autoptr(GHashTable) metadata = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);

	/* not implemented */
	if (device_class->report_metadata_pre == NULL)
		return NULL;

	/* metadata for all devices */
	metadata = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	device_class->report_metadata_pre(self, metadata);
	return g_steal_pointer(&metadata);
}

/**
 * fu_device_report_metadata_post:
 * @self: a #FuDevice
 *
 * Collects metadata that would be useful for debugging a failed update report.
 *
 * Returns: (transfer full) (nullable): a #GHashTable, or %NULL if there is no data
 *
 * Since: 1.5.0
 **/
GHashTable *
fu_device_report_metadata_post(FuDevice *self)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);
	g_autoptr(GHashTable) metadata = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);

	/* not implemented */
	if (device_class->report_metadata_post == NULL)
		return NULL;

	/* metadata for all devices */
	metadata = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	device_class->report_metadata_post(self, metadata);
	return g_steal_pointer(&metadata);
}

/**
 * fu_device_add_security_attrs:
 * @self: a #FuDevice
 * @attrs: a security attribute
 *
 * Adds HSI security attributes.
 *
 * Since: 1.6.0
 **/
void
fu_device_add_security_attrs(FuDevice *self, FuSecurityAttrs *attrs)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_if_fail(FU_IS_DEVICE(self));

	/* optional */
	if (device_class->add_security_attrs != NULL)
		return device_class->add_security_attrs(self, attrs);
}

/**
 * fu_device_bind_driver:
 * @self: a #FuDevice
 * @subsystem: a subsystem string, e.g. `pci`
 * @driver: a kernel module name, e.g. `tg3`
 * @error: (nullable): optional return location for an error
 *
 * Binds a driver to the device, which normally means the kernel driver takes
 * control of the hardware.
 *
 * Returns: %TRUE if driver was bound.
 *
 * Since: 1.5.0
 **/
gboolean
fu_device_bind_driver(FuDevice *self, const gchar *subsystem, const gchar *driver, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(subsystem != NULL, FALSE);
	g_return_val_if_fail(driver != NULL, FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* not implemented */
	if (device_class->bind_driver == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "binding drivers is not supported by device");
		return FALSE;
	}

	/* subclass */
	return device_class->bind_driver(self, subsystem, driver, error);
}

/**
 * fu_device_unbind_driver:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Unbinds the driver from the device, which normally means the kernel releases
 * the hardware so it can be used from userspace.
 *
 * If there is no driver bound then this function will return with success
 * without actually doing anything.
 *
 * Returns: %TRUE if driver was unbound.
 *
 * Since: 1.5.0
 **/
gboolean
fu_device_unbind_driver(FuDevice *self, GError **error)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* not implemented */
	if (device_class->unbind_driver == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "unbinding drivers is not supported by device");
		return FALSE;
	}

	/* subclass */
	return device_class->unbind_driver(self, error);
}

/**
 * fu_device_get_instance_str:
 * @self: a #FuDevice
 * @key: (not nullable): a key, e.g. `REV`
 *
 * Looks up an instance ID by a key.
 *
 * Returns: (nullable) (transfer none): The instance key, or %NULL.
 *
 * Since: 1.8.15
 **/
const gchar *
fu_device_get_instance_str(FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(key != NULL, NULL);
	if (priv->instance_hash == NULL)
		return NULL;
	return g_hash_table_lookup(priv->instance_hash, key);
}

/**
 * fu_device_build_vendor_id:
 * @self: a #FuDevice
 * @prefix: (not nullable): a prefix string, e.g. `USB`
 * @value: (nullable): a value, e.g. `0x1234`
 *
 * Builds a device vendor ID, if @value is not %NULL.
 *
 * Since: 2.0.0
 **/
void
fu_device_build_vendor_id(FuDevice *self, const gchar *prefix, const gchar *value)
{
	g_autofree gchar *vendor_id = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(prefix != NULL);

	if (value == NULL)
		return;
	vendor_id = g_strdup_printf("%s:%s", prefix, value);
	fwupd_device_add_vendor_id(FWUPD_DEVICE(self), vendor_id);
}

/**
 * fu_device_build_vendor_id_u16:
 * @self: a #FuDevice
 * @prefix: (not nullable): a prefix string, e.g. `USB`
 * @value: a value, e.g. 0x1234
 *
 * Builds a device vendor ID, if @value is not 0.
 *
 * Since: 2.0.0
 **/
void
fu_device_build_vendor_id_u16(FuDevice *self, const gchar *prefix, guint16 value)
{
	g_autofree gchar *vendor_id = NULL;

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(prefix != NULL);

	if (value == 0x0)
		return;
	vendor_id = g_strdup_printf("%s:0x%04X", prefix, value);
	fwupd_device_add_vendor_id(FWUPD_DEVICE(self), vendor_id);
}

static void
fu_device_incorporate_instance_ids(FuDevice *self, FuDevice *donor)
{
	FuDevicePrivate *priv_donor = GET_PRIVATE(donor);
	gboolean no_generic_guids;

	if (priv_donor->instance_ids == NULL)
		return;
	no_generic_guids = fu_device_has_private_flag_quark(self, quarks[QUARK_NO_GENERIC_GUIDS]);
	for (guint i = 0; i < priv_donor->instance_ids->len; i++) {
		FuDeviceInstanceIdItem *item = g_ptr_array_index(priv_donor->instance_ids, i);
		if ((item->flags & FU_DEVICE_INSTANCE_FLAG_GENERIC) > 0 && no_generic_guids) {
			continue;
		}
		if (item->instance_id != NULL)
			fu_device_add_instance_id_full(self, item->instance_id, item->flags);
		else
			fu_device_add_instance_id_full(self, item->guid, item->flags);
	}
}

/**
 * fu_device_incorporate:
 * @self: a #FuDevice
 * @donor: Another #FuDevice
 * @flag: Some #FuDeviceIncorporateFlags, e.g. %FU_DEVICE_INCORPORATE_FLAG_PHYSICAL_ID
 *
 * Copy some properties from the donor object if they have not already been set.
 *
 * Since: 2.0.0
 **/
void
fu_device_incorporate(FuDevice *self, FuDevice *donor, FuDeviceIncorporateFlags flag)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	FuDevicePrivate *priv_donor = GET_PRIVATE(donor);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_DEVICE(donor));

	/* do these unconditionally */
	if (priv->ctx == NULL && priv_donor->ctx != NULL)
		fu_device_set_context(self, priv_donor->ctx);
	if (priv->backend == NULL && priv_donor->backend != NULL)
		fu_device_set_backend(self, priv_donor->backend);

	/* bitflags */
	if (flag & FU_DEVICE_INCORPORATE_FLAG_BASECLASS) {
		fwupd_device_incorporate(FWUPD_DEVICE(self), FWUPD_DEVICE(donor));
		if (fu_device_get_id(self) != NULL)
			priv->device_id_valid = TRUE;
		/* remove the baseclass-added serial number and GUIDs if set */
		if (fu_device_has_private_flag_quark(self, quarks[QUARK_NO_SERIAL_NUMBER]))
			fwupd_device_set_serial(FWUPD_DEVICE(self), NULL);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_VENDOR) {
		if (fu_device_get_vendor(self) == NULL && fu_device_get_vendor(donor) != NULL)
			fu_device_set_vendor(self, fu_device_get_vendor(donor));
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_PHYSICAL_ID) {
		if (priv->physical_id == NULL && priv_donor->physical_id != NULL)
			fu_device_set_physical_id(self, priv_donor->physical_id);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_LOGICAL_ID) {
		if (priv->logical_id == NULL && priv_donor->logical_id != NULL)
			fu_device_set_logical_id(self, priv_donor->logical_id);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_BACKEND_ID) {
		if (priv->backend_id == NULL && priv_donor->backend_id != NULL)
			fu_device_set_backend_id(self, priv_donor->backend_id);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_VID) {
		if (priv->vid == 0x0 && priv_donor->vid != 0x0)
			fu_device_set_vid(self, priv_donor->vid);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_PID) {
		if (priv->pid == 0x0 && priv_donor->pid != 0x0)
			fu_device_set_pid(self, priv_donor->pid);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_VENDOR_IDS) {
		GPtrArray *vendor_ids = fu_device_get_vendor_ids(donor);
		for (guint i = 0; i < vendor_ids->len; i++) {
			const gchar *vendor_id = g_ptr_array_index(vendor_ids, i);
			fu_device_add_vendor_id(self, vendor_id);
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_REMOVE_DELAY) {
		if (priv->remove_delay == 0 && priv_donor->remove_delay != 0)
			fu_device_set_remove_delay(self, priv_donor->remove_delay);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_ACQUIESCE_DELAY) {
		if (priv->acquiesce_delay == 0 && priv_donor->acquiesce_delay != 0)
			fu_device_set_acquiesce_delay(self, priv_donor->acquiesce_delay);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_ICONS) {
		if (fu_device_get_icons(self)->len == 0) {
			GPtrArray *icons = fu_device_get_icons(donor);
			for (guint i = 0; i < icons->len; i++) {
				const gchar *icon_name = g_ptr_array_index(icons, i);
				fu_device_add_icon(self, icon_name);
			}
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_EVENTS) {
		if (priv_donor->events != NULL && donor != priv->proxy) {
			for (guint i = 0; i < priv_donor->events->len; i++) {
				FuDeviceEvent *event = g_ptr_array_index(priv_donor->events, i);
				fu_device_add_event(self, event);
			}
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_UPDATE_ERROR) {
		if (fu_device_get_update_error(self) == NULL &&
		    fu_device_get_update_error(donor) != NULL) {
			fu_device_set_update_error(self, fu_device_get_update_error(donor));
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_UPDATE_STATE) {
		if (fu_device_get_update_state(self) == FWUPD_UPDATE_STATE_UNKNOWN &&
		    fu_device_get_update_state(donor) != FWUPD_UPDATE_STATE_UNKNOWN) {
			fu_device_set_update_state(self, fu_device_get_update_state(donor));
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_SUPERCLASS) {
		gpointer device_class_incorporate_last = NULL;
		g_autoptr(GPtrArray) class_parents =
		    fu_device_get_common_class_parents(self, donor);

		/* run every unique ->incorporate() in each subclass */
		for (guint i = 0; i < class_parents->len; i++) {
			FuDeviceClass *device_class = g_ptr_array_index(class_parents, i);
			if (device_class->incorporate != NULL &&
			    device_class->incorporate != device_class_incorporate_last) {
				device_class->incorporate(self, donor);
				device_class_incorporate_last = device_class->incorporate;
			}
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_UPDATE_MESSAGE) {
		if (priv->update_message == NULL && priv_donor->update_message != NULL)
			fu_device_set_update_message(self, priv_donor->update_message);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_UPDATE_IMAGE) {
		if (priv->update_image == NULL && priv_donor->update_image != NULL)
			fu_device_set_update_image(self, priv_donor->update_image);
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_INSTANCE_IDS)
		fu_device_incorporate_instance_ids(self, donor);
	if (flag & FU_DEVICE_INCORPORATE_FLAG_GTYPE) {
		if (priv->specialized_gtype == G_TYPE_INVALID &&
		    priv_donor->specialized_gtype != G_TYPE_INVALID) {
			fu_device_set_specialized_gtype(self, priv_donor->specialized_gtype);
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_POSSIBLE_PLUGINS) {
		for (guint i = 0; i < priv_donor->possible_plugins->len; i++) {
			const gchar *possible_plugin =
			    g_ptr_array_index(priv_donor->possible_plugins, i);
			fu_device_add_possible_plugin(self, possible_plugin);
		}
	}
	if (flag & FU_DEVICE_INCORPORATE_FLAG_INSTANCE_KEYS) {
		if (priv_donor->instance_hash != NULL) {
			GHashTableIter iter;
			gpointer key, value;
			g_hash_table_iter_init(&iter, priv_donor->instance_hash);
			while (g_hash_table_iter_next(&iter, &key, &value)) {
				if (fu_device_get_instance_str(self, key) == NULL)
					fu_device_add_instance_str(self, key, value);
			}
		}
	}

	/* everything else */
	if (flag == FU_DEVICE_INCORPORATE_FLAG_ALL) {
		GPtrArray *instance_ids = fu_device_get_instance_ids(donor);
		GPtrArray *parent_physical_ids = fu_device_get_parent_physical_ids(donor);
		GPtrArray *parent_backend_ids = fu_device_get_parent_backend_ids(donor);

		/* copy from donor FuDevice if has not already been set */
		if (priv_donor->private_flags != NULL) {
			fu_device_register_private_flags(self);
			for (guint i = 0; i < priv_donor->private_flags->len; i++) {
				GQuark flag_quark =
				    g_array_index(priv_donor->private_flags, GQuark, i);
				if (fu_device_private_flags_has_registered_quark(self,
										 flag_quark)) {
					fu_device_add_private_flag_quark(self, flag_quark);
				}
			}
		}
		if (priv->created_usec == 0 && priv_donor->created_usec != 0)
			fu_device_set_created_usec(self, priv_donor->created_usec);
		if (priv->modified_usec == 0 && priv_donor->modified_usec != 0)
			fu_device_set_modified_usec(self, priv_donor->modified_usec);
		if (priv->equivalent_id == NULL && fu_device_get_equivalent_id(donor) != NULL)
			fu_device_set_equivalent_id(self, fu_device_get_equivalent_id(donor));
		if (priv->fwupd_version == NULL && priv_donor->fwupd_version != NULL)
			fu_device_set_fwupd_version(self, priv_donor->fwupd_version);
		if (priv_donor->required_free > 0)
			fu_device_set_required_free(self, priv_donor->required_free);
		if (priv->update_request_id == NULL && priv_donor->update_request_id != NULL)
			fu_device_set_update_request_id(self, priv_donor->update_request_id);
		if (fu_device_has_private_flag_quark(self, quarks[QUARK_REFCOUNTED_PROXY]) &&
		    fu_device_has_private_flag_quark(donor, quarks[QUARK_REFCOUNTED_PROXY])) {
			if (priv->proxy == NULL && priv_donor->proxy != NULL)
				fu_device_set_proxy(self, priv_donor->proxy);
		}
		if (priv->proxy_guid == NULL && priv_donor->proxy_guid != NULL)
			fu_device_set_proxy_guid(self, priv_donor->proxy_guid);
		if (priv->custom_flags == NULL && priv_donor->custom_flags != NULL)
			fu_device_set_custom_flags(self, priv_donor->custom_flags);
		if (priv_donor->parent_guids != NULL) {
			for (guint i = 0; i < priv_donor->parent_guids->len; i++) {
				const gchar *guid = g_ptr_array_index(priv_donor->parent_guids, i);
				fu_device_add_parent_guid(self, guid);
			}
		}
		if (parent_physical_ids != NULL) {
			for (guint i = 0; i < parent_physical_ids->len; i++) {
				const gchar *tmp = g_ptr_array_index(parent_physical_ids, i);
				fu_device_add_parent_physical_id(self, tmp);
			}
		}
		if (parent_backend_ids != NULL) {
			for (guint i = 0; i < parent_backend_ids->len; i++) {
				const gchar *tmp = g_ptr_array_index(parent_backend_ids, i);
				fu_device_add_parent_backend_id(self, tmp);
			}
		}
		if (priv_donor->metadata != NULL) {
			GHashTableIter iter;
			gpointer key, value;
			g_hash_table_iter_init(&iter, priv_donor->metadata);
			while (g_hash_table_iter_next(&iter, &key, &value)) {
				if (fu_device_get_metadata(self, key) == NULL)
					fu_device_set_metadata(self, key, value);
			}
		}

		/* call the set_quirk_kv() vfunc for the superclassed object */
		for (guint i = 0; i < instance_ids->len; i++) {
			const gchar *instance_id = g_ptr_array_index(instance_ids, i);
			g_autofree gchar *guid = fwupd_guid_hash_string(instance_id);
			fu_device_add_guid_quirks(self, guid);
		}
	}
}

/**
 * fu_device_replace:
 * @self: a #FuDevice
 * @donor: the old #FuDevice
 *
 * Copy properties from the old (no-longer-connected) device to the new (connected) device.
 *
 * This is typcically called from the daemon device list and should not be called from plugin code.
 *
 * Since: 1.9.2
 **/
void
fu_device_replace(FuDevice *self, FuDevice *donor)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_DEVICE(donor));

	/* optional subclass */
	if (device_class->replace != NULL)
		device_class->replace(self, donor);
}

/**
 * fu_device_incorporate_flag:
 * @self: a #FuDevice
 * @donor: another device
 * @flag: device flags
 *
 * Copy the value of a specific flag from the donor object.
 *
 * Since: 1.3.5
 **/
void
fu_device_incorporate_flag(FuDevice *self, FuDevice *donor, FwupdDeviceFlags flag)
{
	if (fu_device_has_flag(donor, flag) && !fu_device_has_flag(self, flag)) {
		g_debug("donor set %s", fwupd_device_flag_to_string(flag));
		fu_device_add_flag(self, flag);
	} else if (!fu_device_has_flag(donor, flag) && fu_device_has_flag(self, flag)) {
		g_debug("donor unset %s", fwupd_device_flag_to_string(flag));
		fu_device_remove_flag(self, flag);
	}
}

/**
 * fu_device_incorporate_from_component: (skip):
 * @self: a device
 * @component: a Xmlb node
 *
 * Copy all properties from the donor AppStream component.
 *
 * Since: 1.2.4
 **/
void
fu_device_incorporate_from_component(FuDevice *self, XbNode *component)
{
	const gchar *tmp;
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(XB_IS_NODE(component));
	tmp = xb_node_query_text(component, "custom/value[@key='LVFS::UpdateMessage']", NULL);
	if (tmp != NULL)
		fu_device_set_update_message(self, tmp);
	tmp = xb_node_query_text(component, "custom/value[@key='LVFS::UpdateImage']", NULL);
	if (tmp != NULL)
		fu_device_set_update_image(self, tmp);
}

static void
fu_device_ensure_from_component_name(FuDevice *self, XbNode *component)
{
	const gchar *name = NULL;

	/* copy 1:1 */
	name = xb_node_query_text(component, "name", NULL);
	if (name != NULL) {
		fu_device_set_name(self, name);
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME);
	}
}

static void
fu_device_ensure_from_component_vendor(FuDevice *self, XbNode *component)
{
	const gchar *vendor = NULL;

	/* copy 1:1 */
	vendor = xb_node_query_text(component, "developer_name", NULL);
	if (vendor != NULL) {
		fu_device_set_vendor(self, vendor);
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VENDOR);
	}
}

static void
fu_device_ensure_from_component_signed(FuDevice *self, XbNode *component)
{
	const gchar *value = NULL;

	/* already set, possibly by a quirk */
	if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD) ||
	    fu_device_has_flag(self, FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD))
		return;

	/* copy 1:1 */
	value = xb_node_query_text(component, "custom/value[@key='LVFS::DeviceIntegrity']", NULL);
	if (value != NULL) {
		if (g_strcmp0(value, "signed") == 0) {
			fu_device_add_flag(self, FWUPD_DEVICE_FLAG_SIGNED_PAYLOAD);
		} else if (g_strcmp0(value, "unsigned") == 0) {
			fu_device_add_flag(self, FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
		} else {
			g_warning("payload value unexpected: %s, expected signed|unsigned", value);
		}
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VENDOR);
	}
}

static void
fu_device_ensure_from_component_icon(FuDevice *self, XbNode *component)
{
	const gchar *icon = NULL;

	/* copy 1:1 */
	icon = xb_node_query_text(component, "icon", NULL);
	if (icon != NULL) {
		fu_device_add_icon(self, icon);
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_ICON);
	}
}

static void
fu_device_ensure_from_component_flags(FuDevice *self, XbNode *component)
{
	const gchar *tmp =
	    xb_node_query_text(component, "custom/value[@key='LVFS::DeviceFlags']", NULL);
	if (tmp != NULL) {
		g_auto(GStrv) hints = g_strsplit(tmp, ",", -1);
		for (guint i = 0; hints[i] != NULL; i++)
			fu_device_set_custom_flag(self, hints[i]);
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_FLAGS);
	}
}

static const gchar *
fu_device_category_to_name(const gchar *cat)
{
	if (g_strcmp0(cat, "X-EmbeddedController") == 0)
		return "Embedded Controller";
	if (g_strcmp0(cat, "X-ManagementEngine") == 0)
		return "Intel Management Engine";
	if (g_strcmp0(cat, "X-CorporateManagementEngine") == 0)
		return "Intel Management Engine";
	if (g_strcmp0(cat, "X-ConsumerManagementEngine") == 0)
		return "Intel Management Engine";
	if (g_strcmp0(cat, "X-ThunderboltController") == 0)
		return "Thunderbolt Controller";
	if (g_strcmp0(cat, "X-PlatformSecurityProcessor") == 0)
		return "Platform Security Processor";
	if (g_strcmp0(cat, "X-CpuMicrocode") == 0)
		return "CPU Microcode";
	if (g_strcmp0(cat, "X-Battery") == 0)
		return "Battery";
	if (g_strcmp0(cat, "X-Camera") == 0)
		return "Camera";
	if (g_strcmp0(cat, "X-TPM") == 0)
		return "TPM";
	if (g_strcmp0(cat, "X-Touchpad") == 0)
		return "Touchpad";
	if (g_strcmp0(cat, "X-Mouse") == 0)
		return "Mouse";
	if (g_strcmp0(cat, "X-Keyboard") == 0)
		return "Keyboard";
	if (g_strcmp0(cat, "X-VideoDisplay") == 0)
		return "Display";
	if (g_strcmp0(cat, "X-BaseboardManagementController") == 0)
		return "BMC";
	if (g_strcmp0(cat, "X-UsbReceiver") == 0)
		return "USB Receiver";
	if (g_strcmp0(cat, "X-Gpu") == 0)
		return "GPU";
	if (g_strcmp0(cat, "X-Dock") == 0)
		return "Dock";
	if (g_strcmp0(cat, "X-UsbDock") == 0)
		return "USB Dock";
	if (g_strcmp0(cat, "X-FingerprintReader") == 0)
		return "Fingerprint Reader";
	if (g_strcmp0(cat, "X-GraphicsTablet") == 0)
		return "Graphics Tablet";
	if (g_strcmp0(cat, "X-InputController") == 0)
		return "Input Controller";
	if (g_strcmp0(cat, "X-Headphones") == 0)
		return "Headphones";
	if (g_strcmp0(cat, "X-Headset") == 0)
		return "Headset";
	return NULL;
}

static void
fu_device_ensure_from_component_name_category(FuDevice *self, XbNode *component)
{
	const gchar *name = NULL;
	g_autoptr(GPtrArray) cats = NULL;

	/* get AppStream and safe-compat categories */
	cats = xb_node_query(component, "categories/category|X-categories/category", 0, NULL);
	if (cats == NULL)
		return;
	for (guint i = 0; i < cats->len; i++) {
		XbNode *n = g_ptr_array_index(cats, i);
		name = fu_device_category_to_name(xb_node_get_text(n));
		if (name != NULL)
			break;
	}
	if (name != NULL) {
		fu_device_set_name(self, name);
		fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME_CATEGORY);
	}

	/* batteries updated using capsules should ignore the system power restriction */
	if (g_strcmp0(fu_device_get_plugin(self), "uefi_capsule") == 0) {
		gboolean is_battery = FALSE;
		for (guint i = 0; i < cats->len; i++) {
			XbNode *n = g_ptr_array_index(cats, i);
			if (g_strcmp0(xb_node_get_text(n), "X-Battery") == 0) {
				is_battery = TRUE;
				break;
			}
		}
		if (is_battery) {
			g_info("ignoring system power for %s battery", fu_device_get_id(self));
			fu_device_add_private_flag(self,
						   FU_DEVICE_PRIVATE_FLAG_IGNORE_SYSTEM_POWER);
		}
	}
}

static void
_g_ptr_array_reverse(GPtrArray *array)
{
	guint last_idx = array->len - 1;
	for (guint i = 0; i < array->len / 2; i++) {
		gpointer tmp = array->pdata[i];
		array->pdata[i] = array->pdata[last_idx - i];
		array->pdata[last_idx - i] = tmp;
	}
}

static void
fu_device_ensure_from_component_verfmt(FuDevice *self, XbNode *component)
{
	FwupdVersionFormat verfmt = FWUPD_VERSION_FORMAT_UNKNOWN;
	g_autoptr(GPtrArray) verfmts = NULL;

	/* get metadata */
	verfmts = xb_node_query(component, "custom/value[@key='LVFS::VersionFormat']", 0, NULL);
	if (verfmts == NULL)
		return;
	_g_ptr_array_reverse(verfmts);
	for (guint i = 0; i < verfmts->len; i++) {
		XbNode *value = g_ptr_array_index(verfmts, i);
		verfmt = fwupd_version_format_from_string(xb_node_get_text(value));
		if (verfmt != FWUPD_VERSION_FORMAT_UNKNOWN)
			break;
	}

	/* found and different to existing */
	if (verfmt != FWUPD_VERSION_FORMAT_UNKNOWN &&
	    fu_device_get_version_format(self) != verfmt) {
		fu_device_set_version_format(self, verfmt);
		if (fu_device_get_version_raw(self) != 0x0) {
			g_autofree gchar *version = NULL;
			version = fu_version_from_uint32(fu_device_get_version_raw(self), verfmt);
			fu_device_set_version(self, version);
		}
		if (fu_device_get_version_lowest_raw(self) != 0x0) {
			g_autofree gchar *version = NULL;
			version =
			    fu_version_from_uint32(fu_device_get_version_lowest_raw(self), verfmt);
			fu_device_set_version_lowest(self, version);
		}
		if (fu_device_get_version_bootloader_raw(self) != 0x0) {
			g_autofree gchar *version = NULL;
			version = fu_version_from_uint32(fu_device_get_version_bootloader_raw(self),
							 verfmt);
			fu_device_set_version_bootloader(self, version);
		}
	}

	/* do not try to do this again */
	fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VERFMT);
}

/**
 * fu_device_ensure_from_component: (skip):
 * @self: a device
 * @component: a #XbNode
 *
 * Ensure all properties from the donor AppStream component as required.
 *
 * Since: 1.8.13
 **/
void
fu_device_ensure_from_component(FuDevice *self, XbNode *component)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(XB_IS_NODE(component));

	/* set the name */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME))
		fu_device_ensure_from_component_name(self, component);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME_CATEGORY))
		fu_device_ensure_from_component_name_category(self, component);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_ICON))
		fu_device_ensure_from_component_icon(self, component);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VENDOR))
		fu_device_ensure_from_component_vendor(self, component);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_SIGNED))
		fu_device_ensure_from_component_signed(self, component);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VERFMT))
		fu_device_ensure_from_component_verfmt(self, component);
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_FLAGS))
		fu_device_ensure_from_component_flags(self, component);
}

/**
 * fu_device_ensure_from_release:
 * @self: a #FuDevice
 * @rel: (not nullable): a #XbNode
 *
 * Ensure all properties from the donor AppStream release as required.
 *
 * Since: 2.0.5
 **/
void
fu_device_ensure_from_release(FuDevice *self, XbNode *rel)
{
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(XB_IS_NODE(rel));

	/* set the required free */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_REQUIRED_FREE)) {
		guint64 size;
		size = xb_node_query_text_as_uint(rel,
						  "artifacts/artifact/size[@type='installed']",
						  NULL);
		if (size != G_MAXUINT64) {
			fu_device_set_required_free(self, size);
			fu_device_remove_private_flag(self,
						      FU_DEVICE_PRIVATE_FLAG_MD_SET_REQUIRED_FREE);
		}
	}

	/* optionally filter by device checksum */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_ONLY_CHECKSUM)) {
		gboolean valid = FALSE;
		g_autoptr(GPtrArray) device_checksums = NULL;

		if (fu_device_get_checksums(self)->len == 0)
			return;
		device_checksums = xb_node_query(rel, "checksum[@target='device']", 0, NULL);
		for (guint i = 0; device_checksums != NULL && i < device_checksums->len; i++) {
			XbNode *device_checksum = g_ptr_array_index(device_checksums, i);
			if (fu_device_has_checksum(self, xb_node_get_text(device_checksum))) {
				valid = TRUE;
				break;
			}
		}
		if (!valid)
			return;
	}

	/* set the version */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VERSION)) {
		const gchar *version = xb_node_get_attr(rel, "version");
		if (version != NULL) {
			fu_device_set_version(self, version);
			fu_device_remove_private_flag(self, FU_DEVICE_PRIVATE_FLAG_MD_SET_VERSION);
		}
	}
}

/**
 * fu_device_emit_request:
 * @self: a device
 * @request: a request
 * @progress: (nullable): a #FuProgress
 * @error: (nullable): optional return location for an error
 *
 * Emit a request from a plugin to the client.
 *
 * If the device is emulated then this request is ignored.
 *
 * Since: 1.9.8
 **/
gboolean
fu_device_emit_request(FuDevice *self, FwupdRequest *request, FuProgress *progress, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(FWUPD_IS_REQUEST(request), FALSE);
	g_return_val_if_fail(progress == NULL || FU_IS_PROGRESS(progress), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

#ifndef SUPPORTED_BUILD
	/* nag the developer */
	if (fwupd_request_has_flag(request, FWUPD_REQUEST_FLAG_ALLOW_GENERIC_MESSAGE) &&
	    !fu_device_has_request_flag(self, FWUPD_REQUEST_FLAG_ALLOW_GENERIC_MESSAGE)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "request %s emitted but device %s [%s] does not set "
			    "FWUPD_REQUEST_FLAG_ALLOW_GENERIC_MESSAGE",
			    fwupd_request_get_id(request),
			    fu_device_get_id(self),
			    fu_device_get_plugin(self));
		return FALSE;
	}
	if (!fwupd_request_has_flag(request, FWUPD_REQUEST_FLAG_ALLOW_GENERIC_MESSAGE) &&
	    !fu_device_has_request_flag(self, FWUPD_REQUEST_FLAG_NON_GENERIC_MESSAGE)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "request %s is not a GENERIC_MESSAGE and device %s [%s] does not set "
			    "FWUPD_REQUEST_FLAG_NON_GENERIC_MESSAGE",
			    fwupd_request_get_id(request),
			    fu_device_get_id(self),
			    fu_device_get_plugin(self));
		return FALSE;
	}
#endif

	/* sanity check */
	if (fwupd_request_get_kind(request) == FWUPD_REQUEST_KIND_UNKNOWN) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "a request must have an assigned kind");
		return FALSE;
	}
	if (fwupd_request_get_id(request) == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "a request must have an assigned ID");
		return FALSE;
	}
	if (fwupd_request_get_kind(request) >= FWUPD_REQUEST_KIND_LAST) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "invalid request kind");
		return FALSE;
	}

	/* already cancelled */
	if (progress != NULL && fu_progress_has_flag(progress, FU_PROGRESS_FLAG_NO_SENDER)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no sender, and so cannot process request");
		return FALSE;
	}

	/* ignore */
	if (fu_device_has_flag(self, FWUPD_DEVICE_FLAG_EMULATED)) {
		g_info("ignoring device %s request of %s as emulated",
		       fu_device_get_id(self),
		       fwupd_request_get_id(request));
		return TRUE;
	}

	/* ensure set */
	fwupd_request_set_device_id(request, fu_device_get_id(self));

	/* for compatibility with older clients */
	if (fwupd_request_get_kind(request) == FWUPD_REQUEST_KIND_POST) {
		fu_device_set_update_message(self, fwupd_request_get_message(request));
		fu_device_set_update_image(self, fwupd_request_get_image(request));
	}

	/* proxy to the engine */
	if (progress != NULL) {
		fu_progress_set_status(progress, FWUPD_STATUS_WAITING_FOR_USER);
	} else if (priv->progress != NULL) {
		g_debug("using fallback progress");
		fu_progress_set_status(priv->progress, FWUPD_STATUS_WAITING_FOR_USER);
	} else {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "no progress");
		return FALSE;
	}
	g_signal_emit(self, signals[SIGNAL_REQUEST], 0, request);
	if (fwupd_request_get_kind(request) < FWUPD_REQUEST_KIND_LAST)
		priv->request_cnts[fwupd_request_get_kind(request)]++;
	return TRUE;
}

static void
fu_device_ensure_instance_hash(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	if (priv->instance_hash != NULL)
		return;
	priv->instance_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/**
 * fu_device_add_instance_str:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: (nullable): value
 *
 * Assign a value for the @key.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_str(FuDevice *self, const gchar *key, const gchar *value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash, g_strdup(key), g_strdup(value));
}

static gboolean
fu_device_strsafe_instance_id_is_valid_char(gchar c)
{
	switch (c) {
	case ' ':
	case '_':
	case '&':
	case '/':
	case '\\':
	case '-':
	case '(':
	case ')':
	case ',':
		return FALSE;
	default:
		break;
	}
	return g_ascii_isprint(c);
}

/* NOTE: we can't use fu_strsafe as this behavior is now effectively ABI */
static gchar *
fu_device_strsafe_instance_id(const gchar *str)
{
	g_autoptr(GString) tmp = g_string_new(NULL);
	gboolean has_content = FALSE;

	/* sanity check */
	if (str == NULL)
		return NULL;

	/* use - to replace problematic chars -- but only once per section */
	for (guint i = 0; str[i] != '\0'; i++) {
		gchar c = str[i];
		if (!fu_device_strsafe_instance_id_is_valid_char(c)) {
			if (has_content) {
				g_string_append_c(tmp, '-');
				has_content = FALSE;
			}
		} else {
			g_string_append_c(tmp, c);
			has_content = TRUE;
		}
	}

	/* remove any trailing replacements */
	if (tmp->len > 0 && tmp->str[tmp->len - 1] == '-')
		g_string_truncate(tmp, tmp->len - 1);

	/* nothing left! */
	if (tmp->len == 0)
		return NULL;

	/* success */
	return g_string_free(g_steal_pointer(&tmp), FALSE);
}

/**
 * fu_device_add_instance_strsafe:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: (nullable): value
 *
 * Assign a sanitized value for the @key.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_strsafe(FuDevice *self, const gchar *key, const gchar *value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash,
			    g_strdup(key),
			    fu_device_strsafe_instance_id(value));
}

/**
 * fu_device_add_instance_strup:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: (nullable): value
 *
 * Assign a uppercase value for the @key.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_strup(FuDevice *self, const gchar *key, const gchar *value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash,
			    g_strdup(key),
			    value != NULL ? g_utf8_strup(value, -1) : NULL);
}

/**
 * fu_device_add_instance_u4:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: value
 *
 * Assign a value to the @key, which is padded as %1X.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_u4(FuDevice *self, const gchar *key, guint8 value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash, g_strdup(key), g_strdup_printf("%01X", value));
}

/**
 * fu_device_add_instance_u8:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: value
 *
 * Assign a value to the @key, which is padded as %2X.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_u8(FuDevice *self, const gchar *key, guint8 value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash, g_strdup(key), g_strdup_printf("%02X", value));
}

/**
 * fu_device_add_instance_u16:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: value
 *
 * Assign a value to the @key, which is padded as %4X.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_u16(FuDevice *self, const gchar *key, guint16 value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash, g_strdup(key), g_strdup_printf("%04X", value));
}

/**
 * fu_device_add_instance_u32:
 * @self: a #FuDevice
 * @key: (not nullable): string
 * @value: value
 *
 * Assign a value to the @key, which is padded as %8X.
 *
 * Since: 1.7.7
 **/
void
fu_device_add_instance_u32(FuDevice *self, const gchar *key, guint32 value)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(key != NULL);
	fu_device_ensure_instance_hash(self);
	g_hash_table_insert(priv->instance_hash, g_strdup(key), g_strdup_printf("%08X", value));
}

/**
 * fu_device_build_instance_id_strv:
 * @self: a #FuDevice
 * @subsystem: (not nullable): subsystem, e.g. `NVME`
 * @keys: (nullable): %NULL terminated array of string keys
 * @error: (nullable): optional return location for an error
 *
 * Creates an instance ID from a prefix and an array of key values.
 * If the key value cannot be found, the parent and then proxy is also queried.
 *
 * If any of the key values remain unset then no instance ID is added.
 *
 *	fu_device_add_instance_str(dev, "VID", "1234");
 *	fu_device_add_instance_u16(dev, "PID", 5678);
 *	const gchar *keys[] = {"VID", "PID", NULL};
 *	if (!fu_device_build_instance_id_strv(dev, "NVME", keys, &error))
 *		g_warning("failed to add ID: %s", error->message);
 *
 * Returns: %TRUE if the instance ID was added.
 *
 * Since: 2.0.14
 **/
gboolean
fu_device_build_instance_id_strv(FuDevice *self,
				 const gchar *subsystem,
				 gchar **keys,
				 GError **error)
{
	FuDevice *parent = fu_device_get_parent(self);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(GString) str = g_string_new(subsystem);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(subsystem != NULL, FALSE);

	for (guint i = 0; keys != NULL && keys[i] != NULL; i++) {
		const gchar *key = keys[i];
		const gchar *value;

		value = fu_device_get_instance_str(self, key);
		if (value == NULL && parent != NULL)
			value = fu_device_get_instance_str(parent, key);
		if (value == NULL && priv->proxy != NULL)
			value = fu_device_get_instance_str(priv->proxy, key);
		if (value == NULL) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "no value for %s",
				    key);
			return FALSE;
		}
		g_string_append(str, i == 0 ? "\\" : "&");
		g_string_append_printf(str, "%s_%s", key, value);
	}

	/* success */
	fu_device_add_instance_id(self, str->str);
	return TRUE;
}

/**
 * fu_device_build_instance_id:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 * @subsystem: (not nullable): subsystem, e.g. `NVME`
 * @...: pairs of string key values, ending with %NULL
 *
 * Creates an instance ID from a prefix and some key values.
 * If the key value cannot be found, the parent and then proxy is also queried.
 *
 * If any of the key values remain unset then no instance ID is added.
 *
 *	fu_device_add_instance_str(dev, "VID", "1234");
 *	fu_device_add_instance_u16(dev, "PID", 5678);
 *	if (!fu_device_build_instance_id(dev, &error, "NVME", "VID", "PID", NULL))
 *		g_warning("failed to add ID: %s", error->message);
 *
 * Returns: %TRUE if the instance ID was added.
 *
 * Since: 1.7.7
 **/
gboolean
fu_device_build_instance_id(FuDevice *self, GError **error, const gchar *subsystem, ...)
{
	va_list args;
	g_autoptr(GStrvBuilder) keys_builder = g_strv_builder_new();
	g_auto(GStrv) keys = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(subsystem != NULL, FALSE);

	/* convert varargs to GStrv */
	va_start(args, subsystem);
	for (;;) {
		const gchar *key = va_arg(args, const gchar *);
		if (key == NULL)
			break;
		g_strv_builder_add(keys_builder, key);
	}
	va_end(args);
	keys = g_strv_builder_end(keys_builder);

	/* call the strv version */
	return fu_device_build_instance_id_strv(self, subsystem, keys, error);
}

/**
 * fu_device_build_instance_id_full:
 * @self: a #FuDevice
 * @flags: instance ID flags, e.g. %FU_DEVICE_INSTANCE_FLAG_QUIRKS
 * @error: (nullable): optional return location for an error
 * @subsystem: (not nullable): subsystem, e.g. `NVME`
 * @...: pairs of string key values, ending with %NULL
 *
 * Creates an instance ID with specific flags from a prefix and some key values. If any of the key
 * values are unset then no instance ID is added.
 *
 * Returns: %TRUE if the instance ID was added.
 *
 * Since: 1.9.8
 **/
gboolean
fu_device_build_instance_id_full(FuDevice *self,
				 FuDeviceInstanceFlags flags,
				 GError **error,
				 const gchar *subsystem,
				 ...)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	gboolean ret = TRUE;
	va_list args;
	g_autoptr(GString) str = g_string_new(subsystem);

	g_return_val_if_fail(FU_IS_DEVICE(self), FALSE);
	g_return_val_if_fail(subsystem != NULL, FALSE);

	if (priv->instance_hash == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "no instance hash values defined");
		return FALSE;
	}
	va_start(args, subsystem);
	for (guint i = 0;; i++) {
		const gchar *key = va_arg(args, const gchar *);
		const gchar *value;
		if (key == NULL)
			break;
		value = g_hash_table_lookup(priv->instance_hash, key);
		if (value == NULL) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_DATA,
				    "no value for %s",
				    key);
			ret = FALSE;
			break;
		}
		g_string_append(str, i == 0 ? "\\" : "&");
		g_string_append_printf(str, "%s_%s", key, value);
	}
	va_end(args);

	/* we set an error above */
	if (!ret)
		return FALSE;

	/* success */
	fu_device_add_instance_id_full(self, str->str, flags);
	return TRUE;
}

/**
 * fu_device_security_attr_new:
 * @self: a #FuDevice
 * @appstream_id: (nullable): the AppStream component ID, e.g. `com.intel.BiosGuard`
 *
 * Creates a new #FwupdSecurityAttr for this specific device.
 *
 * Returns: (transfer full): a #FwupdSecurityAttr
 *
 * Since: 1.8.4
 **/
FwupdSecurityAttr *
fu_device_security_attr_new(FuDevice *self, const gchar *appstream_id)
{
	FuDevicePrivate *priv = fu_device_get_instance_private(self);
	g_autoptr(FwupdSecurityAttr) attr = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(appstream_id != NULL, NULL);

	attr = fu_security_attr_new(priv->ctx, appstream_id);
	fwupd_security_attr_set_plugin(attr, fu_device_get_plugin(FU_DEVICE(self)));
	fwupd_security_attr_add_guids(attr, fu_device_get_guids(FU_DEVICE(self)));

	/* if the device is a child of the host firmware then add those GUIDs too */
	if (fu_device_has_private_flag(self, FU_DEVICE_PRIVATE_FLAG_HOST_FIRMWARE_CHILD)) {
		FuDevice *msf_device = fu_device_get_parent(self);
		if (msf_device != NULL) {
			GPtrArray *guids = fu_device_get_guids(msf_device);
			for (guint i = 0; i < guids->len; i++) {
				const gchar *guid = g_ptr_array_index(guids, i);
				fwupd_security_attr_add_guid(attr, guid);
			}
		}
	}

	return g_steal_pointer(&attr);
}

static void
fu_device_ensure_events(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	if (priv->events != NULL)
		return;
	priv->events = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
}

/**
 * fu_device_add_event:
 * @self: a #FuDevice
 * @event: (not nullable): a #FuDeviceEvent
 *
 * Adds an event to the device.
 *
 * Since: 2.0.0
 **/
void
fu_device_add_event(FuDevice *self, FuDeviceEvent *event)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_DEVICE_EVENT(event));

	/* redirect */
	if (priv->target != NULL) {
		fu_device_add_event(priv->target, event);
		return;
	}

	fu_device_ensure_events(self);
	g_ptr_array_add(priv->events, g_object_ref(event));
}

/**
 * fu_device_save_event:
 * @self: a #FuDevice
 * @id: (nullable): the event ID, e.g. `usb:AA:AA:06`
 *
 * Creates a new event with a specific ID and adds it to the device.
 *
 * Returns: (transfer none): a #FuDeviceEvent
 *
 * Since: 2.0.0
 **/
FuDeviceEvent *
fu_device_save_event(FuDevice *self, const gchar *id)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autoptr(FuDeviceEvent) event = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(id != NULL, NULL);

	/* redirect */
	if (priv->target != NULL)
		return fu_device_save_event(priv->target, id);

	/* success */
	event = fu_device_event_new(id);
	fu_device_add_event(self, event);
	g_debug("saved event %s", id);
	return event;
}

/**
 * fu_device_load_event:
 * @self: a #FuDevice
 * @id: (not nullable): the event ID, e.g. `usb:AA:AA:06`
 * @error: (nullable): optional return location for an error
 *
 * Loads a new event with a specific ID from the device.
 *
 * Returns: (transfer none) (nullable): a #FuDeviceEvent
 *
 * Since: 2.0.0
 **/
FuDeviceEvent *
fu_device_load_event(FuDevice *self, const gchar *id, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_autofree gchar *id_hash = NULL;

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);
	g_return_val_if_fail(id != NULL, NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	/* redirect */
	if (priv->target != NULL)
		return fu_device_load_event(priv->target, id, error);

	/* sanity check */
	if (priv->events == NULL) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no events loaded");
		return NULL;
	}

	/* reset back to the beginning */
	if (priv->event_idx >= priv->events->len) {
		g_debug("resetting event index");
		priv->event_idx = 0;
	}

	/* look for the next event in the sequence */
	id_hash = fu_device_event_build_id(id);
	for (guint i = priv->event_idx; i < priv->events->len; i++) {
		FuDeviceEvent *event = g_ptr_array_index(priv->events, i);
		if (g_strcmp0(fu_device_event_get_id(event), id_hash) == 0) {
			priv->event_idx = i + 1;
			return event;
		}
	}

	/* look for *any* event that matches */
	for (guint i = 0; i < priv->events->len; i++) {
		FuDeviceEvent *event = g_ptr_array_index(priv->events, i);
		if (g_strcmp0(fu_device_event_get_id(event), id_hash) == 0) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "found out-of-order event %s at position %u",
				    id,
				    i);
			return NULL;
		}
	}

	/* nothing found */
	g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no event with ID %s", id);
	return NULL;
}

/**
 * fu_device_get_events:
 * @self: a #FuDevice
 *
 * Gets all the #FuDeviceEvent objects added with fu_device_add_event().
 *
 * These events should be added by #FuDevice subclasses to enable the daemon to emulate a specific
 * device type.
 *
 * Returns: (transfer none) (element-type FuDeviceEvent): events
 *
 * Since: 2.0.0
 **/
GPtrArray *
fu_device_get_events(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_IS_DEVICE(self), NULL);

	/* redirect */
	if (priv->target != NULL)
		return fu_device_get_events(priv->target);

	fu_device_ensure_events(self);
	return priv->events;
}

/**
 * fu_device_clear_events:
 * @self: a #FuDevice
 *
 * Clears all the #FuDeviceEvent objects added with fu_device_add_event(), typically after saving
 * the device to an emulation.
 *
 * Since: 2.0.0
 **/
void
fu_device_clear_events(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));

	/* redirect */
	if (priv->target != NULL) {
		fu_device_clear_events(priv->target);
		return;
	}

	if (priv->events == NULL)
		return;
	g_ptr_array_set_size(priv->events, 0);
	priv->event_idx = 0;
}

/**
 * fu_device_set_target:
 * @self: a #FuDevice
 * @target: a #FuDevice
 *
 * Sets the target device where #FuDeviceEvent objects added to @self should actually be added.
 *
 * Any existing events added to @self are added immediately to @target.
 *
 * Since: 2.0.0
 **/
void
fu_device_set_target(FuDevice *self, FuDevice *target)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_if_fail(FU_IS_DEVICE(self));
	g_return_if_fail(FU_IS_DEVICE(target));

	fu_device_incorporate(target, self, FU_DEVICE_INCORPORATE_FLAG_EVENTS);
	g_set_object(&priv->target, target);
}

/* private; used to save an emulated device */
void
fu_device_add_json(FuDevice *self, JsonBuilder *builder, FwupdCodecFlags flags)
{
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	if (fu_device_get_created_usec(self) != 0) {
#if GLIB_CHECK_VERSION(2, 80, 0)
		g_autoptr(GDateTime) dt =
		    g_date_time_new_from_unix_utc_usec(fu_device_get_created_usec(self));
#else
		g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc(
		    fu_device_get_created_usec(self) / G_USEC_PER_SEC);
#endif
		g_autofree gchar *str = g_date_time_format_iso8601(dt);
		json_builder_set_member_name(builder, "Created");
		json_builder_add_string_value(builder, str);
	}

	/* subclassed */
	if (device_class->add_json != NULL)
		device_class->add_json(self, builder, flags);
}

/* private; used to load an emulated device */
gboolean
fu_device_from_json(FuDevice *self, JsonObject *json_object, GError **error)
{
	const gchar *tmp;
	FuDeviceClass *device_class = FU_DEVICE_GET_CLASS(self);

	tmp = json_object_get_string_member_with_default(json_object, "Created", NULL);
	if (tmp != NULL) {
		g_autoptr(GDateTime) dt = g_date_time_new_from_iso8601(tmp, NULL);
#if GLIB_CHECK_VERSION(2, 80, 0)
		if (dt != NULL)
			fu_device_set_created_usec(self, g_date_time_to_unix_usec(dt));
#else
		if (dt != NULL) {
			fu_device_set_created_usec(self, g_date_time_to_unix(dt) * G_USEC_PER_SEC);
		}
#endif
	}

	/* subclassed */
	if (device_class->from_json != NULL) {
		if (!device_class->from_json(self, json_object, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_device_dispose(GObject *object)
{
	FuDevice *self = FU_DEVICE(object);
	FuDevicePrivate *priv = GET_PRIVATE(self);
	g_clear_object(&priv->ctx);
	g_clear_object(&priv->target);
	G_OBJECT_CLASS(fu_device_parent_class)->dispose(object);
}

static void
fu_device_class_init(FuDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);
	GParamSpec *pspec;
	const gchar *quark_flags[] = {
	    FU_DEVICE_PRIVATE_FLAG_NO_PROBE,
	    FU_DEVICE_PRIVATE_FLAG_REFCOUNTED_PROXY,
	    FU_DEVICE_PRIVATE_FLAG_NO_GENERIC_GUIDS,
	    FU_DEVICE_PRIVATE_FLAG_NO_SERIAL_NUMBER,
	    FU_DEVICE_PRIVATE_FLAG_IS_FAKE,
	};

	object_class->dispose = fu_device_dispose;
	object_class->finalize = fu_device_finalize;
	object_class->get_property = fu_device_get_property;
	object_class->set_property = fu_device_set_property;

	device_class->to_string = fu_device_to_string_impl;

	/* used as device flags, order is important! */
	for (guint i = 0; i < G_N_ELEMENTS(quark_flags); i++)
		quarks[i] = g_quark_from_static_string(quark_flags[i]);

	/**
	 * FuDevice::child-added:
	 * @self: the #FuDevice instance that emitted the signal
	 * @device: the #FuDevice child
	 *
	 * The ::child-added signal is emitted when a device has been added as a child.
	 *
	 * Since: 1.0.8
	 **/
	signals[SIGNAL_CHILD_ADDED] = g_signal_new("child-added",
						   G_TYPE_FROM_CLASS(object_class),
						   G_SIGNAL_RUN_LAST,
						   G_STRUCT_OFFSET(FuDeviceClass, child_added),
						   NULL,
						   NULL,
						   g_cclosure_marshal_VOID__OBJECT,
						   G_TYPE_NONE,
						   1,
						   FU_TYPE_DEVICE);
	/**
	 * FuDevice::child-removed:
	 * @self: the #FuDevice instance that emitted the signal
	 * @device: the #FuDevice child
	 *
	 * The ::child-removed signal is emitted when a device has been removed as a child.
	 *
	 * Since: 1.0.8
	 **/
	signals[SIGNAL_CHILD_REMOVED] = g_signal_new("child-removed",
						     G_TYPE_FROM_CLASS(object_class),
						     G_SIGNAL_RUN_LAST,
						     G_STRUCT_OFFSET(FuDeviceClass, child_removed),
						     NULL,
						     NULL,
						     g_cclosure_marshal_VOID__OBJECT,
						     G_TYPE_NONE,
						     1,
						     FU_TYPE_DEVICE);
	/**
	 * FuDevice::request:
	 * @self: the #FuDevice instance that emitted the signal
	 * @request: the #FwupdRequest
	 *
	 * The ::request signal is emitted when the device needs interactive action from the user.
	 *
	 * Since: 1.6.2
	 **/
	signals[SIGNAL_REQUEST] = g_signal_new("request",
					       G_TYPE_FROM_CLASS(object_class),
					       G_SIGNAL_RUN_LAST,
					       G_STRUCT_OFFSET(FuDeviceClass, request),
					       NULL,
					       NULL,
					       g_cclosure_marshal_VOID__OBJECT,
					       G_TYPE_NONE,
					       1,
					       FWUPD_TYPE_REQUEST);

	/**
	 * FuDevice:physical-id:
	 *
	 * The device physical ID.
	 *
	 * Since: 1.1.2
	 */
	pspec = g_param_spec_string("physical-id",
				    NULL,
				    NULL,
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_PHYSICAL_ID, pspec);

	/**
	 * FuDevice:logical-id:
	 *
	 * The device logical ID.
	 *
	 * Since: 1.1.2
	 */
	pspec = g_param_spec_string("logical-id",
				    NULL,
				    NULL,
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_LOGICAL_ID, pspec);

	/**
	 * FuDevice:backend-id:
	 *
	 * The device backend ID.
	 *
	 * Since: 1.5.8
	 */
	pspec = g_param_spec_string("backend-id",
				    NULL,
				    NULL,
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_BACKEND_ID, pspec);

	/**
	 * FuDevice:equivalent-id:
	 *
	 * The device equivalent ID.
	 *
	 * Since: 2.0.0
	 */
	pspec = g_param_spec_string("equivalent-id",
				    NULL,
				    NULL,
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_EQUIVALENT_ID, pspec);
	/**
	 * FuDevice:update-message:
	 *
	 * The device update message.
	 *
	 * Since: 2.0.0
	 */
	pspec = g_param_spec_string("update-message",
				    NULL,
				    NULL,
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_UPDATE_MESSAGE, pspec);

	/**
	 * FuDevice:update-image:
	 *
	 * The update image for the device.
	 *
	 * Since: 2.0.0
	 */
	pspec = g_param_spec_string("update-image",
				    NULL,
				    NULL,
				    NULL,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_UPDATE_IMAGE, pspec);

	/**
	 * FuDevice:context:
	 *
	 * The #FuContext to use.
	 *
	 * Since: 1.6.0
	 */
	pspec = g_param_spec_object("context",
				    NULL,
				    NULL,
				    FU_TYPE_CONTEXT,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_CONTEXT, pspec);

	/**
	 * FuDevice:backend:
	 *
	 * The #FuBackend that created the device.
	 *
	 * Since: 2.0.0
	 */
	pspec = g_param_spec_object("backend",
				    NULL,
				    NULL,
				    FU_TYPE_BACKEND,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_BACKEND, pspec);

	/**
	 * FuDevice:proxy:
	 *
	 * The device proxy to use.
	 *
	 * Since: 1.4.1
	 */
	pspec = g_param_spec_object("proxy",
				    NULL,
				    NULL,
				    FU_TYPE_DEVICE,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_PROXY, pspec);

	/**
	 * FuDevice:parent:
	 *
	 * The device parent.
	 *
	 * Since: 1.0.8
	 */
	pspec = g_param_spec_object("parent",
				    NULL,
				    NULL,
				    FU_TYPE_DEVICE,
				    G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_PARENT, pspec);

	/**
	 * FuDevice:private-flags:
	 *
	 * The device private flags.
	 *
	 * Since: 1.9.1
	 */
	pspec = g_param_spec_uint64("private-flags",
				    NULL,
				    NULL,
				    0,
				    G_MAXUINT64,
				    0,
				    G_PARAM_READABLE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_PRIVATE_FLAGS, pspec);

	/**
	 * FuDevice:vid:
	 *
	 * The device vendor ID.
	 *
	 * Since: 2.0.0
	 */
	pspec = g_param_spec_uint("vid",
				  NULL,
				  NULL,
				  0,
				  G_MAXUINT16,
				  0,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_VID, pspec);

	/**
	 * FuDevice:pid:
	 *
	 * The device product ID.
	 *
	 * Since: 2.0.0
	 */
	pspec = g_param_spec_uint("pid",
				  NULL,
				  NULL,
				  0,
				  G_MAXUINT16,
				  0,
				  G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_PID, pspec);

	/**
	 * FuDevice:required-free:
	 *
	 * The required amount of free firmware space.
	 *
	 * Since: 2.0.12
	 */
	pspec = g_param_spec_uint64("required-free",
				    NULL,
				    NULL,
				    0,
				    G_MAXUINT64,
				    0,
				    G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_REQUIRED_FREE, pspec);
}

static void
fu_device_init(FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE(self);
	priv->order = G_MAXINT;
	priv->possible_plugins = g_ptr_array_new_with_free_func(g_free);
	priv->acquiesce_delay = 50; /* ms */
	priv->private_flags_registered = g_array_new(FALSE, FALSE, sizeof(GQuark));
	priv->private_flags = g_array_new(FALSE, FALSE, sizeof(GQuark));
}

static void
fu_device_finalize(GObject *object)
{
	FuDevice *self = FU_DEVICE(object);
	FuDevicePrivate *priv = GET_PRIVATE(self);

	if (priv->progress != NULL)
		g_object_unref(priv->progress);
	if (priv->proxy != NULL) {
		if (priv->notify_flags_proxy_id != 0)
			g_signal_handler_disconnect(priv->proxy, priv->notify_flags_proxy_id);
		if (fu_device_has_private_flag_quark(self, quarks[QUARK_REFCOUNTED_PROXY])) {
			g_object_unref(priv->proxy);
		} else {
			g_object_remove_weak_pointer(G_OBJECT(priv->proxy),
						     (gpointer *)&priv->proxy);
		}
	}
	if (priv->backend != NULL)
		g_object_remove_weak_pointer(G_OBJECT(priv->backend), (gpointer *)&priv->backend);
	if (priv->poll_id != 0)
		g_source_remove(priv->poll_id);
	if (priv->metadata != NULL)
		g_hash_table_unref(priv->metadata);
	if (priv->inhibits != NULL)
		g_hash_table_unref(priv->inhibits);
	if (priv->instance_hash != NULL)
		g_hash_table_unref(priv->instance_hash);
	if (priv->parent_physical_ids != NULL)
		g_ptr_array_unref(priv->parent_physical_ids);
	if (priv->parent_backend_ids != NULL)
		g_ptr_array_unref(priv->parent_backend_ids);
	if (priv->events != NULL)
		g_ptr_array_unref(priv->events);
	if (priv->retry_recs != NULL)
		g_ptr_array_unref(priv->retry_recs);
	if (priv->instance_ids != NULL)
		g_ptr_array_unref(priv->instance_ids);
	if (priv->parent_guids != NULL)
		g_ptr_array_unref(priv->parent_guids);
	g_array_unref(priv->private_flags);
	g_array_unref(priv->private_flags_registered);
	g_ptr_array_unref(priv->possible_plugins);
	g_free(priv->equivalent_id);
	g_free(priv->physical_id);
	g_free(priv->logical_id);
	g_free(priv->backend_id);
	g_free(priv->update_request_id);
	g_free(priv->update_message);
	g_free(priv->update_image);
	g_free(priv->fwupd_version);
	g_free(priv->proxy_guid);
	g_free(priv->custom_flags);

	G_OBJECT_CLASS(fu_device_parent_class)->finalize(object);
}

/**
 * fu_device_new:
 *
 * Creates a new #Fudevice
 *
 * Since: 1.8.2
 **/
FuDevice *
fu_device_new(FuContext *ctx)
{
	FuDevice *self = g_object_new(FU_TYPE_DEVICE, "context", ctx, NULL);
	return FU_DEVICE(self);
}
