/*
 * Copyright 2025 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fu-backend.h"

#define FU_TYPE_HIDRAW_BACKEND (fu_hidraw_backend_get_type())
G_DECLARE_FINAL_TYPE(FuHidrawBackend, fu_hidraw_backend, FU, HIDRAW_BACKEND, FuBackend)

FuBackend *
fu_hidraw_backend_new(FuContext *ctx) G_GNUC_NON_NULL(1);
