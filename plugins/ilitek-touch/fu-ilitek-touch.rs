// Copyright 2025 Framework Computer Inc
// SPDX-License-Identifier: LGPL-2.1-or-later

// TODO
#[derive(New, Default)]
#[repr(C, packed)]
struct FuStructIlitekTouchRequest {
    report_id: u8 = 0x03,
    b1: u8 = 0xA3,
    data_len: u8,
    read_len: u8,
    message_id: u8,
    data: [u8; 35],
}

// TODO this might be different on different ICs
enum FuIlitekTouchStylusReportId {
    IdFirmware          = 0x27,
    UsiVersion          = 0x28,
}

// TODO
#[derive(New, Default)]
#[repr(C, packed)]
struct FuStructIlitekTouchStylusRequest {
    report_id: u8,
    reserved: [u8; 40],
}

