/** @file
  Register the Boot Manager Menu as the first boot option.

  The stock BDS only enters the Boot Manager when the volume-up combo is
  pressed, which needs a working keypad driver. Until the PMIC keypad is
  wired up, register the menu unconditionally so the UI is reachable.

  Copyright (c) 2026, Silicium. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

EFI_STATUS
EFIAPI
BootMenuEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;

  ZeroMem (&BootOption, sizeof (BootOption));

  Status = EfiBootManagerGetBootManagerMenu (&BootOption);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: Failed to get Boot Manager Menu: %r\n", __FUNCTION__, Status));
    return EFI_SUCCESS;
  }

  Status = EfiBootManagerAddLoadOptionVariable (&BootOption, 0);
  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "%a: Boot Manager Menu registered in BootOrder: %r\n",
    __FUNCTION__,
    Status
    ));

  EfiBootManagerFreeLoadOption (&BootOption);

  return EFI_SUCCESS;
}
