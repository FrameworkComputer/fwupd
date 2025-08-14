---
title: Plugin: Ilitek Touch
---

## Introduction

This plugin supports the Touchscreen ICs from Ilitek.

Currently it can only report the firmware version, not update.

## External Interface Access

This plugin requires read/write access to `/dev/hidraw*`.

## Version Considerations

This plugin has been available since fwupd version `2.0.14`.

## Vendor ID Security

The vendor ID is set from the HID vendor ID.

## Owners

Anyone can submit a pull request to modify this plugin, but the following people should be
consulted before making major or functional changes:

* Daniel Schaefer: @JohnAZoidberg
