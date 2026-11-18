[Defines]
  # Timers are provided by the ARM generic timer running at 19.2 MHz.
  USE_PHYSICAL_TIMER = 1

!include SamsungPkg/SamsungPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv8.2-a+crypto+rcpc

[PcdsFixedAtBuild]
  #
  # ACPI
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemId|"HISI  "
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x686372616C6F7474
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00009010
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorId|0x53494849
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultCreatorRevision|0x00000001

  #
  # CPU
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|12
  gArmPlatformTokenSpaceGuid.PcdClusterCount|3

  #
  # GIC
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0xFF000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0xFF040000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Kirin 9010"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"Kirin9010"

[LibraryClasses]
  #
  # Memory Libraries
  #
  # The Samsung implementation reads a Samsung specific DRAM size register.
  # Kirin 9010 uses the null implementation until a charlotte variant exists.
  #
  RamManagerLib|SiliciumNullPkg/Library/RamManagerLibNull/RamManagerLibNull.inf

  #
  # SEC Libraries
  #
  PlatformSecLib|CharlottePkg/Library/PlatformSecLib/PlatformSecLib.inf
