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
# autostart.ps1 - run the receiver automatically at logon, in tray mode.
#
#   powershell -ExecutionPolicy Bypass -File .\autostart.ps1 -Install
#   powershell -ExecutionPolicy Bypass -File .\autostart.ps1 -Remove
#   powershell -ExecutionPolicy Bypass -File .\autostart.ps1          (status)
#
# Uses a shortcut in the per-user Startup folder rather than a scheduled task or
# a service: no administrator rights needed, it inherits the desktop session
# (which ViGEm and the tray icon both want), and it is trivial to inspect or
# delete by hand.
#
# Pure ASCII on purpose - see the encoding note in setup-pc.ps1.

param(
    [switch]$Install,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"

$exe      = Join-Path $PSScriptRoot "build\Release\receiver.exe"
$startup  = [Environment]::GetFolderPath("Startup")
$linkPath = Join-Path $startup "Controller Streamer.lnk"

function Show-Status {
    Write-Host ""
    if (Test-Path $linkPath) {
        Write-Host "  autostart: ENABLED" -ForegroundColor Green
        Write-Host "  shortcut:  $linkPath"
    } else {
        Write-Host "  autostart: not enabled" -ForegroundColor Yellow
    }
    $p = Get-Process receiver -ErrorAction SilentlyContinue
    if ($p) { Write-Host "  running:   yes (PID $($p.Id -join ', '))" }
    else    { Write-Host "  running:   no" }
    $log = Join-Path $env:LOCALAPPDATA "controller-streamer\receiver.log"
    if (Test-Path $log) { Write-Host "  log:       $log" }
    Write-Host ""
}

if ($Install) {
    if (-not (Test-Path $exe)) {
        Write-Host "  !!  $exe not found - build it first (setup-pc.ps1)" -ForegroundColor Red
        exit 1
    }
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut($linkPath)
    $lnk.TargetPath       = $exe
    $lnk.Arguments        = "--tray"
    $lnk.WorkingDirectory = Split-Path $exe
    $lnk.Description      = "Controller Streamer receiver (tray mode)"
    $lnk.WindowStyle      = 7          # start minimized
    $lnk.Save()
    Write-Host "  OK  autostart enabled" -ForegroundColor Green
    Write-Host "      it will start at your next logon; start it now with:"
    Write-Host "      Start-Process '$exe' -ArgumentList '--tray'"
    Show-Status
    exit 0
}

if ($Remove) {
    if (Test-Path $linkPath) {
        Remove-Item $linkPath -Force
        Write-Host "  OK  autostart removed" -ForegroundColor Green
    } else {
        Write-Host "  autostart was not enabled" -ForegroundColor Yellow
    }
    Show-Status
    exit 0
}

Write-Host "Controller Streamer autostart" -ForegroundColor Cyan
Show-Status
Write-Host "  -Install   enable autostart at logon (tray mode)"
Write-Host "  -Remove    disable it"
Write-Host ""
