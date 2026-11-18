# Kirin 9010 (charlotte) UEFI

A Mu-Silicium based UEFI firmware port for the Kirin 9010 / charlotte platform,
vendored from the Project Silicium reference tree (`uefi-sheng`) and adapted to
this device.

The firmware boots from the `kernel` partition through `BootShim`, relocates the
FD to `0x9FC00000`, and hands control to the generic Silicium DXE core. Display
output uses `SimpleFbDxe`, which reuses the scanout buffer left running by the
bootloader (`0x34380000`, 1840x2800, 32bpp, `x8b8g8r8`).

## Layout

```
uefi/
  build_uefi.py                  Build driver (device config + FD + boot image)
  build.sh                       Fetches Mu/EDK2 dependencies and builds
  BootShim/                      Relocation shim prepended to the FD
  Common/                        Mu and Mu_OEM_Sample (cloned by build.sh)
  Mu_Basecore/                   EDK2 base (cloned by build.sh)
  Platforms/Hisilicon/mroPkg/
    mro.dsc / mro.fdf            Platform description and flash layout
    DeviceBuild.py               Stuart build configuration
    Include/APRIORI.inc          DXE dispatch order
    Include/DXE.inc              DXE module list
    Library/MemoryMapLib/        DRAM / MMIO region table
  Resources/Configs/mro.toml     FD base/size and boot image settings
  Silicon/Hisilicon/CharlottePkg/
    CharlottePkg.dsc.inc         SoC PCDs (GIC, CPU, SMBIOS, timer)
    Library/PlatformSecLib/      SEC-time platform hook
  Silicon/Silicium/SiliciumPkg/  Generic Silicium drivers (SimpleFb, SMBIOS, ...)
```

## Build

```bash
cd uefi
./build.sh            # DEBUG
./build.sh RELEASE    # RELEASE
```

`build.sh` clones `Mu_Basecore`, `Common/Mu`, `Common/Mu_OEM_Sample`,
`Silicium-ACPI` and OpenSSL at the pinned reference commits, then produces
`Mu-mro.bin`, the flat "Image" style payload (boot shim + firmware FD).

CI runs the same flow on every push to `261118-feat-kirin9010-uefi`
(`.github/workflows/build-kirin9010-uefi.yml`).

## Pack and flash

```bash
./pack_boot.sh
```

This copies the payload to `Image` and wraps it into `Mu-mro-boot.img` with
`../tools/charlotte/mkbootimg.py` (page size 2048, kernel at `0x10008000`,
header version 0):

```bash
fastboot flash boot Mu-mro-boot.img
```

`Mu-mro.bin` and `Image` are the same flat payload, so either can also be
placed in a boot image by other tooling.

## Register map assumptions

| Region     | Address      | Size     |
| ---------- | ------------ | -------- |
| UEFI FD    | `0x9FC00000` | `0x300000` |
| UEFI Stack | `0x9FF00000` | `0x40000` |
| DXE Heap   | `0xA0000000` | `0x10000000` |
| Scanout    | `0x34380000` | `0x13AA000` |
| GICD       | `0xFF000000` | `0x10000` |
| GICR       | `0xFF040000` | `0x180000` |
| UART6      | `0xFFB82000` | `0x1000` |
| Ramoops    | `0x2FC40000` | `0x100000` |

The GIC values come from the device tree (`interrupt-controller@ff000000`).
The scanout buffer comes from the `framebuffer@34380000` node of the Linux
device tree (`simple-framebuffer`, stride `7360` = 1840 x 4, format
`x8b8g8r8`, i.e. bytes in memory are B, G, R, X). The earlier
`0x53458000` / 2800x1840 / 11200 values were wrong: the DPU never scanned
that page, so every GOP blit stayed invisible.

## Firmware log (pstore)

`SerialPortLib` is mapped to `PstoreSerialPortLib`, so every `DEBUG()`
message is appended to the ramoops console zone at `0x2FCA0000` while the
firmware runs. The zone survives a watchdog reset, so after the device
reboots into Linux:

```bash
cat /sys/fs/pstore/console-ramoops-0
```

The kernel consumes the zone on boot (`PRZ_FLAG_ZAP_OLD`), which keeps the
previous boot's text available as the first pstore record.

Zone layout comes from the device tree (`ramoops@2fc40000`): three
`0x20000` dump records, then the `0x80000` console zone, then the `0x20000`
pmsg zone. Each zone starts with `struct persistent_ram_buffer`
(`"DBGC"` signature, start offset, payload size).

## User interface

`PcdBootManagerInBootOrder` is `TRUE`, so `EfiBootManagerGetBootManagerMenu`
appends `BootManagerMenuApp` to `BootOrder` while BDS initialises. The menu
opens on every boot and renders through `GraphicsConsoleDxe` on the
bootloader framebuffer. A separate driver is not needed: the one that used to
do this registered an EndOfDxe hook, but `UefiBootManagerLib` pulls in
`UefiHiiServicesLib`, whose library depex (`PcdProtocol`, `HiiString`,
`HiiDatabase`, `HiiConfigRouting`) is ANDed onto the module depex, so the
image was loaded but never dispatched.

Keypad input is not wired yet: charlotte volume keys are mediated by the
PMIC (`powerkey` node with `hisilicon,volume-keys`), so the GPIO based
`KeypadDeviceLib` from the reference platforms does not apply.

## Open items

- `RamManagerLib` is currently Samsung's implementation; a charlotte version is
  required to publish the correct runtime memory partitions.
- The bootloader keeps the DPU scanning out the logo buffer, so no DPU/DSI
  bring-up is needed in firmware; `EnableFrameBufferWrites` in SEC stays a
  no-op.
- UFS, keypad (PMIC side keys) and the UniXNU payload are not wired yet.
- The boot image carries no ramdisk; the firmware runs entirely from
  `Mu-mro.bin`.
