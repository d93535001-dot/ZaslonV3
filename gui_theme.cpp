#define IMGUI_DEFINE_MATH_OPERATORS
#include "gui_theme.h"
#include "imgui_internal.h"
#include <algorithm>
#include "theme_manager.h"
#include <d3d9.h>
#include <math.h> // for sin()
#include <stdio.h>
#include <string>
#include <windows.h>
#include <commdlg.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

extern LPDIRECT3DDEVICE9 g_pd3dDevice; // From main.cpp

ZaslonTheme g_Theme;
bool g_IsWinPE = false;

void ApplyZaslonTheme(const ZaslonTheme &theme) {
  ImGuiStyle &style = ImGui::GetStyle();
  ImVec4 *colors = style.Colors;

  // VS Code Style Dark Minimalist
  colors[ImGuiCol_Text] = theme.TextColor;
  colors[ImGuiCol_TextDisabled] = theme.TextDisabled;
  
  ImVec4 winBg = theme.WindowBg;
  colors[ImGuiCol_WindowBg] = winBg;
  colors[ImGuiCol_ChildBg] = winBg;
  
  ImVec4 popupBg = theme.PopupBg;
  popupBg.w = theme.WindowBg.w; // Use main window alpha as base
  colors[ImGuiCol_PopupBg] = popupBg;
  
  colors[ImGuiCol_Border] = theme.BorderColor;
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  
  ImVec4 frameBg = theme.FrameBg;
  frameBg.w = theme.WindowBg.w * 0.8f; // Frames slightly more transparent
  colors[ImGuiCol_FrameBg] = frameBg;
  
  colors[ImGuiCol_FrameBgHovered] =
      ImVec4(theme.FrameBg.x + 0.1f, theme.FrameBg.y + 0.1f,
             theme.FrameBg.z + 0.1f, theme.WindowBg.w);
  colors[ImGuiCol_FrameBgActive] = theme.AccentColor;
  
  ImVec4 titleBg = theme.WindowBg;
  titleBg.w = theme.WindowBg.w;
  colors[ImGuiCol_TitleBg] = titleBg;
  colors[ImGuiCol_TitleBgActive] = theme.HeaderBg;
  
  ImVec4 headerBg = theme.HeaderBg;
  headerBg.w = theme.WindowBg.w;
  colors[ImGuiCol_Header] = headerBg;
  colors[ImGuiCol_HeaderHovered] =
      ImVec4(theme.HeaderBg.x + 0.1f, theme.HeaderBg.y + 0.1f,
             theme.HeaderBg.z + 0.1f, 1.00f);
  colors[ImGuiCol_HeaderActive] = theme.AccentColor;
  colors[ImGuiCol_Button] = theme.ButtonBg;
  colors[ImGuiCol_ButtonHovered] =
      ImVec4(theme.ButtonBg.x + 0.1f, theme.ButtonBg.y + 0.1f,
             theme.ButtonBg.z + 0.1f, 1.00f);
  colors[ImGuiCol_ButtonActive] = theme.AccentColor;
  colors[ImGuiCol_Separator] = theme.BorderColor;
  colors[ImGuiCol_Tab] = theme.WindowBg;
  colors[ImGuiCol_TabHovered] = theme.HeaderBg;
  colors[ImGuiCol_TabActive] = theme.AccentColor;
  colors[ImGuiCol_TabUnfocused] = theme.WindowBg;
  colors[ImGuiCol_TabUnfocusedActive] = theme.HeaderBg;
  colors[ImGuiCol_CheckMark] = theme.AccentColor;
  colors[ImGuiCol_SliderGrab] = theme.AccentColor;
  colors[ImGuiCol_SliderGrabActive] = theme.AccentColor;
  colors[ImGuiCol_TableHeaderBg] = theme.HeaderBg;
  colors[ImGuiCol_TableBorderStrong] =
      ImVec4(theme.BorderColor.x + 0.1f, theme.BorderColor.y + 0.1f,
             theme.BorderColor.z + 0.1f, 1.00f);
  colors[ImGuiCol_TableBorderLight] = theme.BorderColor;
  colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);

  // Styling metrics "breathing interface"
  style.WindowPadding = ImVec2(10.0f, 10.0f);
  style.FramePadding = ImVec2(6.0f, 4.0f);
  style.ItemSpacing = ImVec2(8.0f, 8.0f);
  style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);

  // Rounding
  style.WindowRounding = 8.0f; // Modern
  style.ChildRounding = 5.0f;
  style.FrameRounding = 5.0f;
  style.PopupRounding = 5.0f;
  style.ScrollbarRounding = 9.0f;
  style.TabRounding = 4.0f;

  // Borders
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;

  // Minimalist: flatten all rounding and reduce borders for a sleek look
  if (theme.Minimalist) {
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 0.5f;
    style.FrameBorderSize = 0.0f;
  }
}

// v2.5.3: Animated Custom Button rendering Gradient & Shimmering
bool ZaslonAnimatedButton(const char *label_raw, const ImVec2 &size_arg,
                          void *customIcon) {
  const char *label = _(label_raw); // Dynamic translation dictionary
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems)
    return false;

  ImGuiContext &g = *GImGui;
  const ImGuiStyle &style = g.Style;
  const ImGuiID id = window->GetID(label);

  // Calculate layout with icon
  ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
  ImVec2 total_content_size = label_size;
  const float icon_size = 20.0f; // fixed icon size for now
  if (customIcon) {
    total_content_size.x +=
        icon_size + style.ItemInnerSpacing.x; // Icon on the left
    total_content_size.y = std::max(total_content_size.y, icon_size);
  }

  ImVec2 pos = window->DC.CursorPos;
  ImVec2 size = ImGui::CalcItemSize(
      size_arg, total_content_size.x + style.FramePadding.x * 2.0f,
      total_content_size.y + style.FramePadding.y * 2.0f);

  const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
  ImGui::ItemSize(size, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id))
    return false;

  bool hovered, held;
  bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  ImU32 col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive
                                 : hovered         ? ImGuiCol_ButtonHovered
                                                   : ImGuiCol_Button);

  if (g_Theme.UseBtnGrad) {
    ImVec4 topColor = g_Theme.BtnGrad1;
    ImVec4 bottomColor = g_Theme.BtnGrad2;

    // Slightly highlight if hovered or clicked
    if (held && hovered) {
      topColor.x *= 0.7f; topColor.y *= 0.7f; topColor.z *= 0.7f;
      bottomColor.x *= 0.6f; bottomColor.y *= 0.6f; bottomColor.z *= 0.6f;
    } else if (hovered) {
      topColor.x *= 1.3f; topColor.y *= 1.3f; topColor.z *= 1.3f;
      bottomColor.x *= 1.1f; bottomColor.y *= 1.1f; bottomColor.z *= 1.1f;
    } else {
        // More distinct gradient in idle state
        topColor.x *= 1.1f; topColor.y *= 1.1f; topColor.z *= 1.1f;
        bottomColor.x *= 0.8f; bottomColor.y *= 0.8f; bottomColor.z *= 0.8f;
    }

    if (g_Theme.EnableShimmer) {
      float time = (float)ImGui::GetTime();
      float pulse = (sinf(time * 3.0f) + 1.0f) * 0.5f; // 0.0 to 1.0
      topColor.x += pulse * 0.15f;
      topColor.y += pulse * 0.15f;
      topColor.z += pulse * 0.15f;
      bottomColor.x += pulse * 0.15f;
      bottomColor.y += pulse * 0.15f;
      bottomColor.z += pulse * 0.15f;
    }

    draw_list->AddRectFilledMultiColor(
        bb.Min, bb.Max, ImGui::ColorConvertFloat4ToU32(topColor),
        ImGui::ColorConvertFloat4ToU32(topColor),
        ImGui::ColorConvertFloat4ToU32(bottomColor),
        ImGui::ColorConvertFloat4ToU32(bottomColor));
  } else {
    ImGui::RenderNavHighlight(bb, id);
    ImGui::RenderFrame(bb.Min, bb.Max, col, true, style.FrameRounding);
  }

  // Render contents inside button
  ImVec2 content_pos =
      ImVec2(bb.Min.x + std::max(style.FramePadding.x,
                            (bb.GetWidth() - total_content_size.x) * 0.5f),
             bb.Min.y + std::max(style.FramePadding.y,
                            (bb.GetHeight() - total_content_size.y) * 0.5f));

  if (customIcon) {
    draw_list->AddImage(
        customIcon, content_pos,
        ImVec2(content_pos.x + icon_size, content_pos.y + icon_size));
    content_pos.x += icon_size + style.ItemInnerSpacing.x;
    // Vertically center text if icon is taller
    if (label_size.y < icon_size) {
      content_pos.y += (icon_size - label_size.y) * 0.5f;
    }
  }

  ImGui::RenderTextClipped(
      content_pos,
      ImVec2(bb.Max.x - style.FramePadding.x, bb.Max.y - style.FramePadding.y),
      label, NULL, &label_size, ImVec2(0.0f, 0.0f), &bb);

  return pressed;
}

// v2.5.3 Loader for Custom Icons
bool LoadTextureFromFile(const char *filename, void **out_texture,
                         int *out_width, int *out_height) {
  if (!g_pd3dDevice)
    return false;

  int image_width = 0;
  int image_height = 0;
  unsigned char *image_data =
      stbi_load(filename, &image_width, &image_height, NULL, 4);
  if (image_data == NULL)
    return false;

  PDIRECT3DTEXTURE9 texture;
  HRESULT hr = g_pd3dDevice->CreateTexture(image_width, image_height, 1,
                                           D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                           D3DPOOL_DEFAULT, &texture, NULL);
  if (FAILED(hr)) {
    stbi_image_free(image_data);
    return false;
  }

  D3DLOCKED_RECT locked_rect;
  hr = texture->LockRect(0, &locked_rect, NULL, 0);
  if (SUCCEEDED(hr)) {
    for (int y = 0; y < image_height; y++) {
      const unsigned char *src = image_data + (y * image_width * 4);
      unsigned char *dst =
          (unsigned char *)locked_rect.pBits + (y * locked_rect.Pitch);
      for (int x = 0; x < image_width; x++) {
        // RGBA -> BGRA
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
        src += 4;
        dst += 4;
      }
    }
    texture->UnlockRect(0);
  } else {
    texture->Release();
    stbi_image_free(image_data);
    return false;
  }

  stbi_image_free(image_data);

  *out_texture = texture;
  *out_width = image_width;
  *out_height = image_height;
  return true;
}

void ThemeSettings_Render() {
  ImGui::TextColored(g_Theme.AccentColor, u8"Настройки интерфейса");
  ImGui::Separator();
  ImGui::Spacing();

  bool changed = false;
  if (ImGui::CollapsingHeader(u8"Базовые цвета", ImGuiTreeNodeFlags_DefaultOpen)) {
      changed |= ImGui::ColorEdit4(u8"Акцент", (float *)&g_Theme.AccentColor);
      changed |= ImGui::ColorEdit4(u8"Фон окна", (float *)&g_Theme.WindowBg);
      
      ImGui::Checkbox(u8"Расширенная настройка", &g_Theme.AdvancedColors);
      if (g_Theme.AdvancedColors) {
        ImGui::Indent();
        changed |= ImGui::ColorEdit4(u8"Текст", (float *)&g_Theme.TextColor);
        changed |= ImGui::ColorEdit4(u8"Тусклый текст", (float *)&g_Theme.TextDisabled);
        changed |= ImGui::ColorEdit4(u8"Элементы", (float *)&g_Theme.FrameBg);
        changed |= ImGui::ColorEdit4(u8"Заголовки", (float *)&g_Theme.HeaderBg);
        changed |= ImGui::ColorEdit4(u8"Кнопки", (float *)&g_Theme.ButtonBg);
        changed |= ImGui::ColorEdit4(u8"Всплывающие", (float *)&g_Theme.PopupBg);
        changed |= ImGui::ColorEdit4(u8"Рамки", (float *)&g_Theme.BorderColor);
        ImGui::Unindent();
      }
  }

  if (ImGui::CollapsingHeader(u8"Градиенты")) {
      if (ImGui::Checkbox(u8"Фон с градиентом", &g_Theme.UseBgGrad)) changed = true;
      if (g_Theme.UseBgGrad) {
          ImGui::Indent();
          if (ImGui::ColorEdit4(u8"Фон: Верх", (float*)&g_Theme.BgGrad1)) changed = true;
          if (ImGui::ColorEdit4(u8"Фон: Низ", (float*)&g_Theme.BgGrad2)) changed = true;
          if (ImGui::SliderFloat(u8"Прозрачность фона", &g_Theme.BgGradAlpha, 0.0f, 1.0f)) changed = true;
          ImGui::Unindent();
      }

      if (ImGui::Checkbox(u8"Навигация с градиентом", &g_Theme.UseNavGrad)) changed = true;
      if (g_Theme.UseNavGrad) {
          ImGui::Indent();
          if (ImGui::ColorEdit4(u8"Нав: Лево", (float*)&g_Theme.NavGrad1)) changed = true;
          if (ImGui::ColorEdit4(u8"Нав: Право", (float*)&g_Theme.NavGrad2)) changed = true;
          ImGui::Unindent();
      }

      if (ImGui::Checkbox(u8"Кнопки с градиентом", &g_Theme.UseBtnGrad)) changed = true;
      if (g_Theme.UseBtnGrad) {
          ImGui::Indent();
          if (ImGui::ColorEdit4(u8"Кнопка: Верх", (float*)&g_Theme.BtnGrad1)) changed = true;
          if (ImGui::ColorEdit4(u8"Кнопка: Низ", (float*)&g_Theme.BtnGrad2)) changed = true;
          ImGui::Unindent();
      }
  }


  if (changed) {
    ApplyZaslonTheme(g_Theme);
    SaveThemeSettings();
  }

  if (ImGui::Button(u8"Сброс к оригиналу", ImVec2(-1, 0))) {
    g_Theme = ZaslonTheme(); // Restore all defaults
    ApplyZaslonTheme(g_Theme);
    SaveThemeSettings();
  }

  // v2.5.3: 12 Preset themes
  ImGui::Spacing();
  ImGui::TextColored(g_Theme.AccentColor, u8"Быстрые темы");
  ImGui::Spacing();

  struct PresetTheme {
    const char *Name;
    ImVec4 Accent;
    ImVec4 WinBg;
    ImVec4 Text;
    ImVec4 HeadBg;
    ImVec4 Grad1;
    ImVec4 Grad2;
    bool Gradient;
    bool Shimmer;
  };

  static const PresetTheme presets[] = {
      {u8"Океанчик", ImVec4(0.20f, 0.60f, 0.90f, 1.0f),
       ImVec4(0.05f, 0.07f, 0.10f, 1.0f), ImVec4(0.90f, 0.95f, 1.00f, 1.0f),
       ImVec4(0.10f, 0.15f, 0.25f, 1.0f), ImVec4(0.15f, 0.50f, 0.85f, 1.0f),
       ImVec4(0.05f, 0.30f, 0.65f, 1.0f), true, true},
      {u8"Неончик", ImVec4(0.90f, 0.20f, 0.80f, 1.0f),
       ImVec4(0.08f, 0.03f, 0.10f, 1.0f), ImVec4(1.00f, 0.85f, 0.95f, 1.0f),
       ImVec4(0.25f, 0.08f, 0.20f, 1.0f), ImVec4(0.85f, 0.10f, 0.70f, 1.0f),
       ImVec4(0.50f, 0.00f, 0.90f, 1.0f), true, true},
      {u8"Огонёчек", ImVec4(0.95f, 0.40f, 0.10f, 1.0f),
       ImVec4(0.08f, 0.04f, 0.02f, 1.0f), ImVec4(1.00f, 0.90f, 0.80f, 1.0f),
       ImVec4(0.25f, 0.10f, 0.05f, 1.0f), ImVec4(0.95f, 0.50f, 0.00f, 1.0f),
       ImVec4(0.80f, 0.10f, 0.00f, 1.0f), true, true},
      {u8"Лесочек", ImVec4(0.20f, 0.70f, 0.30f, 1.0f),
       ImVec4(0.04f, 0.08f, 0.05f, 1.0f), ImVec4(0.85f, 1.00f, 0.90f, 1.0f),
       ImVec4(0.10f, 0.20f, 0.12f, 1.0f), ImVec4(0.30f, 0.80f, 0.40f, 1.0f),
       ImVec4(0.10f, 0.50f, 0.20f, 1.0f), true, false},
      {u8"Спейсик", ImVec4(0.50f, 0.30f, 0.90f, 1.0f),
       ImVec4(0.02f, 0.01f, 0.04f, 1.0f), ImVec4(0.90f, 0.85f, 1.00f, 1.0f),
       ImVec4(0.15f, 0.05f, 0.25f, 1.0f), ImVec4(0.60f, 0.20f, 0.95f, 1.0f),
       ImVec4(0.20f, 0.05f, 0.50f, 1.0f), true, true},
      {u8"Закатик", ImVec4(0.95f, 0.35f, 0.45f, 1.0f),
       ImVec4(0.10f, 0.05f, 0.08f, 1.0f), ImVec4(1.00f, 0.85f, 0.90f, 1.0f),
       ImVec4(0.30f, 0.15f, 0.20f, 1.0f), ImVec4(0.95f, 0.40f, 0.50f, 1.0f),
       ImVec4(0.70f, 0.20f, 0.60f, 1.0f), true, false},
      {u8"Рассветочек", ImVec4(1.00f, 0.70f, 0.30f, 1.0f),
       ImVec4(0.05f, 0.05f, 0.10f, 1.0f), ImVec4(0.95f, 0.95f, 1.00f, 1.0f),
       ImVec4(0.12f, 0.12f, 0.20f, 1.0f), ImVec4(1.00f, 0.60f, 0.40f, 1.0f),
       ImVec4(0.20f, 0.30f, 0.60f, 1.0f), true, true},
      {u8"Матрицочек", ImVec4(0.00f, 0.90f, 0.20f, 1.0f),
       ImVec4(0.02f, 0.05f, 0.02f, 1.0f), ImVec4(0.30f, 0.95f, 0.40f, 1.0f),
       ImVec4(0.05f, 0.15f, 0.05f, 1.0f), ImVec4(0.00f, 0.90f, 0.20f, 1.0f),
       ImVec4(0.00f, 0.40f, 0.10f, 1.0f), true, true},
      {u8"Желток", ImVec4(0.95f, 0.85f, 0.10f, 1.0f),
       ImVec4(0.08f, 0.08f, 0.12f, 1.0f), ImVec4(0.95f, 0.95f, 0.90f, 1.0f),
       ImVec4(0.20f, 0.20f, 0.25f, 1.0f), ImVec4(0.95f, 0.90f, 0.20f, 1.0f),
       ImVec4(0.10f, 0.60f, 0.95f, 1.0f), true, true},
      {u8"Дымочек", ImVec4(0.60f, 0.60f, 0.65f, 1.0f),
       ImVec4(0.10f, 0.10f, 0.11f, 1.0f), ImVec4(0.80f, 0.80f, 0.85f, 1.0f),
       ImVec4(0.20f, 0.20f, 0.22f, 1.0f), ImVec4(0.50f, 0.50f, 0.55f, 1.0f),
       ImVec4(0.30f, 0.30f, 0.32f, 1.0f), false, false},
      {u8"Сакурочек", ImVec4(0.95f, 0.60f, 0.70f, 1.0f),
       ImVec4(0.12f, 0.08f, 0.10f, 1.0f), ImVec4(0.95f, 0.85f, 0.90f, 1.0f),
       ImVec4(0.30f, 0.15f, 0.20f, 1.0f), ImVec4(0.95f, 0.65f, 0.75f, 1.0f),
       ImVec4(0.80f, 0.40f, 0.60f, 1.0f), true, true},
      {u8"Дракончик", ImVec4(0.85f, 0.10f, 0.15f, 1.0f),
       ImVec4(0.05f, 0.05f, 0.05f, 1.0f), ImVec4(0.95f, 0.95f, 0.95f, 1.0f),
       ImVec4(0.15f, 0.15f, 0.15f, 1.0f), ImVec4(0.85f, 0.15f, 0.20f, 1.0f),
       ImVec4(0.50f, 0.05f, 0.05f, 1.0f), false, false},
  };

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
  int cols = 3;
  for (int i = 0; i < 12; i++) {
    if (i % cols != 0)
      ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, presets[i].HeadBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, presets[i].Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, presets[i].Accent);
    ImGui::PushStyleColor(ImGuiCol_Text, presets[i].Text);

    if (ImGui::Button(
            presets[i].Name,
            ImVec2((ImGui::GetContentRegionAvail().x - (cols - 1) * 8) / cols,
                   35))) {
      g_Theme.AccentColor = presets[i].Accent;
      g_Theme.WindowBg = presets[i].WinBg;
      g_Theme.TextColor = presets[i].Text;
      g_Theme.HeaderBg = presets[i].HeadBg;
      g_Theme.ButtonBg = presets[i].HeadBg;
      g_Theme.FrameBg = presets[i].HeadBg;
      g_Theme.PopupBg = presets[i].WinBg;
      g_Theme.BtnGrad1 = presets[i].Grad1;
      g_Theme.BtnGrad2 = presets[i].Grad2;
      g_Theme.UseBtnGrad = presets[i].Gradient;
      g_Theme.EnableShimmer = presets[i].Shimmer;
      g_Theme.BorderColor = presets[i].Accent;
      ApplyZaslonTheme(g_Theme);
      SaveThemeSettings();
    }
    ImGui::PopStyleColor(4);
  }
  ImGui::PopStyleVar();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(g_Theme.AccentColor, u8"Экспорт / Импорт");

  static std::vector<std::string> localThemes;
  static int selectedThemeIdx = -1;
  static bool scannedThemes = false;

  if (!scannedThemes) {
    WIN32_FIND_DATAW fdw;
    HANDLE hFind = FindFirstFileW(L"*.ztheme", &fdw);
    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        char nb[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fdw.cFileName, -1, nb, MAX_PATH, NULL,
                            NULL);
        localThemes.push_back(nb);
      } while (FindNextFileW(hFind, &fdw));
      FindClose(hFind);
    }
    scannedThemes = true;
  }

  if (localThemes.empty()) {
    ImGui::TextDisabled(u8"Темы (.ztheme) в текущей папке не найдены");
  } else {
    if (ImGui::BeginCombo(u8"Доступные темы",
                          selectedThemeIdx >= 0
                              ? localThemes[selectedThemeIdx].c_str()
                              : u8"Выберите тему...")) {
      for (int i = 0; i < localThemes.size(); i++) {
        if (ImGui::Selectable(localThemes[i].c_str(), selectedThemeIdx == i)) {
          selectedThemeIdx = i;
        }
      }
      ImGui::EndCombo();
    }
    if (selectedThemeIdx >= 0) {
      ImGui::SameLine();
      if (ImGui::Button(u8"Загрузить##theme")) {
        int wsize = MultiByteToWideChar(
            CP_UTF8, 0, localThemes[selectedThemeIdx].c_str(), -1, NULL, 0);
        std::wstring wpath(wsize, 0);
        MultiByteToWideChar(CP_UTF8, 0, localThemes[selectedThemeIdx].c_str(),
                            -1, &wpath[0], wsize);
        if (wpath.length() > 0 && wpath.back() == L'\0')
          wpath.pop_back();
        ThemeManager::ImportTheme(wpath);
      }
    }
  }

  if (ImGui::Button(u8"Экспорт текущей темы", ImVec2(-1, 0))) {
    ThemeManager::ExportTheme(L"my_zaslon_theme.ztheme");
    scannedThemes = false; // force rescan to show the new theme
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextColored(g_Theme.AccentColor, u8"Кастомные шрифты");
  // WARNING MESSAGE
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
  ImGui::TextWrapped(
      u8"Рискуешь вась: Если что то решит подменить ваш кастомнй шрифт, "
      u8"интерфейс программы может стать нечитаемым. "
      u8"Для сброса темы, удерживайте Shift при запуске программы.");
  ImGui::PopStyleColor();

  static std::vector<std::string> localFonts;
  static int selectedFontIdx = -1;
  static bool scannedFonts = false;

  if (!scannedFonts) {
    WIN32_FIND_DATAW fdw;
    HANDLE hFind = FindFirstFileW(L"*.ttf", &fdw);
    if (hFind != INVALID_HANDLE_VALUE) {
      do {
        char nb[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fdw.cFileName, -1, nb, MAX_PATH, NULL,
                            NULL);
        localFonts.push_back(nb);
      } while (FindNextFileW(hFind, &fdw));
      FindClose(hFind);
    }
    scannedFonts = true;
  }

  if (localFonts.empty()) {
    ImGui::TextDisabled(u8"Шрифты (.ttf) в текущей папке не найдены");
  } else {
    if (ImGui::BeginCombo(u8"Доступные шрифты",
                          selectedFontIdx >= 0
                              ? localFonts[selectedFontIdx].c_str()
                              : u8"Выберите шрифт...")) {
      for (int i = 0; i < localFonts.size(); i++) {
        if (ImGui::Selectable(localFonts[i].c_str(), selectedFontIdx == i)) {
          selectedFontIdx = i;
        }
      }
      ImGui::EndCombo();
    }
    if (selectedFontIdx >= 0) {
      ImGui::SameLine();
      if (ImGui::Button(u8"Применить (Требуется перезапуск)##font")) {
        int wsize = MultiByteToWideChar(
            CP_UTF8, 0, localFonts[selectedFontIdx].c_str(), -1, NULL, 0);
        std::wstring wpath(wsize, 0);
        MultiByteToWideChar(CP_UTF8, 0, localFonts[selectedFontIdx].c_str(), -1,
                            &wpath[0], wsize);
        if (wpath.length() > 0 && wpath.back() == L'\0')
          wpath.pop_back();
        g_Theme.CustomFontPath = wpath;
        SaveThemeSettings();
      }
    }
  }

  ImGui::SameLine();
  if (ImGui::Button(u8"Обзор... (Требуется перезапуск)##fontbrowse")) {
    wchar_t szFile[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"Fonts (*.ttf; *.otf)\0*.ttf;*.otf\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
      g_Theme.CustomFontPath = szFile;
      SaveThemeSettings();
    }
  }

  if (!g_Theme.CustomFontPath.empty()) {
    char nb[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, g_Theme.CustomFontPath.c_str(), -1, nb,
                        MAX_PATH, NULL, NULL);
    ImGui::TextColored(g_Theme.AccentColor, u8"Текущий шрифт: %s", nb);
    if (ImGui::Button(u8"Удалить кастомный шрифт", ImVec2(-1, 0))) {
      g_Theme.CustomFontPath = L"";
      SaveThemeSettings();
    }
  }

  changed |= ImGui::Checkbox(u8"Плоский", &g_Theme.Minimalist);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    ApplyZaslonTheme(g_Theme);
    SaveThemeSettings();
  }

  ImGui::Checkbox(u8"Поверх всех окон(Рекомендую)", &g_Theme.AlwaysOnTop);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    SaveThemeSettings();
  }

  if (ImGui::Checkbox(u8"Невидимость", &g_Theme.StealthMode)) {
    SaveThemeSettings();
  }
}

void SaveThemeSettings() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring ini = path;
  ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\zaslon_ui.ini";

  auto saveColor = [&](const wchar_t *key, ImVec4 c) {
    wchar_t buf[64];
    swprintf_s(buf, L"%f,%f,%f,%f", c.x, c.y, c.z, c.w);
    WritePrivateProfileStringW(L"Theme", key, buf, ini.c_str());
  };

  saveColor(L"Accent", g_Theme.AccentColor);
  saveColor(L"WindowBg", g_Theme.WindowBg);
  saveColor(L"TextColor", g_Theme.TextColor);
  saveColor(L"TextDisabled", g_Theme.TextDisabled);
  saveColor(L"FrameBg", g_Theme.FrameBg);
  saveColor(L"HeaderBg", g_Theme.HeaderBg);
  saveColor(L"ButtonBg", g_Theme.ButtonBg);
  saveColor(L"PopupBg", g_Theme.PopupBg);
  saveColor(L"BorderColor", g_Theme.BorderColor);
  saveColor(L"BtnGrad1", g_Theme.BtnGrad1);
  saveColor(L"BtnGrad2", g_Theme.BtnGrad2);
  saveColor(L"NavGrad1", g_Theme.NavGrad1);
  saveColor(L"NavGrad2", g_Theme.NavGrad2);
  saveColor(L"BgGrad1", g_Theme.BgGrad1);
  saveColor(L"BgGrad2", g_Theme.BgGrad2);
  saveColor(L"PatternCol", g_Theme.PatternCol);

  wchar_t fVal[32];
  swprintf_s(fVal, L"%f", g_Theme.BgGradAlpha);
  WritePrivateProfileStringW(L"Theme", L"BgGradAlpha", fVal, ini.c_str());

  WritePrivateProfileStringW(L"Theme", L"Minimal",
                             g_Theme.Minimalist ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"OnTop",
                             g_Theme.AlwaysOnTop ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"AdvColors",
                             g_Theme.AdvancedColors ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"Stealth",
                             g_Theme.StealthMode ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"UseBtnGrad",
                             g_Theme.UseBtnGrad ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"UseNavGrad",
                             g_Theme.UseNavGrad ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"UseBgGrad",
                             g_Theme.UseBgGrad ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"Theme", L"EnableShimmer",
                             g_Theme.EnableShimmer ? L"1" : L"0", ini.c_str());
  
  wchar_t pType[16]; swprintf_s(pType, L"%d", g_Theme.PatternType);
  WritePrivateProfileStringW(L"Theme", L"Pattern", pType, ini.c_str());
  
  WritePrivateProfileStringW(L"Theme", L"FontPath", g_Theme.CustomFontPath.c_str(), ini.c_str());
}

void LoadThemeSettings() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring ini = path;
  ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\zaslon_ui.ini";

  auto loadColor = [&](const wchar_t *key, ImVec4 &c) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"Theme", key, L"", buf, 64, ini.c_str());
    if (buf[0])
      swscanf_s(buf, L"%f,%f,%f,%f", &c.x, &c.y, &c.z, &c.w);
  };

  loadColor(L"Accent", g_Theme.AccentColor);
  loadColor(L"WindowBg", g_Theme.WindowBg);
  loadColor(L"TextColor", g_Theme.TextColor);
  loadColor(L"TextDisabled", g_Theme.TextDisabled);
  loadColor(L"FrameBg", g_Theme.FrameBg);
  loadColor(L"HeaderBg", g_Theme.HeaderBg);
  loadColor(L"ButtonBg", g_Theme.ButtonBg);
  loadColor(L"PopupBg", g_Theme.PopupBg);
  loadColor(L"BorderColor", g_Theme.BorderColor);
  loadColor(L"BtnGrad1", g_Theme.BtnGrad1);
  loadColor(L"BtnGrad2", g_Theme.BtnGrad2);
  loadColor(L"NavGrad1", g_Theme.NavGrad1);
  loadColor(L"NavGrad2", g_Theme.NavGrad2);
  loadColor(L"BgGrad1", g_Theme.BgGrad1);
  loadColor(L"BgGrad2", g_Theme.BgGrad2);
  loadColor(L"PatternCol", g_Theme.PatternCol);

  wchar_t buf[64];
  GetPrivateProfileStringW(L"Theme", L"BgGradAlpha", L"1.0", buf, 64, ini.c_str());
  g_Theme.BgGradAlpha = (float)_wtof(buf);

  g_Theme.Minimalist =
      GetPrivateProfileIntW(L"Theme", L"Minimal", 1, ini.c_str()) != 0;
  g_Theme.AlwaysOnTop =
      GetPrivateProfileIntW(L"Theme", L"OnTop", 0, ini.c_str()) != 0;
  g_Theme.AdvancedColors =
      GetPrivateProfileIntW(L"Theme", L"AdvColors", 0, ini.c_str()) != 0;
  g_Theme.StealthMode =
      GetPrivateProfileIntW(L"Theme", L"Stealth", 0, ini.c_str()) != 0;
  g_Theme.UseBtnGrad =
      GetPrivateProfileIntW(L"Theme", L"UseBtnGrad", 0, ini.c_str()) != 0;
  g_Theme.UseNavGrad =
      GetPrivateProfileIntW(L"Theme", L"UseNavGrad", 0, ini.c_str()) != 0;
  g_Theme.UseBgGrad =
      GetPrivateProfileIntW(L"Theme", L"UseBgGrad", 0, ini.c_str()) != 0;
  g_Theme.EnableShimmer =
      GetPrivateProfileIntW(L"Theme", L"EnableShimmer", 0, ini.c_str()) != 0;
  g_Theme.PatternType =
      GetPrivateProfileIntW(L"Theme", L"Pattern", 0, ini.c_str());

  wchar_t fPath[MAX_PATH] = {};
  GetPrivateProfileStringW(L"Theme", L"FontPath", L"", fPath, MAX_PATH, ini.c_str());
  g_Theme.CustomFontPath = fPath;
}
