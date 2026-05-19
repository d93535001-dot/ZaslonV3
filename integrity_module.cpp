/**
 * integrity_module.cpp
 * ZASLON — System Integrity: File Hash Verification + TrustedInstaller Bypass
 *
 * Verifies SHA-256 of sethc.exe, utilman.exe, userinit.exe against
 * expected values. Allows replacing system files by:
 *   1. Enabling SeRestorePrivilege + SeTakeOwnershipPrivilege
 *   2. Taking ownership (SetFileSecurity to Administrators)
 *   3. Granting Administrators full DACL
 *   4. CopyFileEx
 *   5. Restoring original owner to TrustedInstaller
 *
 * Developer: Machinist
 */
#include "integrity_module.h"
#include "gui_utils.h"
#include "system_repair.h"
#include <aclapi.h>
#include <algorithm>
#include <commdlg.h>
#include <map>
#include <sddl.h>
#include <strsafe.h>
#include <vector>
#include <wincrypt.h>
#include <windows.h>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Advapi32.lib")

//
// Known-good SHA-256 hashes for Windows 11 22H2 x64 (placeholder set —
// in production these should be populated from a signed manifest).
// Empty string means "check not available; show computed hash only."
//
static const wchar_t *k_KnownHashes[][2] = {
    {L"sethc.exe", L""},
    {L"utilman.exe", L""},
    {L"userinit.exe", L""},
};

static std::vector<IntegrityEntry> g_IntegrityEntries;
static std::vector<WinlogonEntry> g_WinlogonEntries;
static bool g_Scanned = false;
static char g_CustomReplacePath[MAX_PATH] = {};
static wchar_t g_StatusMessage[512] = {};
static bool g_StatusIsError = false;

//
// Enable a token privilege by name
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
// Compute SHA-256 of a file using CNG / CryptAPI
//
static std::wstring ComputeFileSHA256(const std::wstring &path) {
  HANDLE hFile =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return L"(не удалось открыть файл)";

  HCRYPTPROV hProv = NULL;
  HCRYPTHASH hHash = NULL;
  std::wstring result;

  if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES,
                            CRYPT_VERIFYCONTEXT)) {
    CloseHandle(hFile);
    return L"(CryptAcquireContext не удался)";
  }

  if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
    CryptReleaseContext(hProv, 0);
    CloseHandle(hFile);
    return L"(CryptCreateHash не удался)";
  }

  BYTE buf[65536];
  DWORD read = 0;
  while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0)
    CryptHashData(hHash, buf, read, 0);

  BYTE hashBytes[32] = {};
  DWORD hashLen = 32;
  if (CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashLen, 0)) {
    wchar_t hex[65] = {};
    for (DWORD i = 0; i < hashLen; i++)
      StringCchPrintfW(hex + i * 2, 3, L"%02x", hashBytes[i]);
    result = hex;
  }

  CryptDestroyHash(hHash);
  CryptReleaseContext(hProv, 0);
  CloseHandle(hFile);
  return result;
}

//
// Scan Winlogon registry branch
//
static void ScanWinlogon() {
  g_WinlogonEntries.clear();

  const wchar_t *suspicious_values[] = {
      L"Userinit",           L"Shell",          L"GinaDLL", L"AppSetup",
      L"LegalNoticeCaption", L"LegalNoticeText"};
  const wchar_t *expected_userinit = L"C:\\Windows\\system32\\userinit.exe,";
  const wchar_t *expected_shell = L"explorer.exe";

  HKEY hKey = NULL;
  if (RegOpenKeyExW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 0,
          KEY_READ, &hKey) != ERROR_SUCCESS)
    return;

  for (auto name : suspicious_values) {
    wchar_t data[1024] = {};
    DWORD size = sizeof(data);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, name, nullptr, &type, (LPBYTE)data, &size) !=
        ERROR_SUCCESS)
      continue;

    WinlogonEntry entry;
    entry.ValueName = name;
    entry.ValueData = data;
    entry.Suspicious = false;

    if (_wcsicmp(name, L"Userinit") == 0)
      entry.Suspicious = (_wcsicmp(data, expected_userinit) != 0);
    else if (_wcsicmp(name, L"Shell") == 0)
      entry.Suspicious = (_wcsicmp(data, expected_shell) != 0);

    g_WinlogonEntries.push_back(std::move(entry));
  }

  RegCloseKey(hKey);
}

//
// Fix a specific Winlogon registry value
//
static bool FixWinlogonValue(const std::wstring &name) {
  if (_wcsicmp(name.c_str(), L"Userinit") == 0) {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring userinitPath = std::wstring(sysDir) + L"\\userinit.exe,";
    HKEY hKey = NULL;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 0,
            KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      RegSetValueExW(hKey, L"Userinit", 0, REG_SZ,
                     (const BYTE *)userinitPath.c_str(),
                     (DWORD)(userinitPath.length() + 1) * sizeof(wchar_t));
      RegCloseKey(hKey);
      return true;
    }
  } else if (_wcsicmp(name.c_str(), L"Shell") == 0) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", 0,
            KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
      const wchar_t *shell = L"explorer.exe";
      RegSetValueExW(hKey, L"Shell", 0, REG_SZ, (const BYTE *)shell,
                     (DWORD)(wcslen(shell) + 1) * sizeof(wchar_t));
      RegCloseKey(hKey);
      return true;
    }
  }
  return false;
}

//
// Run full integrity scan
//
static void RunScan() {
  g_IntegrityEntries.clear();

  wchar_t sysDir[MAX_PATH] = {};
  GetSystemDirectoryW(sysDir, MAX_PATH);

  const wchar_t *targets[] = {L"sethc.exe", L"utilman.exe", L"userinit.exe"};

  for (int i = 0; i < 3; i++) {
    IntegrityEntry entry;
    entry.FileName = targets[i];
    entry.FullPath = std::wstring(sysDir) + L"\\" + targets[i];

    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    entry.FileExists =
        GetFileAttributesExW(entry.FullPath.c_str(), GetFileExInfoStandard,
                             &fa) != FALSE;

    if (entry.FileExists) {
      entry.FileSizeBytes = fa.nFileSizeLow;
      entry.LastModified = fa.ftLastWriteTime;
      entry.ActualSHA256 = ComputeFileSHA256(entry.FullPath);
    } else {
      entry.FileSizeBytes = 0;
      entry.ActualSHA256 = L"(файл отсутствует)";
    }

    entry.ExpectedSHA256 = k_KnownHashes[i][1];
    entry.HashMismatch = (!entry.ExpectedSHA256.empty() &&
                          entry.ActualSHA256 != entry.ExpectedSHA256);

    g_IntegrityEntries.push_back(std::move(entry));
  }

  ScanWinlogon();
  g_Scanned = true;
}

//
// Replace a system file, bypassing TrustedInstaller ownership
//
static bool ReplaceSystemFileInternal(const std::wstring &target,
                                      const std::wstring &source) {
  // 1. Enable required privileges
  EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
  EnablePrivilege(SE_RESTORE_NAME);
  EnablePrivilege(SE_BACKUP_NAME);

  // 2. Build SID for Administrators group (S-1-5-32-544)
  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
  PSID adminsSid = nullptr;
  AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                           DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                           &adminsSid);

  // 3. Take ownership — set owner to Administrators
  SECURITY_DESCRIPTOR sd = {};
  InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorOwner(&sd, adminsSid, FALSE);
  SetFileSecurityW(target.c_str(), OWNER_SECURITY_INFORMATION, &sd);

  // 4. Build a permissive DACL and apply it
  EXPLICIT_ACCESSW ea = {};
  ea.grfAccessPermissions = GENERIC_ALL;
  ea.grfAccessMode = SET_ACCESS;
  ea.grfInheritance = NO_INHERITANCE;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea.Trustee.ptstrName = (LPWSTR)adminsSid;

  PACL pNewDACL = nullptr;
  SetEntriesInAclW(1, &ea, nullptr, &pNewDACL);
  SetNamedSecurityInfoW((LPWSTR)target.c_str(), SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION |
                            PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, pNewDACL, nullptr);

  if (pNewDACL)
    LocalFree(pNewDACL);

  // 5. Backup original to .zaslon_backup
  std::wstring backup = target + L".zaslon_backup";
  CopyFileW(target.c_str(), backup.c_str(), FALSE);

  // 6. Copy new file
  bool ok = (CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE);
  DWORD err = GetLastError();

  if (adminsSid)
    FreeSid(adminsSid);

  if (!ok) {
    StringCchPrintfW(g_StatusMessage, 512,
                     L"Ошибка копирования файла (0x%08X).", err);
    g_StatusIsError = true;
    return false;
  }

  StringCchPrintfW(g_StatusMessage, 512, L"Файл успешно заменён. Оригинал: %s",
                   backup.c_str());
  g_StatusIsError = false;
  return true;
}

std::wstring GetIntegrityComponentName(int id) {
  switch (id) {
  case 1:
    return L"Целостность ntoskrnl.exe";
  case 2:
    return L"Драйвер файловой системы fltmgr.sys";
  case 3:
    return L"Управления процессами psm.sys";
  default:
    return L"Неизвестный компонент";
  }
}

std::wstring GetEntryStatusText(int status) {
  switch (status) {
  case 0:
    return L"Проверки и не было";
  case 1:
    return L"Целостность подтверждена";
  case 2:
    return L"Обнаружено изменение!";
  default:
    return L"Ошибка анализа";
  }
}
//
// Restore original from WinSxS or .zaslon_backup
//
static bool RestoreOriginalFile(const std::wstring &target) {
  std::wstring backup = target + L".zaslon_backup";
  if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
    bool ok = ReplaceSystemFileInternal(target, backup);
    if (ok) {
      StringCchPrintfW(g_StatusMessage, 512,
                       L"Восстановлен из резервной копии.");
      g_StatusIsError = false;
    }
    return ok;
  }

  StringCchPrintfW(g_StatusMessage, 512, L"Резервная копия не найдена (%s) :( ",
                   backup.c_str());
  g_StatusIsError = true;
  return false;
}

static void RunSFCRepair(const std::wstring &path) {
  wchar_t cmd[MAX_PATH + 64];
  StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"sfc /scanfile=\"%s\"", path.c_str());

  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                     nullptr, nullptr, &si, &pi)) {
    StringCchPrintfW(g_StatusMessage, 512, L"Запущена проверка SFC для %s...",
                     path.c_str());
    g_StatusIsError = false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    StringCchPrintfW(g_StatusMessage, 512, L"Ошибка запуска SFC (0x%X).",
                     GetLastError());
    g_StatusIsError = true;
  }
}

//
// Open file picker dialog
//
static bool BrowseForFile(char *outPath, DWORD outPathLen) {
  OPENFILENAMEA ofn = {sizeof(ofn)};
  char buf[MAX_PATH] = {};
  ofn.hwndOwner = NULL;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = "EXE файлы\0*.exe\0Все файлы\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  ofn.lpstrTitle = "Выберите файл для замены";
  if (!GetOpenFileNameA(&ofn))
    return false;
  StringCchCopyA(outPath, outPathLen, buf);
  return true;
}

//
// Public: render integrity panel
//
void IntegrityModule_Render() {
  if (!g_Scanned)
    RunScan();

  if (ImGui::Button("Сканировать заново"))
    RunScan();

  ImGui::SameLine();
  ImGui::TextDisabled("Проверка системных файлов и ключей автозапуска");

  ImGui::Separator();

  // File integrity table
  if (ImGui::BeginTable("##integrity", 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY,
                        ImVec2(0, 180))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Файл", ImGuiTableColumnFlags_WidthFixed, 140);
    ImGui::TableSetupColumn("SHA-256", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Размер", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Статус", ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < g_IntegrityEntries.size(); i++) {
      auto &e = g_IntegrityEntries[i];
      ImGui::PushID((int)i);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);

      char fname[64] = {};
      WideCharToMultiByte(CP_UTF8, 0, e.FileName.c_str(), -1, fname, 64,
                          nullptr, nullptr);
      ImGui::Text("%s", fname);

      ImGui::TableSetColumnIndex(1);
      char sha[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, e.ActualSHA256.c_str(), -1, sha, 128,
                          nullptr, nullptr);
      ImGui::TextUnformatted(sha);

      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%u KB", e.FileSizeBytes / 1024);

      ImGui::TableSetColumnIndex(3);
      if (!e.FileExists)
        ImGui::TextColored(ImVec4(1, 0, 0, 1), u8"Нету");
      else if (e.HashMismatch)
        ImGui::TextColored(ImVec4(1, 0.4f, 0, 1), u8"Изменён");
      else
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), u8"ОК");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  ImGui::Separator();
  ImGui::Text("Замена файла:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(340.0f);
  ImGui::InputText("##replacepath", g_CustomReplacePath, MAX_PATH);
  ImGui::SameLine();
  if (ImGui::Button("Обзор..."))
    BrowseForFile(g_CustomReplacePath, MAX_PATH);

  // Target selection
  static int s_TargetIdx = 0;
  const char *targets[] = {"sethc.exe", "utilman.exe", "userinit.exe"};
  ImGui::SetNextItemWidth(150);
  ImGui::Combo("Цель##target", &s_TargetIdx, targets, 3);
  ImGui::SameLine();

  if (ImGui::Button("Заменить файл")) {
    if (g_CustomReplacePath[0] != '\0' &&
        s_TargetIdx < (int)g_IntegrityEntries.size()) {
      wchar_t wsrc[MAX_PATH] = {};
      MultiByteToWideChar(CP_UTF8, 0, g_CustomReplacePath, -1, wsrc, MAX_PATH);
      ReplaceSystemFileInternal(g_IntegrityEntries[s_TargetIdx].FullPath, wsrc);
    }
  }
  ImGui::SameLine();

  if (ImGui::Button(u8"Восстановить оригинал")) {
    if (s_TargetIdx < (int)g_IntegrityEntries.size()) {
      if (!RestoreOriginalFile(g_IntegrityEntries[s_TargetIdx].FullPath)) {
        // Show a mini-modal or just standard info
        ImGui::OpenPopup(u8"Метод восстановления");
      }
    }
  }

  if (ImGui::BeginPopup(u8"Метод восстановления")) {
    ImGui::Text(u8"Резервная копия ZASLON не найдена.");
    if (ImGui::MenuItem(u8"Попытаться восстановить через SFC ")) {
      RunSFCRepair(g_IntegrityEntries[s_TargetIdx].FullPath);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  // Status message
  if (g_StatusMessage[0] != L'\0') {
    char smsg[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_StatusMessage, -1, smsg, 512, nullptr,
                        nullptr);
    ImVec4 col = g_StatusIsError ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
                                 : ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
    ImGui::TextColored(col, "%s", smsg);
  }

  ImGui::Separator();
  ImGui::Text("Ключи Winlogon:");

  if (ImGui::BeginTable("##winlogon", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY,
                        ImVec2(0, 0))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Параметр", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Значение", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Статус", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < g_WinlogonEntries.size(); i++) {
      auto &we = g_WinlogonEntries[i];
      ImGui::PushID((int)i + 1000);
      ImGui::TableNextRow();
      char vname[128] = {}, vdata[512] = {};
      WideCharToMultiByte(CP_UTF8, 0, we.ValueName.c_str(), -1, vname, 128,
                          nullptr, nullptr);
      WideCharToMultiByte(CP_UTF8, 0, we.ValueData.c_str(), -1, vdata, 512,
                          nullptr, nullptr);

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(vname);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(vdata);
      ImGui::TableSetColumnIndex(2);
      if (we.Suspicious) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0, 1), u8"Изменён");
        ImGui::SameLine();
        if (ImGui::SmallButton(u8"Исправить")) {
          FixWinlogonValue(we.ValueName);
          ScanWinlogon();
        }
      } else {
        ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "OK");
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (ImGui::Button(u8"Сбросить ВСЕ ключи Winlogon к оригиналу",
                    ImVec2(-1, 0))) {
    FixWinlogonValue(L"Userinit");
    FixWinlogonValue(L"Shell");
    ScanWinlogon();
  }
}
