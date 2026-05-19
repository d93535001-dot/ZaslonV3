/**
 * restrictions_manager.h
 * ZASLON v2.4 — System Restrictions Scanner
 *
 * Detects malware-planted restrictions:
 *   - IFEO debugger traps
 *   - DisallowRun policies
 *   - Keyboard layout (Scancode Map) manipulation
 */
#pragma once
#include <string>
#include <vector>

struct RestrictionEntry {
    std::wstring Name;
    std::wstring Value;
    std::wstring Location;
    bool         Suspicious;
};

void RestrictionsManager_Render();
void RestrictionsManager_Scan();
void RestrictionsManager_FixAll();
