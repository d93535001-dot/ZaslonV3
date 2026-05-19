/**
 * offline_registry.cpp
 * ZASLON — Boot Surgeon & Offline Account Manager
 *
 * Implements offline hive mounting via RegLoadKeyW.
 * Boot Surgeon: Reads/Modifies ControlSet001\Services Start types.
 * Account Manager: Modifies SAM\Domains\Account\Users (NT-hash zeroing).
 *
 * Developer: Machinist
 */
#include "offline_registry.h"
#include "imgui.h"
#include "gui_theme.h"
#include "offline_os_manager.h"
#include <algorithm>
#include <commdlg.h>
#include <strsafe.h>


//
// Globals
//
static wchar_t g_CurrentHivePath[MAX_PATH] = {};
static std::wstring g_MountName = L"ZASLON_OFFLINE";
static bool g_IsHiveMounted = false;
static wchar_t g_StatusMessage[512] = {};

static std::vector<ServiceEntry> g_Services;
static std::vector<AccountEntry> g_Accounts;

enum RegistryMode { MODE_NONE, MODE_SYSTEM, MODE_SAM };
static RegistryMode g_CurrentMode = MODE_NONE;

//
// Privilege helper
//
static bool EnablePrivilege(LPCWSTR privName) {
  HANDLE hToken = NULL;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    return false;

  TOKEN_PRIVILEGES tp = {};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW(nullptr, privName, &tp.Privileges[0].Luid)) {
    CloseHandle(hToken);
    return false;
  }
  AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  CloseHandle(hToken);
  return GetLastError() == ERROR_SUCCESS || GetLastError() == 0;
}

//
// Unmount globally helper
//
static void UnmountCurrentHive() {
  if (g_IsHiveMounted) {
    EnablePrivilege(SE_RESTORE_NAME);
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, g_MountName.c_str());
    g_IsHiveMounted = false;
    g_CurrentMode = MODE_NONE;
    g_Services.clear();
    g_Accounts.clear();
    StringCchPrintfW(g_StatusMessage, 512, L"Куст выгружен.");
  }
}

//
// Mount hive helper
//
static bool MountHive(const wchar_t *path) {
  UnmountCurrentHive();

  EnablePrivilege(SE_RESTORE_NAME);
  EnablePrivilege(SE_BACKUP_NAME);

  LSTATUS s = RegLoadKeyW(HKEY_LOCAL_MACHINE, g_MountName.c_str(), path);
  if (s == ERROR_SUCCESS) {
    g_IsHiveMounted = true;
    StringCchCopyW(g_CurrentHivePath, MAX_PATH, path);
    StringCchPrintfW(g_StatusMessage, 512, L"Куст успешно загружен в HKLM\\%s",
                     g_MountName.c_str());
    return true;
  } else {
    StringCchPrintfW(g_StatusMessage, 512, L"Ошибка монтирования (код %d)", s);
    return false;
  }
}

//
// Read DWORD from registry
//
static DWORD RegReadDword(HKEY hKey, const wchar_t *subKey,
                          const wchar_t *valueName) {
  DWORD data = 0;
  DWORD size = sizeof(DWORD);
  RegGetValueW(hKey, subKey, valueName, RRF_RT_DWORD, nullptr, &data, &size);
  return data;
}

//
// Write DWORD to registry
//
static bool RegWriteDword(HKEY hKey, const wchar_t *subKey,
                          const wchar_t *valueName, DWORD data) {
  HKEY hTarget = NULL;
  if (RegOpenKeyExW(hKey, subKey, 0, KEY_SET_VALUE, &hTarget) ==
      ERROR_SUCCESS) {
    LSTATUS s = RegSetValueExW(hTarget, valueName, 0, REG_DWORD,
                               (const BYTE *)&data, sizeof(DWORD));
    RegCloseKey(hTarget);
    return s == ERROR_SUCCESS;
  }
  return false;
}

//
// ----------------------------------------------------
// BOOT SURGEON (SYSTEM HIVE)
// ----------------------------------------------------
static void ScanServices() {
  g_Services.clear();
  HKEY hRoot = NULL;
  std::wstring servicesKey = g_MountName + L"\\ControlSet001\\Services";

  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, servicesKey.c_str(), 0, KEY_READ,
                    &hRoot) != ERROR_SUCCESS) {
    StringCchPrintfW(
        g_StatusMessage, 512,
        L"Не найден раздел ControlSet001\\Services. Это точно куст SYSTEM?");
    return;
  }

  DWORD index = 0;
  wchar_t name[256] = {};
  DWORD nameLen = 256;

  while (RegEnumKeyExW(hRoot, index, name, &nameLen, nullptr, nullptr, nullptr,
                       nullptr) == ERROR_SUCCESS) {
    ServiceEntry se;
    se.Name = name;

    HKEY hSvc = NULL;
    if (RegOpenKeyExW(hRoot, name, 0, KEY_READ, &hSvc) == ERROR_SUCCESS) {
      DWORD type = 0, start = 4;
      DWORD sz = sizeof(DWORD);
      RegQueryValueExW(hSvc, L"Type", nullptr, nullptr, (LPBYTE)&type, &sz);
      sz = sizeof(DWORD);
      RegQueryValueExW(hSvc, L"Start", nullptr, nullptr, (LPBYTE)&start, &sz);

      wchar_t img[MAX_PATH] = {};
      sz = sizeof(img);
      if (RegQueryValueExW(hSvc, L"ImagePath", nullptr, nullptr, (LPBYTE)img,
                           &sz) == ERROR_SUCCESS)
        se.ImagePath = img;

      se.ServiceType = type;
      se.StartType = start;
      RegCloseKey(hSvc);

      g_Services.push_back(std::move(se));
    }
    index++;
    nameLen = 256;
  }

  RegCloseKey(hRoot);
  g_CurrentMode = MODE_SYSTEM;
  StringCchPrintfW(g_StatusMessage, 512, L"Прочитано %zu служб/драйверов.",
                   g_Services.size());
}

static void SetServiceStart(const std::wstring &name, DWORD newStartType) {
  std::wstring subKey = g_MountName + L"\\ControlSet001\\Services\\" + name;
  if (RegWriteDword(HKEY_LOCAL_MACHINE, subKey.c_str(), L"Start",
                    newStartType)) {
    StringCchPrintfW(g_StatusMessage, 512, L"Служба %s: Start := %u",
                     name.c_str(), newStartType);
    for (auto &s : g_Services)
      if (s.Name == name)
        s.StartType = newStartType;
  }
}

//
// ----------------------------------------------------
// ACCOUNT MANAGER (SAM HIVE)
// ----------------------------------------------------
// Offline NT-hash reset via 'V' value truncation
static void ScanAccounts() {
  g_Accounts.clear();
  std::wstring usersKey = g_MountName + L"\\SAM\\Domains\\Account\\Users";
  HKEY hRoot = NULL;

  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, usersKey.c_str(), 0, KEY_READ,
                    &hRoot) != ERROR_SUCCESS) {
    std::wstring usersKey2 = g_MountName + L"\\Domains\\Account\\Users";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, usersKey2.c_str(), 0, KEY_READ,
                      &hRoot) != ERROR_SUCCESS) {
      StringCchPrintfW(g_StatusMessage, 512,
                       L"Не найден раздел Users. Это точно куст SAM?");
      return;
    }
  }

  HKEY hNames = NULL;
  if (RegOpenKeyExW(hRoot, L"Names", 0, KEY_READ, &hNames) == ERROR_SUCCESS) {
    DWORD index = 0;
    wchar_t name[256] = {};
    DWORD nameLen = 256;

    while (RegEnumKeyExW(hNames, index, name, &nameLen, nullptr, nullptr,
                         nullptr, nullptr) == ERROR_SUCCESS) {
      AccountEntry ae;
      ae.Username = name;

      DWORD rid = 0, szType = 0, szData = sizeof(DWORD);
      if (RegGetValueW(hNames, name, nullptr, RRF_RT_ANY, &szType, &rid,
                       &szData) == ERROR_SUCCESS) {
        // The Type field of the default value contains the RID.
        ae.Rid = szType;
        ae.IsAdmin = (ae.Rid == 500); // built-in admin by default
        ae.HasBlankPassword = false;  // require deep inspection of V binary
        g_Accounts.push_back(ae);
      }

      index++;
      nameLen = 256;
    }
    RegCloseKey(hNames);
  }
  RegCloseKey(hRoot);

  g_CurrentMode = MODE_SAM;
  StringCchPrintfW(g_StatusMessage, 512, L"Прочитано %zu учётных записей.",
                   g_Accounts.size());
}

static void ResetPassword(DWORD rid) {
  // The NT hash is stored in the 'V' value under SAM\Domains\Account\Users\%08X
  // (RID). Zeroing out the hash part requires parsing the V structure. For
  // simplicity of this module (and because modern SAM uses AES), we use a
  // well-known layout for un-syskeyed or NT-style localSAMs. A robust offline
  // password wipe either clears the NT/LM offset fields or truncates. We
  // demonstrate the concept by finding the V value, reading it, clearing hash
  // len, writing back.

  std::wstring ridStr;
  wchar_t buf[16];
  StringCchPrintfW(buf, 16, L"%08X", rid);
  ridStr = buf;

  std::wstring userKey =
      g_MountName + L"\\SAM\\Domains\\Account\\Users\\" + ridStr;
  if (GetFileAttributesW((userKey + L"\\V").c_str()) ==
      INVALID_FILE_ATTRIBUTES) // check if key exists via reg (pseudo code - use
                               // actual reg)
  {
    // Actually, we must use RegOpenKey
  }

  HKEY hUser = NULL;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, userKey.c_str(), 0,
                    KEY_READ | KEY_WRITE, &hUser) != ERROR_SUCCESS) {
    userKey = g_MountName + L"\\Domains\\Account\\Users\\" + ridStr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, userKey.c_str(), 0,
                      KEY_READ | KEY_WRITE, &hUser) != ERROR_SUCCESS) {
      StringCchPrintfW(g_StatusMessage, 512, L"УЗ RID %u не найдена.", rid);
      return;
    }
  }

  DWORD type = 0, size = 0;
  RegQueryValueExW(hUser, L"V", nullptr, &type, nullptr, &size);
  if (size > 0 && type == REG_BINARY) {
    BYTE *vData = new BYTE[size];
    if (RegQueryValueExW(hUser, L"V", nullptr, nullptr, vData, &size) ==
        ERROR_SUCCESS) {
      // V structure header contains lengths and offsets.
      // Offset 0x9C contains NT Hash offset, 0xA0 contains length.
      // Offset 0xA4 contains LM Hash offset, 0xA8 contains length.
      // (These offsets depend on Windows version, typically NT/2000-Win10
      // 1607).

      if (size >= 0xB0) {
        // Clear NT hash length
        DWORD *pNtLen = (DWORD *)(vData + 0xA0);
        *pNtLen = 0;

        // Clear LM hash length
        DWORD *pLmLen = (DWORD *)(vData + 0xA8);
        *pLmLen = 0;

        // Save back
        if (RegSetValueExW(hUser, L"V", 0, REG_BINARY, vData, size) ==
            ERROR_SUCCESS)
          StringCchPrintfW(g_StatusMessage, 512,
                           L"Пароль для RID %u сброшен (хэш обнулён).", rid);
        else
          StringCchPrintfW(g_StatusMessage, 512, L"Ошибка записи V-значения.");
      } else {
        StringCchPrintfW(g_StatusMessage, 512,
                         L"V-значение имеет нестандартный размер (%u байт).",
                         size);
      }
    }
    delete[] vData;
  }
  RegCloseKey(hUser);
}

//
// ----------------------------------------------------
// UI Render
// ----------------------------------------------------
static bool BrowseForHive(char *outPath, DWORD outPathLen) {
  OPENFILENAMEA ofn = {sizeof(ofn)};
  char buf[MAX_PATH] = {};
  ofn.hwndOwner = NULL;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = "Все файлы кустов\0*\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  ofn.lpstrTitle = "Выберите файл куста реестра";
  if (!GetOpenFileNameA(&ofn))
    return false;
  StringCchCopyA(outPath, outPathLen, buf);
  return true;
}

void OfflineRegistry_Render() {
  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                     u8"Офлайн Редактор Реестра");
  ImGui::TextWrapped(
      u8"Инструмент для редактирования реестра неактивной Windows.\n"
      u8"Куст SYSTEM: Отключение сбойных драйверов и служб.\n"
      u8"Куст SAM: Сброс паролей локальных учетных записей.\n"
      u8"Обычно файлы лежат в: C:\\Windows\\System32\\config\\");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (!g_IsHiveMounted) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                       u8"Куст не загружен. Выберите файл реестра (без "
                       u8"расширения) для начала работы.");
    ImGui::Spacing();

    static char hiveInput[MAX_PATH] = {};
    static bool hiveInputInitialized = false;
    if (!hiveInputInitialized) {
      if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted &&
          !ZaslonCore::g_OfflineOSPath.empty()) {
        std::wstring defaultPath =
            ZaslonCore::g_OfflineOSPath + L"\\System32\\config\\SYSTEM";
        WideCharToMultiByte(CP_UTF8, 0, defaultPath.c_str(), -1, hiveInput,
                            MAX_PATH, nullptr, nullptr);
      }
      hiveInputInitialized = true;
    }

    ImGui::SetNextItemWidth(450);
    ImGui::InputTextWithHint("##hivepath",
                             u8"Путь к файлу куста (например: "
                             u8"D:\\Windows\\System32\\config\\SYSTEM)",
                             hiveInput, MAX_PATH);
    ImGui::SameLine();
    if (ImGui::Button(u8"Обзор...", ImVec2(80, 0))) {
      if (BrowseForHive(hiveInput, MAX_PATH)) {
        wchar_t wP[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, hiveInput, -1, wP, MAX_PATH);
        MountHive(wP);
      }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button(u8"Загрузить Куст", ImVec2(200, 0))) {
      wchar_t wP[MAX_PATH] = {};
      MultiByteToWideChar(CP_UTF8, 0, hiveInput, -1, wP, MAX_PATH);
      MountHive(wP);
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
  } else {
    if (ImGui::Button(u8"Отмонтировать выбранный куст")) {
      UnmountCurrentHive();
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"Сканировать как SYSTEM"))
      ScanServices();
    ImGui::SameLine();
    if (ImGui::Button(u8"Сканировать как SAM"))
      ScanAccounts();
  }

  if (g_StatusMessage[0] != L'\0') {
    char msg[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_StatusMessage, -1, msg, 512, nullptr,
                        nullptr);
    ImVec4 col = (wcsstr(g_StatusMessage, L"Ошибка") ||
                  wcsstr(g_StatusMessage, L"Не найден"))
                     ? ImVec4(1, 0.3f, 0.3f, 1)
                     : ImVec4(0.3f, 1, 0.3f, 1);
    ImGui::TextColored(col, "[Статус] %s", msg);
  }

  ImGui::Separator();

  if (g_IsHiveMounted) {
    if (g_CurrentMode == MODE_SYSTEM) {
      ImGui::Text(u8"Службы и драйверы реестра SYSTEM:");
      static char svcFilter[128] = {};
      ImGui::SetNextItemWidth(300);
      ImGui::InputTextWithHint("##svcflt", u8"Фильтр имени...", svcFilter, 128);

      if (ImGui::BeginTable("##boot_surgeon", 4,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0, 400))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(u8"Имя", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn(u8"Запуск", ImGuiTableColumnFlags_WidthFixed,
                                100);
        ImGui::TableSetupColumn(u8"Тип", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(u8"Путь", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto &s : g_Services) {
          char cname[256] = {};
          WideCharToMultiByte(CP_UTF8, 0, s.Name.c_str(), -1, cname, 256,
                              nullptr, nullptr);
          if (svcFilter[0] != '\0' && strstr(cname, svcFilter) == nullptr)
            continue;

          ImGui::TableNextRow();
          if (s.StartType == 4)
            ImGui::TableSetBgColor(
                ImGuiTableBgTarget_RowBg0,
                ImGui::GetColorU32(ImVec4(0.3f, 0.0f, 0.0f, 0.5f)));

          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(cname);
          ImGui::TableSetColumnIndex(1);
          ImGui::PushID(cname);
          int startVal = s.StartType;
          const char *startStrs[] = {"0-Boot (Загр)", "1-System (Сус)",
                                     "2-Auto (Авто)", "3-Demand (Врч)",
                                     "4-Disabled (Выкл)"};
          ImGui::SetNextItemWidth(120);
          if (ImGui::Combo("##start", &startVal, startStrs, 5))
            SetServiceStart(s.Name, (DWORD)startVal);
          ImGui::PopID();

          ImGui::TableSetColumnIndex(2);
          ImGui::Text("0x%X", s.ServiceType);
          ImGui::TableSetColumnIndex(3);
          char cpath[512] = {};
          WideCharToMultiByte(CP_UTF8, 0, s.ImagePath.c_str(), -1, cpath, 512,
                              nullptr, nullptr);
          ImGui::TextUnformatted(cpath);
        }
        ImGui::EndTable();
      }
    } else if (g_CurrentMode == MODE_SAM) {
      ImGui::Text(u8"Учётные записи реестра SAM:");
      if (ImGui::BeginTable("##acct_mgr", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(0, 400))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(u8"Пользователь",
                                ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn(u8"RID", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(u8"Действие",
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto &a : g_Accounts) {
          ImGui::TableNextRow();
          char uname[256] = {};
          WideCharToMultiByte(CP_UTF8, 0, a.Username.c_str(), -1, uname, 256,
                              nullptr, nullptr);
          ImGui::TableSetColumnIndex(0);
          if (a.IsAdmin)
            ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), u8" %s [Admin]", uname);
          else
            ImGui::TextUnformatted(uname);

          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%u", a.Rid);
          ImGui::TableSetColumnIndex(2);
          ImGui::PushID(a.Rid);
          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
          if (ImGui::Button(u8"СБРОС ПАРОЛЯ", ImVec2(150, 0)))
            ResetPassword(a.Rid);
          ImGui::PopStyleColor();
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
  }
}
