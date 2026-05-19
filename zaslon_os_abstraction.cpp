#include "zaslon_os_abstraction.h"
#include "zaslon_logger.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Zaslon {
namespace Abstraction {

// --------------------------------------------------------------------------
// LiveFileSystemManager Implementation
// --------------------------------------------------------------------------
bool LiveFileSystemManager::FileExists(const std::wstring& path) const {
#ifdef _WIN32
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    return false;
#endif
}

std::vector<std::wstring> LiveFileSystemManager::EnumerateDirectory(const std::wstring& path) const {
    std::vector<std::wstring> files;
#ifdef _WIN32
    WIN32_FIND_DATAW findData;
    std::wstring searchPath = path + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") != 0 && wcscmp(findData.cFileName, L"..") != 0) {
                files.push_back(findData.cFileName);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
#endif
    return files;
}

std::optional<std::vector<uint8_t>> LiveFileSystemManager::ReadFileRaw(const std::wstring& path) const {
    // Stub for live raw file reading
    return std::nullopt;
}

// --------------------------------------------------------------------------
// LiveRegistryManager Implementation
// --------------------------------------------------------------------------
std::optional<std::wstring> LiveRegistryManager::ReadStringValue(const std::wstring& key_path, const std::wstring& value_name) const {
    // Stub for live registry reading
    ZLOG_INFO("LiveRegistryManager::ReadStringValue stub called.");
    return std::nullopt;
}

std::vector<std::wstring> LiveRegistryManager::EnumerateSubKeys(const std::wstring& key_path) const {
    // Stub for live registry enumeration
    return {};
}

// --------------------------------------------------------------------------
// LiveProcessManager Implementation
// --------------------------------------------------------------------------
std::vector<uint32_t> LiveProcessManager::EnumerateProcesses() const {
    // Stub for live process enumeration
    return {};
}

bool LiveProcessManager::TerminateProcess(uint32_t pid) const {
    // Stub for live process termination
    return false;
}

// --------------------------------------------------------------------------
// OfflineFileSystemManager Implementation
// --------------------------------------------------------------------------
std::wstring OfflineFileSystemManager::TranslatePath(const std::wstring& path) const {
    // Example: translate "C:\Windows\System32" to "D:\Windows\System32" (where D: is m_target_drive)
    if (path.length() >= 3 && path[1] == L':' && path[2] == L'\\') {
        return m_target_drive + path.substr(2);
    }
    return path;
}

bool OfflineFileSystemManager::FileExists(const std::wstring& path) const {
    std::wstring translated = TranslatePath(path);

#ifdef _WIN32
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, translated.c_str(), (int)translated.length(), NULL, 0, NULL, NULL);
    std::string str_translated(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, translated.c_str(), (int)translated.length(), &str_translated[0], size_needed, NULL, NULL);
    ZLOG_INFO("OfflineFileSystemManager: Checking file: {}", str_translated);
    DWORD attr = GetFileAttributesW(translated.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    return false;
#endif
}

std::vector<std::wstring> OfflineFileSystemManager::EnumerateDirectory(const std::wstring& path) const {
    std::wstring translated = TranslatePath(path);
    std::vector<std::wstring> files;
#ifdef _WIN32
    WIN32_FIND_DATAW findData;
    std::wstring searchPath = translated + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") != 0 && wcscmp(findData.cFileName, L"..") != 0) {
                files.push_back(findData.cFileName);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
#endif
    return files;
}

std::optional<std::vector<uint8_t>> OfflineFileSystemManager::ReadFileRaw(const std::wstring& path) const {
     // Stub for offline raw file reading
     return std::nullopt;
}

// --------------------------------------------------------------------------
// OfflineRegistryManager Implementation
// --------------------------------------------------------------------------
bool OfflineRegistryManager::InitializeOfflineHives(const std::wstring& system_root) {
#ifdef _WIN32
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, system_root.c_str(), (int)system_root.length(), NULL, 0, NULL, NULL);
    std::string str_root(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, system_root.c_str(), (int)system_root.length(), &str_root[0], size_needed, NULL, NULL);
    ZLOG_INFO("OfflineRegistryManager: Loading offline hives from: {}", str_root);
#else
    ZLOG_INFO("OfflineRegistryManager: Loading offline hives from: {}", std::string(system_root.begin(), system_root.end()));
#endif
    // Implementation would use RegLoadKey to mount the hives temporarily.
    return true;
}

std::wstring OfflineRegistryManager::TranslateKeyPath(const std::wstring& path) const {
    // Translates virtual paths to offline paths
    // e.g. HKEY_LOCAL_MACHINE\Software -> HKEY_LOCAL_MACHINE\Zaslon_Offline_Software
    return path; // Stub
}

std::optional<std::wstring> OfflineRegistryManager::ReadStringValue(const std::wstring& key_path, const std::wstring& value_name) const {
    ZLOG_INFO("OfflineRegistryManager: ReadStringValue stub called.");
    return std::nullopt;
}

std::vector<std::wstring> OfflineRegistryManager::EnumerateSubKeys(const std::wstring& key_path) const {
    return {};
}

} // namespace Abstraction
} // namespace Zaslon
