# Module: lightning.graphics_driver.installer
# Purpose: Setup engine: sign, trust and install the render adapter on a Windows guest.
# Created: 2026-10-09
# Architecture: Run hidden by setup.exe (setup.nsi) and by its
# uninstaller. Windows PnP verifies the INF, catalog and files; this script only
# prepares a development signature on this machine and submits the package.
#
# Usage (elevated PowerShell, inside the guest):
#   .\Install-QemuLightning.ps1                  install the driver and ICD
#   .\Install-QemuLightning.ps1 -EnableTestSigning   also turn on test signing (reboot needed)
#   .\Install-QemuLightning.ps1 -ApplicationDirectory 'C:\Games\App'   copy DXVK and the loader beside an application
#   .\Install-QemuLightning.ps1 -CheckOnly        report state without changing anything
#   .\Install-QemuLightning.ps1 -Uninstall        remove the driver package
[CmdletBinding()]
param(
    [string]$PackageRoot = $PSScriptRoot,
    [string]$WdkTools,
    [string]$ApplicationDirectory,
    [switch]$EnableTestSigning,
    [switch]$CheckOnly,
    [switch]$Uninstall
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$HardwareId = 'PCI\VEN_1234&DEV_11F1'
$ServiceName = 'QemuLightningGraphics'
$SingleDisplayFile = 'QemuLightningSingleDisplay.exe'
$CertificateSubject = 'CN=Qemu Lightning development driver signing'
$CertificateDays = 365
$Inf2CatTargets = '10_GE_X64,10_25H2_X64'
$RebootExitCode = 3010
$NoChangeExitCode = 259 # ERROR_NO_MORE_ITEMS: no matching device needs an update.
$Payload = @('QemuLightningGraphics.sys', 'QemuLightningGraphics.inf', 'QemuLightningVulkan.dll', 'QemuLightningVulkan.json', 'QemuLightningD3D10.dll', 'QemuLightningD3D12Bootstrap.dll', 'QemuLightningShader.dll', 'vulkan-1.dll')
$SignedFiles = @('QemuLightningGraphics.sys', 'QemuLightningVulkan.dll', 'QemuLightningD3D10.dll', 'QemuLightningD3D12Bootstrap.dll', 'QemuLightningShader.dll', 'vulkan-1.dll')
$WowPayload = @('QemuLightningD3D10-x86.dll', 'QemuLightningVulkan-x86.dll', 'QemuLightningVulkan-x86.json', 'vulkan-1-x86.dll')
if (Test-Path -LiteralPath (Join-Path $PackageRoot $WowPayload[0]) -PathType Leaf) {
    $Payload += $WowPayload
    $SignedFiles += @($WowPayload | Where-Object { $_ -like '*.dll' })
}
$ApplicationFiles = @('direct3d\d3d11.dll', 'direct3d\dxgi.dll')

function Step($text) {
    Write-Host ('==> ' + $text)
}

function Require-Administrator {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()

    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated (Administrator) PowerShell.'
    }
}

function Find-WdkTool($name) {
    foreach ($candidate in @((Join-Path $PackageRoot ('tools\' + $name)), (Join-Path $PackageRoot $name))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    if ($WdkTools) {
        $tool = Join-Path $WdkTools $name
        if (Test-Path -LiteralPath $tool -PathType Leaf) {
            return $tool
        }
    }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (Test-Path -LiteralPath $kits) {
        $found = Get-ChildItem -Path $kits -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' -or $_.Name -eq 'Inf2Cat.exe' } |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) {
            return $found.FullName
        }
    }
    throw ('Cannot find ' + $name + '. Install the Windows Driver Kit or pass -WdkTools, or place it in the tools folder beside this script.')
}

function Test-TestSigning {
    $boot = (& bcdedit /enum '{current}' 2>&1 | Out-String)
    return $boot -match 'testsigning\s+Yes'
}

function Get-DevelopmentCertificate {
    $existing = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $CertificateSubject -and $_.NotAfter -gt (Get-Date).AddDays(1) } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
    if ($existing) {
        return $existing
    }
    Step 'Creating a development code-signing certificate for this machine'
    $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertificateSubject `
        -CertStoreLocation 'Cert:\LocalMachine\My' -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable -NotBefore (Get-Date).AddDays(-1) -NotAfter (Get-Date).AddDays($CertificateDays)
    $public = Join-Path $env:TEMP 'QemuLightningDevelopment.cer'
    Export-Certificate -Cert $certificate -FilePath $public | Out-Null
    foreach ($store in @('Root', 'TrustedPublisher')) {
        Import-Certificate -FilePath $public -CertStoreLocation ('Cert:\LocalMachine\' + $store) | Out-Null
    }
    Remove-Item -LiteralPath $public -ErrorAction SilentlyContinue
    return $certificate
}

function Sign-Package {
    $signtool = Find-WdkTool 'signtool.exe'
    $inf2cat = Find-WdkTool 'Inf2Cat.exe'
    $certificate = Get-DevelopmentCertificate
    Step 'Signing the driver and ICD'
    foreach ($name in $SignedFiles) {
        & $signtool sign /fd SHA256 /sm /s My /sha1 $certificate.Thumbprint (Join-Path $PackageRoot $name) | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw ('Signing failed: ' + $name)
        }
    }
    Step 'Building and signing the catalog'
    $staging = Join-Path $env:TEMP ('QemuLightningCatalog-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    try {
        foreach ($name in $Payload) {
            Copy-Item -LiteralPath (Join-Path $PackageRoot $name) -Destination $staging
        }
        & $inf2cat ("/driver:" + $staging) ("/os:" + $Inf2CatTargets) | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw 'Inf2Cat failed; see its output above.'
        }
        $catalog = Join-Path $staging 'QemuLightningGraphics.cat'
        & $signtool sign /fd SHA256 /sm /s My /sha1 $certificate.Thumbprint $catalog | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw 'Catalog signing failed.'
        }
        Copy-Item -LiteralPath $catalog -Destination $PackageRoot -Force
    } finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
}


# The Rust helper changes only a BasicDisplay boot placeholder beside a sole
# active Lightning PCI adapter. Separate physical/remote layouts are preserved.
function Install-SingleDisplayHandoff {
    $source = Join-Path $PackageRoot 'QemuLightningSingleDisplay.exe'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { return }
    $taskName = 'QemuLightning.SingleDisplay'
    $directory = Join-Path $env:ProgramFiles 'Qemu Lightning Graphics'
    $destination = Join-Path $directory 'QemuLightningSingleDisplay.exe'
    $existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($existing -and (@($existing.Actions).Count -ne 1 -or
        $existing.Actions[0].Execute -ine $destination -or
        $existing.Actions[0].Arguments -ne '--apply')) {
        throw 'The display handoff task name belongs to a different action.'
    }
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $action = New-ScheduledTaskAction -Execute $destination -Argument '--apply'
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    $trigger.Delay = 'PT5S'
    $principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-545' -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 1) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
}


# Keep the firmware display available while a native package is being replaced.
# Older installers persisted CONFIGFLAG_DISABLED on this exact root device.
# It can be absent when Lightning or VGA owns the display, so repair its saved
# flag as well; pnputil /enable-device alone cannot repair an absent instance.
function Restore-FirmwareFallback {
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\BASICDISPLAY\0000'
    $device = Get-ItemProperty -LiteralPath $key -Name ConfigFlags -ErrorAction SilentlyContinue
    if ($device -and ($device.ConfigFlags -band 1)) {
        Step 'Restoring the firmware display fallback for driver updates'
        Set-ItemProperty -LiteralPath $key -Name ConfigFlags -Type DWord -Value ($device.ConfigFlags -band (-bnot 1))
        $script:rebootRequired = $true
    }
}

function Get-AdapterState {
    @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object InstanceId -like ($HardwareId + '*') | ForEach-Object {
        [pscustomobject]@{
            Status = [string]$_.Status
            Instance = $_.InstanceId
            Problem = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_ProblemCode).Data
            DriverVersion = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Data)
            Service = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Data)
            DriverInf = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Data)
        }
    })
}

if ($env:OS -ne 'Windows_NT') {
    throw 'This installer runs inside the Windows guest.'
}
$PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
$catalogPath = Join-Path $PackageRoot 'QemuLightningGraphics.cat'
$catalogValid = (Test-Path -LiteralPath $catalogPath) -and ((Get-AuthenticodeSignature -LiteralPath $catalogPath).Status -eq 'Valid')
$state = [ordered]@{
    PackageRoot = $PackageRoot
    TestSigning = Test-TestSigning
    CatalogSignatureValid = $catalogValid
    Adapters = Get-AdapterState
}
if ($CheckOnly) {
    [pscustomobject]$state | ConvertTo-Json -Depth 4; return
}
Require-Administrator
if (Test-Path -LiteralPath (Join-Path $PackageRoot $SingleDisplayFile) -PathType Leaf) {
    $Payload += $SingleDisplayFile
    $SignedFiles += $SingleDisplayFile
}

if ($Uninstall) {
    Restore-FirmwareFallback
    Step 'Removing the driver package'
    & (Join-Path $env:SystemRoot 'System32\pnputil.exe') /enable-device 'ROOT\BASICDISPLAY\0000'
    Unregister-ScheduledTask -TaskName 'QemuLightning.SingleDisplay' -Confirm:$false -ErrorAction SilentlyContinue
    $displayHelper = Join-Path $env:ProgramFiles 'Qemu Lightning Graphics\QemuLightningSingleDisplay.exe'
    Remove-Item -LiteralPath $displayHelper -Force -ErrorAction SilentlyContinue

    # Match each package's own original INF. The former whole-list text test
    # could select every OEM driver whenever one Lightning INF was present.
    $published = @(Get-WindowsDriver -Online -All | Where-Object {
        (Split-Path $_.OriginalFileName -Leaf) -ieq 'QemuLightningGraphics.inf' -and
        $_.Driver -match '^oem\d+\.inf$'
    })

    foreach ($package in $published) {
        & pnputil /delete-driver $package.Driver /uninstall | Out-Null
        if ($LASTEXITCODE -notin @(0, $RebootExitCode)) {
            throw ('Could not remove ' + $package.Driver + ': pnputil exit ' + $LASTEXITCODE)
        }
    }
    Write-Host 'Done. A reboot completes the removal.'
    return
}

foreach ($name in $Payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $name) -PathType Leaf)) {
        throw ('Missing package file: ' + $name)
    }
}
if (@($state.Adapters).Count -eq 0) {
    throw 'No PCI\VEN_1234&DEV_11F1 adapter is present. Start the matching QEMU build with lightning-pci,scanout=on and the Venus worker before installing.'
}

# This development package is signed locally. Setup may have replaced files
# beside an older, still-valid signed catalog; its signature does not prove
# that its hashes match this payload.
$rebootRequired = $false
Restore-FirmwareFallback
if (-not $state.TestSigning) {
    if ($EnableTestSigning) {
        Step 'Enabling test signing; reboot, then run this script again'
        & bcdedit /set testsigning on | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw 'bcdedit failed. Secure Boot must be off for test signing.'
        }
        exit $RebootExitCode
    }
    throw 'Test signing is off. Re-run with -EnableTestSigning (Secure Boot must be off), reboot, then install.'
}
Sign-Package
Step 'Installing through Windows PnP'
$output = (& (Join-Path $env:SystemRoot 'System32\pnputil.exe') /add-driver (Join-Path $PackageRoot 'QemuLightningGraphics.inf') /install 2>&1 | Out-String)
Write-Host $output
if ($LASTEXITCODE -notin @(0, $RebootExitCode, $NoChangeExitCode)) {
    throw ('pnputil failed with exit code ' + $LASTEXITCODE)
}
$rebootRequired = $rebootRequired -or ($LASTEXITCODE -eq $RebootExitCode)

if ($ApplicationDirectory) {
    Step ('Copying the Vulkan loader and DXVK beside ' + $ApplicationDirectory)
    New-Item -ItemType Directory -Path $ApplicationDirectory -Force | Out-Null
    foreach ($name in $ApplicationFiles) {
        $source = Join-Path $PackageRoot $name

        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $ApplicationDirectory (Split-Path $name -Leaf)) -Force
        }
    }
}

Start-Sleep -Seconds 2
$adapters = Get-AdapterState
$versionLine = Get-Content -LiteralPath (Join-Path $PackageRoot 'QemuLightningGraphics.inf') | Where-Object { $_ -match '^\s*DriverVer\s*=\s*[^,]+,\s*([^;\s]+)' } | Select-Object -First 1

if (-not $versionLine) {
    throw 'The package INF has no DriverVer.'
}
$null = $versionLine -match '^\s*DriverVer\s*=\s*[^,]+,\s*([^;\s]+)'
$expectedVersion = $Matches[1]
$failed = @($adapters | Where-Object { $_.Problem -ne 0 -or $_.Service -ne $ServiceName -or $_.DriverVersion -ne $expectedVersion })
$working = @($adapters).Count -gt 0 -and $failed.Count -eq 0
if ($working) {
    Install-SingleDisplayHandoff
}
[pscustomobject]@{
    Installed = $working
    PackageStaged = $true
    RebootRequired = $rebootRequired
    Service = (& sc.exe query $ServiceName 2>&1 | Out-String).Trim()
    Adapters = $adapters
} | ConvertTo-Json -Depth 4
if ($rebootRequired) {
    exit $RebootExitCode
}
if (-not $working) {
    throw 'PnP staged the package, but the adapter did not start. Check the reported driver version, service, problem code, Code Integrity events and QlgStartupDiagnostics before enabling this display.'
}
