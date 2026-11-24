# Module: lightning.direct3d_bridge.install_legacy
# Purpose: Register the matching x86 UMD for the single Lightning adapter.
# Created: 2026-10-25
# Architecture: Administrative Windows package boundary; scoped DLLs and reversible registry values.
param([switch]$Rollback)
$ErrorActionPreference='Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run this installer from 64-bit PowerShell.' }
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator privileges are required to register the x86 adapter driver.' }
$source=$PSScriptRoot
$manifest=Get-Content -LiteralPath (Join-Path $source 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.architecture -ne 'x86') {throw 'Invalid legacy package manifest.'}
foreach ($file in $manifest.files) {
 if ($file.path -match '(^[\\/]|:|(^|[\\/])\.\.([\\/]|$))') {throw 'Unsafe manifest path.'}
 $p=Join-Path $source $file.path
 if ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash -ne $file.sha256) {throw "Package hash mismatch: $($file.path)"}
}
$adapters=@(Get-CimInstance Win32_VideoController | Where-Object {$_.PNPDeviceID -like 'PCI\VEN_1234&DEV_11F1*'})
if ($adapters.Count -ne 1) {throw 'Expected exactly one Qemu-Lightning PCI display adapter.'}
$driver=(Get-ItemProperty -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Enum\'+$adapters[0].PNPDeviceID)).Driver
if ($driver -notmatch '^\{4d36e968-e325-11ce-bfc1-08002be10318\}\\[0-9]{4}$') {throw 'Invalid display class registration.'}
$key='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+$driver
$version=$manifest.runtime_id
if ($version -notmatch '^[a-zA-Z0-9-]+$') {throw 'Invalid runtime version.'}
$destination=Join-Path $env:ProgramData ('QemuLightning\Legacy3D\'+$version)
$backup=Join-Path $destination ('registry-backup-'+($driver.Split('\')[-1])+'.clixml')
$names=@('UserModeDriverNameWow','VulkanDriverNameWow')
if ($Rollback) {
 $saved=Import-Clixml -LiteralPath $backup
 if ($saved.Key -ne $key) {throw 'The saved adapter identity does not match.'}
 foreach ($item in $saved.Values) {
  if ($item.Exists) {New-ItemProperty -LiteralPath $key -Name $item.Name -Value $item.Value -PropertyType $item.Kind -Force | Out-Null}
  else {Remove-ItemProperty -LiteralPath $key -Name $item.Name -ErrorAction SilentlyContinue}
 }
 Write-Output 'Restored the previous x86 adapter registration.';return
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
# Protect executable content; standard users receive read/execute access only.
$acl=[Security.AccessControl.DirectorySecurity]::new();$acl.SetAccessRuleProtection($true,$false)
foreach ($entry in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))) {
 $rule=[Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($entry[0]),$entry[1],'ContainerInherit,ObjectInherit','None','Allow');$acl.AddAccessRule($rule)
}
Set-Acl -LiteralPath $destination -AclObject $acl
if (-not (Test-Path -LiteralPath $backup)) {
 $reg=Get-Item -LiteralPath $key
 $values=foreach ($name in $names) {$exists=$reg.GetValueNames() -contains $name;[pscustomobject]@{Name=$name;Exists=$exists;Value=$(if($exists){$reg.GetValue($name)}else{$null});Kind=$(if($exists){$reg.GetValueKind($name).ToString()}else{'String'})}}
 [pscustomobject]@{Key=$key;Device=$adapters[0].PNPDeviceID;Values=@($values)} | Export-Clixml -LiteralPath $backup
}
foreach ($file in $manifest.files) {
 $out=Join-Path $destination $file.path;New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
 Copy-Item -LiteralPath (Join-Path $source $file.path) -Destination $out -Force
}
Copy-Item -LiteralPath (Join-Path $source 'manifest.json') -Destination $destination -Force
foreach ($folder in @('logs','shader-cache')) {
 $p=Join-Path $destination $folder;New-Item -ItemType Directory -Path $p -Force | Out-Null
 $a=Get-Acl -LiteralPath $p;$a.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'Modify','ContainerInherit,ObjectInherit','None','Allow'));Set-Acl -LiteralPath $p -AclObject $a
}
$umd=Join-Path $destination 'x86\QemuLightningD3D10.dll'
New-ItemProperty -LiteralPath $key -Name UserModeDriverNameWow -Value @($umd,$umd,$umd,$umd) -PropertyType MultiString -Force | Out-Null
New-ItemProperty -LiteralPath $key -Name VulkanDriverNameWow -Value (Join-Path $destination 'x86\QemuLightningVulkan.json') -PropertyType String -Force | Out-Null
Write-Output ('Installed legacy runtime: '+$destination)
Write-Output ('Launch a 32-bit application with: "'+(Join-Path $destination 'QemuLightningLegacy.exe')+'" "C:\path\application.exe"')
