# unblock_taskmgr.ps1
# This script restores Task Manager
# Re-launches as Administrator if necessary

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Requesting Administrator privileges..." -ForegroundColor Cyan
    Start-Process powershell.exe "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

$RegistryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Policies\System"
if (Test-Path $RegistryPath) {
    Remove-ItemProperty -Path $RegistryPath -Name "DisableTaskMgr" -ErrorAction SilentlyContinue
}

$RegistryPathLM = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System"
if (Test-Path $RegistryPathLM) {
    Remove-ItemProperty -Path $RegistryPathLM -Name "DisableTaskMgr" -ErrorAction SilentlyContinue
}

# Clearing Image File Execution Options
$IFEOPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\taskmgr.exe"
if (Test-Path $IFEOPath) {
    Remove-Item -Path $IFEOPath -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Task Manager has been unblocked." -ForegroundColor Green
