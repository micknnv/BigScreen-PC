<#
.SYNOPSIS
    Prepares and stages all Bigscreen NOVR runtime and driver assets into build/native-frida/runtime
    for local execution and Tauri NSIS bundling.
#>
param(
    [string]$BuildDir = "$PSScriptRoot/../build"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$RuntimeDir = Join-Path $RepoRoot "build/native-frida/runtime"

Write-Host "Staging Bigscreen NOVR runtime assets into: $RuntimeDir" -ForegroundColor Cyan

# Ensure runtime and driver directory structures exist
$DriverBinDir = Join-Path $RuntimeDir "driver/bin/win64"
$DriverSettingsDir = Join-Path $RuntimeDir "driver/resources/settings"
New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
New-Item -ItemType Directory -Force -Path $DriverBinDir | Out-Null
New-Item -ItemType Directory -Force -Path $DriverSettingsDir | Out-Null

# 1. Copy license notices
$LicenseDir = Join-Path $RepoRoot "licenses"
Get-ChildItem -Path $LicenseDir -Filter "*.txt" | ForEach-Object {
    Copy-Item $_.FullName -Destination $RuntimeDir -Force
    Write-Host "  [License] Copied $($_.Name)" -ForegroundColor Gray
}

# 2. Stage third_person_agent.js
$CompiledAgent = Join-Path $RepoRoot "runtime_agent/dist/third_person_agent.js"
$ProbeScript = Join-Path $RepoRoot "runtime_camera_probe/v24_gpu_only.js"
$TargetAgent = Join-Path $RuntimeDir "third_person_agent.js"

if (Test-Path $CompiledAgent) {
    Copy-Item $CompiledAgent -Destination $TargetAgent -Force
    Write-Host "  [Agent] Copied compiled agent ($CompiledAgent)" -ForegroundColor Green
} elseif (Test-Path $ProbeScript) {
    Copy-Item $ProbeScript -Destination $TargetAgent -Force
    Write-Host "  [Agent] Staged v24_gpu_only.js as third_person_agent.js" -ForegroundColor Yellow
}

# 3. Stage SteamVR driver manifest and settings
$Manifest = Join-Path $RepoRoot "driver.vrdrivermanifest"
$Settings = Join-Path $RepoRoot "resources/settings/default.vrsettings"

if (Test-Path $Manifest) {
    Copy-Item $Manifest -Destination (Join-Path $RuntimeDir "driver/driver.vrdrivermanifest") -Force
    Copy-Item $Manifest -Destination (Join-Path $RuntimeDir "driver.vrdrivermanifest") -Force
    Write-Host "  [Driver] Staged driver.vrdrivermanifest" -ForegroundColor Green
}
if (Test-Path $Settings) {
    Copy-Item $Settings -Destination (Join-Path $DriverSettingsDir "default.vrsettings") -Force
    Write-Host "  [Driver] Staged default.vrsettings" -ForegroundColor Green
}

# 4. Copy built binaries if available in CMake build tree
$Candidates = @(
    (Join-Path $BuildDir "msvc/runtime"),
    (Join-Path $BuildDir "msvc/Release"),
    (Join-Path $BuildDir "runtime"),
    (Join-Path $BuildDir "Release"),
    (Join-Path $BuildDir "Debug"),
    $BuildDir,
    (Join-Path $BuildDir "BigscreenDesktopDriver/bin/win64")
)

$BinaryNames = @(
    "BigscreenDesktopBridge.exe",
    "BigscreenDesktopViewer.exe",
    "BigscreenThirdPersonHost.exe",
    "BigscreenThirdPersonGpuExport.dll",
    "driver_bigscreen_desktop.dll",
    "openvr_api.dll"
)

foreach ($name in $BinaryNames) {
    $found = $false
    foreach ($cand in $Candidates) {
        $p = Join-Path $cand $name
        if (Test-Path $p) {
            try {
                Copy-Item $p -Destination $RuntimeDir -Force
                if ($name -eq "driver_bigscreen_desktop.dll") {
                    Copy-Item $p -Destination $DriverBinDir -Force
                }
                Write-Host "  [Binary] Copied $name from $cand" -ForegroundColor Green
            } catch {
                Write-Host "  [Warning] Could not copy $name (currently in use/running). Close running instance to replace." -ForegroundColor Yellow
            }
            $found = $true
            break
        }
    }
    if (-not $found) {
        Write-Host "  [Notice] $name not yet built in $BuildDir (run CMake build first)" -ForegroundColor DarkGray
    }
}

Write-Host "`nStaging check completed." -ForegroundColor Cyan
