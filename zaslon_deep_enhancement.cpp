#include "zaslon_deep_enhancement.h"
#include "zaslon_logger.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "psapi.lib")
#endif

namespace Zaslon {
namespace Enhancement {

double PEInspector::CalculateEntropy(const uint8_t* data, size_t size) {
    if (size == 0) return 0.0;

    std::map<uint8_t, size_t> counts;
    for (size_t i = 0; i < size; ++i) {
        counts[data[i]]++;
    }

    double entropy = 0.0;
    for (const auto& pair : counts) {
        double p = static_cast<double>(pair.second) / size;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

std::optional<PEInfo> PEInspector::AnalyzePEFile(const std::wstring& file_path) {
    PEInfo info;
#ifdef _WIN32
    HANDLE hFile = CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        ZLOG_ERROR("PEInspector: Failed to open file for analysis.");
        return std::nullopt;
    }

    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        return std::nullopt;
    }

    LPVOID pBase = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pBase) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return std::nullopt;
    }


    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < sizeof(IMAGE_DOS_HEADER)) {
        UnmapViewOfFile(pBase);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return std::nullopt;
    }

    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)pBase;
    if (pDosHeader->e_magic == IMAGE_DOS_SIGNATURE) {
        // Bounds check before accessing NT headers
        if (pDosHeader->e_lfanew > 0 && pDosHeader->e_lfanew < (fileSize.QuadPart - sizeof(IMAGE_NT_HEADERS32))) {
            PIMAGE_NT_HEADERS pNtHeaders = (PIMAGE_NT_HEADERS)((uint8_t*)pBase + pDosHeader->e_lfanew);
            if (pNtHeaders->Signature == IMAGE_NT_SIGNATURE) {

            info.is_valid_pe = true;
            info.image_base = pNtHeaders->OptionalHeader.ImageBase;
            info.entry_point = pNtHeaders->OptionalHeader.AddressOfEntryPoint;
            info.subsystem = pNtHeaders->OptionalHeader.Subsystem;
            info.is_64bit = (pNtHeaders->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

            LARGE_INTEGER fileSize;
            GetFileSizeEx(hFile, &fileSize);
            size_t entropySize = (fileSize.QuadPart > 4096) ? 4096 : fileSize.QuadPart;
            info.overall_entropy = CalculateEntropy(static_cast<const uint8_t*>(pBase), entropySize);

            if (info.overall_entropy > 7.2) {
                info.has_suspicious_sections = true;
            }
        }
        }
    }

    UnmapViewOfFile(pBase);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return info;
#else
    // Linux Fallback for testing structure
    info.is_valid_pe = true;
    info.overall_entropy = 7.5;
    info.has_suspicious_sections = true;
    return info;
#endif
}

std::vector<PersistenceEntry> PersistenceHunter::ScanAppInitDLLs() {
    std::vector<PersistenceEntry> results;
#ifdef _WIN32
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[1024];
        DWORD value_length = sizeof(value);
        if (RegQueryValueExW(hKey, L"AppInit_DLLs", NULL, NULL, (LPBYTE)&value, &value_length) == ERROR_SUCCESS) {
            if (value_length > 2) {
                results.push_back({
                    L"AppInit_DLL",
                    L"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows\\AppInit_DLLs",
                    value,
                    L"Global DLL Injection Mechanism"
                });
            }
        }
        RegCloseKey(hKey);
    }
#else
    results.push_back({L"Mock_AppInit", L"Mock_Location", L"Mock_Target", L"Mock"});
#endif
    return results;
}

std::vector<PersistenceEntry> PersistenceHunter::ScanWMIPersistence() {
    std::vector<PersistenceEntry> results;
#ifdef _WIN32
    HRESULT hres;
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) return results;

    hres =  CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hres)) { CoUninitialize(); return results; }

    IWbemLocator *pLoc = NULL;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *) &pLoc);
    if (FAILED(hres)) { CoUninitialize(); return results; }

    IWbemServices *pSvc = NULL;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hres)) { pLoc->Release(); CoUninitialize(); return results; }

    hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hres)) { pSvc->Release(); pLoc->Release(); CoUninitialize(); return results; }

    IEnumWbemClassObject* pEnumerator = NULL;
    hres = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM __EventConsumer"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator);

    if (SUCCEEDED(hres)) {
        IWbemClassObject *pclsObj = NULL;
        ULONG uReturn = 0;
        while (pEnumerator) {
            HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
            if(0 == uReturn) break;

            VARIANT vtProp;
            hr = pclsObj->Get(L"Name", 0, &vtProp, 0, 0);
            std::wstring name = (vtProp.vt == VT_BSTR) ? vtProp.bstrVal : L"Unknown";
            VariantClear(&vtProp);

            results.push_back({
                L"WMI Event Consumer",
                L"ROOT\\CIMV2",
                name,
                L"Potential WMI Persistence"
            });
            pclsObj->Release();
        }
        pEnumerator->Release();
    }

    pSvc->Release();
    pLoc->Release();
    CoUninitialize();
#else
    results.push_back({L"Mock_WMI", L"Mock_Location", L"Mock_Target", L"Mock"});
#endif
    return results;
}

#ifdef _WIN32
typedef NTSTATUS (WINAPI *PNT_QUERY_SYSTEM_INFORMATION)(ULONG, PVOID, ULONG, PULONG);
#define SystemHandleInformation 16
#endif

HardwareMetrics HardwareTelemetry::GatherMetricsRing3() {
    HardwareMetrics metrics;
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    metrics.available_physical_memory = memInfo.ullAvailPhys;

    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        auto pNtQuerySystemInformation = (PNT_QUERY_SYSTEM_INFORMATION)GetProcAddress(hNtDll, "NtQuerySystemInformation");
        if (pNtQuerySystemInformation) {
            ULONG returnLength = 0;
            pNtQuerySystemInformation(SystemHandleInformation, NULL, 0, &returnLength);
            if (returnLength > 0) {
                std::vector<uint8_t> buffer(returnLength);

                NTSTATUS status = pNtQuerySystemInformation(SystemHandleInformation, buffer.data(), returnLength, &returnLength);
                if (status == 0) {
                    // SystemHandleInformation struct starts with a ULONG for the count of handles
                    if (buffer.size() >= sizeof(ULONG)) {
                        metrics.active_handles = *(ULONG*)buffer.data();
                    }
                }

            }
        } else {
             metrics.possible_hook_detected = true;
        }
    }
    metrics.cpu_usage_percent = 5.0;
#else
    metrics.available_physical_memory = 1024 * 1024 * 1024; // 1 GB
    metrics.active_handles = 150;
    metrics.possible_hook_detected = false;
#endif
    return metrics;
}

} // namespace Enhancement
} // namespace Zaslon
