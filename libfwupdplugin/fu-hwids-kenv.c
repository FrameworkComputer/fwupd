/*
 * Copyright 2021 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "FuContext"

#include "config.h"

#include "fu-context-private.h"
#include "fu-hwids-private.h"
#include "fu-smbios-struct.h"
#include "fu-string.h"

gboolean
fu_hwids_kenv_setup(FuContext *ctx, FuHwids *self, GError **error)
{
#ifdef HAVE_KENV_H
	struct {
		const gchar *hwid;
		const gchar *key;
	} map[] = {{FU_HWIDS_KEY_BASEBOARD_MANUFACTURER, "smbios.planar.maker"},
		   {FU_HWIDS_KEY_BASEBOARD_PRODUCT, "smbios.planar.product"},
		   {FU_HWIDS_KEY_BIOS_VENDOR, "smbios.bios.vendor"},
		   {FU_HWIDS_KEY_BIOS_VERSION, "smbios.bios.version"},
		   {FU_HWIDS_KEY_FAMILY, "smbios.system.family"},
		   {FU_HWIDS_KEY_MANUFACTURER, "smbios.system.maker"},
		   {FU_HWIDS_KEY_PRODUCT_NAME, "smbios.system.product"},
		   {FU_HWIDS_KEY_PRODUCT_SKU, "smbios.system.sku"},
		   {NULL, NULL}};
	for (guint i = 0; map[i].key != NULL; i++) {
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *value = fu_kenv_get_string(map[i].key, &error_local);
		if (value == NULL) {
			g_debug("ignoring: %s", error_local->message);
			continue;
		}
		fu_hwids_add_value(self, map[i].hwid, value);
	}

	/* FreeBSD provides BIOS major/minor as a combined "major.minor" decimal string */
	{
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *revision = fu_kenv_get_string("smbios.bios.revision",
								&error_local);
		if (revision != NULL) {
			g_auto(GStrv) split = g_strsplit(revision, ".", 2);
			if (g_strv_length(split) == 2) {
				guint64 val = 0;
				if (fu_strtoull(split[0], &val, 0, 0xFF,
						FU_INTEGER_BASE_10, NULL)) {
					g_autofree gchar *tmp = g_strdup_printf("%02x", (guint)val);
					fu_hwids_add_value(self, FU_HWIDS_KEY_BIOS_MAJOR_RELEASE, tmp);
				}
				if (fu_strtoull(split[1], &val, 0, 0xFF,
						FU_INTEGER_BASE_10, NULL)) {
					g_autofree gchar *tmp = g_strdup_printf("%02x", (guint)val);
					fu_hwids_add_value(self, FU_HWIDS_KEY_BIOS_MINOR_RELEASE, tmp);
				}
			}
		}
	}

	/* FreeBSD provides chassis type as a human-readable string, map back to numeric */
	{
		struct {
			const gchar *name;
			FuSmbiosChassisKind kind;
		} chassis_map[] = {
		    {"Other", FU_SMBIOS_CHASSIS_KIND_OTHER},
		    {"Unknown", FU_SMBIOS_CHASSIS_KIND_UNKNOWN},
		    {"Desktop", FU_SMBIOS_CHASSIS_KIND_DESKTOP},
		    {"Low Profile Desktop", FU_SMBIOS_CHASSIS_KIND_LOW_PROFILE_DESKTOP},
		    {"Pizza Box", FU_SMBIOS_CHASSIS_KIND_PIZZA_BOX},
		    {"Mini Tower", FU_SMBIOS_CHASSIS_KIND_MINI_TOWER},
		    {"Tower", FU_SMBIOS_CHASSIS_KIND_TOWER},
		    {"Portable", FU_SMBIOS_CHASSIS_KIND_PORTABLE},
		    {"Laptop", FU_SMBIOS_CHASSIS_KIND_LAPTOP},
		    {"Notebook", FU_SMBIOS_CHASSIS_KIND_NOTEBOOK},
		    {"Hand Held", FU_SMBIOS_CHASSIS_KIND_HAND_HELD},
		    {"Docking Station", FU_SMBIOS_CHASSIS_KIND_DOCKING_STATION},
		    {"All in One", FU_SMBIOS_CHASSIS_KIND_ALL_IN_ONE},
		    {"Sub Notebook", FU_SMBIOS_CHASSIS_KIND_SUB_NOTEBOOK},
		    {"Space-saving", FU_SMBIOS_CHASSIS_KIND_SPACE_SAVING},
		    {"Lunch Box", FU_SMBIOS_CHASSIS_KIND_LUNCH_BOX},
		    {"Main Server Chassis", FU_SMBIOS_CHASSIS_KIND_MAIN_SERVER},
		    {"Expansion Chassis", FU_SMBIOS_CHASSIS_KIND_EXPANSION},
		    {"SubChassis", FU_SMBIOS_CHASSIS_KIND_SUBCHASSIS},
		    {"Bus Expansion Chassis", FU_SMBIOS_CHASSIS_KIND_BUS_EXPANSION},
		    {"Peripheral Chassis", FU_SMBIOS_CHASSIS_KIND_PERIPHERAL},
		    {"RAID Chassis", FU_SMBIOS_CHASSIS_KIND_RAID},
		    {"Rack Mount Chassis", FU_SMBIOS_CHASSIS_KIND_RACK_MOUNT},
		    {"Sealed-case PC", FU_SMBIOS_CHASSIS_KIND_SEALED_CASE_PC},
		    {"Multi-system chassis", FU_SMBIOS_CHASSIS_KIND_MULTI_SYSTEM},
		    {"Compact PCI", FU_SMBIOS_CHASSIS_KIND_COMPACT_PCI},
		    {"Advanced TCA", FU_SMBIOS_CHASSIS_KIND_ADVANCED_TCA},
		    {"Blade", FU_SMBIOS_CHASSIS_KIND_BLADE},
		    {"Blade Enclosure", FU_SMBIOS_CHASSIS_KIND_RESERVED},
		    {"Tablet", FU_SMBIOS_CHASSIS_KIND_TABLET},
		    {"Convertible", FU_SMBIOS_CHASSIS_KIND_CONVERTIBLE},
		    {"Detachable", FU_SMBIOS_CHASSIS_KIND_DETACHABLE},
		    {"IoT Gateway", FU_SMBIOS_CHASSIS_KIND_IOT_GATEWAY},
		    {"Embedded PC", FU_SMBIOS_CHASSIS_KIND_EMBEDDED_PC},
		    {"Mini PC", FU_SMBIOS_CHASSIS_KIND_MINI_PC},
		    {"Stick PC", FU_SMBIOS_CHASSIS_KIND_STICK_PC},
		    {NULL, FU_SMBIOS_CHASSIS_KIND_UNSET}};
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *chassis_str = fu_kenv_get_string("smbios.chassis.type",
								   &error_local);
		if (chassis_str != NULL) {
			for (guint i = 0; chassis_map[i].name != NULL; i++) {
				if (g_strcmp0(chassis_str, chassis_map[i].name) == 0) {
					g_autofree gchar *tmp =
					    g_strdup_printf("%x", chassis_map[i].kind);
					fu_hwids_add_value(self,
							   FU_HWIDS_KEY_ENCLOSURE_KIND,
							   tmp);
					fu_context_set_chassis_kind(ctx, chassis_map[i].kind);
					break;
				}
			}
		}
	}
#endif

	/* success */
	return TRUE;
}
