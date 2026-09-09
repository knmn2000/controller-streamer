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
# send-ctrlc.ps1 - deliver a genuine CTRL_C_EVENT to another console process,
# so we exercise the receiver's SIGINT handler and its teardown order (T4)
# instead of hard-killing it. Pure ASCII.
param([Parameter(Mandatory=$true)][int]$TargetPid)
Add-Type -Namespace W -Name K -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint p);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint e, uint g);
'@
[void][W.K]::FreeConsole()
if (-not [W.K]::AttachConsole([uint32]$TargetPid)) {
    Write-Output "AttachConsole($TargetPid) failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
    exit 1
}
[void][W.K]::SetConsoleCtrlHandler([IntPtr]::Zero, $true)   # do not kill ourselves
[void][W.K]::GenerateConsoleCtrlEvent(0, 0)                 # 0 = CTRL_C_EVENT
Start-Sleep -Milliseconds 300
exit 0
