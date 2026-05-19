#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include "gui_theme.h"

// ─── Dictionary for renaming UI elements ────────────────────────────────
extern std::unordered_map<std::string, std::string> g_CustomStrings;

// Localization wrapper
const char* _(const char* defaultText);

// ─── Theme Manager ──────────────────────────────────────────────────────
namespace ThemeManager {

    // Export current g_Theme to a .ztheme file
    bool ExportTheme(const std::wstring& filePath);

    // Import a .ztheme file and apply it to g_Theme
    bool ImportTheme(const std::wstring& filePath);

    // Load custom font (if specified in theme)
    void ReloadFonts();

    // Check if shift is held down during startup to reset theme (Emergency escape hatch)
    bool CheckEmergencyReset();
}
