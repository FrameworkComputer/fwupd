/*
 * Copyright 2016 Richard Hughes <richard@hughsie.com>
 * Copyright 2020 boger wang <boger@goodix.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

/* protocol */
#define FU_GOODIX_MOC_CMD_ACK	       0xAA
#define FU_GOODIX_MOC_CMD_VERSION      0xD0
#define FU_GOODIX_MOC_CMD_RESET	       0xB4
#define FU_GOODIX_MOC_CMD_UPGRADE      0x80
#define FU_GOODIX_MOC_CMD_UPGRADE_INIT 0x00
#define FU_GOODIX_MOC_CMD_UPGRADE_DATA 0x01
#define FU_GOODIX_MOC_CMD1_DEFAULT     0x00

#define GX_SIZE_CRC32 4

/* type covert */
#define MAKE_CMD_EX(cmd0, cmd1) ((guint16)(((cmd0) << 8) | (cmd1)))

typedef struct {
	guint8 format[2];
	guint8 fwtype[8];
	guint8 fwversion[8];
	guint8 customer[8];
	guint8 mcu[8];
	guint8 sensor[8];
	guint8 algversion[8];
	guint8 interface[8];
	guint8 protocol[8];
	guint8 flashVersion[8];
	guint8 reserved[62];
} FuGoodixMocVersionInfo;

typedef struct {
	guint8 cmd;
	gboolean configured;
} FuGoodixMocAckMsg;

typedef struct {
	guint8 result;
	union {
		FuGoodixMocAckMsg ack_msg;
		FuGoodixMocVersionInfo version_info;
	};
} FuGoodixMocCmdResp;

typedef enum {
	GX_PKG_TYPE_NORMAL = 0x80,
	GX_PKG_TYPE_EOP = 0,
} FuGoodixMocPkgType;

typedef struct __attribute__((__packed__)) { /* nocheck:blocked */
	guint8 cmd0;
	guint8 cmd1;
	guint8 pkg_flag;
	guint8 reserved;
	guint16 len;
	guint8 crc8;
	guint8 rev_crc8;
} FuGoodixMocPkgHeader;
