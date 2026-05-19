#include "installer_tracker.h"
#include "imgui.h"
#include "gui_theme.h"
#include "zaslon_core.h"
#include <atomic>
#include <shobjidl.h> // For file dialog
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// State Action ENUM
enum class TrackerState { IDLE = 0, TRACKING = 1, RESULTS = 2 };

// State Variables
static TrackerState g_TrackerState = TrackerState::IDLE;
static std::string g_TargetInstallerFile = "";
static PROCESS_INFORMATION g_InstallerPi = {0};
static ZaslonCore::SandboxDiffResult g_FinalDiff;
static std::atomic<bool> g_IsMonitoring(false);
static std::string g_TrackerStatusMsg = "";

// Helpers
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

static bool IsSuspiciousUtility(const std::wstring &name) {
  std::wstring n = name;
  for (auto &c : n)
    c = towlower(c);
  if (n == L"cmd.exe" || n == L"powershell.exe" || n == L"pwsh.exe" ||
      n == L"wscript.exe" || n == L"cscript.exe" || n == L"mshta.exe" ||
      n == L"schtasks.exe" || n == L"reg.exe" || n == L"bitsadmin.exe" ||
      n == L"certutil.exe" || n == L"conhost.exe")
    return true;
  return false;
}

// Background monitoring thread
static void TrackerMonitorWorker(DWORD pid, HANDLE hProcess,
                                 std::string targetFile) {
  g_IsMonitoring = true;

  // Wait for main process
  WaitForSingleObject(hProcess, INFINITE);

  g_TrackerStatusMsg = u8"Главный процесс завершен. Ожидание дочерей...";
  Sleep(2000);

  std::vector<ZaslonCore::SandboxSpawnedProcess> children;
  ZaslonCore::SandboxAnalyzer::TrackProcesses(pid, children);

  if (!children.empty()) {
    g_TrackerStatusMsg = u8"Ожидание завершения дочерей...";
    bool anyRunning = true;
    while (anyRunning && g_IsMonitoring) {
      anyRunning = false;
      for (auto &child : children) {
        HANDLE hChild = OpenProcess(SYNCHRONIZE, FALSE, child.Pid);
        if (hChild) {
          if (WaitForSingleObject(hChild, 1000) == WAIT_TIMEOUT) {
            anyRunning = true;
          }
          CloseHandle(hChild);
        }
      }
    }
  }

  if (!g_IsMonitoring)
    return; // Aborted

  g_TrackerStatusMsg = u8"Формирование отчета...";
  ZaslonCore::SandboxAnalyzer::StopRealTimeTracking(
      StringToWString(targetFile));
  g_FinalDiff = ZaslonCore::SandboxAnalyzer::GetRealTimeDiff();
  ZaslonCore::SandboxAnalyzer::TrackProcesses(pid,
                                              g_FinalDiff.SpawnedProcesses);

  g_TrackerState = TrackerState::RESULTS;
  g_IsMonitoring = false;
}

static void OpenFileDialog() {
  IFileOpenDialog *pFileOpen;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                                IID_IFileOpenDialog,
                                reinterpret_cast<void **>(&pFileOpen));
  if (SUCCEEDED(hr)) {
    COMDLG_FILTERSPEC rgSpec[] = {
        {L"Installers (*.exe;*.msi;*.bat)", L"*.exe;*.msi;*.bat"},
        {L"All files", L"*.*"}};
    pFileOpen->SetFileTypes(2, rgSpec);
    hr = pFileOpen->Show(NULL);
    if (SUCCEEDED(hr)) {
      IShellItem *pItem;
      hr = pFileOpen->GetResult(&pItem);
      if (SUCCEEDED(hr)) {
        PWSTR pszFilePath;
        hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
        if (SUCCEEDED(hr)) {
          g_TargetInstallerFile = WStringToString(pszFilePath);
          CoTaskMemFree(pszFilePath);
        }
        pItem->Release();
      }
    }
    pFileOpen->Release();
  }
}

// UI Rendering
void InstallerTracker_Render() {
  ImGui::TextColored(g_Theme.AccentColor, u8"ZASLON — Трассировщик");
  ImGui::TextDisabled(u8"Мониторинг программ с полным "
                      u8"отслеживанием :3");
  ImGui::Separator();
  ImGui::Spacing();

  if (g_TrackerState == TrackerState::IDLE) {
    ImGui::Text(u8"1. Выберите файл:");
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 120);
    char buf[MAX_PATH];
    strcpy_s(buf, g_TargetInstallerFile.c_str());
    if (ImGui::InputText("##TargetFile", buf, MAX_PATH)) {
      g_TargetInstallerFile = buf;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button(u8"Обзор...", ImVec2(100, 0))) {
      OpenFileDialog();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    if (g_TargetInstallerFile.empty()) {
      ImGui::BeginDisabled();
    }

    ImGui::PushStyleColor(ImGuiCol_Button, g_Theme.AccentColor);
    if (ImGui::Button(u8"► Запустить и следить", ImVec2(350, 45))) {
      ZaslonCore::SandboxAnalyzer::StartRealTimeTracking();
      STARTUPINFOW si = {sizeof(si)};
      std::wstring wcmd = StringToWString(g_TargetInstallerFile);
      if (CreateProcessW(wcmd.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                         &si, &g_InstallerPi)) {
        g_TrackerState = TrackerState::TRACKING;
        g_TrackerStatusMsg = u8"Сбор данных об установке...";
        std::thread(TrackerMonitorWorker, g_InstallerPi.dwProcessId,
                    g_InstallerPi.hProcess, g_TargetInstallerFile)
            .detach();
      }
    }
    ImGui::PopStyleColor();

    if (g_TargetInstallerFile.empty()) {
      ImGui::EndDisabled();
    }

  } else if (g_TrackerState == TrackerState::TRACKING) {

    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), u8"Идет установка...");
    ImGui::Text(u8"%s", g_TrackerStatusMsg.c_str());
    ImGui::Spacing();

    // Progress animation
    ImGui::ProgressBar(ImGui::GetTime() * -0.5f, ImVec2(-1, 8), "");

    ImGui::Spacing();
    ImGui::Text(u8"Лог действий:");

    ZaslonCore::SandboxDiffResult liveDiff =
        ZaslonCore::SandboxAnalyzer::GetRealTimeDiff();

    if (ImGui::BeginChild("LiveTrackerOutput", ImVec2(0, -60), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
      for (const auto &wfile : liveDiff.AddedFiles) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[Файл] %s",
                           WStringToString(wfile).c_str());
      }
      for (const auto &wkey : liveDiff.AddedRegistryKeys) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[Реестр] %s",
                           WStringToString(wkey).c_str());
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    if (ImGui::Button(u8"Прервать мониторинг", ImVec2(350, 40))) {
      g_IsMonitoring = false; // Abort worker
      ZaslonCore::SandboxAnalyzer::TrackProcesses(g_InstallerPi.dwProcessId,
                                                  g_FinalDiff.SpawnedProcesses);

      HANDLE hKill =
          OpenProcess(PROCESS_TERMINATE, FALSE, g_InstallerPi.dwProcessId);
      if (hKill) {
        TerminateProcess(hKill, 0);
        CloseHandle(hKill);
      }

      ZaslonCore::SandboxAnalyzer::StopRealTimeTracking(
          StringToWString(g_TargetInstallerFile));
      g_FinalDiff = ZaslonCore::SandboxAnalyzer::GetRealTimeDiff();

      CloseHandle(g_InstallerPi.hProcess);
      CloseHandle(g_InstallerPi.hThread);
      g_TrackerState = TrackerState::RESULTS;
    }

  } else if (g_TrackerState == TrackerState::RESULTS) {

    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                       u8"Завершено. Отчёт готов.");
    ImGui::Spacing();

    if (ImGui::BeginTabBar("SandboxDiffTabs")) {

      if (ImGui::BeginTabItem(u8"Сводный Лог (Все)")) {
        if (ImGui::BeginChild("AllDiff", ImVec2(0, -60), true)) {
          for (const auto &proc : g_FinalDiff.SpawnedProcesses) {
            bool susp = IsSuspiciousUtility(proc.ImageName);
            if (susp) {
              ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                 u8"[!] Что за херня: %s",
                                 WStringToString(proc.ImageName).c_str());
              if (!proc.CommandLine.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                                   u8"    Аргументы: %s",
                                   WStringToString(proc.CommandLine).c_str());
            } else {
              ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.8f, 1.0f),
                                 "[Процесс] PID: %lu -> %s", proc.Pid,
                                 WStringToString(proc.ImageName).c_str());
            }
          }
          for (const auto &wstr : g_FinalDiff.AddedRegistryKeys) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[Реестр] %s",
                               WStringToString(wstr).c_str());
          }
          for (const auto &wstr : g_FinalDiff.TracingFiles) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[Шпион] %s",
                               WStringToString(wstr).c_str());
          }
          for (const auto &wstr : g_FinalDiff.SuspiciousCopies) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[Копия] %s",
                               WStringToString(wstr).c_str());
          }
          for (const auto &wstr : g_FinalDiff.AddedFiles) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "[Файл] %s",
                               WStringToString(wstr).c_str());
          }
          if (g_FinalDiff.SpawnedProcesses.empty() &&
              g_FinalDiff.AddedRegistryKeys.empty() &&
              g_FinalDiff.AddedFiles.empty()) {
            ImGui::TextDisabled(u8"Изменений не обнаружено.");
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem(u8"Созданные Файлы")) {
        ImGui::Text(u8"Всего новых файлов: %zu", g_FinalDiff.AddedFiles.size());
        if (ImGui::BeginChild("FilesDiff", ImVec2(0, -60), true)) {
          for (const auto &wstr : g_FinalDiff.AddedFiles) {
            std::string s = WStringToString(wstr);
            ImGui::Selectable(s.c_str());
            if (ImGui::BeginPopupContextItem()) {
              if (ImGui::MenuItem(u8"Копировать путь")) {
                ImGui::SetClipboardText(s.c_str());
              }
              ImGui::EndPopup();
            }
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem(u8"Ключи Реестра")) {
        ImGui::Text(u8"Новых ключей реестра: %zu",
                    g_FinalDiff.AddedRegistryKeys.size());
        if (ImGui::BeginChild("RegDiff", ImVec2(0, -60), true)) {
          for (const auto &wstr : g_FinalDiff.AddedRegistryKeys) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                               WStringToString(wstr).c_str());
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button(u8"Отменить (Удалить всё)", ImVec2(300, 45))) {
      ZaslonCore::SandboxAnalyzer::Rollback(g_FinalDiff);
      g_TrackerState = TrackerState::IDLE;
      g_TargetInstallerFile.clear();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    if (ImGui::Button(u8"Оставить (Сохранить)", ImVec2(300, 45))) {
      g_TrackerState = TrackerState::IDLE;
      g_TargetInstallerFile.clear();
    }
    ImGui::PopStyleColor();
  }
}
