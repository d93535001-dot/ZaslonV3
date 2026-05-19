/**
 * offline_registry.h
 * ZASLON — Offline Registry Management (Boot Surgeon & Account Manager)
 */
#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct ServiceEntry
{
    std::wstring Name;
    std::wstring ImagePath;
    DWORD        StartType;
    DWORD        ServiceType;
};

struct AccountEntry
{
    std::wstring Username;
    DWORD        Rid;
    bool         HasBlankPassword;
    bool         IsAdmin;
};

void OfflineRegistry_Render();
