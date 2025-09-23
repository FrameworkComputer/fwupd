// Copyright 2024 Richard Hughes <richard@hughsie.com>
// SPDX-License-Identifier: LGPL-2.1-or-later

#[derive(FromString, ToString)]
enum FuParadeLspconDeviceKind {
    Unknown,
    Ps175,
    Ps185,
}

enum FuParadeLspconPage1Addr {
    SafeMode        = 0x00, // On PS883X
}

enum FuParadeLspconPage1Addr {
    Dpcd            = 0xC0, // page1, DPCD
    LowPower        = 0x70, // On PS883X
}

enum FuParadeLspconPage2Addr {
    PowerCtrl       = 0x80, // On PS883X
    Spicfg3         = 0x82,
    FlashAddrLo     = 0x8E, // 24-bit flash address that gets mapped into page 7. On PS883X
    FlashAddrHi     = 0x8F, // writing = 0x01,0x42 will map the 256 bytes from 0x420100 into page 7. On PS883X
    WrFifo          = 0x90, // 16-deep SPI write and read buffer FIFOs. Same on PS883X
    RdFifo          = 0x91, // Same on PS883X
    SpiLen          = 0x92, // low nibble is write operation length, high nibble is read commands. Same on PS883X
    SpiCtl          = 0x93,
    SpiStatus       = 0x9E, // 1=operation-begins, 2=cmd-sent, 0=complete. Same on PS883X
    Iocfg1          = 0xA6,
    IRomCtrl        = 0xB0,
    WrProtect       = 0xB3, // permit flash write operations
    Mpu             = 0xBC, // MPU control register
    SpiMap          = 0xBD, // On PS883X
    MapWrite        = 0xDA, // write a magic to enable writes to page 7
    MpuPs883x       = 0xD6, // On PS883X
    PowerCtrlUnlock = 0xF0, // On PS883X
    RomWpCfg        = 0xF6,
}

enum FuParadeLspconPage5Addr {
    ActivePartition = 0x0E,
}

enum FuParadeLspconPage9Addr {
    FirmwareVersion = 0x01, // On PS883X
    BootBankId      = 0x06, // On PS883X
}

// device registers are split into pages, where each page has its own I2C address
enum FuParadeLspconI2cAddr {
    Page1 = 0x09,
    Page2 = 0x0A,
    Page3 = 0x0B,
    Page4 = 0x0C,
    Page5 = 0x0D,
    Page6 = 0x0E,
    Page7 = 0x0F,
}
