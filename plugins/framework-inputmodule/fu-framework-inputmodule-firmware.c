/*
 * Copyright 2026 Framework Computer Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "fu-framework-inputmodule-firmware.h"
#include "fu-framework-inputmodule-struct.h"

struct _FuFrameworkInputmoduleFirmware {
	FuFirmware parent_instance;
};

G_DEFINE_TYPE(FuFrameworkInputmoduleFirmware, fu_framework_inputmodule_firmware, FU_TYPE_FIRMWARE)

static gboolean
fu_framework_inputmodule_firmware_parse(FuFirmware *firmware,
					FuInputStream *stream,
					FuFirmwareParseFlags flags,
					GError **error)
{
	gsize streamsz = 0;
	guint32 addr_next = 0x10000000;

	if (!fu_input_stream_size(stream, &streamsz, error))
		return FALSE;
	if (streamsz == 0 || streamsz % FU_STRUCT_FRAMEWORK_INPUTMODULE_UF2_SIZE != 0) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "UF2 image must contain complete 512-byte blocks");
		return FALSE;
	}
	for (gsize offset = 0; offset < streamsz;
	     offset += FU_STRUCT_FRAMEWORK_INPUTMODULE_UF2_SIZE) {
		g_autoptr(FuStructFrameworkInputmoduleUf2) st = NULL;
		guint32 addr;
		st = fu_struct_framework_inputmodule_uf2_parse_stream(stream, offset, error);
		if (st == NULL)
			return FALSE;
		addr = fu_struct_framework_inputmodule_uf2_get_target_addr(st);
		if (fu_struct_framework_inputmodule_uf2_get_block_no(st) !=
			offset / FU_STRUCT_FRAMEWORK_INPUTMODULE_UF2_SIZE ||
		    fu_struct_framework_inputmodule_uf2_get_num_blocks(st) !=
			streamsz / FU_STRUCT_FRAMEWORK_INPUTMODULE_UF2_SIZE) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_FILE,
					    "UF2 block numbering invalid");
			return FALSE;
		}
		/* RP2040 erases whole 4KiB sectors; never reach the serial sector at 0xff000.
		 * Require contiguous 256-byte flash pages starting with boot2. */
		if (addr != addr_next || addr >= 0x100ff000) {
			g_set_error(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INVALID_FILE,
				    "UF2 target address 0x%08x invalid, expected 0x%08x below "
				    "serial sector",
				    addr,
				    addr_next);
			return FALSE;
		}
		addr_next += 256;
	}
	/* UF2 bootloader needs the original container, not concatenated payloads. */
	return fu_firmware_set_stream(firmware, stream, error);
}

static void
fu_framework_inputmodule_firmware_init(FuFrameworkInputmoduleFirmware *self)
{
}

static void
fu_framework_inputmodule_firmware_class_init(FuFrameworkInputmoduleFirmwareClass *klass)
{
	FuFirmwareClass *firmware_class = FU_FIRMWARE_CLASS(klass);
	firmware_class->parse = fu_framework_inputmodule_firmware_parse;
	fu_firmware_set_size_max(firmware_class, 0xff000 * 2);
}
