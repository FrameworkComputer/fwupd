/*
 * Copyright 2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <string.h>

#include "fu-redfish-backend.h"
#include "fu-redfish-common.h"
#include "fu-redfish-hpe-device.h"
#include "fu-redfish-legacy-device.h"
#include "fu-redfish-multipart-device.h"
#include "fu-redfish-request.h"
#include "fu-redfish-smbios.h"
#include "fu-redfish-smc-device.h"

struct _FuRedfishBackend {
	FuBackend parent_instance;
	gchar *hostname;
	gchar *username;
	gchar *password;
	gchar *session_key;
	guint port;
	gchar *vendor;
	gchar *version;
	gchar *uuid;
	gchar *update_uri_path;
	gchar *push_uri_path;
	gboolean use_https;
	gboolean cacheck;
	gboolean wildcard_targets;
	gint64 max_image_size; /* bytes */
	gchar *system_id;
	GType device_gtype;
	GHashTable *request_cache; /* str:GByteArray */
	CURLSH *curlsh;
};

G_DEFINE_TYPE(FuRedfishBackend, fu_redfish_backend, FU_TYPE_BACKEND)

const gchar *
fu_redfish_backend_get_vendor(FuRedfishBackend *self)
{
	return self->vendor;
}

const gchar *
fu_redfish_backend_get_version(FuRedfishBackend *self)
{
	return self->version;
}

const gchar *
fu_redfish_backend_get_uuid(FuRedfishBackend *self)
{
	return self->uuid;
}

FuRedfishRequest *
fu_redfish_backend_request_new(FuRedfishBackend *self)
{
	FuRedfishRequest *request = g_object_new(FU_TYPE_REDFISH_REQUEST, NULL);
	CURL *curl;
	CURLU *uri;
	g_autofree gchar *user_agent = NULL;
	g_autofree gchar *port = g_strdup_printf("%u", self->port);

	/* set the cache location */
	fu_redfish_request_set_cache(request, self->request_cache);
	fu_redfish_request_set_curlsh(request, self->curlsh);

	/* set up defaults */
	curl = fu_redfish_request_get_curl(request);
	uri = fu_redfish_request_get_uri(request);
	(void)curl_url_set(uri, CURLUPART_SCHEME, self->use_https ? "https" : "http", 0);
	(void)curl_url_set(uri, CURLUPART_HOST, self->hostname, 0);
	(void)curl_url_set(uri, CURLUPART_PORT, port, 0);
	(void)curl_easy_setopt(curl, CURLOPT_CURLU, uri);

	/* since DSP0266 makes Basic Authorization a requirement,
	 * it is safe to use Basic Auth for all implementations */
	(void)curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (glong)CURLAUTH_BASIC);
	(void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, (glong)180);
	(void)curl_easy_setopt(curl, CURLOPT_USERNAME, self->username);
	(void)curl_easy_setopt(curl, CURLOPT_PASSWORD, self->password);

	/* setup networking */
	user_agent = g_strdup_printf("%s/%s", PACKAGE_NAME, PACKAGE_VERSION);
	(void)curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
	(void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 60L);
	if (!self->cacheck) {
		(void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		(void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	}

	/* success */
	return request;
}

static gboolean
fu_redfish_backend_coldplug_member(FuRedfishBackend *self, JsonObject *member, GError **error)
{
	g_autoptr(FuDevice) dev = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(GError) error_local = NULL;

	/* create of the correct type */
	dev = g_object_new(self->device_gtype,
			   "context",
			   fu_backend_get_context(FU_BACKEND(self)),
			   "backend",
			   self,
			   "member",
			   member,
			   NULL);

	/* Dell specific currently */
	if (self->system_id != NULL) {
		fu_device_add_instance_str(dev, "SYSTEMID", self->system_id);
		/* Ensure the reboot is not done immediately after installation, and only after a
		 * wanted reboot */
		fu_redfish_multipart_device_set_apply_time(FU_REDFISH_MULTIPART_DEVICE(dev),
							   "OnReset");
	}

	/* some vendors do not specify the Targets array when updating */
	if (self->wildcard_targets)
		fu_device_add_private_flag(dev, FU_REDFISH_DEVICE_FLAG_WILDCARD_TARGETS);

	/* probe + setup */
	locker = fu_device_locker_new(dev, &error_local);
	if (locker == NULL) {
		if (g_error_matches(error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
			g_debug("failed to setup: %s", error_local->message);
			return TRUE;
		}
		g_propagate_error(error, g_steal_pointer(&error_local));
		return FALSE;
	}
	if (self->max_image_size != 0)
		fu_device_set_firmware_size_max(dev, (guint64)self->max_image_size);
	fu_backend_device_added(FU_BACKEND(self), dev);
	return TRUE;
}

static gboolean
fu_redfish_backend_coldplug_collection(FuRedfishBackend *self,
				       JsonObject *collection,
				       GError **error)
{
	JsonArray *members = json_object_get_array_member(collection, "Members");
	for (guint i = 0; i < json_array_get_length(members); i++) {
		JsonObject *json_obj;
		JsonObject *member_id;
		const gchar *member_uri;
		g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);

		member_id = json_array_get_object_element(members, i);
		member_uri = json_object_get_string_member(member_id, "@odata.id");
		if (member_uri == NULL) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_FOUND,
					    "no @odata.id string");
			return FALSE;
		}

		/* create the device for the member */
		if (!fu_redfish_request_perform(request,
						member_uri,
						FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
						error))
			return FALSE;
		json_obj = fu_redfish_request_get_json_object(request);
		if (!fu_redfish_backend_coldplug_member(self, json_obj, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
fu_redfish_backend_coldplug_inventory(FuRedfishBackend *self, JsonObject *inventory, GError **error)
{
	JsonObject *json_obj;
	const gchar *collection_uri;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);

	if (inventory == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no inventory object");
		return FALSE;
	}

	collection_uri = json_object_get_string_member(inventory, "@odata.id");
	if (collection_uri == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no @odata.id string");
		return FALSE;
	}

	if (!fu_redfish_request_perform(request,
					collection_uri,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);
	return fu_redfish_backend_coldplug_collection(self, json_obj, error);
}

static void
fu_redfish_backend_check_wildcard_targets(FuRedfishBackend *self)
{
	g_autoptr(GPtrArray) devices = fu_backend_get_devices(FU_BACKEND(self));
	g_autoptr(GHashTable) device_by_id0 = g_hash_table_new(g_str_hash, g_str_equal);

	/* does the SoftwareId exist from a different device */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device_old;
		FuDevice *device_tmp = g_ptr_array_index(devices, i);
		GPtrArray *ids = fu_device_get_instance_ids(device_tmp);
		const gchar *id0 = g_ptr_array_index(ids, 0);
		device_old = g_hash_table_lookup(device_by_id0, id0);
		if (device_old == NULL) {
			g_hash_table_insert(device_by_id0, (gpointer)device_tmp, (gpointer)id0);
			continue;
		}
		fu_device_add_flag(device_tmp, FWUPD_DEVICE_FLAG_WILDCARD_INSTALL);
		fu_device_add_flag(device_old, FWUPD_DEVICE_FLAG_WILDCARD_INSTALL);
	}
}

static void
fu_redfish_backend_set_session_key(FuRedfishBackend *self, const gchar *session_key)
{
	g_free(self->session_key);
	self->session_key = g_strdup(session_key);
}

static size_t
fu_redfish_backend_session_headers_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	FuRedfishBackend *self = FU_REDFISH_BACKEND(userdata);
	if ((size * nmemb) > 16 && g_ascii_strncasecmp(ptr, "X-Auth-Token:", 13) == 0) {
		g_autofree gchar *session_key = NULL;
		/* The string also includes \r\n at the end */
		session_key = g_strndup(ptr + 14, (size * nmemb) - 16);
		fu_redfish_backend_set_session_key(self, session_key);
	}
	return size * nmemb;
}

gboolean
fu_redfish_backend_create_session(FuRedfishBackend *self, GError **error)
{
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "UserName");
	json_builder_add_string_value(builder, self->username);
	json_builder_set_member_name(builder, "Password");
	json_builder_add_string_value(builder, self->password);
	json_builder_end_object(builder);

	(void)curl_easy_setopt(fu_redfish_request_get_curl(request), CURLOPT_HEADERDATA, self);
	(void)curl_easy_setopt(fu_redfish_request_get_curl(request),
			       CURLOPT_HEADERFUNCTION,
			       fu_redfish_backend_session_headers_callback);

	/* create URI and poll */
	if (!fu_redfish_request_perform_full(request,
					     "/redfish/v1/SessionService/Sessions",
					     "POST",
					     builder,
					     FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					     error))
		return FALSE;
	if (fu_redfish_backend_get_session_key(self) == NULL) {
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "failed to get session key");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_redfish_backend_set_push_uri_path(FuRedfishBackend *self, const gchar *push_uri_path)
{
	g_free(self->push_uri_path);
	self->push_uri_path = g_strdup(push_uri_path);
}

static gboolean
fu_redfish_backend_has_smc_update_path(JsonObject *update_svc)
{
	JsonObject *tmp_obj;
	const gchar *tmp_str;

	if (!json_object_has_member(update_svc, "Actions"))
		return FALSE;
	tmp_obj = json_object_get_object_member(update_svc, "Actions");
	if (tmp_obj == NULL || !json_object_has_member(tmp_obj, "#UpdateService.StartUpdate"))
		return FALSE;
	tmp_obj = json_object_get_object_member(tmp_obj, "#UpdateService.StartUpdate");
	if (tmp_obj == NULL || !json_object_has_member(tmp_obj, "target"))
		return FALSE;
	tmp_str = json_object_get_string_member(tmp_obj, "target");
	return g_str_equal(tmp_str, "/redfish/v1/UpdateService/Actions/UpdateService.StartUpdate");
}

static gboolean
fu_redfish_backend_coldplug(FuBackend *backend, FuProgress *progress, GError **error)
{
	FuRedfishBackend *self = FU_REDFISH_BACKEND(backend);
	JsonObject *json_obj;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);

	/* nothing set */
	if (self->update_uri_path == NULL) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL, "no update_uri_path");
		return FALSE;
	}

	/* get the update service */
	if (!fu_redfish_request_perform(request,
					self->update_uri_path,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);
	if (!json_object_has_member(json_obj, "ServiceEnabled")) {
		if (!json_object_get_boolean_member(json_obj, "ServiceEnabled")) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_NOT_SUPPORTED,
					    "service is not enabled");
			return FALSE;
		}
	}
	if (self->push_uri_path == NULL &&
	    json_object_has_member(json_obj, "MultipartHttpPushUri")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "MultipartHttpPushUri");
		if (tmp != NULL) {
			if (g_strcmp0(self->vendor, "SMCI") == 0 &&
			    fu_redfish_backend_has_smc_update_path(json_obj)) {
				self->device_gtype = FU_TYPE_REDFISH_SMC_DEVICE;
			} else {
				self->device_gtype = FU_TYPE_REDFISH_MULTIPART_DEVICE;
			}
			fu_redfish_backend_set_push_uri_path(self, tmp);
		}
	}
	if (self->push_uri_path == NULL && json_object_has_member(json_obj, "HttpPushUri")) {
		const gchar *tmp = json_object_get_string_member(json_obj, "HttpPushUri");
		if (tmp != NULL) {
			if (self->vendor != NULL && g_str_equal(self->vendor, "HPE"))
				self->device_gtype = FU_TYPE_REDFISH_HPE_DEVICE;
			else
				self->device_gtype = FU_TYPE_REDFISH_LEGACY_DEVICE;
			fu_redfish_backend_set_push_uri_path(self, tmp);
		}
	}
	if (self->push_uri_path == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "HttpPushUri and MultipartHttpPushUri are invalid");
		return FALSE;
	}
	if (json_object_has_member(json_obj, "MaxImageSizeBytes")) {
		self->max_image_size = json_object_get_int_member(json_obj, "MaxImageSizeBytes");
	}
	if (json_object_has_member(json_obj, "FirmwareInventory")) {
		JsonObject *tmp = json_object_get_object_member(json_obj, "FirmwareInventory");
		return fu_redfish_backend_coldplug_inventory(self, tmp, error);
	}
	if (json_object_has_member(json_obj, "SoftwareInventory")) {
		JsonObject *tmp = json_object_get_object_member(json_obj, "SoftwareInventory");
		return fu_redfish_backend_coldplug_inventory(self, tmp, error);
	}

	/* work out if we have multiple devices with the same SoftwareId */
	if (self->wildcard_targets)
		fu_redfish_backend_check_wildcard_targets(self);

	/* success */
	return TRUE;
}

static void
fu_redfish_backend_set_update_uri_path(FuRedfishBackend *self, const gchar *update_uri_path)
{
	/* not changed */
	if (g_strcmp0(self->update_uri_path, update_uri_path) == 0)
		return;

	g_free(self->update_uri_path);
	self->update_uri_path = g_strdup(update_uri_path);
}

static gboolean
fu_redfish_backend_setup_dell_member(FuRedfishBackend *self,
				     const gchar *member_uri,
				     GError **error)
{
	JsonObject *dell_obj;
	JsonObject *dell_system_obj;
	JsonObject *json_obj;
	JsonObject *oem_obj;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);

	if (!fu_redfish_request_perform(request,
					member_uri,
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);
	if (!json_object_has_member(json_obj, "Oem")) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no Oem in Member");
		return FALSE;
	}
	oem_obj = json_object_get_object_member(json_obj, "Oem");
	if (oem_obj == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no valid Oem in Member");
		return FALSE;
	}
	if (!json_object_has_member(oem_obj, "Dell")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no OEM/Dell in Member");
		return FALSE;
	}
	dell_obj = json_object_get_object_member(oem_obj, "Dell");
	if (dell_obj == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no valid OEM/Dell in Member");
		return FALSE;
	}
	if (!json_object_has_member(dell_obj, "DellSystem")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no OEM/Dell/DellSystem in Member");
		return FALSE;
	}
	dell_system_obj = json_object_get_object_member(dell_obj, "DellSystem");
	if (dell_system_obj == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no valid OEM/Dell/DellSystem in Member");
		return FALSE;
	}
	if (!json_object_has_member(dell_system_obj, "SystemID")) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "no OEM/Dell/DellSystem/SystemID in Member");
		return FALSE;
	}

	/* success */
	self->system_id =
	    g_strdup_printf("%04X",
			    (guint16)json_object_get_int_member(dell_system_obj, "SystemID"));
	return TRUE;
}

static gboolean
fu_redfish_backend_setup_dell(FuRedfishBackend *self, GError **error)
{
	JsonObject *member;
	JsonArray *members;
	JsonObject *json_obj;
	const gchar *member_uri;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);

	if (!fu_redfish_request_perform(request,
					"/redfish/v1/Systems",
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);
	if (!json_object_has_member(json_obj, "Members")) {
		g_set_error_literal(error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND, "no Members object");
		return FALSE;
	}
	members = json_object_get_array_member(json_obj, "Members");
	if (json_array_get_length(members) == 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_FOUND,
				    "empty Members array");
		return FALSE;
	}
	member = json_array_get_object_element(members, 0);
	member_uri = json_object_get_string_member(member, "@odata.id");
	return fu_redfish_backend_setup_dell_member(self, member_uri, error);
}

static gboolean
fu_redfish_backend_setup(FuBackend *backend,
			 FuBackendSetupFlags flags,
			 FuProgress *progress,
			 GError **error)
{
	FuRedfishBackend *self = FU_REDFISH_BACKEND(backend);
	JsonObject *json_obj;
	JsonObject *json_update_service = NULL;
	const gchar *data_id;
	const gchar *version = NULL;
	g_autoptr(FuRedfishRequest) request = fu_redfish_backend_request_new(self);

	/* sanity check */
	if (self->port == 0 || self->port > G_MAXUINT16) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "invalid port specified: 0x%x",
			    self->port);
		return FALSE;
	}

	/* try to connect */
	if (!fu_redfish_request_perform(request,
					"/redfish/v1/",
					FU_REDFISH_REQUEST_PERFORM_FLAG_LOAD_JSON,
					error))
		return FALSE;
	json_obj = fu_redfish_request_get_json_object(request);
	if (json_object_has_member(json_obj, "ServiceVersion")) {
		version = json_object_get_string_member(json_obj, "ServiceVersion");
	} else if (json_object_has_member(json_obj, "RedfishVersion")) {
		version = json_object_get_string_member(json_obj, "RedfishVersion");
	}
	if (version != NULL) {
		g_free(self->version);
		self->version = g_strdup(version);
	}
	if (json_object_has_member(json_obj, "UUID")) {
		g_free(self->uuid);
		self->uuid = g_strdup(json_object_get_string_member(json_obj, "UUID"));
	}
	if (json_object_has_member(json_obj, "Vendor")) {
		g_free(self->vendor);
		self->vendor = g_strdup(json_object_get_string_member(json_obj, "Vendor"));
	}
	if (g_strcmp0(self->vendor, "Dell") == 0) {
		if (!fu_redfish_backend_setup_dell(self, error))
			return FALSE;
	}
	if (json_object_has_member(json_obj, "UpdateService"))
		json_update_service = json_object_get_object_member(json_obj, "UpdateService");
	if (json_update_service == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_NOT_SUPPORTED,
				    "no UpdateService object");
		return FALSE;
	}
	data_id = json_object_get_string_member(json_update_service, "@odata.id");
	if (data_id == NULL) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "no @odata.id string");
		return FALSE;
	}
	fu_redfish_backend_set_update_uri_path(self, data_id);
	return TRUE;
}

static void
fu_redfish_backend_invalidate(FuBackend *backend)
{
	FuRedfishBackend *self = FU_REDFISH_BACKEND(backend);
	g_hash_table_remove_all(self->request_cache);
}

void
fu_redfish_backend_set_hostname(FuRedfishBackend *self, const gchar *hostname)
{
	g_free(self->hostname);
	self->hostname = g_strdup(hostname);
}

void
fu_redfish_backend_set_port(FuRedfishBackend *self, guint port)
{
	self->port = port;
}

void
fu_redfish_backend_set_https(FuRedfishBackend *self, gboolean use_https)
{
	self->use_https = use_https;
}

void
fu_redfish_backend_set_cacheck(FuRedfishBackend *self, gboolean cacheck)
{
	self->cacheck = cacheck;
}

void
fu_redfish_backend_set_wildcard_targets(FuRedfishBackend *self, gboolean wildcard_targets)
{
	self->wildcard_targets = wildcard_targets;
}

void
fu_redfish_backend_set_username(FuRedfishBackend *self, const gchar *username)
{
	g_free(self->username);
	self->username = g_strdup(username);
}

const gchar *
fu_redfish_backend_get_username(FuRedfishBackend *self)
{
	return self->username;
}

void
fu_redfish_backend_set_password(FuRedfishBackend *self, const gchar *password)
{
	g_free(self->password);
	self->password = g_strdup(password);
}

const gchar *
fu_redfish_backend_get_push_uri_path(FuRedfishBackend *self)
{
	return self->push_uri_path;
}

const gchar *
fu_redfish_backend_get_session_key(FuRedfishBackend *self)
{
	return self->session_key;
}

static void
fu_redfish_backend_to_string(FuBackend *backend, guint idt, GString *str)
{
	FuRedfishBackend *self = FU_REDFISH_BACKEND(backend);
	fwupd_codec_string_append(str, idt, "Hostname", self->hostname);
	fwupd_codec_string_append(str, idt, "Username", self->username);
	fwupd_codec_string_append_bool(str, idt, "Password", self->password != NULL);
	fwupd_codec_string_append(str, idt, "SessionKey", self->session_key);
	fwupd_codec_string_append_int(str, idt, "Port", self->port);
	fwupd_codec_string_append(str, idt, "UpdateUriPath", self->update_uri_path);
	fwupd_codec_string_append(str, idt, "PushUriPath", self->push_uri_path);
	fwupd_codec_string_append_bool(str, idt, "UseHttps", self->use_https);
	fwupd_codec_string_append_bool(str, idt, "Cacheck", self->cacheck);
	fwupd_codec_string_append_bool(str, idt, "WildcardTargets", self->wildcard_targets);
	fwupd_codec_string_append_hex(str, idt, "MaxImageSize", self->max_image_size);
	fwupd_codec_string_append(str, idt, "SystemId", self->system_id);
	fwupd_codec_string_append(str, idt, "DeviceGType", g_type_name(self->device_gtype));
}

static void
fu_redfish_backend_finalize(GObject *object)
{
	FuRedfishBackend *self = FU_REDFISH_BACKEND(object);
	g_hash_table_unref(self->request_cache);
	curl_share_cleanup(self->curlsh);
	g_free(self->update_uri_path);
	g_free(self->push_uri_path);
	g_free(self->hostname);
	g_free(self->username);
	g_free(self->password);
	g_free(self->session_key);
	g_free(self->vendor);
	g_free(self->version);
	g_free(self->uuid);
	g_free(self->system_id);
	G_OBJECT_CLASS(fu_redfish_backend_parent_class)->finalize(object);
}

static void
fu_redfish_backend_class_init(FuRedfishBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	FuBackendClass *backend_class = FU_BACKEND_CLASS(klass);
	backend_class->coldplug = fu_redfish_backend_coldplug;
	backend_class->setup = fu_redfish_backend_setup;
	backend_class->invalidate = fu_redfish_backend_invalidate;
	backend_class->to_string = fu_redfish_backend_to_string;
	object_class->finalize = fu_redfish_backend_finalize;
}

static void
fu_redfish_backend_init(FuRedfishBackend *self)
{
	self->use_https = TRUE;
	self->device_gtype = FU_TYPE_REDFISH_DEVICE;
	self->request_cache = g_hash_table_new_full(g_str_hash,
						    g_str_equal,
						    g_free,
						    (GDestroyNotify)g_byte_array_unref);
	self->curlsh = curl_share_init();
	curl_share_setopt(self->curlsh, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
	curl_share_setopt(self->curlsh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	curl_share_setopt(self->curlsh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
}

FuRedfishBackend *
fu_redfish_backend_new(FuContext *ctx)
{
	return FU_REDFISH_BACKEND(g_object_new(FU_REDFISH_TYPE_BACKEND,
					       "name",
					       "redfish",
					       "can-invalidate",
					       TRUE,
					       "context",
					       ctx,
					       NULL));
}
