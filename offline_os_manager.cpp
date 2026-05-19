#include "offline_os_manager.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace ZaslonCore {

std::wstring g_OfflineOSPath = L"";
std::wstring g_OfflineDriveLetter = L"";
bool g_IsOfflineHivesMounted = false;

static bool EnablePrivilege(LPCWSTR privName) {
  HANDLE hToken = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
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

bool OfflineOSManager::DetectOfflineOS() {
  DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; i++) {
    if (drives & (1 << i)) {
      wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', L'\0'};
      // Пропускаем диск X (обычно системный RAM-диск WinPE)
      if (root[0] == L'X' || root[0] == L'x') continue;

      UINT driveType = GetDriveTypeW(root);
      if (driveType == DRIVE_FIXED) {
        std::wstring windowsPath = std::wstring(root) + L"Windows";
        std::wstring systemConfigPath = windowsPath + L"\\System32\\config\\SYSTEM";
        
        std::error_code ec;
        if (fs::exists(systemConfigPath, ec)) {
          g_OfflineOSPath = windowsPath;
          g_OfflineDriveLetter = std::wstring(root, 2); // e.g., "D:"
          return true;
        }
      }
    }
  }
  return false;
}

bool OfflineOSManager::MountOfflineHives() {
  if (g_OfflineOSPath.empty()) return false;

  EnablePrivilege(SE_RESTORE_NAME);
  EnablePrivilege(SE_BACKUP_NAME);

  std::wstring configPath = g_OfflineOSPath + L"\\System32\\config\\";
  
  struct Hive {
      const wchar_t* name;
      const wchar_t* alias;
  };

  std::vector<Hive> hives = {
      {L"SOFTWARE", L"ZASLON_OFF_SOFTWARE"},
      {L"SYSTEM",   L"ZASLON_OFF_SYSTEM"},
      {L"SAM",      L"ZASLON_OFF_SAM"},
      {L"SECURITY", L"ZASLON_OFF_SECURITY"},
      {L"DEFAULT",  L"ZASLON_OFF_DEFAULT"}
  };

  bool allSuccess = true;
  for (const auto& h : hives) {
      std::wstring fullPath = configPath + h.name;
      LSTATUS status = RegLoadKeyW(HKEY_LOCAL_MACHINE, h.alias, fullPath.c_str());
      if (status != ERROR_SUCCESS) {
          allSuccess = false;
      }
  }

  if (allSuccess) {
    g_IsOfflineHivesMounted = true;
    return true;
  }
  
  return false;
}

void OfflineOSManager::UnmountOfflineHives() {
  if (!g_IsOfflineHivesMounted) return;

  EnablePrivilege(SE_RESTORE_NAME);
  RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"ZASLON_OFF_SOFTWARE");
  RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"ZASLON_OFF_SYSTEM");
  RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"ZASLON_OFF_SAM");
  RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"ZASLON_OFF_SECURITY");
  RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"ZASLON_OFF_DEFAULT");
  g_IsOfflineHivesMounted = false;
}

HKEY OfflineOSManager::OpenRedirectedKey(HKEY hkrRoot, const wchar_t* subKey, REGSAM samDesired) {
  HKEY outKey = nullptr;
  std::wstring subKeyStr = subKey ? subKey : L"";
  
  extern bool g_IsWinPE;
  if (!g_IsWinPE || !g_IsOfflineHivesMounted) {
    if (RegOpenKeyExW(hkrRoot, subKey, 0, samDesired, &outKey) == ERROR_SUCCESS) {
      return outKey;
    }
    return nullptr;
  }

  std::wstring finalSubKey = subKeyStr;
  HKEY targetRoot = HKEY_LOCAL_MACHINE;

  if (hkrRoot == HKEY_LOCAL_MACHINE) {
    std::wstring upperSubKey = subKeyStr;
    for (auto & c: upperSubKey) c = towupper(c);

    if (upperSubKey.find(L"SOFTWARE\\") == 0) {
      finalSubKey = L"ZASLON_OFF_SOFTWARE\\" + subKeyStr.substr(9); 
    } else if (upperSubKey.find(L"SOFTWARE") == 0 && upperSubKey.length() == 8) {
      finalSubKey = L"ZASLON_OFF_SOFTWARE";
    } else if (upperSubKey.find(L"SYSTEM\\") == 0) {
      finalSubKey = L"ZASLON_OFF_SYSTEM\\" + subKeyStr.substr(7);
    } else if (upperSubKey.find(L"SYSTEM") == 0 && upperSubKey.length() == 6) {
      finalSubKey = L"ZASLON_OFF_SYSTEM";
    } else if (upperSubKey.find(L"SAM\\") == 0) {
      finalSubKey = L"ZASLON_OFF_SAM\\" + subKeyStr.substr(4);
    } else if (upperSubKey.find(L"SAM") == 0 && upperSubKey.length() == 3) {
      finalSubKey = L"ZASLON_OFF_SAM";
    } else if (upperSubKey.find(L"SECURITY\\") == 0) {
      finalSubKey = L"ZASLON_OFF_SECURITY\\" + subKeyStr.substr(9);
    } else if (upperSubKey.find(L"SECURITY") == 0 && upperSubKey.length() == 8) {
        finalSubKey = L"ZASLON_OFF_SECURITY";
    }
  } else if (hkrRoot == HKEY_CURRENT_USER) {
      finalSubKey = L"ZASLON_OFF_DEFAULT\\" + subKeyStr;
  } else if (hkrRoot == HKEY_USERS) {
      // Basic HKU redirection to offline DEFAULT if no specific SID is provided
      if (subKeyStr.empty() || subKeyStr == L".DEFAULT") {
          finalSubKey = L"ZASLON_OFF_DEFAULT";
      } else {
          // In WinPE we usually don't have other SID hives mounted unless we search for NTUSER.DAT in user profiles
          // For now, redirect to HKLM original if not handled
      }
  }

  if (RegOpenKeyExW(targetRoot, finalSubKey.c_str(), 0, samDesired, &outKey) == ERROR_SUCCESS) {
    return outKey;
  }

  // Final fallback to original path if redirection failed to find the key
  if (RegOpenKeyExW(hkrRoot, subKey, 0, samDesired, &outKey) == ERROR_SUCCESS) {
      return outKey;
  }
  return nullptr;
}

std::wstring OfflineOSManager::GetOfflinePath(const std::wstring& relativeToWindows) {
    if (g_OfflineOSPath.empty()) return relativeToWindows; // Fallback or logic error
    
    // Check if input is already absolute (starts with \)
    if (!relativeToWindows.empty() && relativeToWindows[0] == L'\\') {
        return g_OfflineOSPath + relativeToWindows;
    }
    
    return g_OfflineOSPath + L"\\" + relativeToWindows;
}


} // namespace ZaslonCore
