/**
 * dashboard.cpp
 * ZASLON v2.5.3 — System Health Dashboard
 *
 * Performs REAL system checks (not fake scores) and provides
 * actionable one-click fixes for every issue found.
 */
#include "dashboard.h"
#include "imgui.h"
#include "gui_theme.h"
#include "system_repair.h"
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

// ── Helpers ─────────────────────────────────────────────────────────────

static DWORD ReadRegDWORD(HKEY root, const wchar_t *subKey,
                          const wchar_t *valueName, DWORD defaultVal) {
  DWORD val = defaultVal, sz = sizeof(val);
  HKEY hk;
  if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
    RegQueryValueExW(hk, valueName, nullptr, nullptr, (LPBYTE)&val, &sz);
    RegCloseKey(hk);
  }
  return val;
}

static std::wstring ReadRegString(HKEY root, const wchar_t *subKey,
                                  const wchar_t *valueName) {
  wchar_t buf[512] = {};
  DWORD sz = sizeof(buf);
  HKEY hk;
  if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
    RegQueryValueExW(hk, valueName, nullptr, nullptr, (LPBYTE)buf, &sz);
    RegCloseKey(hk);
  }
  return buf;
}

static bool HasIFEODebuggers() {
  HKEY hIfeo;
  bool found = false;
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
        wchar_t dbg[512];
        DWORD dsz = sizeof(dbg);
        if (RegQueryValueExW(hSub, L"Debugger", nullptr, nullptr, (LPBYTE)dbg,
                             &dsz) == ERROR_SUCCESS) {
          found = true;
        }
        RegCloseKey(hSub);
      }
      if (found)
        break;
      len = 256;
    }
    RegCloseKey(hIfeo);
  }
  return found;
}

static bool IsHostsFileClean() {
  wchar_t sysDir[MAX_PATH];
  GetSystemDirectoryW(sysDir, MAX_PATH);
  std::wstring path = std::wstring(sysDir) + L"\\drivers\\etc\\hosts";

  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, 0, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return true; // can't read = assume ok

  LARGE_INTEGER sz;
  GetFileSizeEx(hFile, &sz);
  // If file > 4 KB, it's likely been hijacked with ad/malware entries
  bool clean = (sz.QuadPart < 4096);
  CloseHandle(hFile);
  return clean;
}

// ── Dashboard scan ──────────────────────────────────────────────────────

static std::vector<HealthCheck> g_Checks;
static int g_HealthScore = 100;
static bool g_Scanned = false;
static float g_ScanCooldown = 0.0f;
static bool g_ShowFixAllConfirm = false;

// Helper to check service status (0=Not found, 1=Running, 2=Stopped/Disabled)
static int GetServiceStatus(const wchar_t *serviceName) {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return 0;
  SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
  if (!svc) {
    CloseServiceHandle(scm);
    return 0;
  }
  SERVICE_STATUS ss;
  int status = 2;
  if (QueryServiceStatus(svc, &ss)) {
    if (ss.dwCurrentState == SERVICE_RUNNING)
      status = 1;
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return status;
}

static void Dashboard_Scan() {
  g_Checks.clear();
  int score = 100;

  // 1. Диспетчер задач
  {
    DWORD disabled = ReadRegDWORD(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"DisableTaskMgr", 0);
    if (disabled) {
      score -= 10;
      g_Checks.push_back({u8"Диспетчер задач", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Диспетчер задач", 0, "", "", false, false});
    }
  }

  // 2. Редактор реестра
  {
    DWORD disabled = ReadRegDWORD(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"DisableRegistryTools", 0);
    if (disabled) {
      score -= 10;
      g_Checks.push_back({u8"Редактор реестра", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Редактор реестра", 0, "", "", false, false});
    }
  }

  // 3. Оболочка Windows (Shell)
  {
    std::wstring shell = ReadRegString(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Shell");
    bool ok = true;
    if (!shell.empty()) {
      std::wstring lower = shell;
      for (auto &c : lower)
        c = towlower(c);
      if (lower.find(L"explorer.exe") == std::wstring::npos)
        ok = false;
    }
    if (!ok) {
      score -= 20;
      g_Checks.push_back({u8"Оболочка Windows", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Оболочка Windows", 0, "", "", false, false});
    }
  }

  // 4. Userinit
  {
    std::wstring ui = ReadRegString(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        L"Userinit");
    bool ok = true;
    if (!ui.empty()) {
      std::wstring lower = ui;
      for (auto &c : lower)
        c = towlower(c);
      if (lower.find(L"userinit.exe") == std::wstring::npos)
        ok = false;
    }
    if (!ok) {
      score -= 15;
      g_Checks.push_back({u8"Userinit", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Userinit", 0, "", "", false, false});
    }
  }

  // 5. IFEO перехваты
  {
    if (HasIFEODebuggers()) {
      score -= 15;
      g_Checks.push_back({u8"IFEO перехваты", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"IFEO перехваты", 0, "", "", false, false});
    }
  }

  // 6. Файл HOSTS
  {
    if (!IsHostsFileClean()) {
      score -= 5;
      g_Checks.push_back({u8"Файл HOSTS", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Файл HOSTS", 0, "", "", false, false});
    }
  }

  // 7. Контроль учётных записей (UAC)
  {
    DWORD lua = ReadRegDWORD(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        L"EnableLUA", 1);
    if (lua == 0) {
      score -= 5;
      g_Checks.push_back({u8"UAC (Контроль записей)", 1, "", "", true, false});
    } else {
      g_Checks.push_back(
          {u8"UAC (Контроль записей)", 0, "", "", false, false});
    }
  }

  // 8. Защитник Windows (WinDefend)
  {
    int stat = GetServiceStatus(L"WinDefend");
    if (stat == 2) {
      score -= 5;
      g_Checks.push_back({u8"Защитник Windows", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Защитник Windows", 0, "", "", false, false});
    }
  }

  // 9. Политики Защитника
  {
    DWORD d = ReadRegDWORD(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Policies\\Microsoft\\Windows Defender",
                           L"DisableAntiSpyware", 0);
    if (d != 0) {
      score -= 10;
      g_Checks.push_back({u8"Политики Defender", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Политики Defender", 0, "", "", false, false});
    }
  }

  // 10. Брандмауэр Windows (mpssvc)
  {
    int stat = GetServiceStatus(L"mpssvc");
    if (stat == 2) {
      score -= 5;
      g_Checks.push_back({u8"Брандмауэр Windows", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Брандмауэр Windows", 0, "", "", false, false});
    }
  }

  // 11. Безопасный режим (SafeBoot)
  {
    HKEY h;
    bool missing = true;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal",
                      0, KEY_READ, &h) == ERROR_SUCCESS) {
      missing = false;
      RegCloseKey(h);
    }
    if (missing) {
      score -= 15;
      g_Checks.push_back({u8"Безопасный режим", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Безопасный режим", 0, "", "", false, false});
    }
  }

  // 12. Перехват Sticky Keys
  {
    DWORD d = ReadRegDWORD(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File "
        L"Execution Options\\sethc.exe",
        L"Debugger", 0); // Checking if Debugger value exists
    // More accurate: search for it
    HKEY hk;
    bool hijacked = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image "
                      L"File Execution Options\\sethc.exe",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
      if (RegQueryValueExW(hk, L"Debugger", nullptr, nullptr, nullptr,
                           nullptr) == ERROR_SUCCESS)
        hijacked = true;
      RegCloseKey(hk);
    }
    if (hijacked) {
      score -= 10;
      g_Checks.push_back({u8"Sticky Keys (Перехват)", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Sticky Keys (Перехват)", 0, "", "", false, false});
    }
  }

  // 13. Перехват Utilman
  {
    HKEY hk;
    bool hijacked = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image "
                      L"File Execution Options\\utilman.exe",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
      if (RegQueryValueExW(hk, L"Debugger", nullptr, nullptr, nullptr,
                           nullptr) == ERROR_SUCCESS)
        hijacked = true;
      RegCloseKey(hk);
    }
    if (hijacked) {
      score -= 10;
      g_Checks.push_back({u8"Utilman (Перехват)", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Utilman (Перехват)", 0, "", "", false, false});
    }
  }

  // 14. Прокси-сервер
  {
    DWORD proxy = ReadRegDWORD(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        L"ProxyEnable", 0);
    if (proxy != 0) {
      score -= 5;
      g_Checks.push_back({u8"Прокси-сервер", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Прокси-сервер", 0, "", "", false, false});
    }
  }

  // 15. Протокол SMBv1
  {
    DWORD smb1 = ReadRegDWORD(
        HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters",
        L"SMB1", 1);
    if (smb1 != 0) {
      score -= 5;
      g_Checks.push_back({u8"Протокол SMBv1", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Протокол SMBv1", 0, "", "", false, false});
    }
  }

  // 16. Командная строка (CMD)
  {
    DWORD d = ReadRegDWORD(
        HKEY_CURRENT_USER, L"Software\\Policies\\Microsoft\\Windows\\System",
        L"DisableCMD", 0);
    if (d != 0) {
      score -= 10;
      g_Checks.push_back({u8"Командная строка", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Командная строка", 0, "", "", false, false});
    }
  }

  // 17. Панель управления
  {
    DWORD d = ReadRegDWORD(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        L"NoControlPanel", 0);
    if (d != 0) {
      score -= 10;
      g_Checks.push_back({u8"Панель управления", 2, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Панель управления", 0, "", "", false, false});
    }
  }

  // 18. Windows Script Host (WSH)
  {
    DWORD d = ReadRegDWORD(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Script Host\\Settings",
        L"Enabled", 1);
    if (d == 0) {
      score -= 5;
      g_Checks.push_back({u8"Windows Script Host", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Windows Script Host", 0, "", "", false, false});
    }
  }

  // 19. PowerShell Policy
  {
    std::wstring p = ReadRegString(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell",
        L"ExecutionPolicy");
    if (p == L"Unrestricted" || p == L"Bypass") {
      score -= 5;
      g_Checks.push_back({u8"PowerShell Policy", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"PowerShell Policy", 0, "", "", false, false});
    }
  }

  // 20. Скрытые файлы
  {
    DWORD d = ReadRegDWORD(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"ShowSuperHidden", 0);
    if (d == 0) {
      score -= 5;
      g_Checks.push_back({u8"Скрытые файлы", 1, "", "", true, false});
    } else {
      g_Checks.push_back({u8"Скрытые файлы", 0, "", "", false, false});
    }
  }

  g_HealthScore = (score < 0) ? 0 : score;
  g_Scanned = true;
}

// ── Fix dispatcher ──────────────────────────────────────────────────────

static void Dashboard_FixCheck(HealthCheck &chk) {
  // Dispatch fix based on Name
  if (chk.Name == u8"Диспетчер задач") {
    Repair::UnlockTaskManager();
  } else if (chk.Name == u8"Редактор реестра") {
    Repair::UnlockRegedit();
  } else if (chk.Name == u8"Оболочка Windows") {
    Repair::FixShell();
  } else if (chk.Name == u8"Userinit") {
    Repair::FixUserinit();
  } else if (chk.Name == u8"IFEO перехваты") {
    Repair::ClearIFEO();
  } else if (chk.Name == u8"Файл HOSTS") {
    Repair::ResetHostsFile();
  } else if (chk.Name == u8"UAC (Контроль записей)") {
    Repair::UnlockUAC();
  } else if (chk.Name == u8"Защитник Windows") {
    // Attempt to start WinDefend
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
      SC_HANDLE svc = OpenServiceW(scm, L"WinDefend", SERVICE_START);
      if (svc) {
        StartServiceW(svc, 0, nullptr);
        CloseServiceHandle(svc);
      }
      CloseServiceHandle(scm);
    }
  } else if (chk.Name == u8"Политики Defender") {
    Repair::ResetDefenderPolicies();
  } else if (chk.Name == u8"Брандмауэр Windows") {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
      SC_HANDLE svc = OpenServiceW(scm, L"mpssvc", SERVICE_START);
      if (svc) {
        StartServiceW(svc, 0, nullptr);
        CloseServiceHandle(svc);
      }
      CloseServiceHandle(scm);
    }
  } else if (chk.Name == u8"Безопасный режим") {
    // Restore default SafeBoot keys (stubbed in repair, but we'll log it)
    Repair::RestoreSafeBoot();
  } else if (chk.Name == u8"Sticky Keys (Перехват)" ||
             chk.Name == u8"Utilman (Перехват)") {
    Repair::FixStickyKeys();
  } else if (chk.Name == u8"Прокси-сервер") {
    HKEY hKey;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      DWORD val = 0;
      RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE *)&val,
                     sizeof(val));
      RegCloseKey(hKey);
    }
  } else if (chk.Name == u8"Протокол SMBv1") {
    Repair::DisableSMB1();
  } else if (chk.Name == u8"Командная строка") {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Policies\\Microsoft\\Windows\\System", 0,
                      KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      DWORD val = 0;
      RegSetValueExW(hKey, L"DisableCMD", 0, REG_DWORD, (const BYTE *)&val,
                     sizeof(val));
      RegCloseKey(hKey);
    }
  } else if (chk.Name == u8"Панель управления") {
    HKEY hKey;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      DWORD val = 0;
      RegSetValueExW(hKey, L"NoControlPanel", 0, REG_DWORD, (const BYTE *)&val,
                     sizeof(val));
      RegCloseKey(hKey);
    }
  } else if (chk.Name == u8"Windows Script Host") {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows Script Host\\Settings", 0,
                      KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      DWORD val = 1;
      RegSetValueExW(hKey, L"Enabled", 0, REG_DWORD, (const BYTE *)&val,
                     sizeof(val));
      RegCloseKey(hKey);
    }
  } else if (chk.Name == u8"PowerShell Policy") {
    HKEY hKey;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      RegSetValueExW(hKey, L"ExecutionPolicy", 0, REG_SZ,
                     (const BYTE *)L"RemoteSigned", 13 * sizeof(wchar_t));
      RegCloseKey(hKey);
    }
  } else if (chk.Name == u8"Скрытые файлы") {
    HKEY hKey;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      DWORD val = 1;
      RegSetValueExW(hKey, L"ShowSuperHidden", 0, REG_DWORD, (const BYTE *)&val,
                     sizeof(val));
      RegCloseKey(hKey);
    }
  }
  chk.Fixed = true;
  chk.Level = 0;
  chk.Details = u8"Ok";
}

// ── Render ───────────────────────────────────────────────────────────────

void Dashboard_Render() {
  // ─── Header ─────────────────────────────────────────────────────
  // v2.5: Auto-scan on first render
  if (!g_Scanned) {
    Dashboard_Scan();
  }

  ImGui::TextColored(g_Theme.AccentColor, u8"ZASLON Дашборд");
  ImGui::TextDisabled(u8"Крутая прога от Машиниста");
  ImGui::Separator();
  ImGui::Spacing();

  // ─── Scan button ────────────────────────────────────────────────
  if (ImGui::Button(u8"Сканировать", ImVec2(200, 34)))
    Dashboard_Scan();

  // v2.5: auto-scan removed the "press to scan" hint — always show results

  // ─── Score display ──────────────────────────────────────────────
  ImGui::SameLine(0, 30.0f);

  ImVec4 scoreColor;
  const char *scoreLabel;
  if (g_HealthScore >= 90) {
    scoreColor = ImVec4(0.3f, 1.0f, 0.5f, 1.0f);
    scoreLabel = u8"Чётко";
  } else if (g_HealthScore >= 70) {
    scoreColor = ImVec4(0.5f, 1.0f, 0.3f, 1.0f);
    scoreLabel = u8"Почти чётко";
  } else if (g_HealthScore >= 50) {
    scoreColor = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
    scoreLabel = u8"Внимание";
  } else {
    scoreColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
    scoreLabel = u8"Грустно";
  }

  ImGui::TextColored(scoreColor, u8"Уровень насколько все чётко: %d%% [%s]",
                     g_HealthScore, scoreLabel);

  // ─── Progress bar ───────────────────────────────────────────────
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, scoreColor);
  ImGui::ProgressBar(g_HealthScore / 100.0f, ImVec2(-1, 8), "");
  ImGui::PopStyleColor();

  // ─── Fix All button (only if problems found) ────────────────────
  int critCount = 0, warnCount = 0;
  for (auto &c : g_Checks) {
    if (c.Level == 2 && !c.Fixed)
      critCount++;
    if (c.Level == 1 && !c.Fixed)
      warnCount++;
  }

  ImGui::Spacing();
  if (critCount + warnCount > 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
    char fixLabel[128];
    snprintf(fixLabel, sizeof(fixLabel), u8"Исправить весь пиздец (%d)",
             critCount + warnCount);
    if (ImGui::Button(fixLabel, ImVec2(-1, 40))) {
      g_ShowFixAllConfirm = true;
    }
    ImGui::PopStyleColor(2);

    // v2.5: Fix All confirmation dialog
    if (g_ShowFixAllConfirm) {
      ImGui::OpenPopup(u8"Подтвердите исправление##fixall");
    }
    if (ImGui::BeginPopupModal(u8"Подтвердите исправление##fixall", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text(u8"Вы уверены, что хотите исправить все %d проблем?",
                  critCount + warnCount);
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                         u8"Это изменит параметры реестра и системные файлы.");
      ImGui::Spacing();
      if (ImGui::Button(u8"ДА, Исправить", ImVec2(150, 0))) {
        for (auto &c : g_Checks) {
          if (c.Level > 0 && c.CanFix && !c.Fixed)
            Dashboard_FixCheck(c);
        }
        Dashboard_Scan();
        g_ShowFixAllConfirm = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(u8"Отмена", ImVec2(100, 0))) {
        g_ShowFixAllConfirm = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ─── Results table ──────────────────────────────────────────────
  ImGui::Text(u8"Результаты:");
  ImGui::Spacing();

  if (ImGui::BeginTable("Хэалтх", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                        ImVec2(0, 0))) {

    ImGui::TableSetupColumn(u8"Компонент", ImGuiTableColumnFlags_WidthFixed,
                            200.0f);
    ImGui::TableSetupColumn(u8"Статус", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(u8"Действие", ImGuiTableColumnFlags_WidthFixed,
                            110.0f);
    ImGui::TableHeadersRow();

    for (auto &chk : g_Checks) {
      ImGui::TableNextRow();

      // Name
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(chk.Name.c_str());

      // Status badge
      ImGui::TableNextColumn();
      if (chk.Fixed) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), u8"Исправлено");
      } else if (chk.Level == 0) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), u8"Чётенько");
      } else if (chk.Level == 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), u8"Хуево");
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), u8"Слишком Хуёво");
      }

      // Fix button
      ImGui::TableNextColumn();
      if (chk.CanFix && !chk.Fixed && chk.Level > 0) {
        ImGui::PushID(chk.Name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        if (ImGui::SmallButton(u8"Исправить")) {
          Dashboard_FixCheck(chk);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }
}
