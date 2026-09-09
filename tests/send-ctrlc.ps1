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
