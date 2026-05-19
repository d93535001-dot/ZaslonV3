#include "zaslon_core.h"
#include <RestartManager.h>
#include <algorithm>
#include <iphlpapi.h>
#include <psapi.h>
#include <softpub.h>
#include <thread>
#include <tlhelp32.h>
#include <winternl.h>
#include <wintrust.h>

#include "ntapi_defs.h"

#include <atomic>
#include <cmath> // log2f for Shannon entropy
#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>


namespace fs = std::filesystem;

namespace ZaslonCore {

// =========================================================
// MODULE 1: THE TERMINATOR (Extreme Process Killer)
// =========================================================

bool ForceKillProcess(DWORD pid) {
  if (pid == 0 || pid == 4)
    return false; // Пропускаем Idle и System

  bool killed = false;
  HANDLE hProc = nullptr;

  auto CheckDead = [pid]() {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
      return true; // Открыть нельзя = мертв (скорее всего)
    DWORD exitCode = 0;
    GetExitCodeProcess(h, &exitCode);
    CloseHandle(h);
    return (exitCode != STILL_ACTIVE);
  };

  // Level 1: TerminateProcess (Standard ring3)
  hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (hProc) {
    if (TerminateProcess(hProc, 0))
      killed = true;
    CloseHandle(hProc);
    if (CheckDead())
      return true;
  }

  // Level 2: NtTerminateProcess (Native API Bypass ring3 hooks)
  hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (hProc) {
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    pfnNtTerminateProcess NtTerminate =
        (pfnNtTerminateProcess)GetProcAddress(hNtDll, "NtTerminateProcess");
    if (NtTerminate) {
      if (NtTerminate(hProc, 0) >= 0)
        killed = true;
    }
    CloseHandle(hProc);
    if (CheckDead())
      return true;
  }

  // Level 3: Debugger Attach (SeDebugPrivilege required)
  // Подключение отладчика замораживает процесс, а мгновенный выход убивает
  // отлаживаемого юзера
  if (DebugActiveProcess(pid)) {
    DebugActiveProcessStop(pid); // Инициирует Terminate
    if (CheckDead())
      return true;
  }

  // Level 4: CreateRemoteThread + ExitProcess (Injection Suicide)
  hProc =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                      PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                  FALSE, pid);
  if (hProc) {
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LPVOID exitProcAddr = (LPVOID)GetProcAddress(hKernel32, "ExitProcess");
    if (exitProcAddr) {
      HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                          (LPTHREAD_START_ROUTINE)exitProcAddr,
                                          (LPVOID)0, 0, nullptr);
      if (hThread) {
        WaitForSingleObject(hThread, 500);
        CloseHandle(hThread);
      }
    }
    CloseHandle(hProc);
    if (CheckDead())
      return true;
  }

  // Level 5: Job Object Trap
  HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
  if (hJob) {
    hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, pid);
    if (hProc) {
      if (AssignProcessToJobObject(hJob, hProc)) {
        TerminateJobObject(hJob, 0);
      }
      CloseHandle(hProc);
    }
    CloseHandle(hJob);
    if (CheckDead())
      return true;
  }

  // Level 6: Thread Annihilation (Snipe every thread)
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (hSnap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te = {sizeof(te)};
    if (Thread32First(hSnap, &te)) {
      do {
        if (te.th32OwnerProcessID == pid) {
          HANDLE hThread = OpenThread(THREAD_TERMINATE, FALSE, te.th32ThreadID);
          if (hThread) {
            TerminateThread(hThread, 0);
            CloseHandle(hThread);
          }
        }
      } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);
    if (CheckDead())
      return true;
  }

  // Level 7: Handle Closing Attack (Resource Starvation)
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  pfnNtQuerySystemInformation NtQuerySystemInfo =
      (pfnNtQuerySystemInformation)GetProcAddress(hNtDll,
                                                  "NtQuerySystemInformation");

  if (NtQuerySystemInfo) {
    ULONG bufSize = 1024 * 1024 * 4; // 4MB
    PVOID buffer = malloc(bufSize);
    if (buffer) {
      ULONG retLength = 0;
      if (NtQuerySystemInfo(ZASLON_SystemHandleInformation, buffer, bufSize,
                            &retLength) >= 0 ||
          NtQuerySystemInfo(ZASLON_SystemHandleInformation, buffer, retLength,
                            &retLength) >= 0) {
        PZASLON_SYSTEM_HANDLE_INFORMATION handleInfo =
            (PZASLON_SYSTEM_HANDLE_INFORMATION)buffer;
        hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (hProc) {
          // Force close ALL handles, crashing the app violently
          for (ULONG i = 0; i < handleInfo->NumberOfHandles; i++) {
            if (handleInfo->Handles[i].UniqueProcessId == pid) {
              HANDLE hDup = nullptr;
              DuplicateHandle(
                  hProc, (HANDLE)(ULONG_PTR)handleInfo->Handles[i].HandleValue,
                  GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
              if (hDup)
                CloseHandle(hDup);
            }
          }
          CloseHandle(hProc);
        }
      }
      free(buffer);
    }
    if (CheckDead())
      return true;
  }

  return false;
}

// =========================================================
// MODULE 2: SMART UNLOCKER
// =========================================================

std::vector<LockerInfo>
FileUnlocker::GetLockingProcesses(const std::wstring &filePath) {
  std::vector<LockerInfo> lockers;

  DWORD dwSession;
  WCHAR szSessionKey[CCH_RM_SESSION_KEY + 1] = {0};

  if (RmStartSession(&dwSession, 0, szSessionKey) == ERROR_SUCCESS) {
    LPCWSTR rgszResources[] = {filePath.c_str()};
    if (RmRegisterResources(dwSession, 1, rgszResources, 0, nullptr, 0,
                            nullptr) == ERROR_SUCCESS) {
      DWORD dwReason = 0;
      UINT nProcInfoNeeded = 0;
      UINT nProcInfo = 0;

      RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo, nullptr, &dwReason);
      if (nProcInfoNeeded > 0) {
        std::vector<RM_PROCESS_INFO> processInfo(nProcInfoNeeded);
        nProcInfo = nProcInfoNeeded;

        if (RmGetList(dwSession, &nProcInfoNeeded, &nProcInfo,
                      processInfo.data(), &dwReason) == ERROR_SUCCESS) {
          for (UINT i = 0; i < nProcInfo; i++) {
            LockerInfo info;
            info.Pid = processInfo[i].Process.dwProcessId;
            info.Name = processInfo[i].strAppName;

            switch (processInfo[i].ApplicationType) {
            case RmExplorer:
              info.AppType = L"Explorer";
              break;
            case RmService:
              info.AppType = L"Service";
              break;
            case RmMainWindow:
              info.AppType = L"GUI Window";
              break;
            case RmOtherWindow:
              info.AppType = L"GUI Background";
              break;
            case RmConsole:
              info.AppType = L"Console App";
              break;
            case RmCritical:
              info.AppType = L"CRITICAL SYSTEM";
              break;
            default:
              info.AppType = L"Unknown";
              break;
            }
            lockers.push_back(info);
          }
        }
      }
    }
    RmEndSession(dwSession);
  }
  return lockers;
}

bool FileUnlocker::UnlockFile(const std::wstring &filePath,
                              std::vector<LockerInfo> &outLockers) {
  outLockers = GetLockingProcesses(filePath);
  if (outLockers.empty())
    return true;

  bool success = false;
  DWORD dwSession;
  WCHAR szSessionKey[CCH_RM_SESSION_KEY + 1] = {0};

  if (RmStartSession(&dwSession, 0, szSessionKey) == ERROR_SUCCESS) {
    LPCWSTR rgszResources[] = {filePath.c_str()};
    if (RmRegisterResources(dwSession, 1, rgszResources, 0, nullptr, 0,
                            nullptr) == ERROR_SUCCESS) {
      // Force shutdown - ignores prompt asking user to save documents
      if (RmShutdown(dwSession, RmForceShutdown, nullptr) == ERROR_SUCCESS) {
        success = true;
      }
    }
    RmEndSession(dwSession);
  }

  if (!success) {
    for (auto &lock : outLockers) {
      ForceKillProcess(lock.Pid);
    }
    success = true;
  }

  return success;
}

// =========================================================
// MODULE 3: HEURISTIC RISK ENGINE
// =========================================================

static bool CheckAutorunRegistryDir(HKEY root, const wchar_t *subKey,
                                    const std::wstring &path) {
  HKEY hKey;
  if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
    DWORD valuesCount, maxValueLen, maxDataLen;
    RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     &valuesCount, &maxValueLen, &maxDataLen, nullptr, nullptr);
    if (valuesCount > 0) {
      std::vector<wchar_t> valueName(maxValueLen + 1);
      std::vector<BYTE> data(maxDataLen + 1);
      for (DWORD i = 0; i < valuesCount; i++) {
        DWORD vLen = maxValueLen + 1;
        DWORD dLen = maxDataLen + 1;
        DWORD type;
        if (RegEnumValueW(hKey, i, valueName.data(), &vLen, nullptr, &type,
                          data.data(), &dLen) == ERROR_SUCCESS) {
          if (type == REG_SZ || type == REG_EXPAND_SZ) {
            std::wstring valStr((wchar_t *)data.data());
            // Case-insensitive search using std::search
            auto it =
                std::search(valStr.begin(), valStr.end(), path.begin(),
                            path.end(), [](wchar_t ch1, wchar_t ch2) {
                              return std::tolower(ch1) == std::tolower(ch2);
                            });
            if (it != valStr.end()) {
              RegCloseKey(hKey);
              return true;
            }
          }
        }
      }
    }
    RegCloseKey(hKey);
  }
  return false;
}

static BOOL CALLBACK EnumWindowsProcValidate(HWND hwnd, LPARAM lParam) {
  ProcessHeuristicData *data = (ProcessHeuristicData *)lParam;
  DWORD winPid = 0;
  GetWindowThreadProcessId(hwnd, &winPid);
  if (winPid == data->Pid) {
    if (IsWindowVisible(hwnd)) {
      data->IsWindowVisible = true;
      return FALSE; // Stop enumeration
    }
  }
  return TRUE;
}

bool CheckSignature(const std::wstring &path) {
  WINTRUST_FILE_INFO fileInfo = {sizeof(fileInfo), path.c_str(), nullptr,
                                 nullptr};
  WINTRUST_DATA winTrustData = {sizeof(winTrustData)};
  winTrustData.dwUIChoice = WTD_UI_NONE;
  winTrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
  winTrustData.dwUnionChoice = WTD_CHOICE_FILE;
  winTrustData.dwStateAction = WTD_STATEACTION_VERIFY;
  winTrustData.pFile = &fileInfo;

  GUID policyGUID = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG status = WinVerifyTrust(nullptr, &policyGUID, &winTrustData);

  winTrustData.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policyGUID, &winTrustData);

  return (status == ERROR_SUCCESS);
}

// Shannon entropy calculation for a data block
static float CalculateShannonEntropy(const BYTE *data, size_t size) {
  if (size == 0)
    return 0.0f;
  unsigned int freq[256] = {};
  for (size_t i = 0; i < size; i++)
    freq[data[i]]++;
  float entropy = 0.0f;
  for (int i = 0; i < 256; i++) {
    if (freq[i] > 0) {
      float p = (float)freq[i] / (float)size;
      entropy -= p * log2f(p);
    }
  }
  return entropy;
}

bool CheckPEFeatures(const std::wstring &path, bool &isPacked, bool &isDotNet,
                     bool &hasSuspiciousImports, float &maxEntropy) {
  isPacked = false;
  isDotNet = false;
  hasSuspiciousImports = false;
  maxEntropy = 0.0f;
  HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, 0, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    return false;

  DWORD fileSize = GetFileSize(hFile, nullptr);
  HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!hMap) {
    CloseHandle(hFile);
    return false;
  }

  LPVOID pBase = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
  if (!pBase) {
    CloseHandle(hMap);
    CloseHandle(hFile);
    return false;
  }

  bool safe = true;
  __try {
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)pBase;
    if (pDos->e_magic == IMAGE_DOS_SIGNATURE) {
      PIMAGE_NT_HEADERS pNt =
          (PIMAGE_NT_HEADERS)((BYTE *)pBase + pDos->e_lfanew);
      if (pNt->Signature == IMAGE_NT_SIGNATURE) {

        if (pNt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
          PIMAGE_NT_HEADERS32 pNt32 = (PIMAGE_NT_HEADERS32)pNt;
          if (pNt32->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]
                  .VirtualAddress != 0)
            isDotNet = true;
        } else if (pNt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
          PIMAGE_NT_HEADERS64 pNt64 = (PIMAGE_NT_HEADERS64)pNt;
          if (pNt64->OptionalHeader
                  .DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR]
                  .VirtualAddress != 0)
            isDotNet = true;
        }

        // Section analysis: packer detection + entropy
        PIMAGE_SECTION_HEADER pSec = IMAGE_FIRST_SECTION(pNt);
        for (int i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
          char name[9] = {0};
          memcpy(name, pSec[i].Name, 8);
          if (_stricmp(name, "upx0") == 0 || _stricmp(name, "upx1") == 0 ||
              _stricmp(name, ".aspack") == 0 || _stricmp(name, ".vmp0") == 0 ||
              _stricmp(name, ".themida") == 0) {
            isPacked = true;
          }
          // Shannon entropy per section
          if (pSec[i].SizeOfRawData > 0 &&
              pSec[i].PointerToRawData + pSec[i].SizeOfRawData <= fileSize) {
            const BYTE *secData =
                (const BYTE *)pBase + pSec[i].PointerToRawData;
            float ent = CalculateShannonEntropy(secData, pSec[i].SizeOfRawData);
            if (ent > maxEntropy)
              maxEntropy = ent;
          }
        }

        // Import Table analysis: check for suspicious API imports
        DWORD importRVA = 0;
        if (pNt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
          importRVA =
              ((PIMAGE_NT_HEADERS32)pNt)
                  ->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                  .VirtualAddress;
        } else {
          importRVA =
              ((PIMAGE_NT_HEADERS64)pNt)
                  ->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                  .VirtualAddress;
        }

        if (importRVA != 0) {
          // Convert RVA to file offset
          PIMAGE_SECTION_HEADER impSec = IMAGE_FIRST_SECTION(pNt);
          for (int i = 0; i < pNt->FileHeader.NumberOfSections; i++) {
            if (importRVA >= impSec[i].VirtualAddress &&
                importRVA <
                    impSec[i].VirtualAddress + impSec[i].SizeOfRawData) {
              DWORD offset = importRVA - impSec[i].VirtualAddress +
                             impSec[i].PointerToRawData;
              if (offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= fileSize) {
                PIMAGE_IMPORT_DESCRIPTOR pImport =
                    (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)pBase + offset);
                const char *suspiciousAPIs[] = {
                    "WriteProcessMemory", "CreateRemoteThread",
                    "VirtualAllocEx",     "NtWriteVirtualMemory",
                    "QueueUserAPC",       "SetWindowsHookEx"};

                while (pImport->Name != 0) {
                  DWORD nameOff = pImport->Name - impSec[i].VirtualAddress +
                                  impSec[i].PointerToRawData;
                  if (nameOff >= fileSize)
                    break;
                  // Check thunks for suspicious function names
                  DWORD thunkRVA = pImport->OriginalFirstThunk
                                       ? pImport->OriginalFirstThunk
                                       : pImport->FirstThunk;
                  if (thunkRVA != 0) {
                    DWORD thunkOff = thunkRVA - impSec[i].VirtualAddress +
                                     impSec[i].PointerToRawData;
                    if (thunkOff < fileSize) {
                      if (pNt->OptionalHeader.Magic ==
                          IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
                        PIMAGE_THUNK_DATA32 pThunk =
                            (PIMAGE_THUNK_DATA32)((BYTE *)pBase + thunkOff);
                        for (int t = 0;
                             t < 500 && pThunk->u1.AddressOfData != 0;
                             t++, pThunk++) {
                          if (!(pThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32)) {
                            DWORD hintOff = pThunk->u1.AddressOfData -
                                            impSec[i].VirtualAddress +
                                            impSec[i].PointerToRawData;
                            if (hintOff + 3 < fileSize) {
                              const char *funcName =
                                  (const char *)pBase + hintOff + 2;
                              for (const char *api : suspiciousAPIs) {
                                if (strncmp(funcName, api, strlen(api)) == 0) {
                                  hasSuspiciousImports = true;
                                  break;
                                }
                              }
                            }
                          }
                          if (hasSuspiciousImports)
                            break;
                        }
                      } else {
                        PIMAGE_THUNK_DATA64 pThunk =
                            (PIMAGE_THUNK_DATA64)((BYTE *)pBase + thunkOff);
                        for (int t = 0;
                             t < 500 && pThunk->u1.AddressOfData != 0;
                             t++, pThunk++) {
                          if (!(pThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                            DWORD hintOff = (DWORD)(pThunk->u1.AddressOfData) -
                                            impSec[i].VirtualAddress +
                                            impSec[i].PointerToRawData;
                            if (hintOff + 3 < fileSize) {
                              const char *funcName =
                                  (const char *)pBase + hintOff + 2;
                              for (const char *api : suspiciousAPIs) {
                                if (strncmp(funcName, api, strlen(api)) == 0) {
                                  hasSuspiciousImports = true;
                                  break;
                                }
                              }
                            }
                          }
                          if (hasSuspiciousImports)
                            break;
                        }
                      }
                    }
                  }
                  if (hasSuspiciousImports)
                    break;
                  pImport++;
                }
              }
              break;
            }
          }
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    safe = false;
  }

  UnmapViewOfFile(pBase);
  CloseHandle(hMap);
  CloseHandle(hFile);
  return safe;
}

bool CheckSuspiciousPrivileges(HANDLE hProc) {
  HANDLE hToken;
  if (!OpenProcessToken(hProc, TOKEN_QUERY, &hToken))
    return false;

  DWORD len = 0;
  GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &len);
  if (len == 0) {
    CloseHandle(hToken);
    return false;
  }

  TOKEN_PRIVILEGES *tp = (TOKEN_PRIVILEGES *)malloc(len);
  if (GetTokenInformation(hToken, TokenPrivileges, tp, len, &len)) {
    for (DWORD i = 0; i < tp->PrivilegeCount; i++) {
      LUID luidDebug;
      LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luidDebug);
      if (tp->Privileges[i].Luid.LowPart == luidDebug.LowPart &&
          tp->Privileges[i].Luid.HighPart == luidDebug.HighPart) {
        free(tp);
        CloseHandle(hToken);
        return true;
      }
    }
  }
  free(tp);
  CloseHandle(hToken);
  return false;
}

ProcessHeuristicData GatherProcessHeuristics(DWORD pid) {
  ProcessHeuristicData data = {};
  data.Pid = pid;

  if (pid == 0 || pid == 4)
    return data; // System/Idle

  HANDLE hProc =
      OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!hProc)
    hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

  if (hProc) {
    FILETIME ct, et, kt, ut;
    if (GetProcessTimes(hProc, &ct, &et, &kt, &ut)) {
      data.CreationTime =
          ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
      data.FullPath = path;
      size_t pos = data.FullPath.find_last_of(L"\\/");
      if (pos != std::wstring::npos)
        data.ImageName = data.FullPath.substr(pos + 1);
      else
        data.ImageName = data.FullPath;

      data.HasValidSignature = CheckSignature(data.FullPath);
      data.IsMicrosoftSigned = data.HasValidSignature;

      std::wstring wpath = data.FullPath;
      std::transform(wpath.begin(), wpath.end(), wpath.begin(), ::towlower);

      data.HasAutoRun =
          CheckAutorunRegistryDir(
              HKEY_LOCAL_MACHINE,
              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", wpath) ||
          CheckAutorunRegistryDir(
              HKEY_CURRENT_USER,
              L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", wpath) ||
          CheckAutorunRegistryDir(
              HKEY_CURRENT_USER,
              L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", wpath);

      data.RunFromTemp =
          (wpath.find(L"\\appdata\\local\\temp") != std::wstring::npos ||
           wpath.find(L"\\windows\\temp") != std::wstring::npos);
      data.RunFromAppData = (wpath.find(L"\\appdata\\") != std::wstring::npos &&
                             !data.RunFromTemp);
      data.RunFromDownloads =
          (wpath.find(L"\\downloads\\") != std::wstring::npos);
      data.RunFromProgramData =
          (wpath.find(L"\\programdata\\") != std::wstring::npos);

      size_t lastDot = wpath.find_last_of(L".");
      if (lastDot != std::wstring::npos && lastDot > 0) {
        size_t prevDot = wpath.find_last_of(L".", lastDot - 1);
        if (prevDot != std::wstring::npos) {
          std::wstring ext1 = wpath.substr(prevDot + 1, lastDot - prevDot - 1);
          if (ext1 == L"txt" || ext1 == L"jpg" || ext1 == L"png" ||
              ext1 == L"pdf" || ext1 == L"doc") {
            data.HasDoubleExtension = true;
          }
        }
      }

      CheckPEFeatures(data.FullPath, data.IsPacked, data.IsDotNet,
                      data.HasSuspiciousImports, data.MaxSectionEntropy);

      // Zone.Identifier check (file downloaded from internet)
      std::wstring zoneStream = data.FullPath + L":Zone.Identifier";
      HANDLE hZone =
          CreateFileW(zoneStream.c_str(), GENERIC_READ, FILE_SHARE_READ,
                      nullptr, OPEN_EXISTING, 0, nullptr);
      if (hZone != INVALID_HANDLE_VALUE) {
        data.HasZoneIdentifier = true;
        CloseHandle(hZone);
      }

      // File age (hours since creation)
      WIN32_FILE_ATTRIBUTE_DATA fad = {};
      if (GetFileAttributesExW(data.FullPath.c_str(), GetFileExInfoStandard,
                               &fad)) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        uint64_t nowTicks =
            ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
        uint64_t createTicks =
            ((uint64_t)fad.ftCreationTime.dwHighDateTime << 32) |
            fad.ftCreationTime.dwLowDateTime;
        if (nowTicks > createTicks) {
          data.FileAge =
              (nowTicks - createTicks) / 36000000000ULL; // 100ns ticks to hours
        }
      }
    }

    data.HasSuspiciousPrivileges = CheckSuspiciousPrivileges(hProc);

    BOOL isWow64 = FALSE;
    if (IsWow64Process(hProc, &isWow64) && isWow64) {
      std::wstring lname = data.ImageName;
      std::transform(lname.begin(), lname.end(), lname.begin(), ::towlower);
      if (lname == L"lsass.exe" || lname == L"csrss.exe" ||
          lname == L"smss.exe" || lname == L"services.exe") {
        data.ArchMismatch = true;
      }
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W pe = {sizeof(pe)};
      if (Process32FirstW(hSnap, &pe)) {
        do {
          if (pe.th32ProcessID == pid) {
            DWORD parentId = pe.th32ParentProcessID;
            HANDLE hParent =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentId);
            if (hParent) {
              wchar_t ppath[MAX_PATH] = {};
              DWORD psz = MAX_PATH;
              if (QueryFullProcessImageNameW(hParent, 0, ppath, &psz)) {
                std::wstring wpname = ppath;
                size_t ppos = wpname.find_last_of(L"\\/");
                if (ppos != std::wstring::npos)
                  wpname = wpname.substr(ppos + 1);
                std::transform(wpname.begin(), wpname.end(), wpname.begin(),
                               ::towlower);

                std::wstring myname = data.ImageName;
                std::transform(myname.begin(), myname.end(), myname.begin(),
                               ::towlower);

                if (myname == L"svchost.exe" && wpname != L"services.exe")
                  data.ParentMismatch = true;
                if (myname == L"cmd.exe" &&
                    (wpname == L"word.exe" || wpname == L"excel.exe" ||
                     wpname == L"winword.exe"))
                  data.ParentMismatch = true;
              }
              CloseHandle(hParent);
            }
            break;
          }
        } while (Process32NextW(hSnap, &pe));
      }
      CloseHandle(hSnap);
    }

    // CPU Usage calculation is removed from here for performance during
    // snapshot caching.
    CloseHandle(hProc);
  }

  EnumWindows(EnumWindowsProcValidate, (LPARAM)&data);
  return data;
}

RiskResult CalculateRiskScore(const ProcessHeuristicData &data) {
  RiskResult res = {0, {}};
  int maxPossible = 0; // Track max possible score for normalization

  // Rule weights (used for normalization)
  auto addRule = [&](bool condition, int weight, const char *reason) {
    maxPossible += (weight > 0 ? weight : 0);
    if (condition) {
      res.Score += weight;
      res.Reasons.push_back(reason);
    }
  };

  addRule(!data.IsMicrosoftSigned, 25, u8"[+25] Нет цифровой подписи");
  addRule(data.IsPacked, 25, u8"[+25] Файл упакован");
  addRule(data.RunFromTemp, 40, u8"[+40] Запуск из %TEMP%");
  if (!data.RunFromTemp) {
    addRule(data.RunFromAppData, 15, u8"[+15] Запуск из %APPDATA%");
    if (!data.RunFromAppData) {
      addRule(data.RunFromProgramData, 15, u8"[+15] Запуск из %ProgramData%");
    }
  }
  addRule(data.HasDoubleExtension, 50, u8"[+50] Двойное расширение");
  addRule(data.ParentMismatch, 60, u8"[+60] Аномальный родитель");
  addRule(data.ArchMismatch, 70,
          u8"[+70] Системный процесс запущен как 32-битный");
  addRule(!data.IsWindowVisible && data.RunFromAppData, 15,
          u8"[+15] Скрытый процесс из папки пользователя");
  addRule(data.HasAutoRun && !data.IsMicrosoftSigned, 25,
          u8"[+25] Неподписанная автозагрузка");
  addRule(data.HasSuspiciousPrivileges && !data.IsMicrosoftSigned, 20,
          u8"[+20] Требует права отладки без подписи");
  addRule(data.IsDotNet && !data.IsMicrosoftSigned && data.RunFromAppData, 20,
          u8"[+20] .NET программа из AppData");
  addRule(data.RunFromDownloads && !data.IsMicrosoftSigned, 15,
          u8"[+15] Запуск из папки Загрузки");

  // v2.3: New rules — Import Table, Entropy, Zone.Identifier, File Age
  addRule(data.HasSuspiciousImports && !data.IsMicrosoftSigned, 30,
          u8"[+30] Подозрительные импорты");
  addRule(data.MaxSectionEntropy > 7.0f, 35, u8"[+35] Высокая энтропия секции");
  addRule(data.HasZoneIdentifier && !data.IsMicrosoftSigned, 10,
          u8"[+10] Файл скачан из интернета");

  // Negative rule: old resident files are less suspicious
  if (data.FileAge > 720 && data.HasValidSignature) { // >30 days + signed
    res.Score -= 15;
    res.Reasons.push_back(u8"[-15] Давно установленный подписанный файл");
  } else if (data.FileAge > 720) {
    res.Score -= 10;
    res.Reasons.push_back(u8"[-10] Файл старше 30 дней");
  }

  // Typosquatting detection (Levenshtein distance — v2.5: O(n) space single-row
  // DP)
  auto levDist = [](const std::wstring &s1, const std::wstring &s2) {
    size_t m = s1.size(), n = s2.size();
    std::vector<int> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; j++)
      prev[j] = (int)j;
    for (size_t i = 1; i <= m; i++) {
      curr[0] = (int)i;
      for (size_t j = 1; j <= n; j++) {
        if (s1[i - 1] == s2[j - 1])
          curr[j] = prev[j - 1];
        else
          curr[j] = 1 + std::min({prev[j], curr[j - 1], prev[j - 1]});
      }
      std::swap(prev, curr);
    }
    return prev[n];
  };

  std::vector<std::wstring> sys = {L"svchost.exe",  L"lsass.exe",
                                   L"csrss.exe",    L"explorer.exe",
                                   L"winlogon.exe", L"taskmgr.exe"};
  std::wstring lname = data.ImageName;
  std::transform(lname.begin(), lname.end(), lname.begin(), ::towlower);

  for (const auto &s : sys) {
    if (lname != s && levDist(lname, s) <= 2) {
      res.Score += 80;
      maxPossible += 80;
      char buf[128];
      snprintf(buf, sizeof(buf), "[+80] Имя '%ws' маскируется под системное",
               lname.c_str());
      res.Reasons.push_back(buf);
      break;
    } else if (lname == s) {
      std::wstring pathL = data.FullPath;
      std::transform(pathL.begin(), pathL.end(), pathL.begin(), ::towlower);
      if (pathL.find(L"\\windows\\system32\\") == std::wstring::npos &&
          pathL.find(L"\\windows\\syswow64\\") == std::wstring::npos &&
          pathL.find(L"\\windows\\explorer.exe") == std::wstring::npos) {
        res.Score += 40;
        maxPossible += 40;
        res.Reasons.push_back(
            u8"[+40] Системное имя, но из подозрительной папки");
        break;
      }
    }
  }

  // Weighted normalization: cap at 100 with proportional scaling
  if (res.Score < 0)
    res.Score = 0;
  if (res.Score > 100)
    res.Score = 100;
  return res;
}

// =========================================================
// MODULE 4: OVERLAY HUNTER MODE
// =========================================================

static HWND g_hOverlayWindow = nullptr;

LRESULT CALLBACK StaticOverlayProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    // Ничего не рисуем на WM_PAINT, мы рисуем динамически в DrawOverlayRect
    EndPaint(hwnd, &ps);
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

void HunterMode::InitializeOverlay() {
  if (g_hOverlayWindow)
    return;

  WNDCLASS wc = {0};
  wc.lpfnWndProc = StaticOverlayProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"ZaslonHunterOverlay";
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClass(&wc);

  int sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  if (sw == 0) {
    sx = 0;
    sy = 0;
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
  }

  // WS_EX_TOOLWINDOW: Невидимо в панели задач
  // WS_EX_TOPMOST: Поверх всего
  // WS_EX_TRANSPARENT: Прозрачно для кликов мыши!
  // WS_EX_LAYERED: Прозрачность окна
  g_hOverlayWindow = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
      L"ZaslonHunterOverlay", L"", WS_POPUP | WS_VISIBLE, sx, sy, sw, sh,
      nullptr, nullptr, wc.hInstance, nullptr);

  // Устанавливаем ключевой цвет, который будет прозрачным (например RGB(1,1,1))
  SetLayeredWindowAttributes(g_hOverlayWindow, RGB(1, 1, 1), 0, LWA_COLORKEY);
}

void HunterMode::ShutdownOverlay() {
  if (g_hOverlayWindow) {
    DestroyWindow(g_hOverlayWindow);
    g_hOverlayWindow = nullptr;
    UnregisterClass(L"ZaslonHunterOverlay", GetModuleHandle(nullptr));
  }
}

HunterTarget HunterMode::GetTargetFromPoint(POINT pt) {
  HunterTarget target = {nullptr, 0, L"", {0}};

  HWND hWnd = WindowFromPoint(pt);
  if (!hWnd)
    return target;

  // Фильтрация собственного оверлея или UI
  DWORD targetPid = 0;
  GetWindowThreadProcessId(hWnd, &targetPid);
  if (targetPid == GetCurrentProcessId())
    return target;

  // Исключаем невидимые окна и получаем видимого родителя,
  // чтобы рамка не улетала на невидимый GA_ROOT
  HWND hVisible = hWnd;
  while (hVisible && !IsWindowVisible(hVisible)) {
    hVisible = GetParent(hVisible);
  }
  if (hVisible)
    hWnd = hVisible;

  target.Hwnd = hWnd;
  GetWindowThreadProcessId(hWnd, &target.Pid); // Pid главной формы
  GetWindowRect(hWnd, &target.Bounds);

  // Получаем Имя
  HANDLE hProc =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.Pid);
  if (hProc) {
    wchar_t path[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
      std::wstring wpath(path);
      size_t pos = wpath.find_last_of(L"\\/");
      if (pos != std::wstring::npos) {
        target.ImageName = wpath.substr(pos + 1);
      } else {
        target.ImageName = wpath;
      }
    }
    CloseHandle(hProc);
  }

  return target;
}

void HunterMode::DrawOverlayRect(const RECT &rect) {
  if (!g_hOverlayWindow)
    return;

  // Получаем DC нашего оверлея
  HDC hdc = GetDC(g_hOverlayWindow);
  if (!hdc)
    return;

  // Перерисовываем ВСЁ прозрачным цветом (очистка предыдущего кадра)
  RECT clientRect;
  GetClientRect(g_hOverlayWindow, &clientRect);
  HBRUSH clearBrush = CreateSolidBrush(RGB(1, 1, 1));
  FillRect(hdc, &clientRect, clearBrush);
  DeleteObject(clearBrush);

  // Если есть валидный rect, рисуем ЖИРНУЮ красную рамку с отступом
  if (rect.right > rect.left && rect.bottom > rect.top) {
    HPEN hPen = CreatePen(PS_SOLID, 5, RGB(255, 50, 50));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(
        hdc, GetStockObject(NULL_BRUSH)); // Внутренность рамки прозрачная!

    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);

    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
  }

  ReleaseDC(g_hOverlayWindow, hdc);
}

// =========================================================
// MODULE 5: PROCESS INSTALL MONITOR (Sandbox Analyzer)
// =========================================================

namespace SandboxRealTime {
std::atomic<bool> g_IsTracking(false);
std::mutex g_ResultMutex;
SandboxDiffResult g_CurrentDiff;
std::vector<std::thread> g_WatchThreads;
uintmax_t g_OriginalSize = 0;
std::wstring g_OriginalFile = L"";

void WatchDirectoryThread(std::wstring path) {
  HANDLE hDir =
      CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);

  if (hDir == INVALID_HANDLE_VALUE)
    return;

  std::vector<BYTE> buffer(65536);
  DWORD bytesReturned;

  while (g_IsTracking) {
    if (ReadDirectoryChangesW(hDir, buffer.data(), buffer.size(), TRUE,
                              FILE_NOTIFY_CHANGE_FILE_NAME |
                                  FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_SIZE,
                              &bytesReturned, NULL, NULL)) {

      if (!g_IsTracking)
        break;

      FILE_NOTIFY_INFORMATION *fni = (FILE_NOTIFY_INFORMATION *)buffer.data();
      while (true) {
        if (fni->Action == FILE_ACTION_ADDED ||
            fni->Action == FILE_ACTION_MODIFIED) {
          std::wstring fileName(fni->FileName,
                                fni->FileNameLength / sizeof(WCHAR));
          std::wstring fullPath = path + L"\\" + fileName;

          std::wstring lowerFile = fullPath;
          std::transform(lowerFile.begin(), lowerFile.end(), lowerFile.begin(),
                         ::towlower);

          std::lock_guard<std::mutex> lock(g_ResultMutex);

          // Avoid duplicates
          bool exists = false;
          for (const auto &f : g_CurrentDiff.AddedFiles) {
            if (_wcsicmp(f.c_str(), fullPath.c_str()) == 0) {
              exists = true;
              break;
            }
          }

          if (!exists) {
            g_CurrentDiff.AddedFiles.push_back(fullPath);

            // Tracing Analysis
            if (lowerFile.find(L"\\appdata\\roaming\\telegram") !=
                    std::wstring::npos ||
                lowerFile.find(L"\\appdata\\local\\google\\chrome") !=
                    std::wstring::npos ||
                lowerFile.find(L"\\appdata\\roaming\\discord") !=
                    std::wstring::npos) {
              g_CurrentDiff.TracingFiles.push_back(fullPath);
            }

            // Suspicious Copy
            if (g_OriginalSize > 0) {
              std::error_code ec;
              uintmax_t curSize = fs::file_size(fullPath, ec);
              if (!ec && curSize == g_OriginalSize &&
                  _wcsicmp(fullPath.c_str(), g_OriginalFile.c_str()) != 0) {
                g_CurrentDiff.SuspiciousCopies.push_back(fullPath);
              }
            }
          }
        }
        if (fni->NextEntryOffset == 0)
          break;
        fni = (FILE_NOTIFY_INFORMATION *)((BYTE *)fni + fni->NextEntryOffset);
      }
    } else {
      break; // Error or handle closed
    }
  }
  CloseHandle(hDir);
}

void WatchRegistryThread(HKEY root, std::wstring subKeyStr,
                         std::wstring displayName) {
  HKEY hKey;
  if (RegOpenKeyExW(root, subKeyStr.c_str(), 0, KEY_NOTIFY | KEY_READ, &hKey) !=
      ERROR_SUCCESS)
    return;

  HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!hEvent) {
    RegCloseKey(hKey);
    return;
  }

  while (g_IsTracking) {
    if (RegNotifyChangeKeyValue(
            hKey, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
            hEvent, TRUE) == ERROR_SUCCESS) {
      if (WaitForSingleObject(hEvent, 1000) == WAIT_OBJECT_0) {
        if (!g_IsTracking)
          break;

        std::lock_guard<std::mutex> lock(g_ResultMutex);
        // Since it's hard to get the exact added value without diffing, we just
        // record that the key was modified. For a true sandbox, we just flag
        // the root key that was touched.
        std::wstring record = displayName + L" -> Modified";
        bool exists = false;
        for (const auto &k : g_CurrentDiff.AddedRegistryKeys) {
          if (k == record) {
            exists = true;
            break;
          }
        }
        if (!exists) {
          g_CurrentDiff.AddedRegistryKeys.push_back(record);
        }
        ResetEvent(hEvent);
      }
    } else {
      break;
    }
  }
  CloseHandle(hEvent);
  RegCloseKey(hKey);
}
} // namespace SandboxRealTime

void SandboxAnalyzer::StartRealTimeTracking() {
  SandboxRealTime::g_IsTracking = true;
  SandboxRealTime::g_CurrentDiff = SandboxDiffResult();
  SandboxRealTime::g_OriginalSize = 0;
  SandboxRealTime::g_OriginalFile = L"";

  wchar_t appData[MAX_PATH], localAppData[MAX_PATH], temp[MAX_PATH],
      startup[MAX_PATH], progFiles[MAX_PATH], progData[MAX_PATH];
  GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
  GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
  GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH);
  ExpandEnvironmentStringsW(
      L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", startup,
      MAX_PATH);
  GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH);
  GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH);

  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchDirectoryThread, std::wstring(appData)));
  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchDirectoryThread, std::wstring(localAppData)));
  SandboxRealTime::g_WatchThreads.emplace_back(
      std::thread(SandboxRealTime::WatchDirectoryThread, std::wstring(temp)));
  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchDirectoryThread, std::wstring(startup)));
  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchDirectoryThread, std::wstring(progFiles)));
  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchDirectoryThread, std::wstring(progData)));

  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchRegistryThread, HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKCU\\...\\Run"));
  SandboxRealTime::g_WatchThreads.emplace_back(
      std::thread(SandboxRealTime::WatchRegistryThread, HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                  L"HKCU\\...\\RunOnce"));
  SandboxRealTime::g_WatchThreads.emplace_back(std::thread(
      SandboxRealTime::WatchRegistryThread, HKEY_LOCAL_MACHINE,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKLM\\...\\Run"));
  SandboxRealTime::g_WatchThreads.emplace_back(
      std::thread(SandboxRealTime::WatchRegistryThread, HKEY_LOCAL_MACHINE,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                  L"HKLM\\...\\RunOnce"));
}

void SandboxAnalyzer::StopRealTimeTracking(const std::wstring &originalFile) {
  SandboxRealTime::g_IsTracking = false;

  // In a real scenario we might need to send a dummy file event to wake up
  // ReadDirectoryChangesW, but we can just detach and let them die via the
  // atomic flag naturally when next event occurs or process ends.
  for (auto &t : SandboxRealTime::g_WatchThreads) {
    if (t.joinable())
      t.detach();
  }
  SandboxRealTime::g_WatchThreads.clear();

  std::lock_guard<std::mutex> lock(SandboxRealTime::g_ResultMutex);

  // Post process original file info if provided
  if (!originalFile.empty()) {
    std::error_code ec;
    if (fs::exists(originalFile, ec)) {
      uintmax_t origSize = fs::file_size(originalFile, ec);
      for (const auto &wfile : SandboxRealTime::g_CurrentDiff.AddedFiles) {
        uintmax_t curSize = fs::file_size(wfile, ec);
        if (!ec && curSize == origSize &&
            _wcsicmp(wfile.c_str(), originalFile.c_str()) != 0) {
          SandboxRealTime::g_CurrentDiff.SuspiciousCopies.push_back(wfile);
        }
      }
    }
  }
}

SandboxDiffResult SandboxAnalyzer::GetRealTimeDiff() {
  std::lock_guard<std::mutex> lock(SandboxRealTime::g_ResultMutex);
  return SandboxRealTime::g_CurrentDiff;
}

void SandboxAnalyzer::Rollback(const SandboxDiffResult &diff) {
  // 1. Kill Spawned Processes
  for (const auto &proc : diff.SpawnedProcesses) {
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, proc.Pid);
    if (hProc) {
      TerminateProcess(hProc, 0);
      CloseHandle(hProc);
    }
  }

  // 2. Delete Registry Keys
  for (const auto &wkey : diff.AddedRegistryKeys) {
    HKEY hRoot = HKEY_CURRENT_USER;
    std::wstring subKey = wkey;
    if (wkey.find(L"HKLM\\") == 0) {
      hRoot = HKEY_LOCAL_MACHINE;
      subKey = wkey.substr(5);
    } else if (wkey.find(L"HKCU\\") == 0) {
      hRoot = HKEY_CURRENT_USER;
      subKey = wkey.substr(5);
    }

    // Just attempt to delete the exact target subkey (which we tracked
    // recursively) Note: RegDeleteKeyW will fail if the key has subkeys, which
    // is safer.
    RegDeleteKeyW(hRoot, subKey.c_str());
  }

  // 3. Delete Files & Folders
  // Reverse sort so we delete files before attempting to delete their parent
  // directories
  std::vector<std::wstring> filesToDel = diff.AddedFiles;
  std::sort(filesToDel.rbegin(), filesToDel.rend());

  for (const auto &wfile : filesToDel) {
    DWORD attrs = GetFileAttributesW(wfile.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
      continue; // File no longer exists
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
      RemoveDirectoryW(wfile.c_str());
    } else {
      SetFileAttributesW(wfile.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(wfile.c_str());
    }
  }
}

static std::wstring GetProcessCommandLine(HANDLE hProc) {
  HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
  pfnNtQueryInformationProcess NtQueryInfo =
      (pfnNtQueryInformationProcess)GetProcAddress(hNtDll,
                                                   "NtQueryInformationProcess");
  if (!NtQueryInfo)
    return L"";

  ULONG len = 0;
  NtQueryInfo(hProc, 60, nullptr, 0,
              &len); // ProcessCommandLineInformation = 60
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

void SandboxAnalyzer::TrackProcesses(
    DWORD targetPid, std::vector<SandboxSpawnedProcess> &outSpawned) {
  HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnap == INVALID_HANDLE_VALUE)
    return;

  std::unordered_map<DWORD, DWORD> parentMap;
  std::unordered_map<DWORD, std::wstring> nameMap;

  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(hSnap, &pe)) {
    do {
      parentMap[pe.th32ProcessID] = pe.th32ParentProcessID;
      nameMap[pe.th32ProcessID] = pe.szExeFile;
    } while (Process32NextW(hSnap, &pe));
  }
  CloseHandle(hSnap);

  // Find all descendants of targetPid
  std::vector<DWORD> toCheck;
  toCheck.push_back(targetPid);
  std::vector<DWORD> descendants;

  while (!toCheck.empty()) {
    DWORD current = toCheck.back();
    toCheck.pop_back();

    for (auto const &[pid, ppid] : parentMap) {
      if (ppid == current) {
        descendants.push_back(pid);
        toCheck.push_back(pid);
      }
    }
  }

  for (DWORD pid : descendants) {
    // Avoid duplicates
    bool exists = false;
    for (const auto &p : outSpawned) {
      if (p.Pid == pid) {
        exists = true;
        break;
      }
    }
    if (exists)
      continue;

    SandboxSpawnedProcess sp;
    sp.Pid = pid;
    sp.ImageName = nameMap[pid];
    sp.FullPath = L"";

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
      wchar_t path[MAX_PATH];
      DWORD size = MAX_PATH;
      if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
        sp.FullPath = path;
      }
      sp.CommandLine = GetProcessCommandLine(hProc);
      CloseHandle(hProc);
    }

    outSpawned.push_back(sp);
  }
}

// =========================================================
// MODULE 6: ANTI-WINLOCKER (Isolated Desktop)
// =========================================================

static std::atomic<bool> g_IsGuardianActive(false);
static std::thread g_GuardianThread;

static bool IsSuspiciousLocker(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;

  HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!hProc)
    return false;

  wchar_t path[MAX_PATH];
  DWORD size = MAX_PATH;
  if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
    std::wstring p = path;
    for (auto &c : p)
      c = towlower(c);

    // False positive protection: Don't trigger on browser, explorer, or system
    // stuff
    if (p.find(L"\\windows\\") != std::wstring::npos) {
      CloseHandle(hProc);
      return false;
    }
    if (p.find(L"chrome.exe") != std::wstring::npos ||
        p.find(L"msedge.exe") != std::wstring::npos) {
      CloseHandle(hProc);
      return false;
    }

    // Suspicious paths
    if (p.find(L"\\temp\\") != std::wstring::npos ||
        p.find(L"\\appdata\\") != std::wstring::npos) {
      CloseHandle(hProc);
      return true;
    }
  }
  CloseHandle(hProc);
  return false;
}

void AntiWinLocker::StartWinLockerGuardian() {
  if (g_IsGuardianActive)
    return;
  g_IsGuardianActive = true;

  g_GuardianThread = std::thread([]() {
    while (g_IsGuardianActive) {
      HWND hLocker = GetTopWindow(NULL);
      if (hLocker && IsWindowVisible(hLocker)) {
        LONG exStyle = GetWindowLong(hLocker, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOPMOST) {
          RECT rc;
          GetWindowRect(hLocker, &rc);
          int w = rc.right - rc.left;
          int h = rc.bottom - rc.top;

          // If it covers the whole screen (assumed primary for now)
          if (w >= GetSystemMetrics(SM_CXSCREEN) &&
              h >= GetSystemMetrics(SM_CYSCREEN)) {
            if (IsSuspiciousLocker(hLocker)) {
              // AUTO-TRIGGER SECURE DESKTOP
              LaunchIsolatedDesktop(L"cmd.exe");
            }
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  });
  g_GuardianThread.detach();
}

void AntiWinLocker::StopWinLockerGuardian() { g_IsGuardianActive = false; }

static bool LaunchOnDesktop(const std::wstring &cmdLine,
                            const std::wstring &desktopName,
                            HANDLE *phProcess) {
  STARTUPINFOW si = {sizeof(si)};
  si.lpDesktop = (LPWSTR)desktopName.c_str();
  PROCESS_INFORMATION pi = {0};
  std::wstring writableCmd = cmdLine;
  if (CreateProcessW(NULL, &writableCmd[0], NULL, NULL, FALSE, 0, NULL, NULL,
                     &si, &pi)) {
    if (phProcess)
      *phProcess = pi.hProcess;
    else
      CloseHandle(pi.hProcess);
    if (pi.hThread)
      CloseHandle(pi.hThread);
    return true;
  }
  return false;
}

bool AntiWinLocker::LaunchIsolatedDesktop(const std::wstring &processToLaunch) {
  HDESK hOriginalDesktop = GetThreadDesktop(GetCurrentThreadId());
  if (!hOriginalDesktop) {
    hOriginalDesktop = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);
  }

  // Create the new pristine desktop
  HDESK hNewDesktop =
      CreateDesktopW(L"ZaslonSecureDesktop", NULL, NULL, 0, GENERIC_ALL, NULL);

  if (!hNewDesktop)
    return false;

  // Switch immediately
  SwitchDesktop(hNewDesktop);

  std::wstring desktopName = L"ZaslonSecureDesktop";
  // Launch Recovery Tools
  HANDLE hMainProc = NULL;

  // 1. Launch CMD
  LaunchOnDesktop(L"cmd.exe", desktopName, NULL);

  // 2. Launch Task Manager
  LaunchOnDesktop(L"taskmgr.exe", desktopName, NULL);

  // 3. Launch ZASLON itself
  wchar_t zaslonPath[MAX_PATH];
  if (GetModuleFileNameW(NULL, zaslonPath, MAX_PATH)) {
    LaunchOnDesktop(zaslonPath, desktopName, &hMainProc);
  }

  // Also launch the originally requested process if it's different and not CMD
  if (!processToLaunch.empty() && processToLaunch != L"cmd.exe") {
    LaunchOnDesktop(processToLaunch, desktopName,
                    (hMainProc == NULL ? &hMainProc : NULL));
  }

  bool success = (hMainProc != NULL);

  if (success) {
    // Block and wait for the user to close the secure process
    WaitForSingleObject(hMainProc, INFINITE);
    CloseHandle(hMainProc);
  }

  // Restore the user back to the normal desktop when done
  SwitchDesktop(hOriginalDesktop);
  CloseDesktop(hNewDesktop);

  return success;
}

} // namespace ZaslonCore

#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>

namespace ZaslonUltimate {

// =========================================================
// MODULE 1: PRIVILEGE ESCALATION (TOKEN STEALING)
// =========================================================

HANDLE TokenStealer::GetSystemToken() {
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot == INVALID_HANDLE_VALUE)
    return nullptr;

  PROCESSENTRY32W pe = {sizeof(pe)};
  DWORD targetPid = 0;
  if (Process32FirstW(hSnapshot, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0 ||
          _wcsicmp(pe.szExeFile, L"lsass.exe") == 0) {
        targetPid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(hSnapshot, &pe));
  }
  CloseHandle(hSnapshot);

  if (targetPid == 0)
    return nullptr;

  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, targetPid);
  if (!hProcess)
    return nullptr;

  HANDLE hToken = nullptr;
  if (!OpenProcessToken(hProcess,
                        TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
                        &hToken)) {
    CloseHandle(hProcess);
    return nullptr;
  }

  HANDLE hDupToken = nullptr;
  DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                   TokenPrimary, &hDupToken);
  CloseHandle(hToken);
  CloseHandle(hProcess);

  if (hDupToken) {
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 2;
    LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &tp.Privileges[1].Luid);
    tp.Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hDupToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  }

  return hDupToken;
}

bool TokenStealer::RunAsTrustedInstaller(std::wstring cmdLine) {
  HANDLE hToken = GetSystemToken();
  if (!hToken)
    return false;

  // Текущий процесс ZASLON должно иметь привилегию SE_IMPERSONATE_NAME для
  // CreateProcessWithTokenW
  HANDLE hCurrentToken;
  if (OpenProcessToken(GetCurrentProcess(),
                       TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hCurrentToken)) {
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    LookupPrivilegeValueW(nullptr, L"SeImpersonatePrivilege",
                          &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hCurrentToken, FALSE, &tp, sizeof(tp), nullptr,
                          nullptr);
    CloseHandle(hCurrentToken);
  }

  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {0};

  std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
  cmdBuffer.push_back(L'\0');

  bool success = CreateProcessWithTokenW(hToken, LOGON_WITH_PROFILE, nullptr,
                                         cmdBuffer.data(), CREATE_NEW_CONSOLE,
                                         nullptr, nullptr, &si, &pi);

  if (success) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  CloseHandle(hToken);
  return success;
}

// =========================================================
// MODULE 3: NETWORK RADAR (KILL CONNECTIONS)
// =========================================================

std::vector<ConnectionInfo> NetworkManager::GetActiveConnections() {
  std::vector<ConnectionInfo> connections;
  DWORD size = 0;
  GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL,
                      0);

  if (size == 0)
    return connections;

  std::vector<BYTE> buffer(size);
  if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET,
                          TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
    PMIB_TCPTABLE_OWNER_PID pTable =
        reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    for (DWORD i = 0; i < pTable->dwNumEntries; i++) {
      MIB_TCPROW_OWNER_PID &row = pTable->table[i];
      ConnectionInfo info;
      info.Pid = row.dwOwningPid;

      wchar_t local[32] = {};
      swprintf_s(local, 32, L"%d.%d.%d.%d", row.dwLocalAddr & 0xFF,
                 (row.dwLocalAddr >> 8) & 0xFF, (row.dwLocalAddr >> 16) & 0xFF,
                 (row.dwLocalAddr >> 24) & 0xFF);
      info.LocalIP = local;

      wchar_t remote[32] = {};
      swprintf_s(remote, 32, L"%d.%d.%d.%d", row.dwRemoteAddr & 0xFF,
                 (row.dwRemoteAddr >> 8) & 0xFF,
                 (row.dwRemoteAddr >> 16) & 0xFF,
                 (row.dwRemoteAddr >> 24) & 0xFF);
      info.RemoteIP = remote;

      info.RemotePort = (row.dwRemotePort >> 8) |
                        ((row.dwRemotePort & 0xFF) << 8); // ntohs emulation
      info.State = row.dwState;
      // Copy binary-compatible struct
      static_assert(sizeof(TcpRowOwnerPid) == sizeof(MIB_TCPROW_OWNER_PID),
                    "struct size mismatch");
      memcpy(&info.RowData, &row, sizeof(TcpRowOwnerPid));

      connections.push_back(info);
    }
  }
  return connections;
}

bool NetworkManager::CloseConnection(TcpRowOwnerPid row) {
  MIB_TCPROW r = {0};
  r.dwState = MIB_TCP_STATE_DELETE_TCB;
  r.dwLocalAddr = row.dwLocalAddr;
  r.dwLocalPort = row.dwLocalPort;
  r.dwRemoteAddr = row.dwRemoteAddr;
  r.dwRemotePort = row.dwRemotePort;
  return (SetTcpEntry(&r) == NO_ERROR);
}

// =========================================================
// MODULE 4: OFFLINE PERMISSION RESET (ACL EDITOR)
// =========================================================

bool ForceTakeOwnership(std::wstring registryKeyPath) {
  // 1. Включаем критические привилегии
  HANDLE hToken = nullptr;
  if (OpenProcessToken(GetCurrentProcess(),
                       TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
    // To hold 2 privileges, we need to allocate memory or use a customized
    // struct
    struct {
      DWORD PrivilegeCount;
      LUID_AND_ATTRIBUTES Privileges[2];
    } tp;
    tp.PrivilegeCount = 2;
    LookupPrivilegeValueW(nullptr, L"SeTakeOwnershipPrivilege",
                          &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    LookupPrivilegeValueW(nullptr, L"SeRestorePrivilege",
                          &tp.Privileges[1].Luid);
    tp.Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, FALSE, (PTOKEN_PRIVILEGES)&tp, sizeof(tp),
                          nullptr, nullptr);
    CloseHandle(hToken);
  }

  // 2. Форматируем путь реестра (SE_REGISTRY_KEY требует пути без HKEY_
  // префиксов)
  std::wstring formattedPath = registryKeyPath;
  if (formattedPath.find(L"HKEY_LOCAL_MACHINE\\") == 0) {
    formattedPath.replace(0, 19, L"MACHINE\\");
  } else if (formattedPath.find(L"HKEY_CURRENT_USER\\") == 0) {
    formattedPath.replace(0, 18, L"CURRENT_USER\\");
  } else if (formattedPath.find(L"HKEY_USERS\\") == 0) {
    formattedPath.replace(0, 11, L"USERS\\");
  } else if (formattedPath.find(L"HKEY_CLASSES_ROOT\\") == 0) {
    formattedPath.replace(0, 18, L"CLASSES_ROOT\\");
  }

  // 3. Получаем SID группы Administrators
  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
  PSID pAdminSid = nullptr;
  AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                           DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                           &pAdminSid);

  // Меняем владельца объекта на Administrators
  DWORD res = SetNamedSecurityInfoW((LPWSTR)formattedPath.c_str(),
                                    SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION,
                                    pAdminSid, nullptr, nullptr, nullptr);

  if (res != ERROR_SUCCESS) {
    FreeSid(pAdminSid);
    return false;
  }

  // 4. Даем полный доступ всем администраторам (DACL = KEY_ALL_ACCESS)
  EXPLICIT_ACCESSW ea = {0};
  ea.grfAccessPermissions = KEY_ALL_ACCESS;
  ea.grfAccessMode = SET_ACCESS;
  ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea.Trustee.ptstrName = (LPWSTR)pAdminSid;

  PACL pNewDACL = nullptr;
  res = SetEntriesInAclW(1, &ea, nullptr, &pNewDACL);
  if (res == ERROR_SUCCESS) {
    // Применяем новый DACL и отключаем наследование из родительских веток,
    // чтобы ничто больше не переопределяло права
    res = SetNamedSecurityInfoW((LPWSTR)formattedPath.c_str(), SE_REGISTRY_KEY,
                                DACL_SECURITY_INFORMATION |
                                    PROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, pNewDACL, nullptr);
    LocalFree(pNewDACL);
  }

  FreeSid(pAdminSid);
  return res == ERROR_SUCCESS;
}

} // namespace ZaslonUltimate
