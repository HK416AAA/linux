##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = mro
  PLATFORM_GUID                  = 6A0E2C11-9B7D-4E58-9C3A-1F27D6B4A8E5
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/mroPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = mroPkg/mro.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0
  # SamsungPkg's errata library needs an explicit value for this macro
  HAS_CNTFRQ_EL0_EMPTY_ERRATA    = 0

!include CharlottePkg/CharlottePkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x00400000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF00000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Huawei"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"MatePad"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"mro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"charlotte"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Kirin 9010"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|2800
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1840
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

[LibraryClasses]
  #
  # Debug output
  #
  # Mirror DEBUG() output into the ramoops console zone so it survives a
  # reset and can be read from /sys/fs/pstore/console-ramoops-0.
  #
  SerialPortLib|mroPkg/Library/PstoreSerialPortLib/PstoreSerialPortLib.inf

  #
  # Memory Libraries
  #
  MemoryMapLib|mroPkg/Library/MemoryMapLib/MemoryMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  #mro/AcpiTables.inf

  #
  # Boot Manager
  #
  mroPkg/Drivers/BootMenuDxe/BootMenuDxe.inf

  #
  # Input
  #
  #SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  #SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
