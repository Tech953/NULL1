#requires -Version 5.1
<#
Trackport local test installer.

It validates and installs a locally supplied Trackport-Windows-x64.zip.
It makes no network requests, writes no registry or startup settings, and
does not overwrite an existing installation. A current YouTube-enabled ZIP
is required; older packages are rejected before the install directory changes.

Example:
  .\Trackport-Test-Installer.ps1 -PackagePath .\Trackport-Windows-x64.zip
#>
[CmdletBinding()]
param(
  [string]$PackagePath = (Join-Path $PSScriptRoot "Trackport-Windows-x64.zip"),
  [string]$InstallPath = (Join-Path $env:LOCALAPPDATA "Programs\Trackport-Test")
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$stagePath = $null
$exitCode = 0

function Set-InstallProgress([string]$Status, [int]$Percent) {
  Write-Progress -Activity "Trackport test installation" -Status $Status -PercentComplete $Percent
  Write-Output ("[{0,3}%] {1}" -f $Percent, $Status)
}

try {
  Set-InstallProgress "Checking the local package" 5
  if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf)) {
    throw "Package not found: $PackagePath"
  }
  $packageFullPath = (Resolve-Path -LiteralPath $PackagePath).Path
  Add-Type -AssemblyName System.IO.Compression.FileSystem

  Set-InstallProgress "Checking package contents" 15
  $zip = [System.IO.Compression.ZipFile]::OpenRead($packageFullPath)
  try {
    $entryNames = @($zip.Entries | ForEach-Object { $_.FullName.Replace("\", "/") })
    $requiredEntries = @(
      "Trackport.exe",
      "README.txt",
      "source/app/proxy.js",
      "source/app/youtube.js",
      "source/src/server.c"
    )
    foreach ($entry in $requiredEntries) {
      if ($entryNames -notcontains $entry) {
        throw "Package is missing required file: $entry"
      }
    }
  }
  finally {
    $zip.Dispose()
  }

  $stagePath = Join-Path $env:TEMP ("Trackport-TestInstall-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Path $stagePath | Out-Null
  Set-InstallProgress "Extracting into a temporary staging folder" 35
  [System.IO.Compression.ZipFile]::ExtractToDirectory($packageFullPath, $stagePath)

  Set-InstallProgress "Verifying the executable and feature markers" 60
  $exePath = Join-Path $stagePath "Trackport.exe"
  $exeBytes = [System.IO.File]::ReadAllBytes($exePath)
  if ($exeBytes.Length -lt 2 -or $exeBytes[0] -ne 0x4d -or $exeBytes[1] -ne 0x5a) {
    throw "Trackport.exe is not a valid Windows executable."
  }
  $exeText = [System.Text.Encoding]::ASCII.GetString($exeBytes)
  foreach ($marker in @("/api/youtube-search", "/api/proxy-test", "X-Proxy-Enabled")) {
    if (-not $exeText.Contains($marker)) {
      throw "The executable is missing $marker. Rebuild the ZIP from the current main branch."
    }
  }

  $serverSource = Get-Content -LiteralPath (Join-Path $stagePath "source/src/server.c") -Raw
  $proxySource = Get-Content -LiteralPath (Join-Path $stagePath "source/app/proxy.js") -Raw
  foreach ($marker in @("/api/youtube-search", "/api/proxy-test", "X-Proxy-Enabled")) {
    if (-not $serverSource.Contains($marker)) {
      throw "The bundled server source is missing $marker."
    }
  }
  foreach ($key in @("proxyEnabled", "proxyUrl", "proxyUsername", "proxyPassword")) {
    if (-not $proxySource.Contains($key)) {
      throw "The bundled proxy settings are missing $key."
    }
  }

  $targetPath = [System.IO.Path]::GetFullPath($InstallPath)
  if (Test-Path -LiteralPath $targetPath) {
    throw "Install target already exists: $targetPath. Choose a new -InstallPath; existing files were not changed."
  }
  $parentPath = Split-Path -Parent $targetPath
  if (-not (Test-Path -LiteralPath $parentPath -PathType Container)) {
    New-Item -ItemType Directory -Path $parentPath -Force | Out-Null
  }

  Set-InstallProgress "Installing verified files" 85
  Move-Item -LiteralPath $stagePath -Destination $targetPath | Out-Null
  $stagePath = $null
  Set-InstallProgress "Installation complete" 100
  Write-Output "Installed to: $targetPath"
  Write-Output "No files were uploaded and no system settings were changed."
  Write-Output "YouTube alternatives require yt-dlp.exe on PATH."
}
catch {
  $exitCode = 1
  Write-Progress -Activity "Trackport test installation" -Completed
  Write-Output ("INSTALLATION FAILED: " + $_.Exception.Message)
  Write-Output "The existing install target, if any, was not overwritten."
}
finally {
  if ($stagePath -and (Test-Path -LiteralPath $stagePath)) {
    Remove-Item -LiteralPath $stagePath -Recurse -Force -ErrorAction SilentlyContinue
  }
}

exit $exitCode
