/*
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-redfish-backend.h"
#include "fu-redfish-common.h"
#include "fu-redfish-device.h"

typedef struct {
	FuRedfishBackend *backend;
	JsonObject *member;
	guint64 milestone;
	gchar *build;
	guint reset_pre_delay;	/* default of 0ms */
	guint reset_post_delay; /* default of 0ms */
} FuRedfishDevicePrivate;

enum { PROP_0, PROP_BACKEND, PROP_MEMBER, PROP_LAST };

G_DEFINE_TYPE_WITH_PRIVATE(FuRedfishDevice, fu_redfish_device, FU_TYPE_DEVICE)

#define GET_PRIVATE(o) (fu_redfish_device_get_instance_private(o))

static void
fu_redfish_device_to_string(FuDevice *device, guint idt, GString *str)
{
	FuRedfishDevice *self = FU_REDFISH_DEVICE(device);
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	fwupd_codec_string_append_hex(str, idt, "Milestone", priv->milestone);
	fwupd_codec_string_append(str, idt, "Build", priv->build);
	fwupd_codec_string_append_int(str, idt, "ResetPretDelay", priv->reset_pre_delay);
	fwupd_codec_string_append_int(str, idt, "ResetPostDelay", priv->reset_post_delay);
}

static void
fu_redfish_device_set_device_class(FuRedfishDevice *self, const gchar *tmp)
{
	if (g_strcmp0(tmp, "NetworkController") == 0) {
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_NETWORK_WIRED);
		return;
	}
	if (g_strcmp0(tmp, "MassStorageController") == 0) {
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_DRIVE_MULTIDISK);
		return;
	}
	if (g_strcmp0(tmp, "DisplayController") == 0) {
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_VIDEO_DISPLAY);
		return;
	}
	if (g_strcmp0(tmp, "DockingStation") == 0) {
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_DOCK);
		return;
	}
	if (g_strcmp0(tmp, "WirelessController") == 0) {
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_NETWORK_WIRELESS);
		return;
	}
	g_debug("no icon mapping for %s", tmp);
}

static gboolean
fu_redfish_device_probe_related_pcie_item(FuRedfishDevice *self, const gchar *uri, GError **error)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	JsonObject *json_obj;
	guint64 vendor_id = 0x0;
	guint64 model_id = 0x0;
	guint64 subsystem_vendor_id = 0x0;
	guint64 subsystem_model_id = 0x0;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(priv->backend);

	/* get URI */
	if (!fu_redfish_request_perform(request,
					uri,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON |
					    FU_REDFISH_REQUEST_PERFORM_FLAG_USE_CACHE,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);

	/* optional properties */
	if (json_object_has_member(json_obj, "DeviceClass")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "DeviceClass");
		if (tmp != NULL && tmp[0] != '\0')
			fu_redfish_device_set_device_class(self, tmp);
	}
	if (json_object_has_member(json_obj, "VendorId")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "VendorId");
		if (tmp != NULL && tmp[0] != '\0') {
			if (!fu_strtoull(tmp,
					 &vendor_id,
					 0,
					 G_MAXUINT16,
					 FU_INTEGER_BASE_AUTO,
					 error))
				return FALSE;
		}
	}
	if (json_object_has_member(json_obj, "DeviceId")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "DeviceId");
		if (tmp != NULL && tmp[0] != '\0') {
			if (!fu_strtoull(tmp,
					 &model_id,
					 0,
					 G_MAXUINT16,
					 FU_INTEGER_BASE_AUTO,
					 error))
				return FALSE;
		}
	}
	if (json_object_has_member(json_obj, "SubsystemVendorId")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "SubsystemVendorId");
		if (tmp != NULL && tmp[0] != '\0') {
			if (!fu_strtoull(tmp,
					 &subsystem_vendor_id,
					 0,
					 G_MAXUINT16,
					 FU_INTEGER_BASE_AUTO,
					 error))
				return FALSE;
		}
	}
	if (json_object_has_member(json_obj, "SubsystemId")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "SubsystemId");
		if (tmp != NULL && tmp[0] != '\0') {
			if (!fu_strtoull(tmp,
					 &subsystem_model_id,
					 0,
					 G_MAXUINT16,
					 FU_INTEGER_BASE_AUTO,
					 error))
				return FALSE;
		}
	}

	/* add vendor ID */
	fu_device_build_vendor_id_u16(FU_DEVICE(self), "PCI", vendor_id);

	/* add more instance IDs if possible */
	if (vendor_id != 0x0)
		fu_device_add_instance_u16(FU_DEVICE(self), "VEN", vendor_id);
	if (model_id != 0x0)
		fu_device_add_instance_u16(FU_DEVICE(self), "DEV", model_id);
	if (subsystem_vendor_id != 0x0 && subsystem_model_id != 0x0) {
		g_autofree gchar *subsys = NULL;
		subsys = g_strdup_printf("%04X%04X",
					 (guint)subsystem_vendor_id,
					 (guint)subsystem_model_id);
		fu_device_add_instance_str(FU_DEVICE(self), "SUBSYS", subsys);
	}
	fu_device_build_instance_id(FU_DEVICE(self), NULL, "PCI", "VEN", "DEV", NULL);
	fu_device_build_instance_id(FU_DEVICE(self), NULL, "PCI", "VEN", "DEV", "SUBSYS", NULL);

	/* success */
	return TRUE;
}

static gboolean
fu_redfish_device_probe_related_pcie_functions(FuRedfishDevice *self,
					       const gchar *uri,
					       GError **error)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	JsonObject *json_obj;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(priv->backend);

	/* get URI */
	if (!fu_redfish_request_perform(request,
					uri,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON |
					    FU_REDFISH_REQUEST_PERFORM_FLAG_USE_CACHE,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);

	if (json_object_has_member(json_obj, "Members")) {
		JsonArray *members_array = json_object_get_array_member(json_obj, "Members");
		for (guint i = 0; i < json_array_get_length(members_array); i++) {
			JsonObject *related_item;
			related_item = json_array_get_object_element(members_array, i);
			if (json_object_has_member(related_item, "@odata.id")) {
				const gchar *id =
				    json_object_get_string_member(related_item, "@odata.id");
				if (!fu_redfish_device_probe_related_pcie_item(self, id, error))
					return FALSE;
			}
		}
	}

	/* success */
	return TRUE;
}

static gboolean
fu_redfish_device_probe_related_item(FuRedfishDevice *self, const gchar *uri, GError **error)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	JsonObject *json_obj;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(priv->backend);

	/* get URI */
	if (!fu_redfish_request_perform(request,
					uri,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON |
					    FU_REDFISH_REQUEST_PERFORM_FLAG_USE_CACHE,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);

	/* optional properties */
	if (json_object_has_member(json_obj, "SerialNumber")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "SerialNumber");
		if (tmp != NULL && tmp[0] != '\0' && g_strcmp0(tmp, "N/A") != 0)
			fu_device_set_serial(FU_DEVICE(self), tmp);
	}
	if (json_object_has_member(json_obj, "HotPluggable")) {
		/* this is better than the heuristic we get from the device name */
		if (json_object_get_boolean_member(json_obj, "HotPluggable"))
			fu_device_remove_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
		else
			fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	}

	/* sometimes an array, sometimes an object! */
	if (json_object_has_member(json_obj, "PCIeFunctions")) {
		JsonNode *pcie_functions = json_object_get_member(json_obj, "PCIeFunctions");
		if (JSON_NODE_HOLDS_OBJECT(pcie_functions)) {
			JsonObject *obj = json_node_get_object(pcie_functions);
			if (json_object_has_member(obj, "@odata.id")) {
				const gchar *id = json_object_get_string_member(obj, "@odata.id");
				if (!fu_redfish_device_probe_related_pcie_functions(self,
										    id,
										    error))
					return FALSE;
			}
		}
	}
	return TRUE;
}

/* parses a Lenovo XCC-format version like "11A-1.02" */
static gboolean
fu_redfish_device_set_version_lenovo(FuRedfishDevice *self, const gchar *version, GError **error)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	g_autofree gchar *out_build = NULL;
	g_autofree gchar *out_version = NULL;

	/* split up Lenovo format */
	if (!fu_redfish_common_parse_version_lenovo(version, &out_build, &out_version, error))
		return FALSE;

	/* split out milestone */
	priv->milestone = g_ascii_strtoull(out_build, NULL, 10); /* nocheck:blocked */

	/* odd numbered builds are unsigned */
	if (priv->milestone % 2 != 0) {
		fu_device_add_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_UNSIGNED_BUILD);
	}

	/* build is only one letter from A -> Z */
	if (!g_ascii_isalpha(out_build[2])) {
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_DATA, "build letter invalid");
		return FALSE;
	}
	priv->build = g_strndup(out_build + 2, 1);
	fu_device_set_version(FU_DEVICE(self), out_version);
	fu_device_set_version_format(FU_DEVICE(self), fu_version_guess_format(out_version));
	return TRUE;
}

static void
fu_redfish_device_set_version(FuRedfishDevice *self, const gchar *tmp)
{
	/* OEM specific */
	if (g_strcmp0(fu_device_get_vendor(FU_DEVICE(self)), "Lenovo") == 0) {
		g_autoptr(GError) error_local = NULL;
		if (!fu_redfish_device_set_version_lenovo(self, tmp, &error_local)) {
			g_debug("failed to parse Lenovo version %s: %s", tmp, error_local->message);
		}
	}

	/* fallback */
	if (fu_device_get_version(FU_DEVICE(self)) == NULL) {
		g_autofree gchar *ver = fu_redfish_common_fix_version(tmp);
		if (ver != NULL) {
			fu_device_set_version(FU_DEVICE(self), ver);
			fu_device_set_version_format(FU_DEVICE(self), fu_version_guess_format(ver));
		}
	}
}

static void
fu_redfish_device_set_version_lowest(FuRedfishDevice *self, const gchar *tmp)
{
	/* OEM specific */
	if (g_strcmp0(fu_device_get_vendor(FU_DEVICE(self)), "Lenovo") == 0) {
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *out_version = NULL;
		if (!fu_redfish_common_parse_version_lenovo(tmp,
							    NULL,
							    &out_version,
							    &error_local)) {
			g_debug("failed to parse Lenovo version %s: %s", tmp, error_local->message);
		}
		fu_device_set_version_lowest(FU_DEVICE(self), out_version);
	}

	/* fallback */
	if (fu_device_get_version_lowest(FU_DEVICE(self)) == NULL) {
		g_autofree gchar *ver = fu_redfish_common_fix_version(tmp);
		fu_device_set_version_lowest(FU_DEVICE(self), ver);
	}
}

static void
fu_redfish_device_set_name(FuRedfishDevice *self, const gchar *name)
{
	/* useless */
	if (g_str_has_prefix(name, "Firmware:"))
		name += 9;

	/* device type */
	if (g_str_has_prefix(name, "DEVICE-")) {
		name += 7;
		fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	} else if (g_str_has_prefix(name, "DISK-")) {
		name += 5;
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_DRIVE_HARDDISK);
	} else if (g_str_has_prefix(name, "POWER-")) {
		name += 6;
		fu_device_add_icon(FU_DEVICE(self), FU_DEVICE_ICON_AC_ADAPTER);
		fu_device_set_summary(FU_DEVICE(self), "Redfish power supply unit");
	} else {
		fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_INTERNAL);
	}

	/* heuristics */
	if (g_strcmp0(name, "BMC") == 0)
		fu_device_set_summary(FU_DEVICE(self), "Redfish baseboard management controller");
	if (g_str_has_suffix(name, "HBA") == 0)
		fu_device_set_summary(FU_DEVICE(self), "Redfish host bus adapter");

	/* success */
	fu_device_set_name(FU_DEVICE(self), name);
}

static void
fu_redfish_device_set_vendor(FuRedfishDevice *self, const gchar *vendor)
{
	g_autofree gchar *vendor_upper = NULL;

	/* fixup a common mistake */
	if (g_strcmp0(vendor, "LEN") == 0 || g_strcmp0(vendor, "LNVO") == 0)
		vendor = "Lenovo";
	fu_device_set_vendor(FU_DEVICE(self), vendor);

	/* add vendor-id */
	vendor_upper = g_ascii_strup(vendor, -1);
	g_strdelimit(vendor_upper, " ", '_');
	fu_device_build_vendor_id(FU_DEVICE(self), "REDFISH", vendor_upper);
}

static void
fu_redfish_device_smc_license_check(FuRedfishDevice *self)
{
	FuRedfishBackend *backend = fu_redfish_device_get_backend(self);
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(backend);
	g_autoptr(GError) error_local = NULL;

	/* see if we don't get an license error */
	if (!fu_redfish_request_perform(request,
					fu_redfish_backend_get_push_uri_path(backend),
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					&error_local)) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
			fu_device_add_problem(FU_DEVICE(self),
					      FWUPD_DEVICE_PROBLEM_MISSING_LICENSE);
		} else {
			g_debug("supermicro license check returned %s", error_local->message);
		}
	}
}

static gboolean
fu_redfish_device_probe_oem_hpe(FuRedfishDevice *self, JsonObject *json_object, GError **error)
{
	if (json_object_has_member(json_object, "DeviceClass")) {
		const gchar *guid = json_object_get_string_member(json_object, "DeviceClass");
		if (guid != NULL)
			fu_device_add_instance_id(FU_DEVICE(self), guid);
	}
	if (json_object_has_member(json_object, "Targets")) {
		JsonArray *json_array = json_object_get_array_member(json_object, "Targets");
		for (guint i = 0; i < json_array_get_length(json_array); i++) {
			const gchar *guid = json_array_get_string_element(json_array, i);
			if (guid != NULL)
				fu_device_add_instance_id(FU_DEVICE(self), guid);
		}
	}
	return TRUE;
}

static gboolean
fu_redfish_device_probe_oem_dell(FuRedfishDevice *self, JsonObject *json_object, GError **error)
{
	JsonObject *software_info;

	/* ignore */
	if (!json_object_has_member(json_object, "DellSoftwareInventory"))
		return TRUE;
	software_info = json_object_get_object_member(json_object, "DellSoftwareInventory");
	if (json_object_has_member(software_info, "Status")) {
		const gchar *status = json_object_get_string_member(software_info, "Status");
		if (g_strcmp0(status, "AvailableForInstallation") == 0)
			fu_device_add_private_flag(FU_DEVICE(self),
						   FU_REDFISH_DEVICE_FLAG_IS_BACKUP);
	}

	if (json_object_has_member(software_info, "Id")) {
		const gchar *status = json_object_get_string_member(software_info, "Id");
		if (g_ascii_strncasecmp(status, "DCIM:INSTALLED", 12) != 0) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "firmware is in repository");
			return FALSE;
		}
	}

	/* it does not seem that Dell allows targeting a device when updating */
	fu_device_add_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_WILDCARD_TARGETS);

	/* SYSTEMID is set by the backend */
	if (!fu_device_build_instance_id_full(FU_DEVICE(self),
					      FU_DEVICE_INSTANCE_FLAG_QUIRKS,
					      error,
					      "REDFISH",
					      "VENDOR",
					      "SYSTEMID",
					      NULL))
		return FALSE;
	return fu_device_build_instance_id(FU_DEVICE(self),
					   error,
					   "REDFISH",
					   "VENDOR",
					   "SYSTEMID",
					   "SOFTWAREID",
					   NULL);
}

static gboolean
fu_redfish_device_probe(FuDevice *dev, GError **error)
{
	FuRedfishDevice *self = FU_REDFISH_DEVICE(dev);
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	JsonObject *member = priv->member;
	const gchar *software_id = NULL;

	/* sanity check */
	if (priv->member == NULL) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED, "no member");
		return FALSE;
	}

	/* required to POST later */
	if (!json_object_has_member(member, "@odata.id")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no @odata.id string");
		return FALSE;
	}
	fu_device_set_physical_id(dev, "Redfish-Inventory");
	fu_device_set_logical_id(dev, json_object_get_string_member(member, "@odata.id"));
	if (json_object_has_member(member, "Id")) {
		const gchar *tmp = json_object_get_string_member(member, "Id");
		if (tmp != NULL)
			fu_device_set_backend_id(dev, tmp);
	}
	fu_device_add_instance_str(dev, "ID", fu_device_get_backend_id(dev));

	/* device properties */
	if (json_object_has_member(member, "Manufacturer")) {
		const gchar *tmp = json_object_get_string_member(member, "Manufacturer");
		if (tmp != NULL && tmp[0] != '\0')
			fu_redfish_device_set_vendor(self, tmp);
	} else {
		fu_redfish_device_set_vendor(self, fu_redfish_backend_get_vendor(priv->backend));
	}
	fu_device_add_instance_strsafe(dev, "VENDOR", fu_device_get_vendor(dev));

	/* the version can encode the instance ID suffix */
	if (json_object_has_member(member, "Version")) {
		const gchar *tmp = json_object_get_string_member(member, "Version");
		if (tmp != NULL && tmp[0] != '\0')
			fu_redfish_device_set_version(self, tmp);
	}

	/* ReleaseDate may or may not have a timezone */
	if (json_object_has_member(member, "ReleaseDate")) {
		const gchar *tmp = json_object_get_string_member(member, "ReleaseDate");
		if (tmp != NULL && tmp[0] != '\0' && g_strcmp0(tmp, "00:00:00Z") != 0) {
			g_autoptr(GDateTime) dt = NULL;
			g_autoptr(GTimeZone) tz = g_time_zone_new_utc();
			dt = g_date_time_new_from_iso8601(tmp, tz);
			if (dt != NULL) {
				guint64 unixtime = (guint64)g_date_time_to_unix(dt);
				fu_device_set_version_build_date(dev, unixtime);
			} else {
				g_warning("failed to parse ISO8601 %s", tmp);
			}
		}
	}

	/* some vendors use a GUID, others use an ID like BMC-AFBT-10 */
	if (json_object_has_member(member, "SoftwareId"))
		software_id = json_object_get_string_member(member, "SoftwareId");
	if (software_id != NULL) {
		g_autofree gchar *software_id_lower = g_ascii_strdown(software_id, -1);
		if (fwupd_guid_is_valid(software_id_lower)) {
			fu_device_add_instance_id(dev, software_id_lower);
		} else {
			fu_device_add_instance_str(dev, "SOFTWAREID", software_id);
			if (fu_device_has_private_flag(dev, FU_REDFISH_DEVICE_FLAG_UNSIGNED_BUILD))
				fu_device_add_instance_str(dev, "TYPE", "UNSIGNED");
			fu_device_build_instance_id(dev,
						    NULL,
						    "REDFISH",
						    "VENDOR",
						    "SOFTWAREID",
						    "TYPE",
						    NULL);
		}
	}

	/* get vendor-specific properties too */
	if (json_object_has_member(member, "Oem")) {
		JsonObject *oem = json_object_get_object_member(member, "Oem");
		if (oem != NULL && json_object_has_member(oem, "Hpe")) {
			JsonObject *hpe = json_object_get_object_member(oem, "Hpe");
			if (!fu_redfish_device_probe_oem_hpe(self, hpe, error))
				return FALSE;
		}
		if (oem != NULL && json_object_has_member(oem, "Dell")) {
			JsonObject *json_oem = json_object_get_object_member(oem, "Dell");
			if (!fu_redfish_device_probe_oem_dell(self, json_oem, error))
				return FALSE;
		}
	}

	/* used for quirking and parenting */
	fu_device_build_instance_id_full(dev,
					 FU_DEVICE_INSTANCE_FLAG_QUIRKS,
					 NULL,
					 "REDFISH",
					 "VENDOR",
					 "ID",
					 NULL);

	if (json_object_has_member(member, "Name")) {
		const gchar *tmp = json_object_get_string_member(member, "Name");
		if (tmp != NULL && tmp[0] != '\0')
			fu_redfish_device_set_name(self, tmp);
	}
	if (json_object_has_member(member, "LowestSupportedVersion")) {
		const gchar *tmp = json_object_get_string_member(member, "LowestSupportedVersion");
		if (tmp != NULL && tmp[0] != '\0')
			fu_redfish_device_set_version_lowest(self, tmp);
	}
	if (json_object_has_member(member, "Description")) {
		const gchar *tmp = json_object_get_string_member(member, "Description");
		if (tmp != NULL && tmp[0] != '\0')
			fu_device_set_summary(dev, tmp);
	}

	/* reasons why the device might not be updatable */
	if (json_object_has_member(member, "Updateable")) {
		if (json_object_get_boolean_member(member, "Updateable"))
			fu_device_add_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE);
	}

	/* not useful to export */
	if (fu_device_has_private_flag(dev, FU_REDFISH_DEVICE_FLAG_IS_BACKUP)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "%s is a backup partition",
			    fu_device_get_backend_id(dev));
		return FALSE;
	}

	/* use related items to set extra instance IDs */
	if (fu_device_has_flag(dev, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    json_object_has_member(member, "RelatedItem")) {
		JsonArray *related_item_array = json_object_get_array_member(member, "RelatedItem");
		for (guint i = 0; i < json_array_get_length(related_item_array); i++) {
			JsonObject *related_item;
			related_item = json_array_get_object_element(related_item_array, i);
			if (json_object_has_member(related_item, "@odata.id")) {
				const gchar *id =
				    json_object_get_string_member(related_item, "@odata.id");
				if (!fu_redfish_device_probe_related_item(self, id, error))
					return FALSE;
			}
		}
	}

	/* for Supermicro check whether we have a proper Redfish license installed */
	if (g_strcmp0("SMCI", fu_device_get_vendor(dev)) == 0)
		fu_redfish_device_smc_license_check(self);

	/* success */
	return TRUE;
}

FuRedfishBackend *
fu_redfish_device_get_backend(FuRedfishDevice *self)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	return priv->backend;
}

typedef struct {
	gchar *location;
	gboolean completed;
	GHashTable *messages_seen;
	FuProgress *progress;
} FuRedfishDevicePollCtx;

gboolean
fu_redfish_device_parse_message_id(FuRedfishDevice *self,
				   const gchar *message_id,
				   const gchar *message,
				   FuProgress *progress,
				   GError **error)
{
	/* ignore */
	if (g_pattern_match_simple("TaskEvent.*.TaskProgressChanged", message_id) ||
	    g_pattern_match_simple("TaskEvent.*.TaskCompletedWarning", message_id) ||
	    g_pattern_match_simple("TaskEvent.*.TaskCompletedOK", message_id) ||
	    g_pattern_match_simple("Base.*.Success", message_id))
		return TRUE;

	/* set flags */
	if (g_pattern_match_simple("Base.*.ResetRequired", message_id) ||
	    g_pattern_match_simple("IDRAC.*.JCP001", message_id) ||
	    g_pattern_match_simple("IDRAC.*.RED014", message_id)) {
		fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
		return TRUE;
	}

	/* set error code */
	if (g_pattern_match_simple("Update.*.AwaitToActivate", message_id)) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NEEDS_USER_ACTION, message);
		return FALSE;
	}
	if (g_pattern_match_simple("Update.*.TransferFailed", message_id)) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_WRITE, message);
		return FALSE;
	}
	if (g_pattern_match_simple("Update.*.ActivateFailed", message_id)) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE, message);
		return FALSE;
	}
	if (g_pattern_match_simple("Update.*.VerificationFailed", message_id) ||
	    g_pattern_match_simple("LenovoFirmwareUpdateRegistry.*.UpdateVerifyFailed",
				   message_id)) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE, message);
		return FALSE;
	}
	if (g_pattern_match_simple("Update.*.ApplyFailed", message_id) ||
	    g_pattern_match_simple("iLO.*.UpdateFailed", message_id)) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_WRITE, message);
		return FALSE;
	}

	/* set status */
	if (g_pattern_match_simple("Update.*.TargetDetermined", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_LOADING);
		return TRUE;
	}
	if (g_pattern_match_simple("LenovoFirmwareUpdateRegistry.*.UpdateAssignment", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_LOADING);
		return TRUE;
	}
	if (g_pattern_match_simple("LenovoFirmwareUpdateRegistry.*.PayloadApplyInProgress",
				   message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
		return TRUE;
	}
	if (g_pattern_match_simple("LenovoFirmwareUpdateRegistry.*.PayloadApplyCompleted",
				   message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_IDLE);
		return TRUE;
	}
	if (g_pattern_match_simple("LenovoFirmwareUpdateRegistry.*.UpdateVerifyInProgress",
				   message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_VERIFY);
		return TRUE;
	}
	if (g_pattern_match_simple("Update.*.TransferringToComponent", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_LOADING);
		return TRUE;
	}
	if (g_pattern_match_simple("Update.*.VerifyingAtComponent", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_VERIFY);
		return TRUE;
	}
	if (g_pattern_match_simple("Update.*.UpdateInProgress", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
		return TRUE;
	}
	if (g_pattern_match_simple("Update.*.UpdateSuccessful", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_IDLE);
		return TRUE;
	}
	if (g_pattern_match_simple("Update.*.InstallingOnComponent", message_id)) {
		fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
		return TRUE;
	}

	/* nothing to do */
	return TRUE;
}

static gboolean
fu_redfish_device_poll_task_once(FuRedfishDevice *self, FuRedfishDevicePollCtx *ctx, GError **error)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	JsonObject *json_obj;
	const gchar *message = "Unknown failure";
	const gchar *state_tmp;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(priv->backend);

	/* create URI and poll */
	if (!fu_redfish_request_perform(request,
					ctx->location,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;

	/* percentage is optional */
	json_obj = fu_redfish_request_get_json_object(request);
	if (json_object_has_member(json_obj, "PercentComplete")) {
		gint64 pc = json_object_get_int_member(json_obj, "PercentComplete");
		if (pc >= 0 && pc <= 100)
			fu_progress_set_percentage(ctx->progress, (guint)pc);
	}

	/* print all messages we've not seen yet */
	if (json_object_has_member(json_obj, "Messages")) {
		JsonArray *json_msgs = json_object_get_array_member(json_obj, "Messages");
		guint json_msgs_sz = json_array_get_length(json_msgs);

		for (guint i = 0; i < json_msgs_sz; i++) {
			JsonObject *json_message = json_array_get_object_element(json_msgs, i);
			const gchar *message_id = NULL;
			g_autofree gchar *message_key = NULL;

			/* set additional device properties */
			if (json_object_has_member(json_message, "MessageId"))
				message_id =
				    json_object_get_string_member(json_message, "MessageId");
			if (json_object_has_member(json_message, "Message"))
				message = json_object_get_string_member(json_message, "Message");

			/* ignore messages we've seen before */
			message_key = g_strdup_printf("%s;%s", message_id, message);
			if (g_hash_table_contains(ctx->messages_seen, message_key)) {
				g_debug("ignoring %s", message_key);
				continue;
			}
			g_hash_table_add(ctx->messages_seen, g_steal_pointer(&message_key));

			/* use the message */
			g_debug("message #%u [%s]: %s", i, message_id, message);
			if (!fu_redfish_device_parse_message_id(self,
								message_id,
								message,
								ctx->progress,
								error))
				return FALSE;
		}
	}

	/* use taskstate to set context */
	if (!json_object_has_member(json_obj, "TaskState")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "no TaskState for task manager");
		return FALSE;
	}
	state_tmp = json_object_get_string_member(json_obj, "TaskState");
	g_debug("TaskState now %s", state_tmp);
	if (g_strcmp0(state_tmp, "Completed") == 0 ||
	    fu_device_has_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_NEEDS_REBOOT)) {
		ctx->completed = TRUE;
		return TRUE;
	}
	if (g_strcmp0(state_tmp, "Cancelled") == 0) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "Task was cancelled");
		return FALSE;
	}
	if (g_strcmp0(state_tmp, "Exception") == 0 ||
	    g_strcmp0(state_tmp, "UserIntervention") == 0) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, message);
		return FALSE;
	}

	/* try again */
	return TRUE;
}

static FuRedfishDevicePollCtx *
fu_redfish_device_poll_ctx_new(FuProgress *progress, const gchar *location)
{
	FuRedfishDevicePollCtx *ctx = g_new0(FuRedfishDevicePollCtx, 1);
	ctx->messages_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ctx->location = g_strdup(location);
	ctx->progress = g_object_ref(progress);
	return ctx;
}

static void
fu_redfish_device_poll_ctx_free(FuRedfishDevicePollCtx *ctx)
{
	g_hash_table_unref(ctx->messages_seen);
	g_object_unref(ctx->progress);
	g_free(ctx->location);
	g_free(ctx);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuRedfishDevicePollCtx, fu_redfish_device_poll_ctx_free)
#pragma clang diagnostic pop

gboolean
fu_redfish_device_poll_task(FuRedfishDevice *self,
			    const gchar *location,
			    FuProgress *progress,
			    GError **error)
{
	const guint timeout = 2400;
	g_autoptr(GTimer) timer = g_timer_new();
	g_autoptr(FuRedfishDevicePollCtx) ctx = fu_redfish_device_poll_ctx_new(progress, location);

	/* sleep and then reprobe hardware */
	do {
		fu_device_sleep(FU_DEVICE(self), 1000); /* ms */
		if (!fu_redfish_device_poll_task_once(self, ctx, error))
			return FALSE;
		if (ctx->completed) {
			fu_progress_finished(progress);
			return TRUE;
		}
	} while (g_timer_elapsed(timer, NULL) < timeout);

	/* success */
	g_set_error(error,
		    FWUPD_ERROR,
		    FWUPD_ERROR_INVALID_FILE,
		    "failed to poll %s for success after %u seconds",
		    location,
		    timeout);
	return FALSE;
}

guint
fu_redfish_device_get_reset_pre_delay(FuRedfishDevice *self)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	return priv->reset_pre_delay;
}

guint
fu_redfish_device_get_reset_post_delay(FuRedfishDevice *self)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	return priv->reset_post_delay;
}

static gboolean
fu_redfish_device_set_quirk_kv(FuDevice *device,
			       const gchar *key,
			       const gchar *value,
			       GError **error)
{
	FuRedfishDevice *self = FU_REDFISH_DEVICE(device);
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	guint64 tmp = 0;

	if (g_strcmp0(key, "RedfishResetPreDelay") == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		priv->reset_pre_delay = tmp;
		return TRUE;
	}
	if (g_strcmp0(key, "RedfishResetPostDelay") == 0) {
		if (!fu_strtoull(value, &tmp, 0, G_MAXUINT, FU_INTEGER_BASE_AUTO, error))
			return FALSE;
		priv->reset_post_delay = tmp;
		return TRUE;
	}
	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "quirk key not supported");
	return FALSE;
}

static void
fu_redfish_device_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	FuRedfishDevice *self = FU_REDFISH_DEVICE(object);
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	switch (prop_id) {
	case PROP_BACKEND:
		g_value_set_object(value, priv->backend);
		break;
	case PROP_MEMBER:
		g_value_set_pointer(value, priv->member);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_redfish_device_set_member(FuRedfishDevice *self, JsonObject *member)
{
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	if (priv->member != NULL) {
		json_object_unref(priv->member);
		priv->member = NULL;
	}
	if (member != NULL)
		priv->member = json_object_ref(member);
}

static void
fu_redfish_device_set_property(GObject *object,
			       guint prop_id,
			       const GValue *value,
			       GParamSpec *pspec)
{
	FuRedfishDevice *self = FU_REDFISH_DEVICE(object);
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	switch (prop_id) {
	case PROP_BACKEND:
		g_set_object(&priv->backend, g_value_get_object(value));
		break;
	case PROP_MEMBER:
		fu_redfish_device_set_member(self, g_value_get_pointer(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
fu_redfish_device_init(FuRedfishDevice *self)
{
	fu_device_set_summary(FU_DEVICE(self), "Redfish device");
	fu_device_add_protocol(FU_DEVICE(self), "org.dmtf.redfish");
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_NAME);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_VERFMT);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_ICON);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_VENDOR);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_MD_SET_SIGNED);
	fu_device_register_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_IS_BACKUP);
	fu_device_register_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_UNSIGNED_BUILD);
	fu_device_register_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_WILDCARD_TARGETS);
	fu_device_register_private_flag(FU_DEVICE(self), FU_REDFISH_DEVICE_FLAG_MANAGER_RESET);
	fu_device_register_private_flag(FU_DEVICE(self),
					FU_REDFISH_DEVICE_FLAG_NO_MANAGER_RESET_REQUEST);
}

static void
fu_redfish_device_finalize(GObject *object)
{
	FuRedfishDevice *self = FU_REDFISH_DEVICE(object);
	FuRedfishDevicePrivate *priv = GET_PRIVATE(self);
	if (priv->backend != NULL)
		g_object_unref(priv->backend);
	if (priv->member != NULL)
		json_object_unref(priv->member);
	g_free(priv->build);
	G_OBJECT_CLASS(fu_redfish_device_parent_class)->finalize(object);
}

static void
fu_redfish_device_class_init(FuRedfishDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec;
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);

	object_class->get_property = fu_redfish_device_get_property;
	object_class->set_property = fu_redfish_device_set_property;
	object_class->finalize = fu_redfish_device_finalize;

	device_class->to_string = fu_redfish_device_to_string;
	device_class->probe = fu_redfish_device_probe;
	device_class->set_quirk_kv = fu_redfish_device_set_quirk_kv;

	/**
	 * FuRedfishDevice:backend:
	 *
	 * The backend that added the device.
	 */
	pspec =
	    g_param_spec_object("backend",
				NULL,
				NULL,
				FU_TYPE_BACKEND,
				G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_BACKEND, pspec);

	/**
	 * FuRedfishDevice:member:
	 *
	 * The JSON root member for the device.
	 */
	pspec =
	    g_param_spec_pointer("member",
				 NULL,
				 NULL,
				 G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
	g_object_class_install_property(object_class, PROP_MEMBER, pspec);
}
