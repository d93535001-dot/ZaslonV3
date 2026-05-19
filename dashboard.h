/**
 * dashboard.h
 * ZASLON v2.5.3 — System Health Dashboard
 *
 * Real-time system health score based on actual checks:
 *   - Registry restrictions (Task Manager, Regedit blocked)
 *   - Shell integrity (explorer.exe is the shell)
 *   - HOSTS file status (hijacked or clean)
 *   - IFEO debugger traps
 *   - File extension visibility
 */
#pragma once
#include <string>
#include <vector>

struct HealthCheck {
    std::string  Name;        // Display name
    int          Level;       // 0=OK  1=WARN  2=CRIT
    std::string  Details;     // Description
    std::string  FixAction;   // What the fix button does (empty = no fix)
    bool         CanFix;      // true if auto-fixable
    bool         Fixed;       // true after successful fix
};

void Dashboard_Render();
