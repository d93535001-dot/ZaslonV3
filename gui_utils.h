/**
 * gui_utils.h
 * Utilities for ImGui setup
 */
#pragma once
#include <windows.h>
#include "imgui.h"

//
// Pre-compiled compressed TTF string for Russian support
// (A tiny custom subset of Roboto or standard font; here we use base85 dummy for outline,
//  real app would inject full bytes. We'll rely on ImGui standard proggy for now + Russian ranges)
//
#include "tahoma_bytes.h"

inline void LoadRussianFont(ImGuiIO& io)
{
    // Load default ImGui font first (ProggyClean)
    io.Fonts->AddFontDefault();

    ImFontConfig config;
    config.MergeMode = true; // Merge Cyrillic into the default font
    config.FontDataOwnedByAtlas = false; // We own the static array
    config.OversampleH = 1;
    config.OversampleV = 1;

    // Cyrillic ranges
    static const ImWchar ranges[] =
    {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0400, 0x044F, // Cyrillic
        0,
    };

    io.Fonts->AddFontFromMemoryTTF((void*)tahoma_data, tahoma_size, 16.0f, &config, ranges);
}
