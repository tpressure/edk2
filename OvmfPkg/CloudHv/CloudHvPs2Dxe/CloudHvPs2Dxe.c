/** @file
  Expose Cloud Hypervisor's legacy PS/2 keyboard to the standard EDK2 driver.

  Cloud Hypervisor does not emulate a PCI-to-ISA bridge.  Consequently the
  generic OVMF SioBusDxe driver has no parent on which it can enumerate the
  keyboard.  The PS/2 keyboard driver only uses EFI_SIO_PROTOCOL as a device
  model binding marker; its actual controller accesses use IoLib directly.

  The mouse is intentionally left to the guest OS.  Ps2KeyboardDxe and
  Ps2MouseDxe both initialize the shared i8042 controller independently, and
  Ps2MouseDxe can leave the keyboard port disabled after Ps2KeyboardDxe starts.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <IndustryStandard/Acpi.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SuperIo.h>

#pragma pack (1)
typedef struct {
  ACPI_HID_DEVICE_PATH       Acpi;
  EFI_DEVICE_PATH_PROTOCOL  End;
} CLOUDHV_PS2_DEVICE_PATH;
#pragma pack ()

#define CLOUDHV_PS2_DEVICE_PATH(Hid, Uid) \
  {                                         \
    {                                       \
      {                                     \
        ACPI_DEVICE_PATH,                   \
        ACPI_DP,                            \
        {                                   \
          sizeof (ACPI_HID_DEVICE_PATH),    \
          0                                 \
        }                                   \
      },                                    \
      EISA_PNP_ID (Hid),                    \
      Uid                                   \
    },                                      \
    {                                       \
      END_DEVICE_PATH_TYPE,                 \
      END_ENTIRE_DEVICE_PATH_SUBTYPE,       \
      {                                     \
        sizeof (EFI_DEVICE_PATH_PROTOCOL),  \
        0                                   \
      }                                     \
    }                                       \
  }

STATIC CLOUDHV_PS2_DEVICE_PATH  mKeyboardDevicePath =
  CLOUDHV_PS2_DEVICE_PATH (0x0303, 0);

STATIC
EFI_STATUS
EFIAPI
SioRegisterAccess (
  IN CONST EFI_SIO_PROTOCOL  *This,
  IN       BOOLEAN           Write,
  IN       BOOLEAN           ExitCfgMode,
  IN       UINT8             Register,
  IN OUT   UINT8             *Value
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SioGetResources (
  IN  CONST EFI_SIO_PROTOCOL          *This,
  OUT       ACPI_RESOURCE_HEADER_PTR  *ResourceList
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SioSetResources (
  IN CONST EFI_SIO_PROTOCOL          *This,
  IN       ACPI_RESOURCE_HEADER_PTR  ResourceList
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SioPossibleResources (
  IN  CONST EFI_SIO_PROTOCOL          *This,
  OUT       ACPI_RESOURCE_HEADER_PTR  *ResourceCollection
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
SioModify (
  IN CONST EFI_SIO_PROTOCOL         *This,
  IN CONST EFI_SIO_REGISTER_MODIFY  *Command,
  IN       UINTN                    NumberOfCommands
  )
{
  return EFI_UNSUPPORTED;
}

STATIC EFI_SIO_PROTOCOL  mKeyboardSio = {
  SioRegisterAccess,
  SioGetResources,
  SioSetResources,
  SioPossibleResources,
  SioModify
};

EFI_STATUS
EFIAPI
CloudHvPs2DxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  KeyboardHandle;
  EFI_STATUS  Status;

  KeyboardHandle = NULL;
  DEBUG ((DEBUG_INFO, "CloudHvPs2Dxe: installing PS/2 device handles\n"));
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &KeyboardHandle,
                  &gEfiDevicePathProtocolGuid,
                  &mKeyboardDevicePath,
                  &gEfiSioProtocolGuid,
                  &mKeyboardSio,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CloudHvPs2Dxe: keyboard handle: %r\n", Status));
    return Status;
  }

  Status = EfiBootManagerUpdateConsoleVariable (
             ConIn,
             &mKeyboardDevicePath.Acpi.Header,
             NULL
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CloudHvPs2Dxe: ConIn update: %r\n", Status));
    goto UninstallKeyboard;
  }

  DEBUG ((DEBUG_INFO, "CloudHvPs2Dxe: keyboard handle installed\n"));
  return EFI_SUCCESS;

UninstallKeyboard:
  gBS->UninstallMultipleProtocolInterfaces (
         KeyboardHandle,
         &gEfiDevicePathProtocolGuid,
         &mKeyboardDevicePath,
         &gEfiSioProtocolGuid,
         &mKeyboardSio,
         NULL
         );
  return Status;
}
