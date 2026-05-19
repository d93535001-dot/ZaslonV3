#include "process_panel.h"
#include "imgui.h"
#include "gui_theme.h"
#include "gui_utils.h"
#include "zaslon_core.h"

// Correct sequence for NTAPI in usermode
#define WIN32_NO_STATUS
#include <ntstatus.h>
#include <windows.h>
#include <winternl.h>

#undef WIN32_NO_STATUS

#include "ntapi_defs.h"

#include <algorithm>
#include <atomic>
#include <d3d9.h>
#include <map>
#include <mutex>
#include <psapi.h>
#include <shellapi.h>
#include <string>
#include <strsafe.h>
#include <thread>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>

// Digital Signature checks
#include <softpub.h>
#include <wintrust.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

static std::vector<ProcessNode> g_Nodes;
static float g_RefreshTimer = 0.0f;
static float g_RefreshInterval = 2.0f;
static char g_Filter[128] = {};
static int g_SelectedPid = -1;
static int g_InteractionPid = -1;

// Background Worker variables
static std::mutex g_NodesMutex;
static std::vector<ProcessNode> g_NodesBackBuffer;
static std::atomic<bool> g_IsRefreshing(false);
static std::atomic<int>
    g_PendingIconThreads(0); // v2.5: throttle icon extraction
static const int MAX_ICON_THREADS = 8;

// Caching structure
struct CachedProcessInfo {
  uint64_t CreateTime;
  std::wstring FullPath;
  std::wstring FileDescription;
  bool MicrosoftSigned;
  std::wstring UserName;
  std::wstring CommandLine;
  int RiskScore;
  std::vector<std::string> RiskReasons;
};
static std::unordered_map<DWORD, CachedProcessInfo> g_ProcessCache;

// Signature Caching
static std::mutex g_SigCacheMutex;
static std::unordered_map<std::wstring, bool> g_SignatureCache;

// Warnings and Confirmations
static bool g_ShowCriticalWarning = false;
static int g_PendingCriticalPid = -1;

// Caching for Property Tabs to prevent freezes
struct PropertyCache {
  int Pid = -1;
  struct ThreadInfo {
    DWORD Tid;
    int Priority;
  };
  struct ModuleInfo {
    std::string Name;
    void *Base;
    std::string Path;
  };
  struct HandleInfo {
    int Value;
    int Type;
    void *Object;
  };

  std::vector<ThreadInfo> Threads;
  std::vector<ModuleInfo> Modules;
  std::vector<HandleInfo> Handles;
  bool BasicLoaded = false;
  bool HandlesLoaded = false;
};
static PropertyCache g_PropCache;

static void RefreshBasicPropertyCache(int pid) {
  if (g_PropCache.Pid == pid && g_PropCache.BasicLoaded)
    return;

  g_PropCache.Pid = pid;
  g_PropCache.Threads.clear();
  g_PropCache.Modules.clear();
  g_PropCache.Handles.clear();
  g_PropCache.HandlesLoaded = false;

  // 1. Threads
  HANDLE hSnapT = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (hSnapT != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te = {sizeof(te)};
    if (Thread32First(hSnapT, &te)) {
      do {
        if (te.th32OwnerProcessID == pid)
          g_PropCache.Threads.push_back({te.th32ThreadID, te.tpBasePri});
      } while (Thread32Next(hSnapT, &te));
    }
    CloseHandle(hSnapT);
  }

  // 2. Modules
  // Use smaller buffer and avoid full snapshot if it's the current process or
  // if we fail initial snapshot
  HANDLE hSnapM =
      CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (hSnapM != INVALID_HANDLE_VALUE) {
    MODULEENTRY32W me = {sizeof(me)};
    if (Module32FirstW(hSnapM, &me)) {
      do {
        char n[128] = {}, p[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, n, 128, nullptr,
                            nullptr);
        WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, p, MAX_PATH, nullptr,
                            nullptr);
        g_PropCache.Modules.push_back({n, me.modBaseAddr, p});
      } while (Module32NextW(hSnapM, &me));
    }
    CloseHandle(hSnapM);
  }
  g_PropCache.BasicLoaded = true;
}

static void RefreshHandlePropertyCache(int pid) {
  if (g_PropCache.Pid == pid && g_PropCache.HandlesLoaded)
    return;
  g_PropCache.Handles.clear();

  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  pfnNtQuerySystemInformation NtQuerySysInfo =
      (pfnNtQuerySystemInformation)GetProcAddress(hNtDll,
                                                  "NtQuerySystemInformation");
  if (NtQuerySysInfo) {
    ULONG size = 1024 * 1024;
    PVOID buffer = malloc(size);
    if (NtQuerySysInfo(ZASLON_SystemHandleInformation, buffer, size, &size) ==
        0xC0000004) {
      free(buffer);
      buffer = malloc(size);
    }
    if (NtQuerySysInfo(ZASLON_SystemHandleInformation, buffer, size, &size) ==
        0) {
      PZASLON_SYSTEM_HANDLE_INFORMATION pHandleInfo =
          (PZASLON_SYSTEM_HANDLE_INFORMATION)buffer;
      for (ULONG i = 0; i < pHandleInfo->NumberOfHandles; i++) {
        if (pHandleInfo->Handles[i].UniqueProcessId ==
            (USHORT)pid) { // v2.5: compare full PID field
          g_PropCache.Handles.push_back(
              {(int)pHandleInfo->Handles[i].HandleValue,
               (int)pHandleInfo->Handles[i].ObjectTypeIndex,
               pHandleInfo->Handles[i].Object});
        }
      }
    }
    free(buffer);
  }
  g_PropCache.HandlesLoaded = true;
}

// Helper: Get File Description
static std::wstring GetFileDescription(const std::wstring &path) {
  if (path.empty())
    return L"";
  DWORD dummy;
  DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
  if (!size)
    return L"";
  std::vector<BYTE> buf(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()))
    return L"";

  struct TRANSLATION {
    WORD language;
    WORD codePage;
  } *pTrans;
  UINT transLen = 0;
  if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                     (LPVOID *)&pTrans, &transLen) &&
      transLen >= sizeof(TRANSLATION)) {
    wchar_t subBlock[50];
    StringCchPrintfW(subBlock, 50,
                     L"\\StringFileInfo\\%04x%04x\\FileDescription",
                     pTrans[0].language, pTrans[0].codePage);
    wchar_t *desc = nullptr;
    UINT descLen = 0;
    if (VerQueryValueW(buf.data(), subBlock, (LPVOID *)&desc, &descLen) &&
        descLen > 0) {
      return std::wstring(desc);
    }
  }
  return L"";
}

// Icon extraction and D3D texture caching
extern LPDIRECT3DDEVICE9 g_pd3dDevice;

static IDirect3DTexture9 *CreateTextureFromHICON(HICON hIcon) {
  if (!g_pd3dDevice || !hIcon)
    return nullptr;

  ICONINFO iconInfo = {0};
  if (!GetIconInfo(hIcon, &iconInfo))
    return nullptr;

  BITMAP bmp = {0};
  GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmp);

  if (bmp.bmWidth == 0 || bmp.bmHeight == 0) {
    DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask)
      DeleteObject(iconInfo.hbmMask);
    return nullptr;
  }

  HDC hDC = GetDC(nullptr);
  BITMAPINFO bmi = {0};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = bmp.bmWidth;
  bmi.bmiHeader.biHeight = -bmp.bmHeight; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  std::vector<BYTE> pixels(bmp.bmWidth * bmp.bmHeight * 4);
  GetDIBits(hDC, iconInfo.hbmColor, 0, bmp.bmHeight, pixels.data(), &bmi,
            DIB_RGB_COLORS);
  ReleaseDC(nullptr, hDC);

  DeleteObject(iconInfo.hbmColor);
  if (iconInfo.hbmMask)
    DeleteObject(iconInfo.hbmMask);

  IDirect3DTexture9 *pTexture = nullptr;
  if (SUCCEEDED(g_pd3dDevice->CreateTexture(bmp.bmWidth, bmp.bmHeight, 1, 0,
                                            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                            &pTexture, nullptr))) {
    D3DLOCKED_RECT rect;
    if (SUCCEEDED(pTexture->LockRect(0, &rect, nullptr, 0))) {
      BYTE *dest = (BYTE *)rect.pBits;
      BYTE *src = pixels.data();
      for (int y = 0; y < bmp.bmHeight; y++) {
        memcpy(dest + y * rect.Pitch, src + y * bmp.bmWidth * 4,
               bmp.bmWidth * 4);
      }
      pTexture->UnlockRect(0);
    }
  }
  return pTexture;
}

// Async Icon Structures
struct PendingIconData {
  std::wstring Path;
  int Width, Height;
  std::vector<BYTE> Pixels;
};
static std::mutex g_PendingIconsMutex;
static std::vector<PendingIconData> g_PendingIcons;
static std::unordered_map<std::wstring, bool> g_PendingIconsSet;

static std::unordered_map<std::wstring, IDirect3DTexture9 *> g_IconCache;

static IDirect3DTexture9 *GetCachedProcessIcon(const std::wstring &path) {
  if (path.empty())
    return nullptr;
  auto it = g_IconCache.find(path);
  if (it != g_IconCache.end())
    return it->second;

  {
    std::lock_guard<std::mutex> lock(g_PendingIconsMutex);
    if (g_PendingIconsSet.find(path) != g_PendingIconsSet.end())
      return nullptr;
    // v2.5: Throttle concurrent icon extraction threads
    if (g_PendingIconThreads.load() >= MAX_ICON_THREADS)
      return nullptr;
    g_PendingIconsSet[path] = true;
    g_PendingIconThreads++;
  }

  std::thread([path]() {
    HICON hIcon = nullptr;
    SHFILEINFOW sfi = {0};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON)) {
      hIcon = sfi.hIcon;
    }

    // Fallback for system/hidden files
    if (!hIcon) {
      wchar_t winDir[MAX_PATH];
      GetWindowsDirectoryW(winDir, MAX_PATH);
      std::wstring fallbackPath = std::wstring(winDir) + L"\\explorer.exe";
      if (SHGetFileInfoW(fallbackPath.c_str(), 0, &sfi, sizeof(sfi),
                         SHGFI_ICON | SHGFI_SMALLICON)) {
        hIcon = sfi.hIcon;
      }
    }

    PendingIconData data;
    data.Path = path;
    data.Width = 0;
    data.Height = 0;

    if (hIcon) {
      ICONINFO iconInfo = {0};
      if (GetIconInfo(hIcon, &iconInfo)) {
        BITMAP bmp = {0};
        GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmp);
        if (bmp.bmWidth > 0 && bmp.bmHeight > 0) {
          HDC hDC = GetDC(nullptr);
          BITMAPINFO bmi = {0};
          bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
          bmi.bmiHeader.biWidth = bmp.bmWidth;
          bmi.bmiHeader.biHeight = -bmp.bmHeight;
          bmi.bmiHeader.biPlanes = 1;
          bmi.bmiHeader.biBitCount = 32;
          bmi.bmiHeader.biCompression = BI_RGB;

          data.Width = bmp.bmWidth;
          data.Height = bmp.bmHeight;
          data.Pixels.resize(bmp.bmWidth * bmp.bmHeight * 4);
          GetDIBits(hDC, iconInfo.hbmColor, 0, bmp.bmHeight, data.Pixels.data(),
                    &bmi, DIB_RGB_COLORS);
          ReleaseDC(nullptr, hDC);
        }
        DeleteObject(iconInfo.hbmColor);
        if (iconInfo.hbmMask)
          DeleteObject(iconInfo.hbmMask);
      }
      DestroyIcon(hIcon);
    }

    std::lock_guard<std::mutex> lock(g_PendingIconsMutex);
    g_PendingIcons.push_back(std::move(data));
    g_PendingIconThreads--;
  }).detach();

  return nullptr;
}

// Helper: Verify Microsoft Signature with Caching
static bool VerifyMicrosoftSignature(const std::wstring &filePath) {
  if (filePath.empty())
    return false;

  {
    std::lock_guard<std::mutex> lock(g_SigCacheMutex);
    auto it = g_SignatureCache.find(filePath);
    if (it != g_SignatureCache.end()) {
      return it->second;
    }
  }

  WINTRUST_FILE_INFO fileData;
  ZeroMemory(&fileData, sizeof(fileData));
  fileData.cbStruct = sizeof(WINTRUST_FILE_INFO);
  fileData.pcwszFilePath = filePath.c_str();

  WINTRUST_DATA trustData;
  ZeroMemory(&trustData, sizeof(trustData));
  trustData.cbStruct = sizeof(WINTRUST_DATA);
  trustData.dwUIChoice = WTD_UI_NONE;
  trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
  trustData.dwUnionChoice = WTD_CHOICE_FILE;
  trustData.dwStateAction = WTD_STATEACTION_VERIFY;
  trustData.pFile = &fileData;

  GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG status = WinVerifyTrust(nullptr, &policyGUID, &trustData);

  trustData.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policyGUID, &trustData);

  bool isSigned = (status == ERROR_SUCCESS);

  {
    std::lock_guard<std::mutex> lock(g_SigCacheMutex);
    g_SignatureCache[filePath] = isSigned;
  }

  return isSigned;
}

// Helper: Detect fake system processes
static bool IsFakeSystemProcess(const std::wstring &imgName,
                                const std::wstring &fullPath) {
  if (fullPath.empty())
    return false;

  // Convert to lower
  std::wstring nameL = imgName;
  std::wstring pathL = fullPath;
  for (auto &c : nameL)
    c = towlower(c);
  for (auto &c : pathL)
    c = towlower(c);

  // Common system names
  if (nameL == L"svchost.exe" || nameL == L"csrss.exe" ||
      nameL == L"lsass.exe" || nameL == L"winlogon.exe" ||
      nameL == L"services.exe" || nameL == L"smss.exe" ||
      nameL == L"explorer.exe") {
    // If they don't reside in System32, SysWOW64, or Windows, they are highly
    // suspicious
    if (pathL.find(L"\\windows\\system32\\") == std::wstring::npos &&
        pathL.find(L"\\windows\\syswow64\\") == std::wstring::npos &&
        pathL.find(L"\\windows\\explorer.exe") == std::wstring::npos) {
      return true;
    }
  }
  return false;
}

#include <psapi.h>
#include <sddl.h>

struct CPUHistory {
  uint64_t lastProcessTime = 0;
  uint64_t lastSystemTime = 0;
};
static std::unordered_map<DWORD, CPUHistory> g_CPUHistory;

static float CalculateCPU(DWORD pid, HANDLE hProc, uint64_t currentSystemTime) {
  FILETIME ct, et, kt, ut;
  if (GetProcessTimes(hProc, &ct, &et, &kt, &ut)) {
    uint64_t currentProcTime =
        (((uint64_t)kt.dwHighDateTime << 32) | kt.dwLowDateTime) +
        (((uint64_t)ut.dwHighDateTime << 32) | ut.dwLowDateTime);

    auto &hist = g_CPUHistory[pid];
    if (hist.lastSystemTime == 0) {
      hist.lastProcessTime = currentProcTime;
      hist.lastSystemTime = currentSystemTime;
      return 0.0f;
    }

    uint64_t systemDiff = currentSystemTime - hist.lastSystemTime;
    uint64_t procDiff = currentProcTime - hist.lastProcessTime;

    hist.lastProcessTime = currentProcTime;
    hist.lastSystemTime = currentSystemTime;

    if (systemDiff == 0)
      return 0.0f;

    static int numCores = -1;
    if (numCores == -1) {
      SYSTEM_INFO sysInfo;
      GetSystemInfo(&sysInfo);
      numCores = sysInfo.dwNumberOfProcessors;
    }

    float usage = ((float)procDiff / (float)systemDiff) * 100.0f;
    if (usage > 100.0f)
      usage = 100.0f;
    if (usage < 0.0f)
      usage = 0.0f;
    return usage;
  }
  return 0.0f;
}

static void GetProcessRAM(HANDLE hProc, SIZE_T *ws, SIZE_T *privateUsage) {
  PROCESS_MEMORY_COUNTERS_EX pmc;
  if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS *)&pmc,
                           sizeof(pmc))) {
    if (ws)
      *ws = pmc.WorkingSetSize;
    if (privateUsage)
      *privateUsage = pmc.PrivateUsage;
  }
}

static std::wstring GetProcessUser(HANDLE hProc) {
  HANDLE hToken = nullptr;
  if (!OpenProcessToken(hProc, TOKEN_QUERY, &hToken))
    return L"Unknown";

  DWORD len = 0;
  GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
  if (len == 0) {
    CloseHandle(hToken);
    return L"Unknown";
  }

  std::vector<BYTE> buffer(len);
  if (GetTokenInformation(hToken, TokenUser, buffer.data(), len, &len)) {
    TOKEN_USER *pTU = (TOKEN_USER *)buffer.data();
    wchar_t name[256], domain[256];
    DWORD nameLen = 256, domainLen = 256;
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, pTU->User.Sid, name, &nameLen, domain,
                          &domainLen, &use)) {
      CloseHandle(hToken);
      return std::wstring(domain) + L"\\" + name;
    }
  }
  CloseHandle(hToken);
  return L"Unknown";
}

// Process command line retrieval: use forward-declared function from
// zaslon_core.cpp (avoids duplication of NtQueryInformationProcess logic)
static std::wstring GetProcessCommandLineLocal(HANDLE hProc) {
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  pfnNtQueryInformationProcess NtQueryInfo =
      (pfnNtQueryInformationProcess)GetProcAddress(hNtDll,
                                                   "NtQueryInformationProcess");
  if (!NtQueryInfo)
    return L"";

  ULONG len = 0;
  NtQueryInfo(hProc, 60, nullptr, 0, &len);
  if (len == 0)
    return L"";

  std::vector<BYTE> buffer(len);
  if (NtQueryInfo(hProc, 60, buffer.data(), len, &len) >= 0) {
    UNICODE_STRING *pCmd = (UNICODE_STRING *)buffer.data();
    if (pCmd->Buffer && pCmd->Length > 0) {
      return std::wstring(pCmd->Buffer, pCmd->Length / sizeof(wchar_t));
    }
  }
  return L"";
}

// Build snapshot
static void BuildFromSnapshotAsync() {
  FILETIME sysIdle, sysKernel, sysUser;
  uint64_t currentSysTime = 0;
  if (GetSystemTimes(&sysIdle, &sysKernel, &sysUser)) {
    currentSysTime =
        (((uint64_t)sysKernel.dwHighDateTime << 32) | sysKernel.dwLowDateTime) +
        (((uint64_t)sysUser.dwHighDateTime << 32) | sysUser.dwLowDateTime);
  }

  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE) {
    g_IsRefreshing = false;
    return;
  }

  std::vector<ProcessNode> nodes;
  PROCESSENTRY32W pe = {sizeof(pe)};

  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  pfnNtQueryInformationProcess NtQueryInfo =
      (pfnNtQueryInformationProcess)GetProcAddress(hNtDll,
                                                   "NtQueryInformationProcess");

  if (Process32FirstW(hSnap, &pe)) {
    DWORD myPid = GetCurrentProcessId();
    do {
      if (pe.th32ProcessID == myPid)
        continue;

      ProcessNode n;
      n.Pid = pe.th32ProcessID;
      n.ParentPid = pe.th32ParentProcessID;
      n.ThreadCount = pe.cntThreads;
      n.HandleCount = 0;
      n.Flags = 0;
      n.ImageName = pe.szExeFile;
      n.MicrosoftSigned = false;

      HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                 FALSE, pe.th32ProcessID);
      if (!hProc)
        hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                            pe.th32ProcessID);

      uint64_t createTime = 0;
      if (hProc) {
        FILETIME ct, et, kt, ut;
        if (GetProcessTimes(hProc, &ct, &et, &kt, &ut)) {
          createTime = ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
        }
      }

      auto it = g_ProcessCache.find(n.Pid);
      bool useCache =
          (it != g_ProcessCache.end() && it->second.CreateTime == createTime);

      // Handle processes strictly based on cache
      if (hProc) {
        if (!useCache) {
          wchar_t fullPath[MAX_PATH] = {};
          DWORD sz = MAX_PATH;
          if (QueryFullProcessImageNameW(hProc, 0, fullPath, &sz)) {
            n.FullPath = fullPath;
            n.FileDescription = GetFileDescription(n.FullPath);
            n.MicrosoftSigned = VerifyMicrosoftSignature(n.FullPath);
          }
          n.UserName = GetProcessUser(hProc);
          n.CommandLine = GetProcessCommandLineLocal(hProc);
        } else {
          n.FullPath = it->second.FullPath;
          n.FileDescription = it->second.FileDescription;
          n.MicrosoftSigned = it->second.MicrosoftSigned;
          n.UserName = it->second.UserName;
          n.CommandLine = it->second.CommandLine;
        }

        // ALWAYS update dynamic metrics
        GetProcessHandleCount(hProc, &n.HandleCount);
        GetProcessRAM(hProc, &n.WorkingSetSize, &n.PrivateUsage);
        n.CpuUsagePercent = CalculateCPU(n.Pid, hProc, currentSysTime);

        // Check Critical Flag (BreakOnTermination = 29)
        if (NtQueryInfo) {
          ULONG isCritical = 0;
          if (NtQueryInfo(hProc, 29, &isCritical, sizeof(isCritical),
                          nullptr) >= 0) {
            if (isCritical)
              n.Flags |= ZASLON_PFLAG_CRITICAL;
          }
        }
        CloseHandle(hProc);
      } else if (useCache) {
        // If we can't open the process (access denied), use cached static info
        // but metrics stay 0
        n.FullPath = it->second.FullPath;
        n.FileDescription = it->second.FileDescription;
        n.MicrosoftSigned = it->second.MicrosoftSigned;
        n.CpuUsagePercent = 0.0f;
      }

      if (n.Pid <= 8)
        n.Flags |= ZASLON_PFLAG_SYSTEM;
      if (IsFakeSystemProcess(n.ImageName, n.FullPath))
        n.Flags |= ZASLON_PFLAG_FAKE_SYS;

      if (!useCache) {
        // Compute Heuristic Risk Score using ZaslonCore for NEW processes
        ZaslonCore::ProcessHeuristicData heurData =
            ZaslonCore::GatherProcessHeuristics(n.Pid);
        ZaslonCore::RiskResult risk = ZaslonCore::CalculateRiskScore(heurData);
        n.RiskScore = risk.Score;
        n.RiskReasons = std::move(risk.Reasons);

        // Update Cache
        CachedProcessInfo info;
        info.CreateTime = createTime;
        info.FullPath = n.FullPath;
        info.FileDescription = n.FileDescription;
        info.MicrosoftSigned = n.MicrosoftSigned;
        info.UserName = n.UserName;
        info.CommandLine = n.CommandLine;
        info.RiskScore = n.RiskScore;
        info.RiskReasons = n.RiskReasons;
        g_ProcessCache[n.Pid] = info;
      } else {
        n.RiskScore = it->second.RiskScore;
        n.RiskReasons = it->second.RiskReasons;
      }

      nodes.push_back(std::move(n));
    } while (Process32NextW(hSnap, &pe));
  }
  CloseHandle(hSnap);

  std::sort(
      nodes.begin(), nodes.end(),
      [](const ProcessNode &a, const ProcessNode &b) { return a.Pid < b.Pid; });

  // Build Parent-Child hierarchies
  std::unordered_map<ULONG, ProcessNode *> pidMap;
  for (auto &n : nodes)
    pidMap[n.Pid] = &n;

  for (auto &n : nodes) {
    if (n.ParentPid != 0 && n.ParentPid != n.Pid) {
      auto parentIt = pidMap.find(n.ParentPid);
      if (parentIt != pidMap.end()) {
        parentIt->second->Children.push_back(n.Pid);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_NodesMutex);
    g_NodesBackBuffer = std::move(nodes);
  }

  // v2.5: Clean stale CPU history entries for dead processes
  {
    std::unordered_map<DWORD, CPUHistory> cleanedHistory;
    for (const auto &n :
         g_NodesBackBuffer.empty() ? g_Nodes : g_NodesBackBuffer) {
      auto it = g_CPUHistory.find(n.Pid);
      if (it != g_CPUHistory.end()) {
        cleanedHistory[n.Pid] = it->second;
      }
    }
    g_CPUHistory = std::move(cleanedHistory);
  }

  g_IsRefreshing = false;
}

void ProcessPanel_Refresh() {
  if (!g_IsRefreshing) {
    g_IsRefreshing = true;
    std::thread(BuildFromSnapshotAsync).detach();
  }
}

// Forward declarations for detail tabs
static void RenderGeneralTab(ProcessNode *n);
static void RenderThreadsTab(ULONG pid);
static void RenderModulesTab(ULONG pid);
static void RenderHandlesTab(ULONG pid);
static void RenderSecurityTab(ProcessNode *n);

static void StripCriticalAndKillSafeAsync(int pid) {
  std::thread([pid]() {
    HANDLE hProc =
        OpenProcess(PROCESS_SET_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (hProc) {
      HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
      pfnNtSetInformationProcess NtSetInfo =
          (pfnNtSetInformationProcess)GetProcAddress(hNtDll,
                                                     "NtSetInformationProcess");
      if (NtSetInfo) {
        ULONG isCritical = 0;
        NtSetInfo(hProc, 29, &isCritical, sizeof(isCritical));
      }
      TerminateProcess(hProc, 0);
      CloseHandle(hProc);
    }
  }).detach();
}

static void StripCriticalSafeAsync(int pid) {
  std::thread([pid]() {
    HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (hProc) {
      HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
      pfnNtSetInformationProcess NtSetInfo =
          (pfnNtSetInformationProcess)GetProcAddress(hNtDll,
                                                     "NtSetInformationProcess");
      if (NtSetInfo) {
        ULONG isCritical = 0;
        NtSetInfo(hProc, 29, &isCritical, sizeof(isCritical));
      }
      CloseHandle(hProc);
    }
  }).detach();
}

static bool IsProcessSuspended(DWORD pid) {
  bool isSuspended = false;
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (hSnap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te = {sizeof(te)};
    if (Thread32First(hSnap, &te)) {
      do {
        if (te.th32OwnerProcessID == pid) {
          HANDLE hThread =
              OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
          if (hThread) {
            DWORD count = SuspendThread(hThread);
            if (count != (DWORD)-1) {
              if (count > 0)
                isSuspended = true;
              ResumeThread(hThread); // revert our test
            }
            CloseHandle(hThread);
            break;
          }
        }
      } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);
  }
  return isSuspended;
}

static void RenderProcessNode(ProcessNode *n,
                              std::unordered_map<ULONG, ProcessNode *> &map,
                              const wchar_t *wFilter) {
  // Filter matching
  if (wFilter[0] != L'\0') {
    wchar_t pidStr[16] = {};
    StringCchPrintfW(pidStr, 16, L"%u", n->Pid);
    if (wcsstr(n->ImageName.c_str(), wFilter) == nullptr &&
        wcsstr(pidStr, wFilter) == nullptr) {
      // Check if any child matches the filter, if not, skip rendering this
      // branch
      bool childMatch = false;
      for (ULONG childPid : n->Children) {
        if (map.count(childPid)) {
          wchar_t cpidStr[16] = {};
          StringCchPrintfW(cpidStr, 16, L"%u", childPid);
          if (wcsstr(map[childPid]->ImageName.c_str(), wFilter) != nullptr ||
              wcsstr(cpidStr, wFilter) != nullptr) {
            childMatch = true;
            break;
          }
        }
      }
      if (!childMatch)
        return;
    }
  }

  ImGui::TableNextRow(ImGuiTableRowFlags_None, 24.0f);

  if (n->Flags & ZASLON_PFLAG_FAKE_SYS) {
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                           ImGui::GetColorU32(ImVec4(0.4f, 0.0f, 0.0f, 0.6f)));
  } else if (!n->MicrosoftSigned && !n->FullPath.empty()) {
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                           ImGui::GetColorU32(ImVec4(0.3f, 0.1f, 0.1f, 0.4f)));
  }

  ImGui::TableNextColumn();

  ImVec4 textColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
  if (n->Flags & ZASLON_PFLAG_SYSTEM)
    textColor = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);
  if (n->Flags & ZASLON_PFLAG_CRITICAL)
    textColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

  ImGui::PushStyleColor(ImGuiCol_Text, textColor);

  char imgName[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, n->ImageName.c_str(), -1, imgName, 128,
                      nullptr, nullptr);

  bool isSelected = (g_SelectedPid == (int)n->Pid);
  bool isLeaf = n->Children.empty();
  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_SpanFullWidth |
      (isLeaf ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet
              : ImGuiTreeNodeFlags_OpenOnArrow);
  if (isSelected)
    flags |= ImGuiTreeNodeFlags_Selected;
  // Auto-open trees if filter is active
  if (wFilter[0] != L'\0')
    flags |= ImGuiTreeNodeFlags_DefaultOpen;

  // Use spaces so the tree node is wide enough to be clicked, but hide the ID
  // segment
  bool open =
      ImGui::TreeNodeEx((void *)(intptr_t)n->Pid, flags, "   ##%u", n->Pid);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    g_SelectedPid = (int)n->Pid;
  }

  // The context menu must immediately follow the TreeNode so it registers the
  // click on the full row
  bool popup = ImGui::BeginPopupContextItem();

  ImGui::SameLine(0, 4.0f);
  IDirect3DTexture9 *iconTex = GetCachedProcessIcon(n->FullPath);
  if (iconTex) {
    ImGui::Image((void *)iconTex, ImVec2(16, 16));
    ImGui::SameLine(0, 4.0f);
  }

  ImGui::TextUnformatted(imgName);

  // Context Menu Body
  if (popup) {
    g_SelectedPid = (int)n->Pid;
    g_InteractionPid = (int)n->Pid;
    ImGui::TextDisabled(u8"%s (PID: %u)", imgName, n->Pid);
    ImGui::Separator();

    if (ImGui::MenuItem(u8"Завершить")) {
      HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, n->Pid);
      if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
      }
      ProcessPanel_Refresh();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
    if (ImGui::MenuItem(u8"Уничтожить")) {
      ZaslonCore::ForceKillProcess(n->Pid);
      ProcessPanel_Refresh();
    }
    ImGui::PopStyleColor();

    if (n->Flags & ZASLON_PFLAG_CRITICAL) {
      if (ImGui::MenuItem(u8"Снять статус 'Critical'")) {
        StripCriticalSafeAsync(n->Pid);
        ProcessPanel_Refresh();
      }
      if (ImGui::MenuItem(u8"Снять статус 'Critical' и Завершить")) {
        if (n->MicrosoftSigned) {
          g_PendingCriticalPid = n->Pid;
          g_ShowCriticalWarning = true;
        } else {
          StripCriticalAndKillSafeAsync(n->Pid);
          ProcessPanel_Refresh();
        }
      }
    }

    ImGui::Separator();
    if (ImGui::BeginMenu(u8"Приоритет...")) {
      auto setPriority = [&](DWORD pclass) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, n->Pid);
        if (h) {
          SetPriorityClass(h, pclass);
          CloseHandle(h);
        }
      };
      if (ImGui::MenuItem(u8"Реального времени"))
        setPriority(REALTIME_PRIORITY_CLASS);
      if (ImGui::MenuItem(u8"Высокий"))
        setPriority(HIGH_PRIORITY_CLASS);
      if (ImGui::MenuItem(u8"Выше среднего"))
        setPriority(ABOVE_NORMAL_PRIORITY_CLASS);
      if (ImGui::MenuItem(u8"Обычный"))
        setPriority(NORMAL_PRIORITY_CLASS);
      if (ImGui::MenuItem(u8"Ниже среднего"))
        setPriority(BELOW_NORMAL_PRIORITY_CLASS);
      if (ImGui::MenuItem(u8"Низкий"))
        setPriority(IDLE_PRIORITY_CLASS);
      ImGui::EndMenu();
    }

    ImGui::Separator();
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    pfnNtSuspendProcess NtSuspendInfo =
        (pfnNtSuspendProcess)GetProcAddress(hNtDll, "NtSuspendProcess");
    pfnNtResumeProcess NtResumeInfo =
        (pfnNtResumeProcess)GetProcAddress(hNtDll, "NtResumeProcess");

    bool isSuspended = IsProcessSuspended(n->Pid);
    if (isSuspended) {
      if (ImGui::MenuItem(u8"Разморозить")) {
        HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, n->Pid);
        if (h && NtResumeInfo) {
          NtResumeInfo(h);
          CloseHandle(h);
        }
        ProcessPanel_Refresh();
      }
    } else {
      if (ImGui::MenuItem(u8"Заморозить")) {
        HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, n->Pid);
        if (h && NtSuspendInfo) {
          NtSuspendInfo(h);
          CloseHandle(h);
        }
        ProcessPanel_Refresh();
      }
    }

    ImGui::Separator();
    if (ImGui::MenuItem(u8"Свойства..."))
      g_InteractionPid = (int)n->Pid;
    if (ImGui::MenuItem(u8"Открыть папку", nullptr, false,
                        !n->FullPath.empty())) {
      std::wstring args = L"/select,\"" + n->FullPath + L"\"";
      ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                    SW_SHOW);
    }
    ImGui::EndPopup();
  }

  // Table Columns (6 total defined in BeginTable)
  // 1. Process Name (Already handled by TreeNode)

  // 2. PID
  ImGui::TableNextColumn();
  ImGui::Text("%u", n->Pid);

  // 3. Risk Score (NEW)
  ImGui::TableNextColumn();
  if (n->RiskScore > 0) {
    ImVec4 riskColor =
        n->RiskScore >= 50
            ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f)
            : (n->RiskScore >= 20 ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f)
                                  : ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, riskColor);
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImVec4(0.0f, 0.0f, 0.0f, 1.0f)); // Black text for high contrast
    char riskBuf[32];
    snprintf(riskBuf, sizeof(riskBuf), "%d%%", n->RiskScore);
    ImGui::ProgressBar(n->RiskScore / 100.0f, ImVec2(-FLT_MIN, 16), riskBuf);
    ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered() && !n->RiskReasons.empty()) {
      ImGui::BeginTooltip();
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), u8"Анализ Угроз:");
      ImGui::Separator();
      for (const auto &reason : n->RiskReasons) {
        ImGui::TextUnformatted(reason.c_str());
      }
      ImGui::EndTooltip();
    }
  } else {
    ImGui::TextDisabled("Пойдет");
  }

  // 4. Publisher
  ImGui::TableNextColumn();
  char utfDesc[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, n->FileDescription.c_str(), -1, utfDesc, 256,
                      nullptr, nullptr);
  if (!n->MicrosoftSigned && n->FileDescription.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"Неизвестный");
  } else {
    if (!n->MicrosoftSigned)
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
    ImGui::TextUnformatted(utfDesc[0] ? utfDesc : u8"Неизвестный");
    if (!n->MicrosoftSigned)
      ImGui::PopStyleColor();
  }

  // 4. CPU %
  ImGui::TableNextColumn();
  if (n->CpuUsagePercent > 0.1f) {
    ImGui::Text("%.1f%%", n->CpuUsagePercent);
  } else {
    ImGui::TextDisabled("0.0%%");
  }

  // 5. RAM
  ImGui::TableNextColumn();
  ImGui::Text("%.1f MB", (float)n->WorkingSetSize / (1024.0f * 1024.0f));

  ImGui::PopStyleColor();

  if (open) {
    for (ULONG childPid : n->Children) {
      if (map.count(childPid)) {
        RenderProcessNode(map[childPid], map, wFilter);
      }
    }
    ImGui::TreePop();
  }
}

void ProcessPanel_Render() {
  // Process async icons on the UI thread for D3D creation
  if (g_PendingIconsMutex.try_lock()) {
    for (auto &pending : g_PendingIcons) {
      IDirect3DTexture9 *tex = nullptr;
      if (pending.Width > 0 && pending.Height > 0 &&
          pending.Pixels.size() > 0 && g_pd3dDevice) {
        if (SUCCEEDED(g_pd3dDevice->CreateTexture(
                pending.Width, pending.Height, 1, 0, D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED, &tex, nullptr))) {
          D3DLOCKED_RECT rect;
          if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0))) {
            BYTE *dest = (BYTE *)rect.pBits;
            BYTE *src = pending.Pixels.data();
            for (int y = 0; y < pending.Height; y++) {
              memcpy(dest + y * rect.Pitch, src + y * pending.Width * 4,
                     pending.Width * 4);
            }
            tex->UnlockRect(0);
          }
        }
      }

      if (tex) {
        g_IconCache[pending.Path] = tex;
      } else {
        // If texture failed, remove from pending set so it can be retried later
        g_PendingIconsSet.erase(pending.Path);
      }
    }
    g_PendingIcons.clear();
    g_PendingIconsMutex.unlock();
  }

  // Sync buffer from async thread
  if (g_NodesMutex.try_lock()) {
    if (!g_NodesBackBuffer.empty()) {
      g_Nodes = g_NodesBackBuffer;
      g_NodesBackBuffer.clear(); // Copy and clear
    }
    g_NodesMutex.unlock();
  }

  g_RefreshTimer += ImGui::GetIO().DeltaTime;
  if ((g_RefreshTimer >= g_RefreshInterval || g_Nodes.empty()) &&
      !g_IsRefreshing) {
    g_RefreshTimer = 0.0f;
    ProcessPanel_Refresh();
  }

  // Modal for Critical warning
  if (g_ShowCriticalWarning) {
    ImGui::OpenPopup(u8"Уведомление: Системный Процесс");
  }

  if (ImGui::BeginPopupModal(u8"Уведомление: Системный Процесс", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                       u8"ВНИМАНИЕ! Этот процесс имеет статус КРИТИЧЕСКИЙ\nи "
                       u8"подписан сертификатом Microsoft.");
    ImGui::Text(u8"Снятие флага и завершение может привести к немедленному "
                u8"BSOD.\nПродолжить?");
    ImGui::Spacing();
    if (ImGui::Button(u8"ДА, Убить", ImVec2(240, 0))) {
      StripCriticalAndKillSafeAsync(g_PendingCriticalPid);
      g_ShowCriticalWarning = false;
      g_PendingCriticalPid = -1;
      ProcessPanel_Refresh();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"ОТМЕНА", ImVec2(100, 0))) {
      g_ShowCriticalWarning = false;
      g_PendingCriticalPid = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  // Top Controls
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
  ImGui::SetNextItemWidth(250.0f);
  ImGui::InputTextWithHint("##filter", u8" Поиск...", g_Filter,
                           sizeof(g_Filter));

  ImGui::SameLine(0, 10.0f);
  if (ImGui::Button(g_IsRefreshing ? u8"Обновление..." : u8"Обновить",
                    ImVec2(100, 0)))
    ProcessPanel_Refresh();

  ImGui::PopStyleVar();

  // v2.5: Process count and threat count
  ImGui::SameLine(0, 20.0f);
  int totalProcs = (int)g_Nodes.size();
  int threatCount = 0;
  for (const auto &n : g_Nodes) {
    if (n.RiskScore >= 30)
      threatCount++;
  }
  ImGui::TextDisabled(u8"Процессов: %d", totalProcs);
  ImGui::SameLine(0, 15.0f);
  if (threatCount > 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"Угроз: %d",
                       threatCount);
  } else {
    ImGui::TextDisabled(u8"Угроз: 0");
  }

  ImGui::Spacing();

  // Table
  if (ImGui::BeginTable("ProcessTable", 6,
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn(u8"Имя", ImGuiTableColumnFlags_WidthStretch, 250);
    ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(u8"Угроза", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn(u8"Издатель", ImGuiTableColumnFlags_WidthStretch,
                            200);
    ImGui::TableSetupColumn(u8"CPU %", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(u8"RAM", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableHeadersRow();

    wchar_t wFilter[128] = {};
    MultiByteToWideChar(CP_UTF8, 0, g_Filter, -1, wFilter, 128);

    std::unordered_map<ULONG, ProcessNode *> pidMap;
    for (auto &n : g_Nodes)
      pidMap[n.Pid] = &n;

    for (auto &n : g_Nodes) {
      // Only start recursive render from root nodes (no parent, or parent
      // doesn't exist)
      if (n.ParentPid == 0 || pidMap.find(n.ParentPid) == pidMap.end()) {
        RenderProcessNode(&n, pidMap, wFilter);
      }
    }
    ImGui::EndTable();
  }

  // End of Hunter mode UI

  // Ext Properties Modal
  if (g_InteractionPid != -1 &&
      ImGui::IsPopupOpen(u8"Свойства Процесса##modal") == false)
    ImGui::OpenPopup(u8"Свойства Процесса##modal");

  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Appearing);

  if (ImGui::BeginPopupModal(u8"Свойства Процесса##modal", nullptr,
                             ImGuiWindowFlags_NoResize)) {
    ProcessNode *target = nullptr;
    for (auto &node : g_Nodes) {
      if ((int)node.Pid == g_InteractionPid) {
        target = &node;
        break;
      }
    }

    if (target) {
      if (ImGui::BeginTabBar("ProcessDetailsTabs")) {
        if (ImGui::BeginTabItem(u8"Общее")) {
          RenderGeneralTab(target);
          ImGui::EndTabItem();
        }

        // Initialize basic cache on modal open
        RefreshBasicPropertyCache(target->Pid);

        if (ImGui::BeginTabItem(u8"Потоки")) {
          RenderThreadsTab(target->Pid);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(u8"Модули")) {
          RenderModulesTab(target->Pid);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(u8"Дескрипторы")) {
          RefreshHandlePropertyCache(
              target->Pid); // Only load handles when tab is opened
          RenderHandlesTab(target->Pid);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(u8"Безопасность")) {
          RenderSecurityTab(target);
          ImGui::EndTabItem();
        }

        if (ImGui::TabItemButton(u8"Обновить данные")) {
          g_PropCache.BasicLoaded = false;
          g_PropCache.HandlesLoaded = false;
          RefreshBasicPropertyCache(target->Pid);
        }
        ImGui::EndTabBar();
      }

      ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 40);
      ImGui::Separator();
      ImGui::Spacing();

      // Left-aligned action buttons
      if (ImGui::Button(u8"Завершить", ImVec2(100, 0))) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, target->Pid);
        if (h) {
          TerminateProcess(h, 0);
          CloseHandle(h);
        }
        ProcessPanel_Refresh();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"Обычное завершение процесса");

      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
      if (ImGui::Button(u8"Уничтожить", ImVec2(100, 0))) {
        ZaslonCore::ForceKillProcess(target->Pid);
        ProcessPanel_Refresh();
      }
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"Жесткое завершение");

      ImGui::SameLine();
      if (ImGui::Button(u8"В Google", ImVec2(90, 0))) {
        std::wstring url =
            L"https://www.google.com/search?q=" + target->ImageName;
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOW);
      }

      // Right-aligned OK button
      ImGui::SameLine(ImGui::GetWindowWidth() - 110);
      if (ImGui::Button(u8"ОК", ImVec2(100, 0))) {
        g_InteractionPid = -1;
        g_PropCache.BasicLoaded = false;
        g_PropCache.HandlesLoaded = false;
        ImGui::CloseCurrentPopup();
      }
    } else {
      ImGui::Text(u8"Процесс больше не существует.");
      if (ImGui::Button(u8"Закрыть")) {
        g_InteractionPid = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

static void RenderSecurityTab(ProcessNode *n) {
  ImGui::Spacing();
  ImGui::TextColored(g_Theme.AccentColor, u8"Опасные операции");
  ImGui::Spacing();

  ImGui::TextWrapped(u8"Изменение этих параметров может привести к "
                     u8"нестабильности системы (BSOD).");
  ImGui::Spacing();

  bool isCritical = (n->Flags & ZASLON_PFLAG_CRITICAL) != 0;

  if (isCritical) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       u8"Внимание: Этот процесс помечен как КРИТИЧЕСКИЙ");
    ImGui::Spacing();

    if (ImGui::Button(u8"Снять статус 'Critical'", ImVec2(320, 0))) {
      StripCriticalSafeAsync(n->Pid);
      ProcessPanel_Refresh();
    }

    ImGui::Spacing();
    if (ImGui::Button(u8"Снять статус и Убить", ImVec2(320, 0))) {
      if (n->MicrosoftSigned) {
        g_PendingCriticalPid = n->Pid;
        g_ShowCriticalWarning = true;
      } else {
        StripCriticalAndKillSafeAsync(n->Pid);
        ProcessPanel_Refresh();
      }
    }
  } else {
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                       u8"Этот процесс не является критическим для системы.");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::TextColored(g_Theme.AccentColor, u8"Управление потоками исполнения");
  ImGui::Spacing();

  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  pfnNtSuspendProcess NtSuspendInfo =
      (pfnNtSuspendProcess)GetProcAddress(hNtDll, "NtSuspendProcess");
  pfnNtResumeProcess NtResumeInfo =
      (pfnNtResumeProcess)GetProcAddress(hNtDll, "NtResumeProcess");

  if (ImGui::Button(u8"Приостановить", ImVec2(180, 0))) {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, n->Pid);
    if (h && NtSuspendInfo) {
      NtSuspendInfo(h);
      CloseHandle(h);
    }
    ProcessPanel_Refresh();
  }
  ImGui::SameLine();
  if (ImGui::Button(u8"Возобновить", ImVec2(180, 0))) {
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, n->Pid);
    if (h && NtResumeInfo) {
      NtResumeInfo(h);
      CloseHandle(h);
    }
    ProcessPanel_Refresh();
  }
}

// Low-level Tabs Implementation
static void RenderGeneralTab(ProcessNode *n) {
  char imgName[128] = {}, fullPath[MAX_PATH] = {}, cmdLine[1024] = {},
       userName[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, n->ImageName.c_str(), -1, imgName, 128,
                      nullptr, nullptr);
  WideCharToMultiByte(CP_UTF8, 0, n->FullPath.c_str(), -1, fullPath, MAX_PATH,
                      nullptr, nullptr);
  WideCharToMultiByte(CP_UTF8, 0, n->CommandLine.c_str(), -1, cmdLine, 1024,
                      nullptr, nullptr);
  WideCharToMultiByte(CP_UTF8, 0, n->UserName.c_str(), -1, userName, 256,
                      nullptr, nullptr);

  char utfDesc[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, n->FileDescription.c_str(), -1, utfDesc, 256,
                      nullptr, nullptr);

  ImGui::Spacing();

  // Header section: Icon + Name
  IDirect3DTexture9 *iconTex = GetCachedProcessIcon(n->FullPath);
  if (iconTex) {
    ImGui::Image((void *)iconTex, ImVec2(32, 32));
    ImGui::SameLine();
  }
  ImGui::BeginGroup();
  ImGui::Text(u8"%s", imgName);
  ImGui::TextDisabled(u8"%s", utfDesc[0] ? utfDesc : u8"Описание отсутствует");
  ImGui::EndGroup();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // Key-Value pairs in a table for clean alignment
  if (ImGui::BeginTable("general_info", 2, ImGuiTableFlags_None)) {
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthStretch);

    auto DrawRow = [](const char *k, const char *v) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(k);
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", v);
    };

    char pidStr[32];
    snprintf(pidStr, sizeof(pidStr), "%u", n->Pid);
    DrawRow(u8"PID:", pidStr);
    char parentStr[32];
    snprintf(parentStr, sizeof(parentStr), "%u", n->ParentPid);
    DrawRow(u8"Родитель:", parentStr);
    DrawRow(u8"Пользователь:", userName);
    DrawRow(u8"Путь:", fullPath);
    DrawRow(u8"Запуск:", cmdLine);

    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::TextColored(g_Theme.AccentColor, u8"Ресурсы и Статистика");
  ImGui::Spacing();

  if (ImGui::BeginTable("perf_info", 2, ImGuiTableFlags_None)) {
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthStretch);

    auto DrawRow = [](const char *k, const char *v) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(k);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(v);
    };

    char wsStr[64];
    snprintf(wsStr, sizeof(wsStr), "%.2f MB",
             (float)n->WorkingSetSize / (1024 * 1024));
    DrawRow(u8"Работающее мн.:", wsStr);

    char privStr[64];
    snprintf(privStr, sizeof(privStr), "%.2f MB",
             (float)n->PrivateUsage / (1024 * 1024));
    DrawRow(u8"Личные байты:", privStr);

    char threadStr[64];
    snprintf(threadStr, sizeof(threadStr), "%u", n->ThreadCount);
    DrawRow(u8"Потоков:", threadStr);

    char handleStr[64];
    snprintf(handleStr, sizeof(handleStr), "%u", n->HandleCount);
    DrawRow(u8"Дескрипторов:", handleStr);

    ImGui::EndTable();
  }
}

static void RenderThreadsTab(ULONG pid) {
  if (ImGui::BeginTable("ThreadsTable", 3,
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders)) {
    ImGui::TableSetupColumn("TID", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(u8"Приоритет", ImGuiTableColumnFlags_WidthFixed,
                            80);
    ImGui::TableSetupColumn(u8"Стартовый адрес",
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const auto &t : g_PropCache.Threads) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%u", t.Tid);
      ImGui::TableNextColumn();
      ImGui::Text("%d", t.Priority);
      ImGui::TableNextColumn();
      ImGui::Text("0x%p", (void *)0);
    }
    ImGui::EndTable();
  }
}

static void RenderModulesTab(ULONG pid) {
  if (ImGui::BeginTable("ModulesTable", 3,
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders)) {
    ImGui::TableSetupColumn(u8"Имя", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn(u8"Адрес", ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableSetupColumn(u8"Путь", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const auto &m : g_PropCache.Modules) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m.Name.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("0x%p", m.Base);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(m.Path.c_str());
    }
    ImGui::EndTable();
  }
}

static void RenderHandlesTab(ULONG pid) {
  if (ImGui::BeginTable("HandlesTable", 3,
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders)) {
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const auto &h : g_PropCache.Handles) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("0x%X", h.Value);
      ImGui::TableNextColumn();
      ImGui::Text("%d", h.Type);
      ImGui::TableNextColumn();
      ImGui::Text("0x%p", h.Object);
    }
    ImGui::EndTable();
  }
}
