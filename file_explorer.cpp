/**
 * file_explorer.cpp
 * ZASLON — Custom File Explorer
 *
 * Simple file manager implemented in ImGui for fallback navigation.
 */
#include "file_explorer.h"
#include "imgui.h"
#include "gui_theme.h" // For g_IsWinPE
#include "gui_utils.h"
#include "zaslon_core.h"


#include <algorithm>
#include <atomic>
#include <commdlg.h>
#include <filesystem>
#include <mutex>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <thread>
#include <vector>
#include <wincrypt.h>
#include <windows.h>


namespace fs = std::filesystem;

struct FileEntry {
  std::string Name;
  std::string Path;
  bool IsDirectory;
  uintmax_t Size;
  std::string DateModified;
};
static std::string g_CurrentPath = "C:\\";
static char g_PathBuffer[MAX_PATH] = "C:\\";
static std::vector<FileEntry> g_Files;
static bool g_NeedsRefresh = true;

// Async loading
static std::mutex g_FilesMutex;
static std::vector<FileEntry> g_FilesBackBuffer;
static std::atomic<bool> g_IsLoadingFiles{false};

static bool g_ShowUnlockerModal = false;
static std::string g_UnlockerTargetFile = "";
static std::vector<ZaslonCore::LockerInfo> g_Lockers;
static std::atomic<bool> g_IsUnlocking(false);

// Helper to convert wstring to utf8 string
static std::string WStringToString(const std::wstring &wstr) {
  if (wstr.empty())
    return std::string();
  int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(),
                                        NULL, 0, NULL, NULL);
  std::string strTo(size_needed, 0);
  WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0],
                      size_needed, NULL, NULL);
  return strTo;
}

// Convert string to wstring
static std::wstring StringToWString(const std::string &str) {
  if (str.empty())
    return std::wstring();
  int size_needed =
      MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
  std::wstring wstrTo(size_needed, 0);
  MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0],
                      size_needed);
  return wstrTo;
}

static bool g_ShowFileInspectorModal = false;
static std::string g_InspectorTargetFile = "";
static std::vector<std::string> g_InspectorLog;
static bool g_IsInspecting = false;

static std::wstring GetDesktopPath() {
  wchar_t path[MAX_PATH];
  if (SUCCEEDED(
          SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, path))) {
    return std::wstring(path);
  }
  return L"C:\\";
}

static std::string FormatBytes(uintmax_t bytes) {
  const char *suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  int s = 0;
  double count = static_cast<double>(bytes);
  while (count >= 1024 && s < 4) {
    s++;
    count /= 1024;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%.2f %s", count, suffixes[s]);
  return std::string(buf);
}

static void RefreshFilesAsync(std::string path) {
  std::vector<FileEntry> newFiles;
  try {
    fs::path p(path);
    if (!fs::exists(p) || !fs::is_directory(p)) {
      // Invalid, default back to C:\ or stay empty
      g_IsLoadingFiles = false;
      return;
    }

    if (p.has_parent_path() && p != p.parent_path()) {
      FileEntry parent;
      parent.Name = "..";
      parent.Path = p.parent_path().string();
      parent.IsDirectory = true;
      parent.Size = 0;
      parent.DateModified = "";
      newFiles.push_back(parent);
    }

    for (const auto &entry : fs::directory_iterator(
             path, fs::directory_options::skip_permission_denied)) {
      FileEntry f;
      f.Name = entry.path().filename().string();
      f.Path = entry.path().string();

      std::error_code ec;
      f.IsDirectory = fs::is_directory(entry.status(ec));

      if (!f.IsDirectory) {
        f.Size = fs::file_size(entry, ec);
        if (ec)
          f.Size = 0;
      } else {
        f.Size = 0;
      }

      auto ftime = fs::last_write_time(entry, ec);
      if (!ec) {
        auto sctp =
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - fs::file_time_type::clock::now() +
                std::chrono::system_clock::now());
        std::time_t cftime = std::chrono::system_clock::to_time_t(sctp);
        char timeBuf[64];
        struct tm ti;
        if (localtime_s(&ti, &cftime) == 0) {
          std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &ti);
          f.DateModified = timeBuf;
        }
      }
      newFiles.push_back(f);
    }
  } catch (...) {
  }

  std::sort(newFiles.begin(), newFiles.end(),
            [](const FileEntry &a, const FileEntry &b) {
              if (a.Name == "..")
                return true;
              if (b.Name == "..")
                return false;
              if (a.IsDirectory != b.IsDirectory)
                return a.IsDirectory > b.IsDirectory;
              return _stricmp(a.Name.c_str(), b.Name.c_str()) < 0;
            });

  {
    std::lock_guard<std::mutex> lock(g_FilesMutex);
    g_FilesBackBuffer = std::move(newFiles);
  }

  g_IsLoadingFiles = false;
}

static void ExecuteFile(const std::string &path, bool asAdmin = false) {
  std::wstring wpath = StringToWString(path);
  SHELLEXECUTEINFOW shExInfo = {0};
  shExInfo.cbSize = sizeof(shExInfo);
  shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
  shExInfo.hwnd = 0;
  shExInfo.lpVerb = asAdmin ? L"runas" : L"open";
  shExInfo.lpFile = wpath.c_str();
  shExInfo.lpParameters = L"";
  shExInfo.lpDirectory = 0;
  shExInfo.nShow = SW_SHOW;
  shExInfo.hInstApp = 0;

  ShellExecuteExW(&shExInfo);
}

void FileExplorer_Render() {
  // Sync buffer
  if (g_FilesMutex.try_lock()) {
    if (!g_IsLoadingFiles && !g_FilesBackBuffer.empty()) {
      g_Files = g_FilesBackBuffer;
      g_FilesBackBuffer.clear();
    } else if (!g_IsLoadingFiles && g_NeedsRefresh) {
      // We just finished refreshing and it's completely empty or access denied
      if (g_Files.empty()) {
        fs::path p(g_CurrentPath);
        if (!fs::exists(p))
          g_CurrentPath = "C:\\"; // recovery
      }
    }
    g_FilesMutex.unlock();
  }

  if (g_NeedsRefresh && !g_IsLoadingFiles) {
    g_IsLoadingFiles = true;
    g_NeedsRefresh = false;
    std::thread(RefreshFilesAsync, g_CurrentPath).detach();
  }

  // Controls
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 8));
  if (ImGui::Button(u8"< Вверх")) {
    fs::path p(g_CurrentPath);
    if (p.has_parent_path()) {
      g_CurrentPath = p.parent_path().string();
      // Ensure trailing slash for root paths
      if (g_CurrentPath.length() == 2 && g_CurrentPath[1] == ':') {
        g_CurrentPath += "\\";
      }
      strcpy_s(g_PathBuffer, sizeof(g_PathBuffer), g_CurrentPath.c_str());
      g_NeedsRefresh = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(g_IsLoadingFiles ? u8"Ожидайте нахуй..." : u8"Обновить")) {
    if (!g_IsLoadingFiles)
      g_NeedsRefresh = true;
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
  if (ImGui::InputText("##PathAddress", g_PathBuffer, sizeof(g_PathBuffer),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    g_CurrentPath = g_PathBuffer;
    g_NeedsRefresh = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"Перейти", ImVec2(90, 0))) {
    g_CurrentPath = g_PathBuffer;
    if (!g_IsLoadingFiles)
      g_NeedsRefresh = true;
  }
  ImGui::PopStyleVar();

  ImGui::Spacing();

  // Quick drives
  DWORD drives = GetLogicalDrives();
  for (int i = 0; i < 26; i++) {
    if (drives & (1 << i)) {
      char driveName[8];
      snprintf(driveName, sizeof(driveName), "%c:\\", 'A' + i);
      if (ImGui::Button(driveName)) {
        g_CurrentPath = driveName;
        strcpy_s(g_PathBuffer, sizeof(g_PathBuffer), g_CurrentPath.c_str());
        g_NeedsRefresh = true;
      }
      ImGui::SameLine();
    }
  }
  ImGui::NewLine();

  // File Table
  ImGuiTableFlags table_flags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
      ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable |
      ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY;

  if (ImGui::BeginTable("FileTable", 3, table_flags)) {
    ImGui::TableSetupScrollFreeze(0, 1); // Freeze top row
    ImGui::TableSetupColumn(u8"Имя", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn(u8"Размер", ImGuiTableColumnFlags_WidthFixed,
                            100.0f);
    ImGui::TableSetupColumn(u8"Дата", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableHeadersRow();

    for (const auto &file : g_Files) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();

      // Icon / Color based on IsDirectory
      ImVec4 color = ImVec4(0.95f, 0.95f, 0.95f, 1.0f); // Default text
      const char *icon = "";

      auto stringEndsWith = [](const std::string &str,
                               const std::string &suffix) {
        if (str.length() >= suffix.length()) {
          return (0 == _stricmp(str.c_str() + str.length() - suffix.length(),
                                suffix.c_str()));
        } else {
          return false;
        }
      };

      if (file.IsDirectory) {
        color = ImVec4(0.9f, 0.75f, 0.3f, 1.0f); // Folder color
        icon = u8"[ДИР] ";
      } else {
        if (stringEndsWith(file.Name, ".exe") ||
            stringEndsWith(file.Name, ".scr") ||
            stringEndsWith(file.Name, ".com")) {
          color = ImVec4(1.0f, 0.55f, 0.0f, 1.0f); // Orange for threats
          icon = u8"[ЭХЭшник] ";
        } else if (stringEndsWith(file.Name, ".dll") ||
                   stringEndsWith(file.Name, ".sys")) {
          color = ImVec4(0.6f, 0.7f, 0.8f, 1.0f);
          icon = u8"[СУСник] ";
        } else {
          icon = u8"[ФАЙЛ] ";
        }
      }

      ImGui::PushStyleColor(ImGuiCol_Text, color);

      // Build selectable label
      char label[512];
      snprintf(label, sizeof(label), "%s%s", icon, file.Name.c_str());

      bool isSelected = false; // We can add selection later if needed
      if (ImGui::Selectable(label, isSelected,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          if (file.IsDirectory) {
            g_CurrentPath = file.Path;
            strcpy_s(g_PathBuffer, sizeof(g_PathBuffer), g_CurrentPath.c_str());
            g_NeedsRefresh = true;
          } else {
            ExecuteFile(file.Path);
          }
        }
      }

      // Context Menu
      if (ImGui::BeginPopupContextItem()) {
        if (file.IsDirectory) {
          if (ImGui::MenuItem(u8"Открыть папку")) {
            g_CurrentPath = file.Path;
            strcpy_s(g_PathBuffer, sizeof(g_PathBuffer), g_CurrentPath.c_str());
            g_NeedsRefresh = true;
          }
          if (ImGui::MenuItem(u8"Показать в Проводнике Windows")) {
            ExecuteFile(file.Path);
          }
          ImGui::Separator();
          if (ImGui::MenuItem(u8"Узнать блокировки")) {
            g_UnlockerTargetFile = file.Path;
            std::wstring wpath = StringToWString(g_UnlockerTargetFile);
            g_Lockers = ZaslonCore::FileUnlocker::GetLockingProcesses(wpath);
            g_ShowUnlockerModal = true;
          }
        } else {
          if (ImGui::MenuItem(u8"Открыть")) {
            ExecuteFile(file.Path);
          }
          if (ImGui::MenuItem(u8"Запуск от имени Администратора")) {
            ExecuteFile(file.Path, true);
          }
          ImGui::Separator();
          if (ImGui::MenuItem(g_IsWinPE ? u8"Сброс прав"
                                        : u8"Узнать кто блокирует")) {
            g_UnlockerTargetFile = file.Path;
            if (g_IsWinPE) {
              g_Lockers.clear();

              ZaslonCore::LockerInfo mock;
              mock.Pid = 0;
              mock.Name = L"NTFS Permissions";
              mock.AppType = L"ACL Block";
              g_Lockers.push_back(mock);
            } else {
              std::wstring wpath = StringToWString(g_UnlockerTargetFile);
              g_Lockers = ZaslonCore::FileUnlocker::GetLockingProcesses(wpath);
            }
            g_ShowUnlockerModal = true;
          }
          if (stringEndsWith(file.Name, ".exe") ||
              stringEndsWith(file.Name, ".bat") ||
              stringEndsWith(file.Name, ".com") ||
              stringEndsWith(file.Name, ".dll") ||
              stringEndsWith(file.Name, ".sys")) {
            if (ImGui::MenuItem(u8"Инспектор Файла")) {
              g_InspectorTargetFile = file.Path;
              g_ShowFileInspectorModal = true;
              g_InspectorLog.clear();
              g_IsInspecting = false;
            }
          }

          ImGui::Separator();
          if (ImGui::MenuItem(u8"Принудительное удаление")) {
            std::wstring wpath = StringToWString(file.Path);
            SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (remove(file.Path.c_str()) != 0) {
              // If locked by process, schedule for reboot
              MoveFileExW(wpath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
              // We can't refresh immediately if it's delayed, but we try
            }
            g_NeedsRefresh = true;
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(u8"Копировать путь в буфер")) {
          ImGui::SetClipboardText(file.Path.c_str());
        }
        ImGui::EndPopup();
      }

      ImGui::PopStyleColor();

      // Size Column
      ImGui::TableNextColumn();
      if (!file.IsDirectory) {
        ImGui::TextUnformatted(FormatBytes(file.Size).c_str());
      }

      // Date Column
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(file.DateModified.c_str());
    }
    ImGui::EndTable();
  }

  // Smart Unlocker Modal
  if (g_ShowUnlockerModal) {
    ImGui::OpenPopup(u8"Smart Unlocker##Modal");
  }

  if (ImGui::BeginPopupModal(u8"Smart Unlocker##Modal", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text(u8"Анализ блокировок для:");
    ImGui::TextWrapped("%s", g_UnlockerTargetFile.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_Lockers.empty()) {
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                         u8"Файл свободен. Блокирующие процессы не найдены.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         u8"Найдено процессов: %zu", g_Lockers.size());
      if (ImGui::BeginTable("LockersTable", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY,
                            ImVec2(500, 200))) {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(u8"Процесс",
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(u8"Тип", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableHeadersRow();

        for (const auto &locker : g_Lockers) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Text("%u", locker.Pid);
          ImGui::TableNextColumn();
          char utfName[256];
          WideCharToMultiByte(CP_UTF8, 0, locker.Name.c_str(), -1, utfName, 256,
                              nullptr, nullptr);
          ImGui::TextUnformatted(utfName);
          ImGui::TableNextColumn();
          char utfType[256];
          WideCharToMultiByte(CP_UTF8, 0, locker.AppType.c_str(), -1, utfType,
                              256, nullptr, nullptr);
          ImGui::TextUnformatted(utfType);
        }
        ImGui::EndTable();
      }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!g_Lockers.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
      if (ImGui::Button(g_IsUnlocking ? u8"Освобождение из интерпола..."
                                      : (g_IsWinPE ? u8"Вскрыть права "
                                                   : u8"Разблокировать"),
                        ImVec2(250, 30)) &&
          !g_IsUnlocking) {
        g_IsUnlocking = true;
        std::thread([]() {
          std::wstring wpath = StringToWString(g_UnlockerTargetFile);

          if (g_IsWinPE) {
            // WinPE Offline NTFS Bypass
            std::wstring cmdTakeOwn =
                L"takeown /f \"" + wpath + L"\" /a >nul 2>&1";
            std::wstring cmdIcacls =
                L"icacls \"" + wpath +
                L"\" /grant administrators:F /c /l /q >nul 2>&1";

            _wsystem(cmdTakeOwn.c_str());
            _wsystem(cmdIcacls.c_str());

            // Clear the mock locker
            g_Lockers.clear();
          } else {
            // Normal Live OS process unlocking
            std::vector<ZaslonCore::LockerInfo> remaining;
            ZaslonCore::FileUnlocker::UnlockFile(wpath, remaining);
            g_Lockers = ZaslonCore::FileUnlocker::GetLockingProcesses(wpath);
          }
          g_IsUnlocking = false;
        }).detach();
      }
      ImGui::PopStyleColor();
      ImGui::SameLine();
    }

    float offsetX = g_Lockers.empty() ? ImGui::GetWindowWidth() / 2 - 60
                                      : ImGui::GetContentRegionAvail().x - 120;
    if (!g_Lockers.empty())
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x - 120);
    else
      ImGui::SetCursorPosX(offsetX);

    if (ImGui::Button(u8"Закрыть", ImVec2(120, 30))) {
      g_ShowUnlockerModal = false;
      g_UnlockerTargetFile.clear();
      g_Lockers.clear();
      ImGui::CloseCurrentPopup();
      g_NeedsRefresh = true;
    }

    ImGui::EndPopup();
  }

  // File Inspector Modal
  if (g_ShowFileInspectorModal) {
    ImGui::OpenPopup(u8"Инспектор Файла##Modal");
  }

  ImGui::SetNextWindowSizeConstraints(ImVec2(600, 450), ImVec2(1000, 800));
  if (ImGui::BeginPopupModal(u8"Инспектор Файла##Modal", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), u8"Статический Анализ:");
    ImGui::TextWrapped("%s", g_InspectorTargetFile.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_InspectorLog.empty()) {
      g_IsInspecting = true;
      std::string target = g_InspectorTargetFile;

      // Sync Analysis for simplicity (or async if we want to not freeze UI, but
      // it's fast)
      std::wstring wpath = StringToWString(target);
      std::vector<std::string> log;
      log.push_back(u8"[*] Статический анализ...");

      // 1. Check Signature
      if (ZaslonCore::CheckSignature(wpath)) {
        log.push_back(u8"[+] Цифровая подпись присутствует и валидна "
                      u8"(Надежный Издатель)");
      } else {
        log.push_back(u8"[-] Цифровая подпись отсутствует или недействительна "
                      u8"(Шпиончик!)");
      }

      // 2. Check PE Features
      bool isPacked = false, isDotNet = false, hasSuspiciousImports = false;
      float maxEntropy = 0.0f;
      if (ZaslonCore::CheckPEFeatures(wpath, isPacked, isDotNet,
                                      hasSuspiciousImports, maxEntropy)) {
        if (isPacked) {
          log.push_back(u8"[!] Уведомление: Обнаружен упаковщик (UPX, Themida, "
                        u8"VMP и тд)");
          log.push_back(
              u8"    -> Упаковщики часто используются Вредоносами всякими "
              u8"Троянчиксами и Локерами для сокрытия кода.");
        } else {
          log.push_back(u8"[+] Упаковщик: Не обнаружено");
        }

        if (isDotNet) {
          log.push_back(
              u8"[i] Архитектура: .NET Framework Application (Managed Code)");
        } else {
          log.push_back(u8"[i] Архитектура: Native Win32 / Win64");
        }

        // Entropy analysis
        char entropyBuf[128];
        snprintf(entropyBuf, sizeof(entropyBuf),
                 u8"[i] Максимальная энтропия секции: %.2f / 8.00", maxEntropy);
        log.push_back(entropyBuf);
        if (maxEntropy > 7.0f) {
          log.push_back(
              u8"[!] Высокая энтропия — признак шифрования или упаковки!");
        } else if (maxEntropy > 6.0f) {
          log.push_back(u8"[i] Умеренная энтропия — может быть сжатые ресурсы");
        }

        // Suspicious imports
        if (hasSuspiciousImports) {
          log.push_back(u8"[!] Импорты: WriteProcessMemory, "
                        u8"CreateRemoteThread, VirtualAllocEx");
          log.push_back(u8"    -> Эти импорты в частности используются для "
                        u8"внедрения кода в "
                        u8"другие процессы (Мб Чит либо Вредоносик)");
        } else {
          log.push_back(u8"[+] Опасных импортов не обнаружено");
        }
      } else {
        log.push_back(u8"[-] Ошибка чтения PE заголовков. Возможно, файл не "
                      u8"является EXE/DLL.");
      }

      log.push_back(u8"[*] Инспектор завершил работу.");
      g_InspectorLog = log;
      g_IsInspecting = false;
    }

    if (ImGui::BeginChild("InspectorLog", ImVec2(0, 250), true)) {
      for (const auto &line : g_InspectorLog) {
        if (line.find("[+]") == 0)
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                             line.c_str());
        else if (line.find("[-]") == 0)
          ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                             line.c_str());
        else if (line.find("[!]") == 0)
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s",
                             line.c_str());
        else if (line.find("[i]") == 0)
          ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s",
                             line.c_str());
        else
          ImGui::TextUnformatted(line.c_str());
      }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(u8"Закрыть", ImVec2(120, 30))) {
      g_ShowFileInspectorModal = false;
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button(u8"Найти в VirusTotal (Не актуально)", ImVec2(200, 30))) {
      // Compute SHA256 and open direct VirusTotal search
      std::wstring wpath = StringToWString(g_InspectorTargetFile);
      HANDLE hFile =
          CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (hFile != INVALID_HANDLE_VALUE) {
        HCRYPTPROV hProv = 0;
        HCRYPTHASH hHash = 0;
        if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES,
                                 CRYPT_VERIFYCONTEXT) &&
            CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
          BYTE buf[65536];
          DWORD read = 0;
          while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0)
            CryptHashData(hHash, buf, read, 0);
          BYTE hashBytes[32] = {};
          DWORD hashLen = 32;
          if (CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashLen, 0)) {
            char hex[65] = {};
            for (DWORD i = 0; i < hashLen; i++)
              snprintf(hex + i * 2, 3, "%02x", hashBytes[i]);
            std::string url =
                "https://www.virustotal.com/gui/file/" + std::string(hex);
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                          SW_SHOW);
          }
          CryptDestroyHash(hHash);
        }
        if (hProv)
          CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
      }
    }

    ImGui::EndPopup();
  }
}
