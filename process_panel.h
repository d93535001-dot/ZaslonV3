/**
 * process_panel.h
 * ZASLON — Process Tree UI Panel
 */
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include "imgui.h"

// PFLAG Definitions
#define ZASLON_PFLAG_SYSTEM      (1 << 0)
#define ZASLON_PFLAG_PROTECTED   (1 << 1)
#define ZASLON_PFLAG_CRITICAL    (1 << 2) // Found by NtQueryInformationProcess
#define ZASLON_PFLAG_FROZEN      (1 << 3)
#define ZASLON_PFLAG_FAKE_SYS    (1 << 4) // Suspicious! AppData svchost etc

struct ProcessNode
{
    ULONG               Pid = 0;
    ULONG               ParentPid = 0;
    ULONG               ThreadCount = 0;
    ULONG               HandleCount = 0;
    ULONG               Flags = 0;      // ZASLON_PFLAG_*
    std::wstring        ImageName;
    std::wstring        FullPath;
    std::wstring        CommandLine;
    std::wstring        UserName;
    SIZE_T              WorkingSetSize = 0;
    SIZE_T              PrivateUsage = 0;
    FILETIME            CreateTime = {};
    bool                MicrosoftSigned = false;
    std::wstring        FileDescription;
    int                 RiskScore = 0;
    std::vector<std::string> RiskReasons;
    float               CpuUsagePercent = 0.0f;
    std::vector<int>    Children;
};

void ProcessPanel_Render();
void ProcessPanel_Refresh();
