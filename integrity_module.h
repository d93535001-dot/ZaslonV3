/**
 * integrity_module.h
 * ZASLON — System Integrity Verification and File Replacement
 */
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "imgui.h"

struct IntegrityEntry
{
    std::wstring FileName;       // e.g. L"sethc.exe"
    std::wstring FullPath;       // e.g. L"C:\\Windows\\System32\\sethc.exe"
    std::wstring ExpectedSHA256; // hex string, empty = unknown
    std::wstring ActualSHA256;
    bool         HashMismatch;
    bool         FileExists;
    DWORD        FileSizeBytes;
    FILETIME     LastModified;
};

struct WinlogonEntry
{
    std::wstring ValueName;
    std::wstring ValueData;
    bool         Suspicious;
};

void IntegrityModule_Render();
