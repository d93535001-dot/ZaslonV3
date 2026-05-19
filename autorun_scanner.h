/**
 * autorun_scanner.h
 * ZASLON — Advanced Autorun & WMI Persistence Scanner
 */
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "imgui.h"

enum class AutoRunType {
    Registry,
    Task,
    Service,
    WMISubscription
};

struct AutoRunEntry {
    std::wstring        Name;
    std::wstring        Path;
    std::wstring        Location;
    AutoRunType         Type;
    bool                DangerInfo;
    bool                Selected = false;
};

void AutorunScanner_Render();
void AutorunScanner_Refresh();
void AutorunScanner_PurgeWMI();
