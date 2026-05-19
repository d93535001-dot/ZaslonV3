#pragma once


// clang-format off
// CRITICAL: winsock2.h MUST come before windows.h
// WIN32_LEAN_AND_MEAN (defined in CMake) prevents windows.h from including old winsock.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// NOTE: iphlpapi.h is NOT included here — it conflicts with WIN32_LEAN_AND_MEAN.
// Include it only in .cpp files that need it, AFTER windows.h.
// clang-format on

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace ZaslonCore {

// =========================================================
// MODULE 1: THE TERMINATOR (Extreme Process Killer)
// =========================================================

// Пытается убить процесс с 7 уровнями эскалации (включая Debugger Attach)
bool ForceKillProcess(DWORD pid);

// =========================================================
// MODULE 2: SMART UNLOCKER (Restart Manager API)
// =========================================================

struct LockerInfo {
  DWORD Pid;
  std::wstring Name;    // Имя исполняемого файла
  std::wstring AppType; // Тип (Service, Explorer, Standalone и т.д.)
};

class FileUnlocker {
public:
  // Возвращает список процессов, которые держат лок на файл/папку
  static std::vector<LockerInfo>
  GetLockingProcesses(const std::wstring &filePath);

  // Вежливо (или принудительно) просит процессы отпустить файл через Restart
  // Manager
  static bool UnlockFile(const std::wstring &filePath,
                         std::vector<LockerInfo> &outLockers);
};

// =========================================================
// MODULE 3: HEURISTIC RISK ENGINE (Pro AnVir/ProcessHacker Level)
// =========================================================

struct RiskResult {
  int Score; // 0 - 100%
  std::vector<std::string> Reasons;
};

struct ProcessHeuristicData {
  DWORD Pid;
  std::wstring ImageName;
  std::wstring FullPath;
  std::wstring FileDescription;
  std::wstring CompanyName;

  bool IsMicrosoftSigned;
  bool HasValidSignature;
  bool IsWindowVisible;
  float CpuUsagePercent;

  uint64_t CreationTime;

  // Advanced Heuristics (20+ checks)
  bool RunFromTemp;
  bool RunFromAppData;
  bool RunFromDownloads;
  bool RunFromProgramData;

  bool HasDoubleExtension;
  bool IsDotNet;
  bool IsPacked;
  bool HasSuspiciousImports; // VirtualAlloc, WriteProcessMemory,
                             // CreateRemoteThread

  bool ParentMismatch;
  bool ArchMismatch;

  bool HasAutoRun;
  bool HasSuspiciousPrivileges;

  // Phase 4 v2.3: Enhanced analysis
  float MaxSectionEntropy; // Shannon entropy, >7.0 = suspicious
                           // (packed/encrypted)
  bool HasZoneIdentifier; // File downloaded from internet (Zone.Identifier ADS)
  uint64_t FileAge;       // File age in hours since creation
};

// Сбор полного спектра данных (CPU, окна, подписи, пути) для процесса
ProcessHeuristicData GatherProcessHeuristics(DWORD pid);

// Вычисляет продвинутый эвристический рейтинг угрозы (инжекты, упаковщики,
// маскировка)
RiskResult CalculateRiskScore(const ProcessHeuristicData &data);

// File Inspector Static Analysis
bool CheckSignature(const std::wstring &path);
bool CheckPEFeatures(const std::wstring &path, bool &isPacked, bool &isDotNet,
                     bool &hasSuspiciousImports, float &maxEntropy);

// =========================================================
// MODULE 4: OVERLAY HUNTER MODE (Снайперский прицел)
// =========================================================

struct HunterTarget {
  HWND Hwnd;
  DWORD Pid;
  std::wstring ImageName;
  RECT Bounds;
};

class HunterMode {
public:
  // Создает прозрачное окно-рамку для идеальной подсветки (без мерцания GDI)
  static void InitializeOverlay();
  static void ShutdownOverlay();

  // Поиск окна под текущими координатами курсора
  static HunterTarget GetTargetFromPoint(POINT pt);

  // Рисует красную рамку поверх всего экрана (без артефактов)
  static void DrawOverlayRect(const RECT &rect);
};

// =========================================================
// MODULE 5: PROCESS INSTALL MONITOR (Sandbox Analyzer)
// =========================================================

struct SandboxSnapshot {
  std::vector<std::wstring> Files;
  std::vector<std::wstring> RegistryKeys;
};

struct SandboxSpawnedProcess {
  DWORD Pid;
  std::wstring ImageName;
  std::wstring FullPath;
  std::wstring CommandLine;
};

struct SandboxDiffResult {
  std::vector<std::wstring> AddedFiles;
  std::vector<std::wstring> AddedRegistryKeys;

  // Advanced Analysis Fields
  std::vector<SandboxSpawnedProcess> SpawnedProcesses;
  std::vector<std::wstring> SuspiciousCopies; // Same size as original
  std::vector<std::wstring> TracingFiles;     // Touched AppData/Roaming etc
};

class SandboxAnalyzer {
public:
  // v2.5.3: Zero-Lag Real-Time Installation Tracker
  static void StartRealTimeTracking();
  static void StopRealTimeTracking(const std::wstring &originalFile);
  static SandboxDiffResult GetRealTimeDiff();

  // Откат всех изменений: удаление ключей, файлов, убийство запущенных
  // процессов
  static void Rollback(const SandboxDiffResult &diff);

  // Мониторинг запущенных дочерних процессов
  static void TrackProcesses(DWORD targetPid,
                             std::vector<SandboxSpawnedProcess> &outSpawned);
};

// =========================================================
// MODULE 6: ANTI-WINLOCKER (Isolated Desktop)
// =========================================================

class AntiWinLocker {
public:
  // Создает новый чистый Desktop и запускает на нем процесс (например cmd.exe)
  // Вирусы-винлокеры остаются на 'Default' рабочем столе.
  static bool LaunchIsolatedDesktop(const std::wstring &processToLaunch);

  // Фоновый поток для авто-детекта локеров
  static void StartWinLockerGuardian();
  static void StopWinLockerGuardian();
};

} // namespace ZaslonCore

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace ZaslonUltimate {

// =========================================================
// MODULE 1: PRIVILEGE ESCALATION (TOKEN STEALING)
// =========================================================
class TokenStealer {
public:
  // Ищет winlogon.exe или lsass.exe, дублирует токен и включает привилегии
  // (SeDebug, SeRestore)
  static HANDLE GetSystemToken();

  // Использует украденный токен для запуска процесса
  static bool RunAsTrustedInstaller(std::wstring cmdLine);
};

// =========================================================
// MODULE 3: NETWORK RADAR (KILL CONNECTIONS)
// =========================================================

// Binary-compatible replacement for MIB_TCPROW_OWNER_PID
// This avoids including <iphlpapi.h> in the header (conflicts with
// WIN32_LEAN_AND_MEAN)
struct TcpRowOwnerPid {
  DWORD dwState;
  DWORD dwLocalAddr;
  DWORD dwLocalPort;
  DWORD dwRemoteAddr;
  DWORD dwRemotePort;
  DWORD dwOwningPid;
};

struct ConnectionInfo {
  DWORD Pid;
  std::wstring LocalIP;
  std::wstring RemoteIP;
  DWORD RemotePort;
  DWORD State;
  TcpRowOwnerPid RowData; // For passing to CloseConnection
};

class NetworkManager {
public:
  // Возвращает список активных TCP соединений
  static std::vector<ConnectionInfo> GetActiveConnections();

  // Принудительно разрывает TCP соединение без убийства процесса
  static bool CloseConnection(TcpRowOwnerPid row);
};

// =========================================================
// MODULE 4: OFFLINE PERMISSION RESET (ACL EDITOR)
// =========================================================

// Включает SeTakeOwnershipPrivilege, меняет владельца ветки реестра на
// Администраторов и сбрасывает DACL на KEY_ALL_ACCESS
bool ForceTakeOwnership(std::wstring registryKeyPath);

} // namespace ZaslonUltimate
