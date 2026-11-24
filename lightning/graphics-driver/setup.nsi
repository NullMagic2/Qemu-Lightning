; Module: lightning.graphics_driver.setup
; Purpose: Build the setup.exe Windows installer for the render adapter.
; Created: 2026-10-09
; Architecture: NSIS wraps the package files and the signing and PnP steps of
; Install-QemuLightning.ps1, which runs hidden from Setup.exe. The compiler is
; invoked by tools/lightning/driver_package.rs with /DPACKAGE pointing at the
; assembled package folder and /DVERSION at the driver version.
Unicode true
RequestExecutionLevel admin
SetCompressor /SOLID lzma
Name "Qemu Lightning render adapter ${VERSION}"
OutFile "${OUTPUT}"
InstallDir "$PROGRAMFILES64\Qemu Lightning"
InstallDirRegKey HKLM "Software\Qemu Lightning" "InstallDir"
BrandingText "Qemu Lightning"
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!define REGISTRY_UNINSTALL "Software\Microsoft\Windows\CurrentVersion\Uninstall\QemuLightning"
!define POWERSHELL "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
!define REBOOT_EXIT_CODE 3010
Var TestSigningSwitch
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "This driver is for 64-bit Windows."
        Abort
    ${EndIf}
    StrCpy $TestSigningSwitch ""
FunctionEnd

Section "Render adapter driver and Vulkan ICD (required)" SectionDriver
    SectionIn RO
    SetOutPath "$INSTDIR"
    File "${PACKAGE}\QemuLightningGraphics.sys"
    File "${PACKAGE}\QemuLightningSingleDisplay.exe"
    File "${PACKAGE}\QemuLightningGraphics.inf"
    File "${PACKAGE}\QemuLightningVulkan.dll"
    File "${PACKAGE}\QemuLightningVulkan.json"
    File "${PACKAGE}\QemuLightningD3D10.dll"
    File "${PACKAGE}\QemuLightningD3D12Bootstrap.dll"
    File "${PACKAGE}\QemuLightningShader.dll"
    File "${PACKAGE}\QemuLightningD3D12Probe.exe"
    File "${PACKAGE}\driver-package.json"
    File "${PACKAGE}\Install-QemuLightning.ps1"
    File "${PACKAGE}\vulkan-1.dll"
    File /nonfatal "${PACKAGE}\QemuLightningD3D10-x86.dll"
    File /nonfatal "${PACKAGE}\QemuLightningVulkan-x86.dll"
    File /nonfatal "${PACKAGE}\QemuLightningVulkan-x86.json"
    File /nonfatal "${PACKAGE}\vulkan-1-x86.dll"
    SetOutPath "$INSTDIR\direct3d"
    File /nonfatal "${PACKAGE}\direct3d\*.dll"
    SetOutPath "$INSTDIR"
    WriteRegStr HKLM "Software\Qemu Lightning" "InstallDir" "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "${REGISTRY_UNINSTALL}" "DisplayName" "Qemu Lightning render adapter"
    WriteRegStr HKLM "${REGISTRY_UNINSTALL}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${REGISTRY_UNINSTALL}" "Publisher" "Qemu Lightning"
    WriteRegStr HKLM "${REGISTRY_UNINSTALL}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKLM "${REGISTRY_UNINSTALL}" "NoModify" 1
    WriteRegDWORD HKLM "${REGISTRY_UNINSTALL}" "NoRepair" 1
    DetailPrint "Signing and installing the driver through Windows PnP"
    nsExec::ExecToLog '"${POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\Install-QemuLightning.ps1" -PackageRoot "$INSTDIR" $TestSigningSwitch'
    Pop $0
    ${If} $0 == ${REBOOT_EXIT_CODE}
        SetRebootFlag true
    ${ElseIf} $0 != 0
        MessageBox MB_ICONSTOP "Driver installation failed (exit code $0). See the details above: the Windows Driver Kit signtool and Inf2Cat must be available, test signing must be on and Secure Boot off."
        Abort
    ${EndIf}
SectionEnd

Section /o "Enable test signing (reboot, then run Setup again)" SectionTestSigning
    StrCpy $TestSigningSwitch "-EnableTestSigning"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SectionDriver} "Copies the Venus render adapter (QemuLightningGraphics.sys) and its Vulkan ICD, signs them with a development certificate on this machine and installs them."
    !insertmacro MUI_DESCRIPTION_TEXT ${SectionTestSigning} "Turns on Windows test signing so the development-signed driver can load. Requires a reboot and Secure Boot off."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    nsExec::ExecToLog '"${POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\Install-QemuLightning.ps1" -PackageRoot "$INSTDIR" -Uninstall'
    Pop $0
    Delete "$INSTDIR\direct3d\*.dll"
    RMDir "$INSTDIR\direct3d"
    Delete "$INSTDIR\*.*"
    RMDir "$INSTDIR"
    DeleteRegKey HKLM "${REGISTRY_UNINSTALL}"
    DeleteRegKey HKLM "Software\Qemu Lightning"
    SetRebootFlag true
SectionEnd
