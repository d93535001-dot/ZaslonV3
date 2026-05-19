# Prepare resources for ZASLON build
# This script copies the necessary DirectX DLLs to the local resources folder

$ProjectDir = Get-Location
$ResDir = Join-Path $ProjectDir "resources"

if (-not (Test-Path $ResDir)) {
    New-Item -ItemType Directory -Path $ResDir
}

# Find d3d9.dll (assuming 64-bit for x64 build)
$SysDir = [System.Environment]::SystemDirectory
$D3D9Path = Join-Path $SysDir "d3d9.dll"
$D3D8ThkPath = Join-Path $SysDir "d3d8thk.dll"

if (Test-Path $D3D9Path) {
    Write-Host "Copying d3d9.dll..."
    Copy-Item $D3D9Path (Join-Path $ResDir "d3d9.dll") -Force
} else {
    Write-Warning "d3d9.dll not found in $SysDir"
}

if (Test-Path $D3D8ThkPath) {
    Write-Host "Copying d3d8thk.dll..."
    Copy-Item $D3D8ThkPath (Join-Path $ResDir "d3d8thk.dll") -Force
}

$OtherDLLs = @("Rstrtmgr.dll", "iphlpapi.dll", "netapi32.dll")
foreach ($dll in $OtherDLLs) {
    $dllPath = Join-Path $SysDir $dll
    if (Test-Path $dllPath) {
        Write-Host "Copying $dll..."
        Copy-Item $dllPath (Join-Path $ResDir $dll) -Force
    }
}

Write-Host "Resources prepared successfully."
