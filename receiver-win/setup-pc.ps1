# controller-streamer - stream a game controller over the LAN to a Windows PC
# Copyright (C) 2026 knmn2000
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# setup-pc.ps1 - one-shot PC setup for the Controller Streamer receiver.
# Run from an ELEVATED PowerShell, inside the receiver-win folder:
#   powershell -ExecutionPolicy Bypass -File .\setup-pc.ps1
#
# Steps: [1] admin check  [2] toolchain check  [3] ViGEmBus driver
#        [4] firewall rule  [5] build  [6] next steps
# Safe to re-run: every step checks before acting.

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Step($n, $msg) { Write-Host "`n[$n/6] $msg" -ForegroundColor Cyan }
function Ok($msg)       { Write-Host "  OK  $msg" -ForegroundColor Green }
function Fail($msg)     { Write-Host "  !!  $msg" -ForegroundColor Red; exit 1 }

# --- [1] elevation -----------------------------------------------------------
Step 1 "Checking for administrator rights"
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "Not elevated. Right-click PowerShell -> 'Run as administrator', then re-run."
}
Ok "elevated"

# --- [2] toolchain -----------------------------------------------------------
Step 2 "Checking build toolchain (git, cmake, MSVC)"
foreach ($tool in "git", "cmake") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool not found. Install Visual Studio 2022 with the 'Desktop development with C++' workload (includes both), or: winget install Git.Git Kitware.CMake"
    }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "Visual Studio not detected. Install VS 2022 Community with the 'Desktop development with C++' workload: winget install Microsoft.VisualStudio.2022.Community"
}
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName
if (-not $vs) {
    Fail "VS found but the C++ toolset is missing. Open 'Visual Studio Installer' -> Modify -> check 'Desktop development with C++'."
}
Ok "git, cmake, and '$vs' with C++ tools"

# Pick the CMake generator explicitly. Without -G, CMake takes its default,
# and on a PC where the winlibs/mingw package is installed (it was, for an
# earlier compile-check) gcc.exe and ninja.exe sit on PATH - CMake then picks
# Ninja + GNU and quietly builds with mingw instead of MSVC. That also breaks
# step 5's artifact check: single-config Ninja ignores --config Release and
# writes build\receiver.exe, not build\Release\receiver.exe.
$vsver   = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$vsmajor = ($vsver -split '\.')[0]
$genmap  = @{ "16" = "Visual Studio 16 2019"
              "17" = "Visual Studio 17 2022"
              "18" = "Visual Studio 18 2026" }
$gen = $genmap[$vsmajor]
if (-not $gen) { Fail "Unrecognized Visual Studio major version '$vsmajor'. Add it to the generator map in this script." }
Ok "generator: $gen (x64)"

# --- [3] ViGEmBus driver -----------------------------------------------------
Step 3 "Checking ViGEmBus driver"
$svc = Get-Service -Name "ViGEmBus" -ErrorAction SilentlyContinue
if ($svc) {
    Ok "driver already installed (service '$($svc.Name)' present)"
} else {
    Write-Host "  driver not found -> fetching latest official installer from GitHub..."
    $rel = Invoke-RestMethod "https://api.github.com/repos/nefarius/ViGEmBus/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -like "*.exe" } | Select-Object -First 1
    if (-not $asset) { Fail "No .exe asset in latest ViGEmBus release. Install manually: https://github.com/nefarius/ViGEmBus/releases" }
    $dst = Join-Path $env:TEMP $asset.name
    Invoke-WebRequest $asset.browser_download_url -OutFile $dst
    Write-Host "  launching $($asset.name) - click through the installer, then this script continues..."
    Start-Process $dst -Wait
    if (-not (Get-Service -Name "ViGEmBus" -ErrorAction SilentlyContinue)) {
        Fail "ViGEmBus service still not present after install. Reboot may be required; re-run this script afterwards."
    }
    Ok "driver installed"
}

# --- [4] firewall ------------------------------------------------------------
Step 4 "Firewall rule for UDP 47800"
if (Get-NetFirewallRule -DisplayName "Controller Streamer" -ErrorAction SilentlyContinue) {
    Ok "rule already exists"
} else {
    New-NetFirewallRule -DisplayName "Controller Streamer" -Direction Inbound `
        -Protocol UDP -LocalPort 47800 -Action Allow | Out-Null
    Ok "rule created"
}

# --- [5] build ---------------------------------------------------------------
Step 5 "Building the receiver (fetches ViGEmClient SDK on first configure)"
if (-not (Test-Path (Join-Path $PSScriptRoot "CMakeLists.txt"))) {
    Fail "Run this script from inside the receiver-win folder."
}
Push-Location $PSScriptRoot
try {
    # A build dir configured by a different generator cannot be reused, so drop
    # it and keep this script safe to re-run (see the -G note in step 2).
    $cache = Join-Path $PSScriptRoot "build\CMakeCache.txt"
    if (Test-Path $cache) {
        $line   = Select-String -Path $cache -Pattern "^CMAKE_GENERATOR:INTERNAL=" | Select-Object -First 1
        $cached = if ($line) { ($line.Line -split "=", 2)[1] } else { "" }
        if ($cached -ne $gen) {
            Write-Host "  build dir was configured with '$cached'; removing it to use '$gen'"
            Remove-Item -Recurse -Force (Join-Path $PSScriptRoot "build")
        }
    }
    cmake -B build -G $gen -A x64 | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed (see output above)." }
    cmake --build build --config Release | Out-Host
    if ($LASTEXITCODE -ne 0) { Fail "Build failed (see output above)." }
} finally { Pop-Location }
$exe = Join-Path $PSScriptRoot "build\Release\receiver.exe"
if (-not (Test-Path $exe)) { Fail "Build reported success but $exe is missing." }
Ok "built: $exe"

# --- [6] next steps ----------------------------------------------------------
Step 6 "Done - next steps"
$ips = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" }).IPAddress
Write-Host "  This PC's LAN IPv4 address(es): $($ips -join ', ')"
Write-Host "  On the sending machine: ./build/sender <one-of-those-ips>"
Write-Host "  (or just open the Android app - it discovers this PC by itself)"
Write-Host "  Verify pads here:  Win+R -> joy.cpl  (two 'Xbox 360 Controller' entries once running)"
$ans = Read-Host "`nStart the receiver now? (y/n)"
if ($ans -eq "y") { & $exe }
