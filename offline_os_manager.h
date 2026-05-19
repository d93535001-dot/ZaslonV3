#pragma once

#include <string>
#include <windows.h>

namespace ZaslonCore {

class OfflineOSManager {
public:
  // Ищет оффлайн Windows (например D:\Windows) перебирая диски.
  // Возвращает true если удалось найти и заполняет g_OfflineOSPath.
  static bool DetectOfflineOS();

  // Монтирует ветки SOFTWARE и SYSTEM оффлайн-реестра в HKLM
  static bool MountOfflineHives();

  // Отмонтирует ветки
  static void UnmountOfflineHives();

  // Вспомогательная функция для получения правильного HKEY
  // Если WinPE - возвращает смонтированный куст (надо обязательно закрыть ключи потом).
    // Registry Redirection: Returns a handle to the key in the offline hives if mounted, 
    // otherwise opens the real live registry key.
    static HKEY OpenRedirectedKey(HKEY hkrRoot, const wchar_t* subKey, REGSAM samDesired);

    // Path Redirection: Returns full absolute path on the offline drive (e.g., C:\Windows\...)
    static std::wstring GetOfflinePath(const std::wstring& relativeToWindows);
};

// Global state for WinPE mode
extern std::wstring g_OfflineOSPath;       // e.g. "D:\Windows"
extern std::wstring g_OfflineDriveLetter;  // e.g. "D:"
extern bool g_IsOfflineHivesMounted;

} // namespace ZaslonCore
