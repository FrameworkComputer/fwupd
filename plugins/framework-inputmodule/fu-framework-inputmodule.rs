// Copyright 2026 Framework Computer Inc
// SPDX-License-Identifier: LGPL-2.1-or-later

#[derive(New, ParseStream, Default)]
#[repr(C, packed)]
struct FuStructFrameworkInputmoduleUf2 {
    magic0: u32le == 0x0A324655,
    magic1: u32le == 0x9E5D5157,
    flags: u32le == 0x00002000,
    target_addr: u32le,
    payload_size: u32le == 256,
    block_no: u32le,
    num_blocks: u32le,
    family_id: u32le == 0xE48BFF56,
    data: [u8; 476],
    magic_end: u32le == 0x0AB16F30,
}
