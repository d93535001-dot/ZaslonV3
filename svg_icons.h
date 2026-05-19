#pragma once

#include "imgui.h"
#include "gui_theme.h"

namespace ZaslonGUI {

// Draws one of the embedded icons as vector primitives
void DrawIcon(int iconId, ImVec2 pos, float size, ImU32 color);

// Icon IDs
enum Icons {
    ICON_DASHBOARD = 0,
    ICON_PROCESSES = 1,
    ICON_FILES = 2,
    ICON_SYSTEM = 3,
    ICON_REGISTRY = 4,
    ICON_AUTORUN = 5,
    ICON_RECOVERY = 6,
    ICON_RESTRICTIONS = 7,
    ICON_USERS = 8,
    ICON_HELP = 9,
    ICON_SETTINGS = 10,
    ICON_INSTALLER = 11
};

} // namespace ZaslonGUI
