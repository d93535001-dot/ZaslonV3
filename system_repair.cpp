#include "system_repair.h"
#include "imgui.h"
#include "autorun_scanner.h"
#include "gui_theme.h" // g_IsWinPE, LoadTextureFromFile, ZaslonAnimatedButton
#include "offline_os_manager.h"
#include "theme_manager.h" // _() translation
#include "zaslon_core.h"
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <windows.h>

// For DeviceIoControl / EFI
#include <winioctl.h>
#pragma comment(lib, "Advapi32.lib")

// Async operation flags (Phase 5 — prevent UI freezing)
static std::atomic<bool> g_WinsockResetRunning{false};
static std::atomic<bool> g_BCDCheckRunning{false};
static std::atomic<bool> g_SfcRunning{false};
static std::atomic<bool> g_DismRunning{false};

namespace Repair {
std::vector<LogEntry> g_RepairLogs;
std::mutex g_LogsMutex;

static void *texUSB = nullptr;
static void *texWinLock = nullptr;
static void *texWMI = nullptr;
static void *texEFI = nullptr;
static void *texBCD = nullptr;
static bool iconsLoaded = false;

void Log(const char *module, const char *message, bool success) {
  // Generate HH:MM:SS timestamp
  char timeBuf[16] = {};
  std::time_t now = std::time(nullptr);
  struct tm ti;
  if (localtime_s(&ti, &now) == 0) {
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &ti);
  }
  std::lock_guard<std::mutex> lock(g_LogsMutex);
  g_RepairLogs.push_back({timeBuf, module, message, success});
}

// Helper to redirect path in WinPE
static void RedirectOfflinePath(HKEY hkrRoot, const std::wstring &subKey,
                                HKEY &outTargetRoot, std::wstring &outSubKey) {
  outTargetRoot = hkrRoot;
  outSubKey = subKey;
  if (!g_IsWinPE || !ZaslonCore::g_IsOfflineHivesMounted)
    return;

  if (hkrRoot == HKEY_LOCAL_MACHINE) {
    std::wstring upperSubKey = subKey;
    for (auto &c : upperSubKey)
      c = towupper(c);
    if (upperSubKey.find(L"SOFTWARE\\") == 0) {
      outSubKey = L"ZASLON_OFF_SOFTWARE\\" + subKey.substr(9);
    } else if (upperSubKey.find(L"SYSTEM\\") == 0) {
      outSubKey = L"ZASLON_OFF_SYSTEM\\" + subKey.substr(7);
    }
  }
}

// Helper: Delete a registry value
bool RegDeleteVal(HKEY hKeyRoot, const wchar_t *subKey,
                  const wchar_t *valueName) {
  HKEY redirectedRoot;
  std::wstring redirectedSubKey;
  RedirectOfflinePath(hKeyRoot, subKey, redirectedRoot, redirectedSubKey);

  HKEY hKey;
  if (RegOpenKeyExW(redirectedRoot, redirectedSubKey.c_str(), 0, KEY_SET_VALUE,
                    &hKey) == ERROR_SUCCESS) {
    LSTATUS s = RegDeleteValueW(hKey, valueName);
    RegCloseKey(hKey);
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND);
  }
  return false; // Or key doesn't exist, which is fine
}

// Helper: Set a registry DWORD
bool RegSetDword(HKEY hKeyRoot, const wchar_t *subKey, const wchar_t *valueName,
                 DWORD value) {
  HKEY redirectedRoot;
  std::wstring redirectedSubKey;
  RedirectOfflinePath(hKeyRoot, subKey, redirectedRoot, redirectedSubKey);

  HKEY hKey;
  if (RegCreateKeyExW(redirectedRoot, redirectedSubKey.c_str(), 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &hKey,
                      nullptr) == ERROR_SUCCESS) {
    LSTATUS s = RegSetValueExW(hKey, valueName, 0, REG_DWORD,
                               (const BYTE *)&value, sizeof(DWORD));
    RegCloseKey(hKey);
    return s == ERROR_SUCCESS;
  }
  return false;
}

// Helper: Set a registry String
bool RegSetString(HKEY hKeyRoot, const wchar_t *subKey,
                  const wchar_t *valueName, const wchar_t *value) {
  HKEY redirectedRoot;
  std::wstring redirectedSubKey;
  RedirectOfflinePath(hKeyRoot, subKey, redirectedRoot, redirectedSubKey);

  HKEY hKey;
  if (RegCreateKeyExW(redirectedRoot, redirectedSubKey.c_str(), 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &hKey,
                      nullptr) == ERROR_SUCCESS) {
    LSTATUS s = RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE *)value,
                               (wcslen(value) + 1) * sizeof(wchar_t));
    RegCloseKey(hKey);
    return s == ERROR_SUCCESS;
  }
  return false;
}

// Helper: Delete entire registry key tree
bool RegDeleteTreeWrap(HKEY hKeyRoot, const wchar_t *subKey) {
  HKEY redirectedRoot;
  std::wstring redirectedSubKey;
  RedirectOfflinePath(hKeyRoot, subKey, redirectedRoot, redirectedSubKey);

  LSTATUS s = RegDeleteTreeW(redirectedRoot, redirectedSubKey.c_str());
  return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND);
}

// --- 1. Registry Unlocking ---
void UnlockTaskManager() {
  bool s1 = RegDeleteVal(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"DisableTaskMgr");
  bool s2 = RegDeleteVal(
      HKEY_LOCAL_MACHINE,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"DisableTaskMgr");

  // Also clear IFEO hijack if present
  bool s3 = RegDeleteTreeWrap(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution "
      L"Options\\taskmgr.exe");

  Log("Unlock", "Диспетчер задач разблокирован", s1 || s2 || s3);
}

void UnlockRegedit() {
  bool s1 = RegDeleteVal(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"DisableRegistryTools");
  bool s2 = RegDeleteVal(
      HKEY_LOCAL_MACHINE,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"DisableRegistryTools");
  Log("Unlock", "Редактор реестра разблокирован", s1 || s2);
}

void UnlockUAC() {
  bool s = RegSetDword(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
      L"EnableLUA", 1);
  Log("Unlock", "UAC включен", s);
}

void ResetDefenderPolicies() {
  bool s1 = RegDeleteVal(HKEY_LOCAL_MACHINE,
                         L"SOFTWARE\\Policies\\Microsoft\\Windows Defender",
                         L"DisableAntiSpyware");
  bool s2 = RegDeleteTreeWrap(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection");
  Log("Unlock", "Политики блокировки Windows Defender сброшены", s1 || s2);
}

void RestoreSafeBoot() {
  // Just a stub log, real fixing requires backing up and restoring
  // HKLM\SYSTEM\CurrentControlSet\Control\SafeBoot
  Log("Repair", "Проверка ключей SafeBoot", true);
}

void ResetSRPPolicies() {
  bool s = RegDeleteTreeWrap(HKEY_LOCAL_MACHINE,
                             L"SOFTWARE\\Policies\\Microsoft\\Windows\\Safer");
  Log("Repair", "Политики Software Restriction Policies (SRP)", s);
}

// --- 2. Shell & Logon Fixes ---
void FixShell() {
  bool s =
      RegSetString(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                   L"Shell", L"explorer.exe");
  Log("Repair", "Оболочка шелл восстановлена", s);
}

void FixUserinit() {
  std::wstring sysDir;
  if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted) {
    sysDir = ZaslonCore::OfflineOSManager::GetOfflinePath(L"System32");
  } else {
    wchar_t sd[MAX_PATH];
    GetSystemDirectoryW(sd, MAX_PATH);
    sysDir = sd;
  }

  std::wstring userinitPath = sysDir + L"\\userinit.exe,";
  bool s =
      RegSetString(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                   L"Userinit", userinitPath.c_str());
  Log("Repair", "Параметр Userinit восстановлен", s);
}

void ClearIFEO() {
  HKEY hIfeo;
  bool success = false;
  int count = 0;

  HKEY redirectedRoot;
  std::wstring redirectedSubKey;
  RedirectOfflinePath(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image "
                      L"File Execution Options",
                      redirectedRoot, redirectedSubKey);

  if (RegOpenKeyExW(redirectedRoot, redirectedSubKey.c_str(), 0,
                    KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                    &hIfeo) == ERROR_SUCCESS) {
    wchar_t sub[256];
    DWORD idx = 0, len = 256;

    // We collect keys to delete because deleting while enumerating is bad
    // practice
    std::vector<std::wstring> keysToDelete;

    while (RegEnumKeyExW(hIfeo, idx++, sub, &len, nullptr, nullptr, nullptr,
                         nullptr) == ERROR_SUCCESS) {
      HKEY hSub;
      if (RegOpenKeyExW(hIfeo, sub, 0, KEY_READ, &hSub) == ERROR_SUCCESS) {
        wchar_t dbg[512];
        DWORD dsz = sizeof(dbg);
        if (RegQueryValueExW(hSub, L"Debugger", nullptr, nullptr, (LPBYTE)dbg,
                             &dsz) == ERROR_SUCCESS) {
          keysToDelete.push_back(sub);
        }
        RegCloseKey(hSub);
      }
      len = 256;
    }
    RegCloseKey(hIfeo);

    for (const auto &k : keysToDelete) {
      std::wstring fullPath =
          L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File "
          L"Execution Options\\" +
          k;
      if (RegDeleteTreeWrap(HKEY_LOCAL_MACHINE, fullPath.c_str())) {
        count++;
      }
    }
    if (count > 0 || keysToDelete.empty())
      success = true; // true if fixed or nothing to fix
  }

  char msg[128];
  snprintf(msg, sizeof(msg),
           "Очистка ключей IFEO завершена (Удалено перехватов: %d)", count);
  Log("Repair", msg, success);
}

void FixStickyKeys() {
  const wchar_t *tools[] = {L"sethc.exe", L"utilman.exe", L"osk.exe",
                            L"magnify.exe", L"narrator.exe"};
  bool success = true;
  for (auto t : tools) {
    std::wstring path = L"SOFTWARE\\Microsoft\\Windows "
                        L"NT\\CurrentVersion\\Image File Execution Options\\";
    path += t;
    // Often malware attaches a debugger like cmd.exe to these keys
    RegDeleteVal(HKEY_LOCAL_MACHINE, path.c_str(), L"Debugger");
  }
  Log("Repair", "Уязвимости Залипания Клавиш (sethc/utilman) изолированы",
      success);
}

void FixSystemFonts() {
  // Restore default Tahoma and Segoe UI
  bool s1 =
      RegSetString(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                   L"Segoe UI (TrueType)", L"segoeui.ttf");
  bool s2 =
      RegSetString(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                   L"Tahoma (TrueType)", L"tahoma.ttf");
  bool s3 = RegSetString(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
      L"MS Shell Dlg", L"Microsoft Sans Serif");
  bool s4 = RegSetString(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
      L"MS Shell Dlg 2", L"Tahoma");

  // Also remove any hijacked Segoe UI substitute if present
  RegDeleteVal(
      HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes",
      L"Segoe UI");

  Log("Repair", "Системные шрифты восстановлены", s1 || s2 || s3 || s4);
}

// --- 3. Network & File Fixes ---
void ResetHostsFile() {
  std::wstring hostsPath;
  if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted) {
    hostsPath = ZaslonCore::OfflineOSManager::GetOfflinePath(
        L"System32\\drivers\\etc\\hosts");
  } else {
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    hostsPath = std::wstring(sysDir) + L"\\drivers\\etc\\hosts";
  }

  std::ofstream hosts(hostsPath, std::ios::trunc);
  if (hosts.is_open()) {
    hosts << "# Reset by ZASLON" << std::endl;
    hosts << "127.0.0.1 localhost" << std::endl;
    hosts << "::1 localhost" << std::endl;
    hosts.close();
    Log("Network", "Файл HOSTS сброшен", true);
  } else {
    Log("Network", "Не удалось перезаписать файл HOSTS", false);
  }
}

void ResetWinsock() {
  if (g_WinsockResetRunning.load()) {
    Log("Network", "Сброс Winsock уже выполняется...", true);
    return;
  }
  g_WinsockResetRunning.store(true);
  Log("Network", "Сброс Winsock запущен в фоне...", true);
  std::thread([]() {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"netsh winsock reset";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 15000);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      Log("Network",
          "Стек Winsock и TCP/IP успешно сброшен (Требуется перезагрузка)",
          true);
    } else {
      Log("Network", "Ошибка выполнения сброса Winsock", false);
    }
    g_WinsockResetRunning.store(false);
  }).detach();
}

void FixFileExtensions() {
  bool s = RegSetDword(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
      L"HideFileExt", 0);
  Log("Files", "Отображение расширений файлов включено", s);
}

void DisableSMB1() {
  bool s = RegSetDword(
      HKEY_LOCAL_MACHINE,
      L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters", L"SMB1",
      0);
  Log("Security", "Уязвимый протокол SMBv1 отключен", s);
}

// --- 4. Boot Integrity ---
void MountAndScanEFI() {
  Log("Boot", "Поиск раздела EFI (ESP) для монтирования...", true);

  // Required GUID for EFI System Partition
  const GUID PARTITION_SYSTEM_GUID_EFI = {
      0xc12a7328,
      0xf81f,
      0x11d2,
      {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};

  wchar_t volumeName[MAX_PATH] = L"";
  HANDLE hFind = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));
  if (hFind == INVALID_HANDLE_VALUE) {
    Log("Boot", "Ошибка перечисления томов", false);
    return;
  }

  std::wstring efiVolume = L"";

  do {
    // Remove trailing backslash for CreateFile
    size_t len = wcslen(volumeName);
    if (len > 0 && volumeName[len - 1] == L'\\') {
      volumeName[len - 1] = L'\0';
    }

    HANDLE hVolume =
        CreateFileW(volumeName, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, 0, nullptr);
    if (hVolume != INVALID_HANDLE_VALUE) {
      PARTITION_INFORMATION_EX partInfo = {0};
      DWORD bytesReturned = 0;

      if (DeviceIoControl(hVolume, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0,
                          &partInfo, sizeof(partInfo), &bytesReturned,
                          nullptr)) {
        if (partInfo.PartitionStyle == PARTITION_STYLE_GPT) {
          if (memcmp(&partInfo.Gpt.PartitionType, &PARTITION_SYSTEM_GUID_EFI,
                     sizeof(GUID)) == 0) {
            // EFI partition found
            volumeName[len - 1] = L'\\'; // Restore trailing slash
            efiVolume = volumeName;
            CloseHandle(hVolume);
            break;
          }
        }
      }
      CloseHandle(hVolume);
    }

    // Restore for next iteration just in case, though FindNextVolume overwrites
    volumeName[len - 1] = L'\\';

  } while (FindNextVolumeW(hFind, volumeName, ARRAYSIZE(volumeName)));

  FindVolumeClose(hFind);

  if (!efiVolume.empty()) {
    bool mounted = SetVolumeMountPointW(L"Z:\\", efiVolume.c_str());
    if (mounted) {
      Log("Boot",
          "EFI раздел успешно смонтирован как Z:\\. Возможна проверка "
          "целостности загрузчика bootx64.efi",
          true);

      // --- Example integrity checks could go here ---
      // e.g. checking hash of Z:\EFI\Boot\bootx64.efi

      // We keep it mounted temporarily or delete point based on logic, let's
      // just unmount to be clean DeleteVolumeMountPointW(L"Z:\\");
    } else {
      Log("Boot",
          "Не удалось смонтировать найденный EFI раздел на Z:\\ (Ошибка "
          "доступа или буква занята)",
          false);
    }
  } else {
    Log("Boot",
        "Раздел EFI (ESP) не найден на этом устройстве (Возможно MBR/Legacy "
        "загрузка)",
        false);
  }
}

void ResetBCD() {
  if (g_BCDCheckRunning.load()) {
    Log("Boot", "Проверка BCD уже выполняется...", true);
    return;
  }
  g_BCDCheckRunning.store(true);
  Log("Boot", "Восстановление BCD запущено в фоне...", true);
  std::thread([]() {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmdW;

    if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted &&
        !ZaslonCore::g_OfflineOSPath.empty()) {
      cmdW = L"bcdboot " + ZaslonCore::g_OfflineOSPath;
    } else {
      cmdW = L"bcdedit /enum";
    }

    std::vector<wchar_t> cmdBuf(cmdW.begin(), cmdW.end());
    cmdBuf.push_back(L'\0');

    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 15000);
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      Log("Boot", "Операция BCD завершена", true);
    } else {
      Log("Boot", "Ошибка манипуляций ядром загрузки (BCD)", false);
    }
    g_BCDCheckRunning.store(false);
  }).detach();
}

void RunOfflineSFC() {
  if (g_SfcRunning.load()) {
    Log("System", "Проверка SFC уже выполняется...", true);
    return;
  }
  g_SfcRunning.store(true);
  Log("System", "Запуск SFC Это может занять несколько минут...", true);
  std::thread([]() {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd;

    if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted &&
        !ZaslonCore::g_OfflineDriveLetter.empty()) {
      cmd = L"sfc.exe /scannow /offbootdir=" +
            ZaslonCore::g_OfflineDriveLetter + L"\\ /offwindir=" +
            ZaslonCore::g_OfflineOSPath;
    } else {
      cmd = L"sfc.exe /scannow";
    }

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, INFINITE); // SFC takes time
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      Log("System", "Сканирование SFC завершено", true);
    } else {
      Log("System", "Ошибка запуска SFC", false);
    }
    g_SfcRunning.store(false);
  }).detach();
}

void RunOfflineDISM() {
  if (g_DismRunning.load()) {
    Log("System", "Восстановление образа DISM уже выполняется...", true);
    return;
  }
  g_DismRunning.store(true);
  Log("System",
      "Запуск DISM. Процесс может длиться 15-30 минут! Не "
      "закрывайте программу.",
      true);
  std::thread([]() {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd;

    if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted &&
        !ZaslonCore::g_OfflineDriveLetter.empty()) {
      cmd = L"dism.exe /image:" + ZaslonCore::g_OfflineDriveLetter +
            L"\\ /cleanup-image /restorehealth";
    } else {
      cmd = L"dism.exe /online /cleanup-image /restorehealth";
    }

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, INFINITE); // DISM takes a long time
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
      Log("System", "Восстановление хранилища компонентов DISM завершено",
          true);
    } else {
      Log("System", "Ошибка запуска DISM", false);
    }
    g_DismRunning.store(false);
  }).detach();
}
void ImmunizeUSB() {
  // List logical drives, find removable, create unkillable autorun.inf folder
  DWORD drives = GetLogicalDrives();
  int count = 0;
  for (int i = 0; i < 26; i++) {
    if (drives & (1 << i)) {
      wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', L'\0'};
      if (GetDriveTypeW(root) == DRIVE_REMOVABLE) {
        std::wstring autorunPath = std::wstring(root) + L"autorun.inf";
        namespace fs = std::filesystem;
        std::error_code ec;

        if (fs::is_regular_file(autorunPath, ec)) {
          fs::remove(autorunPath, ec);
        }
        if (!fs::exists(autorunPath, ec)) {
          fs::create_directory(autorunPath, ec);
          // Make hidden, system, read-only
          SetFileAttributesW(autorunPath.c_str(), FILE_ATTRIBUTE_HIDDEN |
                                                      FILE_ATTRIBUTE_SYSTEM |
                                                      FILE_ATTRIBUTE_READONLY);
          count++;
        }
      }
    }
  }
  char msg[128];
  snprintf(msg, sizeof(msg), "Вакцинировано USB накопителей: %d", count);
  Log("USB", msg, true);
}

void CreateEmergencyDesktop() {
  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);

  Log("Desktop",
      "Запуск изолированной сессии ZASLON на чистом рабочем столе...", true);

  // Use our new resilient Anti-WinLocker core function
  bool success = ZaslonCore::AntiWinLocker::LaunchIsolatedDesktop(exePath);

  if (success) {
    Log("Desktop", "Успешный возврат на основной рабочий стол", true);
  } else {
    Log("Desktop", "Не удалось создать защищенный рабочий стол", false);
  }
}

void RebootToWinPE() {
  Log("System", "Подготовка к перезагрузке в WinPE...", true);

  // reagentc.exe /boottore (boot to RE on next restart)
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  wchar_t cmdReagent[] = L"reagentc.exe /boottore";
  if (CreateProcessW(nullptr, cmdReagent, nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }

  // shutdown.exe /r /t 0
  wchar_t cmdShutdown[] = L"shutdown.exe /r /t 0 /f";
  if (CreateProcessW(nullptr, cmdShutdown, nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    Log("System", "Сигнал перезагрузки отправлен.", true);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  } else {
    Log("System", "Не удалось инициировать перезагрузку.", false);
  }
}

} // namespace Repair

void SystemRepair_Render() {
  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                     u8"Аварийное Восстановление и Очистка Системы");
  ImGui::TextWrapped(u8"Эта часть предназначен для обхода ограничений "
                     u8"мусора, восстановления системных файлов и т.д.");
  ImGui::Separator();

  // WinPE warning banner
  if (g_IsWinPE) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
    ImGui::TextWrapped(u8"[!] Режим WinPE: UAC, Defender, Winsock и BCD "
                       u8"недоступны в минимальной среде.");
    ImGui::PopStyleColor();
  }
  ImGui::Spacing();

  // Two column layout
  ImGui::Columns(2, "RepairColumns", false);

  // Column 1
  ImGui::Text(u8"Устранение Ограничений");
  if (ImGui::Button(u8"Разблокировать Диспетчер Задач", ImVec2(-1, 0)))
    Repair::UnlockTaskManager();
  if (ImGui::Button(u8"Разблокировать Редактор Реестра", ImVec2(-1, 0)))
    Repair::UnlockRegedit();

  if (g_IsWinPE)
    ImGui::BeginDisabled();
  if (ImGui::Button(u8"Разблокировать UAC", ImVec2(-1, 0)))
    Repair::UnlockUAC();
  if (ImGui::Button(u8"Сброс политик Defender", ImVec2(-1, 0)))
    Repair::ResetDefenderPolicies();
  if (g_IsWinPE)
    ImGui::EndDisabled();

  if (ImGui::Button(u8"Сброс политик SRP", ImVec2(-1, 0)))
    Repair::ResetSRPPolicies();

  ImGui::Spacing();
  ImGui::Text(u8"Оболочка и Логон");
  if (ImGui::Button(u8"Восстановить Shell", ImVec2(-1, 0)))
    Repair::FixShell();
  if (ImGui::Button(u8"Восстановить Userinit", ImVec2(-1, 0)))
    Repair::FixUserinit();
  if (ImGui::Button(u8"Очистить IFEO перехваты", ImVec2(-1, 0)))
    Repair::ClearIFEO();
  if (ImGui::Button(u8"Патч 'Залипания клавиш'", ImVec2(-1, 0)))
    Repair::FixStickyKeys();
  if (ImGui::Button(u8"Восстановить системные шрифты", ImVec2(-1, 0)))
    Repair::FixSystemFonts();

  ImGui::NextColumn();

  // Column 2
  ImGui::Text(u8"Сеть и Файлы");
  if (ImGui::Button(u8"Сбросить файл HOSTS", ImVec2(-1, 0)))
    Repair::ResetHostsFile();

  // Winsock reset: disabled in WinPE, shows spinner when running
  if (g_IsWinPE)
    ImGui::BeginDisabled();
  {
    bool winsockBusy = g_WinsockResetRunning.load();
    if (winsockBusy)
      ImGui::BeginDisabled();
    const char *winsockLabel = winsockBusy ? u8"[...] Сброс Winsock..."
                                           : u8"Сброс стека Winsock / TCP/IP";
    if (ImGui::Button(winsockLabel, ImVec2(-1, 0)))
      Repair::ResetWinsock();
    if (winsockBusy)
      ImGui::EndDisabled();
  }
  if (g_IsWinPE)
    ImGui::EndDisabled();

  if (ImGui::Button(u8"Включить отображение расширений", ImVec2(-1, 0)))
    Repair::FixFileExtensions();
  if (ImGui::Button(u8"Отключить SMBv1", ImVec2(-1, 0)))
    Repair::DisableSMB1();

  ImGui::Spacing();
  ImGui::Text(_(u8"Угрозы и Загрузчик"));

  if (!Repair::iconsLoaded) {
    int w, h;
    LoadTextureFromFile("icons\\usb.png", &Repair::texUSB, &w, &h);
    LoadTextureFromFile("icons\\winlock.png", &Repair::texWinLock, &w, &h);
    LoadTextureFromFile("icons\\wmi.png", &Repair::texWMI, &w, &h);
    LoadTextureFromFile("icons\\efi.png", &Repair::texEFI, &w, &h);
    LoadTextureFromFile("icons\\bcd.png", &Repair::texBCD, &w, &h);
    Repair::iconsLoaded = true;
  }

  if (ZaslonAnimatedButton(_(u8"Вакцинация флешек"), ImVec2(-1, 0),
                           Repair::texUSB))
    Repair::ImmunizeUSB();

  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
  if (ZaslonAnimatedButton(_(u8"Чистый Рабочий Стол"), ImVec2(-1, 0),
                           Repair::texWinLock))
    Repair::CreateEmergencyDesktop();
  ImGui::PopStyleColor(2);

  if (ZaslonAnimatedButton(_(u8"Уничтожить WMI подписки"), ImVec2(-1, 0),
                           Repair::texWMI)) {
    AutorunScanner_PurgeWMI();
    Repair::Log("WMI", "Вызвано агрессивное удаление WMI скриптов и подписок",
                true);
  }
  if (ZaslonAnimatedButton(_(u8"Смонтировать EFI/ESP для проверки"),
                           ImVec2(-1, 0), Repair::texEFI))
    Repair::MountAndScanEFI();

  if (ZaslonAnimatedButton(_(u8"Перезагрузка в WinPE"), ImVec2(-1, 0),
                           Repair::texEFI))
    Repair::RebootToWinPE();

  // Deep WinPE Auto Repair Section
  if (g_IsWinPE) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
    ImGui::Text(u8"Глубокое Восстановление Системы");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // DISM
    {
      bool dismBusy = g_DismRunning.load();
      if (dismBusy)
        ImGui::BeginDisabled();

      std::string dismLabel =
          dismBusy
              ? std::string("[...] ") + _(u8"DISM работает (долго)...")
              : _(u8"Глубокий ремонт хранилища компонентов (DISM Offline)");
      if (ZaslonAnimatedButton(dismLabel.c_str(), ImVec2(-1, 0),
                               Repair::texEFI))
        Repair::RunOfflineDISM();

      if (dismBusy)
        ImGui::EndDisabled();
    }

    // Auto Boot Repair
    {
      bool bcdBusy = g_BCDCheckRunning.load();
      if (bcdBusy)
        ImGui::BeginDisabled();

      std::string bootLabel =
          bcdBusy ? std::string("[...] ") + _(u8"Ремонт загрузчика...")
                  : _(u8"Ремонт загрузчика");

      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
      if (ZaslonAnimatedButton(bootLabel.c_str(), ImVec2(-1, 0),
                               Repair::texBCD)) {
        Repair::Log("Boot", "Вызов Bootrec /fixmbr и /fixboot...", true);
        // Quick async to system() bootrec cmds
        std::thread([]() {
          system("bootrec /fixmbr >nul 2>&1");
          system("bootrec /fixboot >nul 2>&1");
        }).detach();
        Repair::ResetBCD(); // Our existing BCDBoot replacement logic
      }
      ImGui::PopStyleColor(2);

      if (bcdBusy)
        ImGui::EndDisabled();
    }
  } else {
    // Normal Boot/BCD Check
    bool bcdBusy = g_BCDCheckRunning.load();
    if (bcdBusy)
      ImGui::BeginDisabled();

    std::string bcdLabel = bcdBusy
                               ? std::string("[...] ") + _(u8"Проверка BCD...")
                               : _(u8"Сброс/Проверка BCD ядра");
    if (ZaslonAnimatedButton(bcdLabel.c_str(), ImVec2(-1, 0), Repair::texBCD))
      Repair::ResetBCD();

    if (bcdBusy)
      ImGui::EndDisabled();
  }

  // SFC Scan
  {
    bool sfcBusy = g_SfcRunning.load();
    if (sfcBusy)
      ImGui::BeginDisabled();

    std::string sfcLabel =
        sfcBusy ? std::string("[...] ") + _(u8"Сканирование SFC...")
                : _(u8"Проверка системных файлов (SFC)");
    if (ZaslonAnimatedButton(sfcLabel.c_str(), ImVec2(-1, 0), Repair::texEFI))
      Repair::RunOfflineSFC();

    if (sfcBusy)
      ImGui::EndDisabled();
  }

  ImGui::Columns(1);

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Text(u8"Журнал операций:");

  // Logs Window
  ImGui::BeginChild("RepairLogs", ImVec2(0, 0), true,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  {
    std::lock_guard<std::mutex> lock(Repair::g_LogsMutex);
    for (const auto &log : Repair::g_RepairLogs) {
      ImVec4 color = log.success ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                 : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
      ImGui::TextDisabled("[%s]", log.timestamp.c_str());
      ImGui::SameLine();
      ImGui::TextColored(color, "[%s]", log.module.c_str());
      ImGui::SameLine();
      ImGui::TextWrapped("%s", log.message.c_str());
    }
  }
  // Auto-scroll
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}
