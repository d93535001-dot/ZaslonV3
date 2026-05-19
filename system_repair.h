#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace Repair
{
    // 1. Registry Unlocking
    void UnlockTaskManager();
    void UnlockRegedit();
    void UnlockUAC();
    void ResetDefenderPolicies();
    void RestoreSafeBoot();
    void ResetSRPPolicies();
    
    // 2. Shell & Logon Fixes
    void FixShell();
    void FixUserinit();
    void ClearIFEO();
    void FixStickyKeys();
    
    // 3. Network & File Fixes
    void ResetHostsFile();
    void ResetWinsock();
    void FixFileExtensions();
    void DisableSMB1();

    // 4. Boot Integrity
    void MountAndScanEFI(); // Stub for logical EFI mounting
    void ResetBCD();
    
    // 5. Anti-Worm & Anti-WinLock
    void ImmunizeUSB();
    void CreateEmergencyDesktop();
    
    struct LogEntry {
        std::string timestamp; // HH:MM:SS
        std::string module;
        std::string message;
        bool success;
    };
    
    extern std::vector<LogEntry> g_RepairLogs;
    extern std::mutex g_LogsMutex; // protects g_RepairLogs from background threads
    void Log(const char* module, const char* message, bool success);
}

void SystemRepair_Render();
