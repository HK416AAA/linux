/** @file
  SerialPortLib implementation that persists UEFI debug output to the
  Linux ramoops console zone, so it survives a reset and can be read back
  from /sys/fs/pstore/console-ramoops-0.

  Layout taken from arch/arm64/boot/dts/hisilicon/charlotte.dtsi:

    ramoops@2fc40000 {
      record-size  = 0x20000;
      console-size = 0x80000;
      pmsg-size    = 0x20000;
    };

  The kernel consumes the dump area first, then the console zone, then the
  pmsg zone, so the zones live at:

    0x2FC40000  dmesg   3 * 0x20000
    0x2FCA0000  console 0x80000   <- this library
    0x2FD20000  pmsg    0x20000

  Every zone starts with "struct persistent_ram_buffer" (signature, start
  offset, payload size) followed by the payload, which the kernel treats as
  a ring buffer.

  Copyright (c) 2026, Silicium. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/SerialPortLib.h>

#define PSTORE_SIGNATURE      0x43474244U // "DBGC"
#define PSTORE_CONSOLE_BASE   0x2FCA0000U
#define PSTORE_CONSOLE_SIZE   0x00080000U
#define PSTORE_HEADER_SIZE    12U
#define PSTORE_DATA_SIZE      (PSTORE_CONSOLE_SIZE - PSTORE_HEADER_SIZE)

typedef struct {
  volatile UINT32    Signature;
  volatile INT32     Start;
  volatile INT32     Size;
  volatile UINT8     Data[PSTORE_DATA_SIZE];
} PSTORE_CONSOLE_RING;

STATIC volatile PSTORE_CONSOLE_RING  *mRing = (volatile PSTORE_CONSOLE_RING *)(UINTN)PSTORE_CONSOLE_BASE;
STATIC BOOLEAN                        mBannerWritten = FALSE;

STATIC
VOID
PstoreFlush (
  IN UINTN  Offset,
  IN UINTN  Length
  )
{
  if (Length == 0) {
    return;
  }

  WriteBackDataCacheRange ((VOID *)(UINTN)(mRing->Data + Offset), Length);
}

STATIC
VOID
PstoreAppend (
  IN CONST UINT8  *Buffer,
  IN UINTN         Length
  )
{
  UINTN  Start;
  UINTN  Size;
  UINTN  Offset;
  UINTN  First;
  UINTN  Drop;

  if (Length == 0) {
    return;
  }

  Start = 0;
  Size  = 0;
  if (mRing->Signature == PSTORE_SIGNATURE) {
    Start = (UINTN)mRing->Start;
    Size  = (UINTN)mRing->Size;
    if ((Size > PSTORE_DATA_SIZE) || (Start >= PSTORE_DATA_SIZE) || ((Start + Size) > PSTORE_DATA_SIZE)) {
      Start = 0;
      Size  = 0;
    }
  }

  // Keep only the tail of oversized writes.
  if (Length > PSTORE_DATA_SIZE) {
    Buffer += Length - PSTORE_DATA_SIZE;
    Length  = PSTORE_DATA_SIZE;
  }

  // Drop the oldest bytes when the ring is full.
  if ((Size + Length) > PSTORE_DATA_SIZE) {
    Drop  = Size + Length - PSTORE_DATA_SIZE;
    Start = (Start + Drop) % PSTORE_DATA_SIZE;
    Size -= Drop;
  }

  Offset = (Start + Size) % PSTORE_DATA_SIZE;
  First  = PSTORE_DATA_SIZE - Offset;
  if (First >= Length) {
    CopyMem ((VOID *)(UINTN)(mRing->Data + Offset), Buffer, Length);
    PstoreFlush (Offset, Length);
  } else {
    CopyMem ((VOID *)(UINTN)(mRing->Data + Offset), Buffer, First);
    CopyMem ((VOID *)(UINTN)mRing->Data, Buffer + First, Length - First);
    PstoreFlush (Offset, First);
    PstoreFlush (0, Length - First);
  }

  Size += Length;

  // Publish the payload first, then the metadata and finally the signature.
  mRing->Start     = (INT32)Start;
  mRing->Size      = (INT32)Size;
  mRing->Signature = PSTORE_SIGNATURE;
  WriteBackDataCacheRange ((VOID *)(UINTN)mRing, PSTORE_HEADER_SIZE);
}

STATIC
UINTN
EFIAPI
PstoreWrite (
  IN CONST UINT8  *Buffer,
  IN UINTN         NumberOfBytes
  )
{
  PstoreAppend (Buffer, NumberOfBytes);
  return NumberOfBytes;
}

/**
  Initialize the serial device. Appends a boot marker once per boot.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  STATIC CONST CHAR8  Banner[] = "\r\n===== UEFI boot (ramoops console) =====\r\n";

  if (!mBannerWritten) {
    mBannerWritten = TRUE;
    PstoreWrite ((CONST UINT8 *)Banner, sizeof (Banner) - 1);
  }

  return RETURN_SUCCESS;
}

/**
  Write data to the ramoops console ring.
**/
UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN   NumberOfBytes
  )
{
  return PstoreWrite (Buffer, NumberOfBytes);
}

/**
  Read data. The ramoops console is a write-only sink.
**/
UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN   NumberOfBytes
  )
{
  (VOID)Buffer;
  (VOID)NumberOfBytes;
  return 0;
}

/**
  Poll for input. Always false.
**/
BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

/**
  Set control bits. Unsupported.
**/
EFI_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  (VOID)Control;
  return EFI_UNSUPPORTED;
}

/**
  Get control bits. No modem lines.
**/
EFI_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  if (Control == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Control = 0;
  return EFI_SUCCESS;
}

/**
  Set device attributes. Configuration is fixed.
**/
EFI_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  (VOID)BaudRate;
  (VOID)ReceiveFifoDepth;
  (VOID)Timeout;
  (VOID)Parity;
  (VOID)DataBits;
  (VOID)StopBits;
  return EFI_UNSUPPORTED;
}
