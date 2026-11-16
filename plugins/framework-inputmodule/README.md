---
title: Plugin: Framework Input Module
---

## Introduction

This plugin supports the Framework Laptop 16 LED Matrix Input Module (USB `32ac:0020`),
including shipped firmware version 0.2.0. No bootstrap firmware update is required.

The runtime exposes CDC ACM. The plugin claims the CDC interfaces, asserts DTR/RTS,
and sends `32 ac 02 00` on the data OUT endpoint to enter the RP2040 ROM bootloader.
The UF2 plugin writes the image through the bootloader's mass-storage interface
(`2e8a:0003`) and waits for the runtime to return.

## Firmware Format

The cabinet contains the production `ledmatrix.uf2` image. Before detaching, the
plugin checks RP2040 family ID `e48bff56`, block numbering, and contiguous 256-byte
flash pages starting at `10000000`. Writes must end before `100ff000`, where the
manufacturing serial-number sector starts. The original UF2 bytes are preserved
for the UF2 plugin to write. Other hardware and development firmware variants
must not be published under the production LED matrix GUID.

The protocol ID is `com.microsoft.uf2`. Firmware is not authenticated by the
RP2040 ROM; LVFS cabinet signing provides host-side distribution authentication.

## GUID Generation

The update target is the runtime instance ID `USB\VID_32AC&PID_0020`, with GUID
`edea9941-7727-5ec7-b1f7-c0aedfe32387`.
Do not target the generic RP2040 bootloader or UF2 family GUID with LED matrix releases.
The bootloader counterpart is `BLOCK\VEN_2E8A&DEV_0003`. The runtime physical ID
uses the USB device path, matching the block-device backend so replug matching
can prefer the same USB port before falling back to the counterpart GUID.

## Version Considerations

The USB device release is encoded as `(major << 8) | (minor << 4) | patch`.
The plugin converts this explicitly to a triplet, so `0020` is `0.2.0`.
The release field represents firmware version, not hardware revision.

## External Interface Access

Runtime access requires read/write access to `/dev/bus/usb`. The CDC kernel driver
is temporarily detached while the interfaces are claimed, and restored on close
when the device is not rebooting. The UF2 plugin additionally requires access to
mount and write the mass-storage volume.

## Validation

Run discovery using `fwupdtool --plugins framework_inputmodule --plugins uf2 get-devices`.
For a local update use `fwupdtool --plugins framework_inputmodule --plugins uf2 write-blob
ledmatrix.uf2 DEVICE-ID`.

Hardware validation should start from shipped 0.2.0, check that the updated version
returns, and compare serial numbers before and after. Repeat with two matrices
and an unrelated RP2040 connected to verify correct replug selection. Recovery
from interrupted flashing uses the RP2040 BOOTSEL procedure.

## LVFS Packaging

Use the runtime GUID above in the metainfo `provides/firmware` element, with
`type="flashed"`. Release metadata should use triplet version strings and the
production `ledmatrix.uf2` payload. Build a local cabinet using:

```sh
fwupdtool build-cabinet ledmatrix.cab ledmatrix.uf2 firmware.metainfo.xml
```

LVFS publication additionally requires the vendor account, release notes,
licensing, and the reviewed production firmware release. The plugin does not
require device-side changes to update units running 0.2.0.
