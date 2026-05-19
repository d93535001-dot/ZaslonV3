
#include "restrictions_manager.h"
#include "imgui.h"
#include "gui_theme.h"
#include <string>
#include <vector>
#include <windows.h>

static std::vector<RestrictionEntry> g_Restrictions;
static bool g_Scanned = false;

// ── Scan logic ──────────────────────────────────────────────────────────

void RestrictionsManager_Scan() {
  g_Restrictions.clear();

  auto scanValues = [&](HKEY hRoot, const wchar_t *subKey,
                        const wchar_t *rootLabel) {
    HKEY hk;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
      return;
    DWORD idx = 0;
    wchar_t valName[1024];
    BYTE data[2048];
    while (true) {
      DWORD nLen = 1024, dLen = 2048, type;
      if (RegEnumValueW(hk, idx++, valName, &nLen, nullptr, &type, data,
                        &dLen) != ERROR_SUCCESS)
        break;

      RestrictionEntry e;
      e.Name = valName;
      e.Location = std::wstring(rootLabel) + L"\\" + subKey;
      e.Suspicious = true;
      if (type == REG_SZ || type == REG_EXPAND_SZ)
        e.Value = (wchar_t *)data;
      else if (type == REG_DWORD)
        e.Value = std::to_wstring(*(DWORD *)data);
      else
        e.Value = L"[Binary]";
      g_Restrictions.push_back(e);
    }
    RegCloseKey(hk);
  };

  // 1. DisallowRun (HKCU + HKLM)
  scanValues(HKEY_CURRENT_USER,
             L"Software\\Microsoft\\Windows\\CurrentVersion"
             L"\\Policies\\Explorer\\DisallowRun",
             L"HKCU");
  scanValues(HKEY_LOCAL_MACHINE,
             L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
             L"\\Policies\\Explorer\\DisallowRun",
             L"HKLM");

  // 2. Keyboard layout (Scancode Map)
  scanValues(HKEY_LOCAL_MACHINE,
             L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layout", L"HKLM");

  // 3. IFEO Debugger entries
  HKEY hIfeo;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
                    L"\\Image File Execution Options",
                    0, KEY_ENUMERATE_SUB_KEYS, &hIfeo) == ERROR_SUCCESS) {
    wchar_t sub[256];
    DWORD idx = 0, len = 256;
    while (RegEnumKeyExW(hIfeo, idx++, sub, &len, nullptr, nullptr, nullptr,
                         nullptr) == ERROR_SUCCESS) {
      HKEY hSub;
      if (RegOpenKeyExW(hIfeo, sub, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
        wchar_t dbg[1024];
        DWORD dsz = sizeof(dbg);
        if (RegQueryValueExW(hSub, L"Debugger", nullptr, nullptr, (LPBYTE)dbg,
                             &dsz) == ERROR_SUCCESS) {
          RestrictionEntry e;
          e.Name = std::wstring(sub) + L"\\Debugger";
          e.Value = dbg;
          e.Location = L"HKLM\\...\\Image File Execution Options";
          e.Suspicious = true;
          g_Restrictions.push_back(e);
        }
        RegCloseKey(hSub);
      }
      len = 256;
    }
    RegCloseKey(hIfeo);
  }
  g_Scanned = true;
}

// ── Fix all ─────────────────────────────────────────────────────────────

void RestrictionsManager_FixAll() {
  for (auto &e : g_Restrictions) {
    HKEY root = nullptr;
    std::wstring subKey;
    if (e.Location.find(L"HKLM\\") == 0) {
      root = HKEY_LOCAL_MACHINE;
      subKey = e.Location.substr(5);
    } else if (e.Location.find(L"HKCU\\") == 0) {
      root = HKEY_CURRENT_USER;
      subKey = e.Location.substr(5);
    }
    if (!root)
      continue;

    // IFEO: nested key handling
    if (e.Location.find(L"Image File Execution Options") !=
        std::wstring::npos) {
      size_t slash = e.Name.find_last_of(L"\\");
      if (slash != std::wstring::npos) {
        std::wstring realSub =
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
            L"\\Image File Execution Options\\" +
            e.Name.substr(0, slash);
        std::wstring val = e.Name.substr(slash + 1);
        HKEY hk;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, realSub.c_str(), 0, KEY_SET_VALUE,
                          &hk) == ERROR_SUCCESS) {
          RegDeleteValueW(hk, val.c_str());
          RegCloseKey(hk);
        }
      }
    } else {
      HKEY hk;
      if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &hk) ==
          ERROR_SUCCESS) {
        RegDeleteValueW(hk, e.Name.c_str());
        RegCloseKey(hk);
      }
    }
  }
  // Re-scan to confirm removal
  RestrictionsManager_Scan();
}

// ── GUI ─────────────────────────────────────────────────────────────────

void RestrictionsManager_Render() {
  ImGui::TextColored(g_Theme.AccentColor, u8"Менеджер ограничений");
  ImGui::TextWrapped(
      u8"Сканирует реестр на мусорные правила, запрещающие запуск программ "
      u8"перехваты IFEO Debugger и манипуляции с клавиатурой.");
  ImGui::Separator();
  ImGui::Spacing();

  if (ImGui::Button(u8"Сканировать реестр", ImVec2(200, 30)))
    RestrictionsManager_Scan();

  if (!g_Scanned) {
    ImGui::SameLine();
    ImGui::TextDisabled(u8"(нажмите для сканирования)");
    return;
  }

  ImGui::SameLine(0, 20.0f);
  if (g_Restrictions.empty()) {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                       u8"Ограничений не обнаружено.");
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"Найдено записей: %d",
                       (int)g_Restrictions.size());
  }

  ImGui::Spacing();

  if (ImGui::BeginTable("RestTable", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                        ImVec2(0, 0))) {

    ImGui::TableSetupColumn(u8"Параметр", ImGuiTableColumnFlags_WidthFixed,
                            250.0f);
    ImGui::TableSetupColumn(u8"Значение", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(u8"Расположение", ImGuiTableColumnFlags_WidthFixed,
                            220.0f);
    ImGui::TableHeadersRow();

    for (auto &e : g_Restrictions) {
      ImGui::TableNextRow();

      ImGui::TableNextColumn();
      char buf[256];
      WideCharToMultiByte(CP_UTF8, 0, e.Name.c_str(), -1, buf, 256, nullptr,
                          nullptr);
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "%s", buf);

      ImGui::TableNextColumn();
      char vb[512];
      WideCharToMultiByte(CP_UTF8, 0, e.Value.c_str(), -1, vb, 512, nullptr,
                          nullptr);
      ImGui::TextWrapped("%s", vb);

      ImGui::TableNextColumn();
      char lb[256];
      WideCharToMultiByte(CP_UTF8, 0, e.Location.c_str(), -1, lb, 256, nullptr,
                          nullptr);
      ImGui::TextDisabled("%s", lb);
    }
    ImGui::EndTable();
  }

  if (!g_Restrictions.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
    if (ImGui::Button(u8"СНЯТЬ ВСЕ ОГРАНИЧЕНИЯ", ImVec2(-1, 36)))
      RestrictionsManager_FixAll();
    ImGui::PopStyleColor(2);
  }
}
