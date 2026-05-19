#pragma once
#include "imgui.h"
#include <string>

struct ZaslonTheme {
  // 1. Core Colors
  ImVec4 AccentColor = ImVec4(0.40f, 0.65f, 0.95f, 1.00f);
  ImVec4 WindowBg = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
  ImVec4 TextColor = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
  ImVec4 TextDisabled = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
  ImVec4 FrameBg = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
  ImVec4 HeaderBg = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
  ImVec4 ButtonBg = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
  ImVec4 PopupBg = ImVec4(0.15f, 0.15f, 0.15f, 0.96f);
  ImVec4 BorderColor = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);

  // 2. Gradients (Granular)
  bool UseBgGrad = false;
  ImVec4 BgGrad1 = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
  ImVec4 BgGrad2 = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
  float BgGradAlpha = 1.0f;

  bool UseNavGrad = false;
  ImVec4 NavGrad1 = ImVec4(0.18f, 0.18f, 0.19f, 1.00f);
  ImVec4 NavGrad2 = ImVec4(0.40f, 0.65f, 0.95f, 1.00f);

  bool UseBtnGrad = false;
  ImVec4 BtnGrad1 = ImVec4(0.40f, 0.65f, 0.95f, 1.00f);
  ImVec4 BtnGrad2 = ImVec4(0.20f, 0.35f, 0.65f, 1.00f);

  // 3. Effects & Patterns
  bool EnableShimmer = false;
  int PatternType = 0; // 0=None, 1=Dots, 2=Grid
  ImVec4 PatternCol = ImVec4(1.0f, 1.0f, 1.0f, 0.05f);

  // 4. State & System
  bool Minimalist = true;
  bool AlwaysOnTop = false;
  bool AdvancedColors = false;
  bool StealthMode = false;
  std::wstring CustomFontPath = L"";
};

extern ZaslonTheme g_Theme;

// Custom Animated Button renderer (Gradients + Shimmering support + Optional
// Icon)
bool ZaslonAnimatedButton(const char *label, const ImVec2 &size = ImVec2(0, 0),
                          void *customIcon = nullptr);

// Image Loader (stb_image to D3D9 Texture)
bool LoadTextureFromFile(const char *filename, void **out_texture,
                         int *out_width, int *out_height);

// Global WinPE detection flag — set once at startup
extern bool g_IsWinPE;

void ApplyZaslonTheme(const ZaslonTheme &theme);
void ThemeSettings_Render();
void LoadThemeSettings();
void SaveThemeSettings();
