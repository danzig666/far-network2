<#
.SYNOPSIS
    Replaces the Network plugin of an installed Far Manager, and puts the original back on request.

.DESCRIPTION
    Only Network.dll and its map file are touched. Language files, help files and
    plugin settings, including favorites, stay as they are.
    The original goes to Network.dll.stock, unless that already exists.

.EXAMPLE
    .\install.ps1

.EXAMPLE
    .\install.ps1 -FarPath 'D:\Tools\Far'

.EXAMPLE
    .\install.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$FarPath,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$MinFarBuild = 6682

function Find-Far {
    $candidates = @(
        "$env:ProgramFiles\Far Manager"
        "${env:ProgramFiles(x86)}\Far Manager"
        "$env:LOCALAPPDATA\Programs\Far Manager"
    )
    foreach ($key in 'HKLM:\SOFTWARE\Far Manager', 'HKCU:\SOFTWARE\Far Manager', 'HKLM:\SOFTWARE\WOW6432Node\Far Manager') {
        $value = Get-ItemProperty -Path $key -Name InstallDir -ErrorAction SilentlyContinue
        if ($value) { $candidates += $value.InstallDir }
    }
    $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ 'Far.exe')) } | Select-Object -Unique
}

function Get-PeArchitecture([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = New-Object IO.BinaryReader $stream
        $stream.Position = 0x3C
        $stream.Position = $reader.ReadInt32() + 4
        switch ($reader.ReadUInt16()) {
            0x8664 { 'x64' }
            0x014C { 'x86' }
            0xAA64 { 'arm64' }
            default { 'unknown' }
        }
    }
    finally { $stream.Dispose() }
}

if (-not $FarPath) {
    $found = @(Find-Far)
    if ($found.Count -eq 0) { throw 'Far Manager not found. Use -FarPath.' }
    if ($found.Count -gt 1) { throw "More than one Far Manager found, choose one with -FarPath:`n  $($found -join "`n  ")" }
    $FarPath = $found[0]
}

$farExe = Join-Path $FarPath 'Far.exe'
$pluginDir = Join-Path $FarPath 'Plugins\Network'
$target = Join-Path $pluginDir 'Network.dll'
$map = Join-Path $pluginDir 'Network.map'
$backup = "$target.stock"
$mapBackup = "$map.stock"

if (-not (Test-Path $farExe)) { throw "No Far.exe in $FarPath" }
if (-not (Test-Path $pluginDir)) { throw "No Network plugin folder in $FarPath" }

$resolvedExe = (Resolve-Path $farExe).Path
$running = @(Get-Process -Name Far -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $resolvedExe })
if ($running.Count) {
    throw "This Far is running (process $($running.Id -join ', ')). Close it first: Windows keeps a loaded DLL locked."
}

$build = (Get-Item $farExe).VersionInfo.FileBuildPart
if (-not $Uninstall -and $build -lt $MinFarBuild) {
    throw "Far build $build is too old for this plugin, it needs $MinFarBuild or newer."
}

# Writing to Program Files needs elevation.
$probe = Join-Path $pluginDir ".install-$PID.tmp"
try {
    [IO.File]::WriteAllText($probe, '')
    Remove-Item $probe
}
catch [UnauthorizedAccessException] {
    $identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if ($identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw }
    Write-Host 'Asking for administrator rights...'
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-FarPath', "`"$FarPath`"")
    if ($Uninstall) { $arguments += '-Uninstall' }
    $process = Start-Process powershell.exe -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    exit $process.ExitCode
}

if ($Uninstall) {
    if (-not (Test-Path $backup)) { throw "No $backup to put back." }
    Move-Item -Force $backup $target
    if (Test-Path $mapBackup) { Move-Item -Force $mapBackup $map }
    Write-Host "Restored the original Network.dll in $pluginDir"
    exit 0
}

$architecture = Get-PeArchitecture $farExe
$source = Join-Path $PSScriptRoot "dist\$architecture\Network.dll"
if (-not (Test-Path $source)) { $source = Join-Path $PSScriptRoot "$architecture\Network.dll" }
if (-not (Test-Path $source)) { throw "No $architecture build of Network.dll next to this script." }
if ((Get-PeArchitecture $source) -ne $architecture) { throw "$source is not a $architecture DLL." }

if (-not (Test-Path $backup)) {
    Copy-Item $target $backup
    if (Test-Path $map) { Copy-Item $map $mapBackup }
}

Copy-Item -Force $source $target
$sourceMap = [IO.Path]::ChangeExtension($source, '.map')
if (Test-Path $sourceMap) { Copy-Item -Force $sourceMap $map }

Write-Host "Installed the $architecture build into $pluginDir"
Write-Host "The original is in $backup. To go back: .\install.ps1 -Uninstall -FarPath '$FarPath'"
