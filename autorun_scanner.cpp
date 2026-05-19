#define _WIN32_DCOM
#include <filesystem>
#include <iostream>
#include <vector>
#include <string>

#include "autorun_scanner.h"
#include "imgui.h"
#include <comdef.h>
#include <taskschd.h>
#include <wbemidl.h>

#include "gui_theme.h"
#include "offline_os_manager.h"

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "taskschd.lib")

// namespace fs = std::filesystem; // Removed to fix undeclared identifier issue
static std::vector<AutoRunEntry> g_AutoRuns;
static int g_CurrentAutoRunTab = 0;
static int g_RegSubTab = 0; // 0=Run, 1=RunOnce, 2=Winlogon

// COM Helper for WMI
static void ScanWMIPersistence() {
  HRESULT hres;
  hres = CoInitializeEx(0, COINIT_MULTITHREADED);
  if (FAILED(hres))
    return;

  hres =
      CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                           RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

  IWbemLocator *pLoc = NULL;
  hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID *)&pLoc);
  if (FAILED(hres)) {
    CoUninitialize();
    return;
  }

  IWbemServices *pSvc = NULL;
  hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\SUBSCRIPTION"), NULL, NULL, 0,
                             NULL, 0, 0, &pSvc);
  if (FAILED(hres)) {
    pLoc->Release();
    CoUninitialize();
    return;
  }

  hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);
  if (FAILED(hres)) {
    pSvc->Release();
    pLoc->Release();
    CoUninitialize();
    return;
  }

  IEnumWbemClassObject *pEnumerator = NULL;
  hres = pSvc->ExecQuery(bstr_t("WQL"),
                         bstr_t("SELECT * FROM __FilterToConsumerBinding"),
                         WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                         NULL, &pEnumerator);

  if (FAILED(hres)) {
    pSvc->Release();
    pLoc->Release();
    CoUninitialize();
    return;
  }

  IWbemClassObject *pclsObj = NULL;
  ULONG uReturn = 0;
  while (pEnumerator) {
    HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
    if (0 == uReturn || FAILED(hr))
      break;

    VARIANT vtProp;
    VariantInit(&vtProp);

    // Consumer
    hr = pclsObj->Get(L"Consumer", 0, &vtProp, 0, 0);
    std::wstring consumerStr = vtProp.bstrVal ? vtProp.bstrVal : L"";
    VariantClear(&vtProp);

    // Filter
    hr = pclsObj->Get(L"Filter", 0, &vtProp, 0, 0);
    std::wstring filterStr = vtProp.bstrVal ? vtProp.bstrVal : L"";
    VariantClear(&vtProp);

    AutoRunEntry entry;
    entry.Name = L"WMI Binding";
    entry.Location = filterStr;
    entry.Path = consumerStr; // Path holds the payload description
    entry.Type = AutoRunType::WMISubscription;
    entry.DangerInfo = true; // WMI subscriptions are high-risk

    g_AutoRuns.push_back(entry);
    pclsObj->Release();
  }

  pEnumerator->Release();
  pSvc->Release();
  pLoc->Release();
  CoUninitialize();
}

void AutorunScanner_PurgeWMI() {
  HRESULT hres;
  hres = CoInitializeEx(0, COINIT_MULTITHREADED);
  if (FAILED(hres))
    return;

  IWbemLocator *pLoc = NULL;
  hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID *)&pLoc);
  if (SUCCEEDED(hres)) {
    IWbemServices *pSvc = NULL;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\SUBSCRIPTION"), NULL, NULL, 0,
                               NULL, 0, 0, &pSvc);
    if (SUCCEEDED(hres)) {
      CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                        NULL, EOAC_NONE);

      auto deleteInstancesOfClass = [&](const wchar_t *className) {
        IEnumWbemClassObject *pEnumerator = NULL;
        if (SUCCEEDED(pSvc->CreateInstanceEnum(bstr_t(className),
                                               WBEM_FLAG_FORWARD_ONLY |
                                                   WBEM_FLAG_RETURN_IMMEDIATELY,
                                               NULL, &pEnumerator))) {
          IWbemClassObject *pclsObj = NULL;
          ULONG uReturn = 0;
          while (pEnumerator) {
            if (FAILED(
                    pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn)) ||
                0 == uReturn)
              break;
            VARIANT vtProp;
            if (SUCCEEDED(pclsObj->Get(L"__RELPATH", 0, &vtProp, 0, 0))) {
              pSvc->DeleteInstance(vtProp.bstrVal, 0, NULL, NULL);
              VariantClear(&vtProp);
            }
            pclsObj->Release();
          }
          pEnumerator->Release();
        }
      };

      // Aggressive Purge of all bindings and filters/consumers
      deleteInstancesOfClass(L"__FilterToConsumerBinding");
      deleteInstancesOfClass(L"__EventFilter");
      deleteInstancesOfClass(L"CommandLineEventConsumer");
      deleteInstancesOfClass(L"ActiveScriptEventConsumer");
      deleteInstancesOfClass(L"SMTPEventConsumer");
      deleteInstancesOfClass(L"LogFileEventConsumer");

      pSvc->Release();
    }
    pLoc->Release();
  }
  CoUninitialize();

  // Refresh to update UI
  AutorunScanner_Refresh();
}

static void ScanRegistryKey(HKEY hkrRoot, const wchar_t *subKey,
                            const wchar_t *locName) {
  HKEY hKey = ZaslonCore::OfflineOSManager::OpenRedirectedKey(hkrRoot, subKey,
                                                              KEY_READ);
  if (hKey) {
    DWORD index = 0;
    wchar_t valueName[1024];
    BYTE data[2048];

    while (true) {
      DWORD nameLen = 1024;
      DWORD dataLen = 2048;
      DWORD type;
      LSTATUS ret = RegEnumValueW(hKey, index, valueName, &nameLen, nullptr,
                                  &type, data, &dataLen);
      if (ret != ERROR_SUCCESS)
        break;

      if (type == REG_SZ || type == REG_EXPAND_SZ) {
        AutoRunEntry entry;
        entry.Name = valueName;
        entry.Path = (wchar_t *)data;
        entry.Location = std::wstring(locName) + L"\\" + subKey;
        entry.Type = AutoRunType::Registry;
        entry.DangerInfo = false;
        entry.Selected = false;
        g_AutoRuns.push_back(entry);
      }
      index++;
    }
    RegCloseKey(hKey);
  }
}

static void ScanScheduledTasks() {
  HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool coInit = SUCCEEDED(hrInit) || hrInit == RPC_E_CHANGED_MODE;

  ITaskService *pService = NULL;
  if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                              IID_ITaskService, (void **)&pService))) {
    if (coInit)
      CoUninitialize();
    return;
  }
  if (FAILED(pService->Connect(_variant_t(), _variant_t(), _variant_t(),
                               _variant_t()))) {
    pService->Release();
    if (coInit)
      CoUninitialize();
    return;
  }

  ITaskFolder *pRootFolder = NULL;
  if (SUCCEEDED(pService->GetFolder(_bstr_t(L"\\"), &pRootFolder))) {
    IRegisteredTaskCollection *pTaskCollection = NULL;
    if (SUCCEEDED(pRootFolder->GetTasks(TASK_ENUM_HIDDEN, &pTaskCollection))) {
      LONG numTasks = 0;
      pTaskCollection->get_Count(&numTasks);

      for (LONG i = 0; i < numTasks; i++) {
        IRegisteredTask *pRegisteredTask = NULL;
        if (SUCCEEDED(pTaskCollection->get_Item(_variant_t(i + 1),
                                                &pRegisteredTask))) {
          BSTR taskName = NULL;
          pRegisteredTask->get_Name(&taskName);

          BSTR taskPath = NULL;
          pRegisteredTask->get_Path(&taskPath);

          ITaskDefinition *pDefinition = NULL;
          if (SUCCEEDED(pRegisteredTask->get_Definition(&pDefinition))) {
            VARIANT_BOOL isEnabled = VARIANT_FALSE;
            pRegisteredTask->get_Enabled(&isEnabled);

            IActionCollection *pActionCollection = NULL;
            if (SUCCEEDED(pDefinition->get_Actions(&pActionCollection))) {
              LONG numActions = 0;
              pActionCollection->get_Count(&numActions);
              if (numActions > 0) {
                IAction *pAction = NULL;
                if (SUCCEEDED(pActionCollection->get_Item(1, &pAction))) {
                  IExecAction *pExecAction = NULL;
                  if (SUCCEEDED(pAction->QueryInterface(
                          IID_IExecAction, (void **)&pExecAction))) {
                    BSTR path = NULL;
                    BSTR args = NULL;
                    pExecAction->get_Path(&path);
                    pExecAction->get_Arguments(&args);

                    std::wstring fullCmd = std::wstring(path ? path : L"") +
                                           L" " + (args ? args : L"");
                    if (isEnabled == VARIANT_FALSE)
                      fullCmd = L"[ОТКЛЮЧЕНО] " + fullCmd;

                    AutoRunEntry e;
                    e.Name = taskName ? taskName : L"Unknown Task";
                    e.Location = taskPath ? taskPath : L"Task Scheduler";
                    e.Path = fullCmd;
                    e.Type = AutoRunType::Task;
                    // Highlight tasks running cmd/powershell/wscript
                    if (fullCmd.find(L"cmd.exe") != std::wstring::npos ||
                        fullCmd.find(L"powershell") != std::wstring::npos ||
                        fullCmd.find(L"wscript") != std::wstring::npos) {
                      e.DangerInfo = true;
                    } else {
                      e.DangerInfo = false;
                    }
                    e.Selected = false;
                    g_AutoRuns.push_back(e);

                    if (path)
                      SysFreeString(path);
                    if (args)
                      SysFreeString(args);
                    pExecAction->Release();
                  }
                  pAction->Release();
                }
              }
              pActionCollection->Release();
            }
            pDefinition->Release();
          }
          if (taskName)
            SysFreeString(taskName);
          if (taskPath)
            SysFreeString(taskPath);
          pRegisteredTask->Release();
        }
      }
      pTaskCollection->Release();
    }
    pRootFolder->Release();
  }
  pService->Release();
  if (coInit)
    CoUninitialize();
}

static void ScanOfflineTasks() {
  if (ZaslonCore::g_OfflineOSPath.empty())
    return;

  std::wstring tasksDir = ZaslonCore::g_OfflineOSPath + L"\\System32\\Tasks";
  std::error_code ec;

  if (!std::filesystem::exists(tasksDir, ec))
    return;

  for (const auto &entry : std::filesystem::recursive_directory_iterator(tasksDir, ec)) {
    if (entry.is_regular_file()) {
      AutoRunEntry e;
      e.Name = entry.path().filename().wstring();
      e.Location = L"[WinPE Offset] " + entry.path().wstring();
      e.Path = L"XML Task File (Manual inspection recommended)";
      e.Type = AutoRunType::Task;
      e.DangerInfo = false;
      e.Selected = false;

      // Heuristic check: if filename looks suspicious or common malware name
      std::wstring lowerName = e.Name;
      for (auto &c : lowerName)
        c = towlower(c);
      if (lowerName == L"updates" || lowerName == L"system" ||
          lowerName == L"defender" || lowerName == L"winlogon") {
        e.DangerInfo = true;
      }

      g_AutoRuns.push_back(e);
    }
  }
}

static void DeleteScheduledTask(const std::wstring &taskPath) {
  HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  bool coInit = SUCCEEDED(hrInit) || hrInit == RPC_E_CHANGED_MODE;

  ITaskService *pService = NULL;
  if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, NULL,
                                 CLSCTX_INPROC_SERVER, IID_ITaskService,
                                 (void **)&pService))) {
    if (SUCCEEDED(pService->Connect(_variant_t(), _variant_t(), _variant_t(),
                                    _variant_t()))) {
      ITaskFolder *pRootFolder = NULL;
      // Get root, then split task path or just call DeleteTask with the full
      // path if root handles it (often works)
      if (SUCCEEDED(pService->GetFolder(_bstr_t(L"\\"), &pRootFolder))) {
        pRootFolder->DeleteTask(_bstr_t(taskPath.c_str()), 0);
        pRootFolder->Release();
      }
    }
    pService->Release();
  }
  if (coInit)
    CoUninitialize();
}

void AutorunScanner_Refresh() {
  g_AutoRuns.clear();

  // Registry Scans
  ScanRegistryKey(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                  L"HKLM");
  ScanRegistryKey(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                  L"HKLM");
  ScanRegistryKey(HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  L"HKCU");
  ScanRegistryKey(HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                  L"HKCU");

  // Winlogon
  ScanRegistryKey(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                  L"HKLM");

  // Task Scheduler
  if (g_IsWinPE && ZaslonCore::g_IsOfflineHivesMounted) {
    ScanOfflineTasks();
  } else {
    ScanScheduledTasks();
  }

  // WMI Scanner (Live only)
  if (!g_IsWinPE) {
    ScanWMIPersistence();
  }
}

void AutorunScanner_Render() {
  if (g_AutoRuns.empty()) {
    AutorunScanner_Refresh();
  }

  ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), u8"Менеджер Автозагрузки");
  ImGui::Spacing();

  // Tabs
  if (ImGui::Button(g_CurrentAutoRunTab == 0 ? u8"[ Реестр ]" : u8"Реестр"))
    g_CurrentAutoRunTab = 0;
  ImGui::SameLine();
  if (ImGui::Button(g_CurrentAutoRunTab == 1 ? u8"[ Планировщик Задач ]"
                                             : u8"Планировщик Задач"))
    g_CurrentAutoRunTab = 1;
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  if (ImGui::Button(g_CurrentAutoRunTab == 2 ? u8"[ WMI Скрипты ]"
                                             : u8"WMI Скрипты"))
    g_CurrentAutoRunTab = 2;
  ImGui::PopStyleColor(2);

  ImGui::SameLine(0, 50.0f);
  if (ImGui::Button(u8"Обновить Список"))
    AutorunScanner_Refresh();

  ImGui::Spacing();

  // Registry Sub-Tabs
  if (g_CurrentAutoRunTab == 0) {
    if (ImGui::BeginTabBar("RegTabs")) {
      if (ImGui::BeginTabItem(u8"Run")) {
        g_RegSubTab = 0;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(u8"RunOnce")) {
        g_RegSubTab = 1;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(u8"Winlogon")) {
        g_RegSubTab = 2;
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }

  if (g_CurrentAutoRunTab == 2) {
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                       u8"Уведомление: Вредоностное ПО часто создаёт "
                       u8"бестелесные подписки в WMI (root\\subscription).");
    if (ImGui::Button(u8"Уничтожить все WMI Подписки", ImVec2(400, 30))) {
      AutorunScanner_PurgeWMI();
    }
  }

  ImGui::Spacing();

  // Mass actions
  if (ImGui::Button(u8"Удалить выбранные", ImVec2(300, 30))) {
    for (auto &n : g_AutoRuns) {
      if (n.Selected) {
        if (n.Type == AutoRunType::Registry) {
          HKEY root = NULL;
          if (n.Location.find(L"HKLM\\") == 0)
            root = HKEY_LOCAL_MACHINE;
          else if (n.Location.find(L"HKCU\\") == 0)
            root = HKEY_CURRENT_USER;

          if (root) {
            std::wstring subKey = n.Location.substr(5);
            HKEY hKey = ZaslonCore::OfflineOSManager::OpenRedirectedKey(
                root, subKey.c_str(), KEY_SET_VALUE);
            if (hKey) {
              RegDeleteValueW(hKey, n.Name.c_str());
              RegCloseKey(hKey);
            }
          }
        } else if (n.Type == AutoRunType::Task) {
          DeleteScheduledTask(n.Location);
        } else if (n.Type == AutoRunType::WMISubscription) {
          AutorunScanner_PurgeWMI(); // purge all WMI if selected
        }
      }
    }
    AutorunScanner_Refresh();
  }

  ImGui::Spacing();

  ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("AutorunTable", 3, flags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(u8"Имя", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn(u8"Расположение", ImGuiTableColumnFlags_WidthFixed,
                            300.0f);
    ImGui::TableSetupColumn(u8"Цель", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (auto &n : g_AutoRuns) {
      // Filter by active tab
      if (g_CurrentAutoRunTab == 0) {
        if (n.Type != AutoRunType::Registry)
          continue;

        bool isRunOnce = (n.Location.find(L"\\RunOnce") != std::wstring::npos);
        bool isWinlogon =
            (n.Location.find(L"\\Winlogon") != std::wstring::npos);
        bool isRun = !isRunOnce && !isWinlogon;

        if (g_RegSubTab == 0 && !isRun)
          continue;
        if (g_RegSubTab == 1 && !isRunOnce)
          continue;
        if (g_RegSubTab == 2 && !isWinlogon)
          continue;
      }
      if (g_CurrentAutoRunTab == 1 && n.Type != AutoRunType::Task &&
          n.Type != AutoRunType::Service)
        continue;
      if (g_CurrentAutoRunTab == 2 && n.Type != AutoRunType::WMISubscription)
        continue;

      ImGui::TableNextRow();

      if (n.DangerInfo)
        ImGui::TableSetBgColor(
            ImGuiTableBgTarget_RowBg0,
            ImGui::GetColorU32(ImVec4(0.5f, 0.0f, 0.0f, 0.6f)));

      ImGui::TableNextColumn();

      char utfName[256];
      WideCharToMultiByte(CP_UTF8, 0, n.Name.c_str(), -1, utfName, 256, nullptr,
                          nullptr);

      ImGui::PushID((const void *)&n);
      ImGui::Checkbox("##sel", &n.Selected);
      ImGui::SameLine();
      ImGui::Selectable(utfName, false, ImGuiSelectableFlags_SpanAllColumns);
      ImGui::PopID();

      if (ImGui::BeginPopupContextItem()) {
        if (n.Type == AutoRunType::Registry) {
          if (ImGui::MenuItem(u8"Удалить ключ реестра")) {
            // Extract Root and SubKey from n.Location
            HKEY root = NULL;
            if (n.Location.find(L"HKLM\\") == 0)
              root = HKEY_LOCAL_MACHINE;
            else if (n.Location.find(L"HKCU\\") == 0)
              root = HKEY_CURRENT_USER;

            if (root) {
              std::wstring subKey = n.Location.substr(5); // Skip "HKXX\"
              HKEY hKey = ZaslonCore::OfflineOSManager::OpenRedirectedKey(
                  root, subKey.c_str(), KEY_SET_VALUE);
              if (hKey) {
                RegDeleteValueW(hKey, n.Name.c_str());
                RegCloseKey(hKey);
              }
            }
            AutorunScanner_Refresh();
          }
        } else if (n.Type == AutoRunType::Task) {
          if (ImGui::MenuItem(u8"Удалить задачу планировщика")) {
            DeleteScheduledTask(n.Location);
            AutorunScanner_Refresh();
          }
        } else if (n.Type == AutoRunType::WMISubscription) {
          if (ImGui::MenuItem(u8"Очистить ВСЕ WMI подписки")) {
            AutorunScanner_PurgeWMI();
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(u8"Скопировать значение")) {
          char utfPath[1024];
          WideCharToMultiByte(CP_UTF8, 0, n.Path.c_str(), -1, utfPath, 1024,
                              nullptr, nullptr);
          ImGui::SetClipboardText(utfPath);
        }
        ImGui::EndPopup();
      }

      ImGui::TableNextColumn();
      char utfLoc[512];
      WideCharToMultiByte(CP_UTF8, 0, n.Location.c_str(), -1, utfLoc, 512,
                          nullptr, nullptr);
      ImGui::TextWrapped("%s", utfLoc);

      ImGui::TableNextColumn();
      char utfPath[1024];
      WideCharToMultiByte(CP_UTF8, 0, n.Path.c_str(), -1, utfPath, 1024,
                          nullptr, nullptr);
      ImGui::TextWrapped("%s", utfPath);
    }
    ImGui::EndTable();
  }
}
