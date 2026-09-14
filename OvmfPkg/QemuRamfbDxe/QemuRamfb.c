/** @file
  This driver is a implementation of the Graphics Output Protocol
  for the QEMU ramfb device.

  Copyright (c) 2018, Red Hat Inc.
  Copyright (c) 2026, Cyberus Technology GmbH.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Protocol/GraphicsOutput.h>
#include <Protocol/PciEnumerationComplete.h>
#include <Protocol/PciIo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/QemuFwCfgLib.h>

#include <Guid/QemuRamfb.h>

#define RAMFB_FORMAT           0x34325258 /* DRM_FORMAT_XRGB8888 */
#define RAMFB_BPP              4
#define DISPLAY_HID_VENDOR_ID  0x8086
#define DISPLAY_HID_DEVICE_ID  0x7020
#define DISPLAY_HID_BAR_INDEX  0

#pragma pack (1)
typedef struct RAMFB_CONFIG {
  UINT64    Address;
  UINT32    FourCC;
  UINT32    Flags;
  UINT32    Width;
  UINT32    Height;
  UINT32    Stride;
} RAMFB_CONFIG;
#pragma pack ()

STATIC EFI_HANDLE              mRamfbHandle;
STATIC EFI_HANDLE              mGopHandle;
STATIC FRAME_BUFFER_CONFIGURE  *mQemuRamfbFrameBufferBltConfigure;
STATIC UINTN                   mQemuRamfbFrameBufferBltConfigureSize;
STATIC FIRMWARE_CONFIG_ITEM    mRamfbFwCfgItem;
STATIC UINT64                  mPublishedFrameBufferBase;
STATIC VOID                    *mPciEnumerationRegistration;
STATIC BOOLEAN                 mFramebufferAddressPublished;

STATIC
EFI_STATUS
PublishFramebufferAddress (
  VOID
  )
{
  EFI_HANDLE           *Handles;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  EFI_STATUS           Status;
  UINTN                Count;
  UINTN                Index;
  UINT16               Identity[2];
  UINT32               Bar0;
  UINT64               FrameBufferBase;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (
                         Handles[Index],
                         &gEfiPciIoProtocolGuid,
                         (VOID **)&PciIo
                         )))
    {
      continue;
    }

    if (EFI_ERROR (PciIo->Pci.Read (
                              PciIo,
                              EfiPciIoWidthUint16,
                              0,
                              2,
                              Identity
                              )) ||
        (Identity[0] != DISPLAY_HID_VENDOR_ID) ||
        (Identity[1] != DISPLAY_HID_DEVICE_ID))
    {
      continue;
    }

    // The real PIIX3 UHCI device has no BAR0. Require an assigned MMIO BAR.
    if (EFI_ERROR (PciIo->Pci.Read (
                              PciIo,
                              EfiPciIoWidthUint32,
                              0x10,
                              1,
                              &Bar0
                              )) ||
        ((Bar0 & 1) != 0) ||
        ((Bar0 & 0xfffff000) == 0))
    {
      continue;
    }

    Status = PciIo->Attributes (
                      PciIo,
                      EfiPciIoAttributeOperationEnable,
                      EFI_PCI_IO_ATTRIBUTE_MEMORY,
                      NULL
                      );
    if (EFI_ERROR (Status)) {
      break;
    }

    // BAR0 offset zero is a little-endian 64-bit framebuffer GPA.
    FrameBufferBase = mPublishedFrameBufferBase;
    Status          = PciIo->Mem.Write (
                                 PciIo,
                                 EfiPciIoWidthUint64,
                                 DISPLAY_HID_BAR_INDEX,
                                 0,
                                 1,
                                 &FrameBufferBase
                                 );
    if (!EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "Ramfb: published framebuffer GPA 0x%lx to display HID BAR0\n",
        FrameBufferBase
        ));
    }

    break;
  }

  FreePool (Handles);
  return Status;
}

STATIC
VOID
EFIAPI
OnPciEnumerationComplete (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  VOID        *Interface;

  if (mFramebufferAddressPublished) {
    return;
  }

  (VOID)Event;
  (VOID)Context;

  Status = gBS->LocateProtocol (
                  &gEfiPciEnumerationCompleteProtocolGuid,
                  NULL,
                  &Interface
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = PublishFramebufferAddress ();
  if (!EFI_ERROR (Status)) {
    mFramebufferAddressPublished = TRUE;
  } else if (Status != EFI_NOT_FOUND) {
    DEBUG ((DEBUG_WARN, "Ramfb: publishing framebuffer GPA failed: %r\n", Status));
  }
}

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mQemuRamfbModeInfo[] = {
  {
    0,    // Version
    640,  // HorizontalResolution
    480,  // VerticalResolution
  },{
    0,    // Version
    800,  // HorizontalResolution
    600,  // VerticalResolution
  },{
    0,    // Version
    1024, // HorizontalResolution
    768,  // VerticalResolution
  }
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE  mQemuRamfbMode = {
  ARRAY_SIZE (mQemuRamfbModeInfo),                // MaxMode
  0,                                              // Mode
  mQemuRamfbModeInfo,                             // Info
  sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),  // SizeOfInfo
};

STATIC
EFI_STATUS
EFIAPI
QemuRamfbGraphicsOutputQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo;

  if ((Info == NULL) || (SizeOfInfo == NULL) ||
      (ModeNumber >= mQemuRamfbMode.MaxMode))
  {
    return EFI_INVALID_PARAMETER;
  }

  ModeInfo = &mQemuRamfbModeInfo[ModeNumber];

  *Info = AllocateCopyPool (
            sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
            ModeInfo
            );
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QemuRamfbGraphicsOutputSetMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN  UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo;
  RAMFB_CONFIG                          Config;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL         Black;
  RETURN_STATUS                         Status;

  if (ModeNumber >= mQemuRamfbMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  ModeInfo = &mQemuRamfbModeInfo[ModeNumber];

  DEBUG ((
    DEBUG_INFO,
    "Ramfb: SetMode %u (%ux%u)\n",
    ModeNumber,
    ModeInfo->HorizontalResolution,
    ModeInfo->VerticalResolution
    ));

  Config.Address = SwapBytes64 (mQemuRamfbMode.FrameBufferBase);
  Config.FourCC  = SwapBytes32 (RAMFB_FORMAT);
  Config.Flags   = SwapBytes32 (0);
  Config.Width   = SwapBytes32 (ModeInfo->HorizontalResolution);
  Config.Height  = SwapBytes32 (ModeInfo->VerticalResolution);
  Config.Stride  = SwapBytes32 (ModeInfo->HorizontalResolution * RAMFB_BPP);

  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)mQemuRamfbMode.FrameBufferBase,
             ModeInfo,
             mQemuRamfbFrameBufferBltConfigure,
             &mQemuRamfbFrameBufferBltConfigureSize
             );

  if (Status == RETURN_BUFFER_TOO_SMALL) {
    if (mQemuRamfbFrameBufferBltConfigure != NULL) {
      FreePool (mQemuRamfbFrameBufferBltConfigure);
    }

    mQemuRamfbFrameBufferBltConfigure =
      AllocatePool (mQemuRamfbFrameBufferBltConfigureSize);
    if (mQemuRamfbFrameBufferBltConfigure == NULL) {
      mQemuRamfbFrameBufferBltConfigureSize = 0;
      return EFI_OUT_OF_RESOURCES;
    }

    Status = FrameBufferBltConfigure (
               (VOID *)(UINTN)mQemuRamfbMode.FrameBufferBase,
               ModeInfo,
               mQemuRamfbFrameBufferBltConfigure,
               &mQemuRamfbFrameBufferBltConfigureSize
               );
  }

  if (RETURN_ERROR (Status)) {
    ASSERT (Status == RETURN_UNSUPPORTED);
    return Status;
  }

  mQemuRamfbMode.Mode = ModeNumber;
  mQemuRamfbMode.Info = ModeInfo;

  QemuFwCfgSelectItem (mRamfbFwCfgItem);
  QemuFwCfgWriteBytes (sizeof (Config), &Config);

  //
  // clear screen
  //
  ZeroMem (&Black, sizeof (Black));
  Status = FrameBufferBlt (
             mQemuRamfbFrameBufferBltConfigure,
             &Black,
             EfiBltVideoFill,
             0,                               // SourceX -- ignored
             0,                               // SourceY -- ignored
             0,                               // DestinationX
             0,                               // DestinationY
             ModeInfo->HorizontalResolution,  // Width
             ModeInfo->VerticalResolution,    // Height
             0                                // Delta -- ignored
             );
  if (RETURN_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: clearing the screen failed: %r\n",
      __func__,
      Status
      ));
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QemuRamfbGraphicsOutputBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN  UINTN                              SourceX,
  IN  UINTN                              SourceY,
  IN  UINTN                              DestinationX,
  IN  UINTN                              DestinationY,
  IN  UINTN                              Width,
  IN  UINTN                              Height,
  IN  UINTN                              Delta
  )
{
  return FrameBufferBlt (
           mQemuRamfbFrameBufferBltConfigure,
           BltBuffer,
           BltOperation,
           SourceX,
           SourceY,
           DestinationX,
           DestinationY,
           Width,
           Height,
           Delta
           );
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  mQemuRamfbGraphicsOutput = {
  QemuRamfbGraphicsOutputQueryMode,
  QemuRamfbGraphicsOutputSetMode,
  QemuRamfbGraphicsOutputBlt,
  &mQemuRamfbMode,
};

EFI_STATUS
EFIAPI
InitializeQemuRamfb (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *RamfbDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *GopDevicePath;
  VOID                      *DevicePath;
  VENDOR_DEVICE_PATH        VendorDeviceNode;
  ACPI_ADR_DEVICE_PATH      AcpiDeviceNode;
  EFI_STATUS                Status;
  EFI_PHYSICAL_ADDRESS      FbBase;
  UINTN                     FbSize, MaxFbSize, Pages;
  UINTN                     FwCfgSize;
  UINTN                     Index;
  EFI_EVENT                 PciEnumerationEvent;

  if (!QemuFwCfgIsAvailable ()) {
    DEBUG ((DEBUG_INFO, "Ramfb: no FwCfg\n"));
    return EFI_NOT_FOUND;
  }

  Status = QemuFwCfgFindFile ("etc/ramfb", &mRamfbFwCfgItem, &FwCfgSize);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  if (FwCfgSize != sizeof (RAMFB_CONFIG)) {
    DEBUG ((
      DEBUG_ERROR,
      "Ramfb: FwCfg size mismatch (expected %lu, got %lu)\n",
      (UINT64)sizeof (RAMFB_CONFIG),
      (UINT64)FwCfgSize
      ));
    return EFI_PROTOCOL_ERROR;
  }

  MaxFbSize = 0;
  for (Index = 0; Index < ARRAY_SIZE (mQemuRamfbModeInfo); Index++) {
    mQemuRamfbModeInfo[Index].PixelsPerScanLine =
      mQemuRamfbModeInfo[Index].HorizontalResolution;
    mQemuRamfbModeInfo[Index].PixelFormat =
      PixelBlueGreenRedReserved8BitPerColor;
    FbSize = RAMFB_BPP *
             mQemuRamfbModeInfo[Index].HorizontalResolution *
             mQemuRamfbModeInfo[Index].VerticalResolution;
    if (MaxFbSize < FbSize) {
      MaxFbSize = FbSize;
    }

    DEBUG ((
      DEBUG_INFO,
      "Ramfb: Mode %lu: %ux%u, %lu kB\n",
      (UINT64)Index,
      mQemuRamfbModeInfo[Index].HorizontalResolution,
      mQemuRamfbModeInfo[Index].VerticalResolution,
      (UINT64)(FbSize / 1024)
      ));
  }

  Pages     = EFI_SIZE_TO_PAGES (MaxFbSize);
  MaxFbSize = EFI_PAGES_TO_SIZE (Pages);
  FbBase    = (EFI_PHYSICAL_ADDRESS)(UINTN)AllocateReservedPages (Pages);
  if (FbBase == 0) {
    DEBUG ((DEBUG_ERROR, "Ramfb: memory allocation failed\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((
    DEBUG_INFO,
    "Ramfb: Framebuffer at 0x%lx, %lu kB, %lu pages\n",
    (UINT64)FbBase,
    (UINT64)(MaxFbSize / 1024),
    (UINT64)Pages
    ));
  mQemuRamfbMode.FrameBufferSize = MaxFbSize;
  mQemuRamfbMode.FrameBufferBase = FbBase;
  mPublishedFrameBufferBase      = FbBase;

  //
  // 1024 x 768
  //
  QemuRamfbGraphicsOutputSetMode (&mQemuRamfbGraphicsOutput, 2);

  //
  // ramfb vendor devpath
  //
  VendorDeviceNode.Header.Type    = HARDWARE_DEVICE_PATH;
  VendorDeviceNode.Header.SubType = HW_VENDOR_DP;
  CopyGuid (&VendorDeviceNode.Guid, &gQemuRamfbGuid);
  SetDevicePathNodeLength (
    &VendorDeviceNode.Header,
    sizeof (VENDOR_DEVICE_PATH)
    );

  RamfbDevicePath = AppendDevicePathNode (
                      NULL,
                      (EFI_DEVICE_PATH_PROTOCOL *)&VendorDeviceNode
                      );
  if (RamfbDevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeFramebuffer;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mRamfbHandle,
                  &gEfiDevicePathProtocolGuid,
                  RamfbDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Ramfb: install Ramfb Vendor DevicePath failed: %r\n",
      Status
      ));
    goto FreeRamfbDevicePath;
  }

  //
  // gop devpath + protocol
  //
  AcpiDeviceNode.Header.Type    = ACPI_DEVICE_PATH;
  AcpiDeviceNode.Header.SubType = ACPI_ADR_DP;
  AcpiDeviceNode.ADR            = ACPI_DISPLAY_ADR (
                                    1,                                      // DeviceIdScheme
                                    0,                                      // HeadId
                                    0,                                      // NonVgaOutput
                                    1,                                      // BiosCanDetect
                                    0,                                      // VendorInfo
                                    ACPI_ADR_DISPLAY_TYPE_EXTERNAL_DIGITAL, // Type
                                    0,                                      // Port
                                    0                                       // Index
                                    );
  SetDevicePathNodeLength (
    &AcpiDeviceNode.Header,
    sizeof (ACPI_ADR_DEVICE_PATH)
    );

  GopDevicePath = AppendDevicePathNode (
                    RamfbDevicePath,
                    (EFI_DEVICE_PATH_PROTOCOL *)&AcpiDeviceNode
                    );
  if (GopDevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeRamfbHandle;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mGopHandle,
                  &gEfiDevicePathProtocolGuid,
                  GopDevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mQemuRamfbGraphicsOutput,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Ramfb: install GOP DevicePath failed: %r\n",
      Status
      ));
    goto FreeGopDevicePath;
  }

  Status = gBS->OpenProtocol (
                  mRamfbHandle,
                  &gEfiDevicePathProtocolGuid,
                  &DevicePath,
                  gImageHandle,
                  mGopHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Ramfb: OpenProtocol failed: %r\n", Status));
    goto FreeGopHandle;
  }

  // PCI BARs may not be assigned when this driver starts. Publish the GPA
  // after enumeration, or immediately if enumeration already completed.
  PciEnumerationEvent = EfiCreateProtocolNotifyEvent (
                          &gEfiPciEnumerationCompleteProtocolGuid,
                          TPL_CALLBACK,
                          OnPciEnumerationComplete,
                          NULL,
                          &mPciEnumerationRegistration
                          );
  if (PciEnumerationEvent == NULL) {
    DEBUG ((DEBUG_WARN, "Ramfb: could not watch PCI enumeration\n"));
  }

  return EFI_SUCCESS;

FreeGopHandle:
  gBS->UninstallMultipleProtocolInterfaces (
         mGopHandle,
         &gEfiDevicePathProtocolGuid,
         GopDevicePath,
         &gEfiGraphicsOutputProtocolGuid,
         &mQemuRamfbGraphicsOutput,
         NULL
         );
FreeGopDevicePath:
  FreePool (GopDevicePath);
FreeRamfbHandle:
  gBS->UninstallMultipleProtocolInterfaces (
         mRamfbHandle,
         &gEfiDevicePathProtocolGuid,
         RamfbDevicePath,
         NULL
         );
FreeRamfbDevicePath:
  FreePool (RamfbDevicePath);
FreeFramebuffer:
  FreePages ((VOID *)(UINTN)mQemuRamfbMode.FrameBufferBase, Pages);
  return Status;
}
