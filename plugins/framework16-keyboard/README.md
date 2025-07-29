---
title: Plugin: Framework Computer Inputmodule
---

## Introduction

TODO

Devices are updated by triggering a reset, and then entering the Raspberry Pi Pico bootrom.

The bootrom speaks the UF2 protocol.

## External Interface Access

This plugin requires read/write access to `/dev/hidraw*`.

## Version Considerations

This plugin has been available since fwupd version `2.0.2`.

## Owners

Anyone can submit a pull request to modify this plugin, but the following people should be
consulted before making major or functional changes:

* Daniel Schaefer: @JohnAZoidberg
