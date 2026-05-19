/**
 * ntapi_defs.h
 * ZASLON — Shared NT API type definitions
 *
 * Centralises undocumented NTAPI structs and function-pointer typedefs
 * so that they are defined exactly once across the entire project.
 *
 * This header is self-contained: it conditionally defines NTSTATUS and
 * NTAPI so it can be included before or after <winternl.h>.
 */
#pragma once
#include <windows.h>

// ─── Ensure NTSTATUS and NTAPI are available ────────────────────────────
// <winternl.h> may or may not be included before this header.
#ifndef _NTDEF_
  #ifndef NTSTATUS
    typedef LONG NTSTATUS;
  #endif
#endif

#ifndef NTAPI
  #define NTAPI __stdcall
#endif

// ─── System information class constants ─────────────────────────────────
#define ZASLON_SystemHandleInformation 16

// ─── Function-pointer typedefs for runtime-resolved NTAPI ───────────────

typedef NTSTATUS(NTAPI *pfnNtQueryInformationProcess)(
    HANDLE ProcessHandle, ULONG ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength,
    PULONG ReturnLength);

typedef NTSTATUS(NTAPI *pfnNtSetInformationProcess)(
    HANDLE ProcessHandle, ULONG ProcessInformationClass,
    PVOID ProcessInformation, ULONG ProcessInformationLength);

typedef NTSTATUS(NTAPI *pfnNtTerminateProcess)(HANDLE ProcessHandle,
                                                NTSTATUS ExitStatus);

typedef NTSTATUS(NTAPI *pfnNtQuerySystemInformation)(
    ULONG SystemInformationClass, PVOID SystemInformation,
    ULONG SystemInformationLength, PULONG ReturnLength);

typedef NTSTATUS(NTAPI *pfnNtSuspendProcess)(HANDLE ProcessHandle);
typedef NTSTATUS(NTAPI *pfnNtResumeProcess)(HANDLE ProcessHandle);

// ─── Undocumented handle enumeration structures ─────────────────────────

typedef struct _ZASLON_SYSTEM_HANDLE_TABLE_ENTRY_INFO {
  USHORT UniqueProcessId;
  USHORT CreatorBackTraceIndex;
  UCHAR ObjectTypeIndex;
  UCHAR HandleAttributes;
  USHORT HandleValue;
  PVOID Object;
  ULONG GrantedAccess;
} ZASLON_SYSTEM_HANDLE_TABLE_ENTRY_INFO,
    *PZASLON_SYSTEM_HANDLE_TABLE_ENTRY_INFO;

typedef struct _ZASLON_SYSTEM_HANDLE_INFORMATION {
  ULONG NumberOfHandles;
  ZASLON_SYSTEM_HANDLE_TABLE_ENTRY_INFO Handles[1];
} ZASLON_SYSTEM_HANDLE_INFORMATION, *PZASLON_SYSTEM_HANDLE_INFORMATION;
