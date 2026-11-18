/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/IoLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/MemoryMapHelperLib.h>

#include "PlatformRegisters.h"

VOID
EnableFrameBufferWrites ()
{
  //
  // Kirin 9010 keeps the bootloader's scanout enabled across the firmware
  // handoff, so no display controller re-trigger is required here.  The
  // reserved scanout buffer is described by the MemoryMapLib region table.
  //
}

VOID
PlatformInitialize ()
{
  // Enable Frame Buffer Writes
  EnableFrameBufferWrites ();
}
