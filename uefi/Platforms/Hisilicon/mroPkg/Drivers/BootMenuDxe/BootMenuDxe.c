/** @file
  Register the Boot Manager Menu as the first boot option.

  The stock BDS only enters the Boot Manager when the volume-up combo is
  pressed, which needs a working keypad driver. Until the PMIC keypad is
  wired up, register the menu unconditionally so the UI is reachable.

  Registration is deferred to the EndOfDxe event because the boot option
  needs the variable services and the FV2 protocol, and because it has to
  land before BDS processes BootOrder.

  Copyright (c) 2026, Silicium. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

STATIC EFI_EVENT  mEndOfDxeEvent = NULL;

STATIC
VOID
EFIAPI
RegisterBootManagerMenu (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;

  ZeroMem (&BootOption, sizeof (BootOption));

  Status = EfiBootManagerGetBootManagerMenu (&BootOption);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to get Boot Manager Menu: %r\n", __FUNCTION__, Status));
    return;
  }

  Status = EfiBootManagerAddLoadOptionVariable (&BootOption, 0);
  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO,
    "%a: Boot Manager Menu registered at BootOrder[0]: %r\n",
    __FUNCTION__,
    Status
    ));

  EfiBootManagerFreeLoadOption (&BootOption);
}

EFI_STATUS
EFIAPI
BootMenuEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  RegisterBootManagerMenu,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &mEndOfDxeEvent
                  );
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INFO, "%a: EndOfDxe hook armed: %r\n", __FUNCTION__, Status));

  return EFI_SUCCESS;
}
