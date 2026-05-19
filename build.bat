@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ========================================
echo ZASLON Production Build Pipeline
echo ========================================

:: 1. Validate environment
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [CRITICAL] CMake not found in PATH.
    echo Please install CMake 3.25+ and add it to your PATH.
    exit /b 1
)

where powershell >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [CRITICAL] PowerShell not found. Required for resource prep.
    exit /b 1
)

:: 2. Check for clean target
if /I "%~1"=="clean" (
    echo [INFO] Cleaning build and bin directories...
    if exist "build" rmdir /s /q "build"
    if exist "bin" rmdir /s /q "bin"
    echo [OK] Clean completed.
    exit /b 0
)

:: 3. Parse arguments
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
if /I not "%ARCH%"=="x64" (
    echo [ERROR] Invalid architecture '%ARCH%'. Only 'x64' is supported by default presets.
    exit /b 1
)

set CONFIG=%2
if "%CONFIG%"=="" set CONFIG=Release
if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo [ERROR] Invalid configuration '%CONFIG%'. Use 'Debug' or 'Release'.
    exit /b 1
)

set PRESET=%ARCH%-%CONFIG%
echo [INFO] Target Preset: %PRESET%

:: 4. Locate Visual Studio via vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)
if "%VS_PATH%"=="" (
    echo [CRITICAL] Visual Studio 2022 with C++ tools not found.
    exit /b 1
)
echo [INFO] Found Visual Studio at: %VS_PATH%

:: 5. Prepare embedded resources
echo [INFO] Executing resource preparation...
powershell -ExecutionPolicy Bypass -File prepare_resources.ps1
if %ERRORLEVEL% NEQ 0 (
    echo [CRITICAL] Resource preparation failed!
    exit /b 1
)

:: 6. CMake configuration
echo [INFO] Generating build system for preset '%PRESET%'...
cmake --preset %PRESET%
if %ERRORLEVEL% NEQ 0 (
    echo [CRITICAL] CMake configuration failed.
    exit /b 1
)

:: 7. Compilation
echo [INFO] Compiling Zaslon...
cmake --build --preset %PRESET% --parallel
if %ERRORLEVEL% NEQ 0 (
    echo [CRITICAL] Build failed! Check compiler logs.
    exit /b 1
)

:: 8. Code Signing (Optional but recommended)
set TARGET_EXE="bin\%CONFIG%\zaslon.exe"
if not exist %TARGET_EXE% (
    echo [CRITICAL] Executable not found at %TARGET_EXE%. Build might have failed silently or output dir changed.
    exit /b 1
)

echo [INFO] Checking for signtool...
where signtool >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    echo [INFO] Applying Authenticode signature...
    signtool sign /a /fd SHA256 /t http://timestamp.digicert.com %TARGET_EXE%
    if %ERRORLEVEL% EQU 0 (
        echo [OK] Binary signed successfully.
    ) else (
        echo [WARN] Signing failed. Output is unsigned.
    )
) else (
    echo [WARN] signtool not found. Skipping code signing.
)

:: 9. Output location
echo ========================================
echo [SUCCESS] Build completed successfully.
echo Output binary: %TARGET_EXE%
echo ========================================
exit /b 0
