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
# udp-probe.ps1 - listen on UDP 47800 and dump whatever arrives.
# Pure ASCII on purpose. Stands in for "ncat -u -l 47800" (HANDOFF T5) without
# installing Nmap, and additionally decodes wire protocol v3 headers so the
# same tool is useful for T6 diagnosis.
#
#   pwsh -NoProfile -ExecutionPolicy Bypass -File udp-probe.ps1 [-Seconds 60]
#
# Run this only while receiver.exe is STOPPED - they both want port 47800.

param([int]$Seconds = 60, [int]$Port = 47800)

$PT = @{ 1 = "INPUT"; 2 = "RUMBLE"; 3 = "HEARTBEAT"; 4 = "DISCONNECT"; 5 = "LATENCY" }

try {
    $udp = [System.Net.Sockets.UdpClient]::new($Port)
} catch {
    Write-Host "!! cannot bind UDP ${Port}: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "   is receiver.exe still running?" -ForegroundColor Red
    exit 1
}
$udp.Client.ReceiveTimeout = 500

$ips = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" })
Write-Host "listening on UDP $Port for $Seconds s. This PC's IPv4 addresses:" -ForegroundColor Cyan
foreach ($i in $ips) { Write-Host ("   {0,-16} {1}" -f $i.IPAddress, $i.InterfaceAlias) }
Write-Host ""

$deadline = (Get-Date).AddSeconds($Seconds)
$remote   = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
$count    = 0

while ((Get-Date) -lt $deadline) {
    try { $data = $udp.Receive([ref]$remote) } catch { continue }
    $count++
    $hex = ($data | ForEach-Object { $_.ToString("x2") }) -join " "
    $txt = -join ($data | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { "." } })
    $stamp = Get-Date -Format "HH:mm:ss.fff"
    Write-Host "[$stamp] from $($remote.Address):$($remote.Port)  $($data.Length) bytes" -ForegroundColor Green
    Write-Host "   hex  $hex"
    Write-Host "   text $txt"
    # decode protocol v3 header when the magic matches ("MX" = 4d 58)
    if ($data.Length -ge 6 -and $data[0] -eq 0x4d -and $data[1] -eq 0x58) {
        $ver  = $data[2]
        $type = $PT[[int]$data[3]]; if (-not $type) { $type = "0x{0:x2}" -f $data[3] }
        $cid  = if ($data[4] -eq 0xFF) { "none" } else { $data[4] }
        Write-Host "   PROTO v$ver type=$type controller=$cid seq=$($data[5])" -ForegroundColor Yellow
    }
    Write-Host ""
}
Write-Host "done. $count datagram(s) received." -ForegroundColor Cyan
$udp.Close()
