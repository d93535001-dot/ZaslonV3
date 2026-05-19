# block_taskmgr.ps1
# This script blocks Task Manager for testing purposes (ZASLON)
# Re-launches as Administrator if necessary

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Requesting Administrator privileges..." -ForegroundColor Cyan
    Start-Process powershell.exe "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

$RegistryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Policies\System"
if (-not (Test-Path $RegistryPath)) {
    New-Item -Path $RegistryPath -Force | Out-Null
}
Set-ItemProperty -Path $RegistryPath -Name "DisableTaskMgr" -Value 1

$RegistryPathLM = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System"
if (-not (Test-Path $RegistryPathLM)) {
    New-Item -Path $RegistryPathLM -Force | Out-Null
}
Set-ItemProperty -Path $RegistryPathLM -Name "DisableTaskMgr" -Value 1

# Using Image File Execution Options to redirect taskmgr.exe
$IFEOPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe"
if (-not (Test-Path $IFEOPath)) {
    New-Item -Path $IFEOPath -Force | Out-Null
}
Set-ItemProperty -Path $IFEOPath -Name "Debugger" -Value 'cmd.exe /c "echo Task Manager is disabled for ZASLON testing & pause"'

Write-Host "Task Manager has been blocked." -ForegroundColor Red
Write-Host "Try pressing Ctrl+Shift+Esc or running taskmgr.exe." -ForegroundColor Yellow
